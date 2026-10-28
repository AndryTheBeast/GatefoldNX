/*
 * Gatefold NX background player · main.cpp
 * The sysmodule's start-up and its IPC service ("gatefold"): a small CMIF server on the main
 * thread. libnx has no server framework, so requests are parsed from TLS by hand; the protocol
 * is gfbg.h. Atmosphère starts the module at boot (flags/boot2.flag).
 */
#include <switch.h>

#include <cstring>

#include "engine.hpp"
#include "gfbg.h"

#define HEAP_SIZE 0x180000   // 1.5 MB: decoders, tables, queue paths and audout buffers

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
size_t gfHeapSize = HEAP_SIZE;

void __libnx_initheap(void) {
    static u8 heap[HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof heap;
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    pmdmntInitialize();
    timeInitialize();     // the clock for play statistics (bg-plays.txt); without it plays are dated by Gatefold
    pdmqryInitialize();   // failing is fine: then "in front" just means "running"
    if (hosversionAtLeast(11, 0, 0)) audaInitialize();   // game volume
    else audoutaInitialize();
    // sm stays open: audout is opened later, from the audio thread, once audio is up.
}

void __appExit(void) {
    pdmqryExit();
    audaExit();
    audoutaExit();
    pmdmntExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

}  // extern "C"

namespace {

Engine engine;

/** Writes a CMIF reply into TLS: the result, then out (size bytes). */
void reply(void* base, Result rc, const void* out = nullptr, size_t size = 0) {
    HipcMetadata meta = {};
    meta.num_data_words = u32((16 + sizeof(CmifOutHeader) + size + 3) / 4);
    HipcRequest h = hipcMakeRequest(base, meta);
    auto* o = static_cast<CmifOutHeader*>(cmifGetAlignedDataStart(h.data_words, base));
    o->magic = CMIF_OUT_HEADER_MAGIC;
    o->version = 0;
    o->result = rc;
    o->token = 0;
    if (size) memcpy(o + 1, out, size);
}

constexpr Result BadArgs = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result NoSuchCmd = MAKERESULT(Module_Libnx, LibnxError_NotFound);
constexpr Result QueueTooBig = GFBG_QUEUE_TOO_BIG;

struct Session { Handle h; u32 kind; };

/** One request: false when the session is to be closed (the client asked, or sent garbage). */
bool handle(Session& s) {
    void* base = armGetTls();
    const HipcParsedRequest r = hipcParseRequest(base);
    const u32 type = r.meta.type;
    if (type == CmifCommandType_Close) return false;
    auto* in = static_cast<CmifInHeader*>(cmifGetAlignedDataStart(r.data.data_words, base));
    if (type == CmifCommandType_Control || type == CmifCommandType_ControlWithContext) {
        // libnx asks QueryPointerBufferSize (3) when it opens a session; we have no pointer buffer.
        if (in->command_id == 3) { const u16 zero = 0; reply(base, 0, &zero, sizeof zero); }
        else reply(base, NoSuchCmd);
        return true;
    }
    if ((type != CmifCommandType_Request && type != CmifCommandType_RequestWithContext) || in->magic != CMIF_IN_HEADER_MAGIC) return false;

    // The arguments are copied out before the reply overwrites the same TLS.
    u8 arg[160];   // the largest is GfbgEq (116 bytes, protocol 5); TLS holds 0x100 in all
    memcpy(arg, in + 1, sizeof arg);
    const u32 cmd = in->command_id;
    auto sendBuf = [&](size_t& n) -> u8* {
        if (r.meta.num_send_buffers < 1) return nullptr;
        n = hipcGetBufferSize(&r.data.send_buffers[0]);
        return static_cast<u8*>(hipcGetBufferAddress(&r.data.send_buffers[0]));
    };
    auto recvBuf = [&](size_t& n) -> u8* {
        if (r.meta.num_recv_buffers < 1) return nullptr;
        n = hipcGetBufferSize(&r.data.recv_buffers[0]);
        return static_cast<u8*>(hipcGetBufferAddress(&r.data.recv_buffers[0]));
    };
    u32 u = 0;
    memcpy(&u, arg, 4);

    switch (cmd) {
    case GfbgCmd_Hello: {
        s.kind = u;
        engine.hello(u, r.meta.send_pid ? r.pid : 0);
        const u32 protocol = GFBG_PROTOCOL;
        reply(base, 0, &protocol, sizeof protocol);
        break;
    }
    case GfbgCmd_GetState: {
        const GfbgState st = engine.state();
        reply(base, 0, &st, sizeof st);
        break;
    }
    case GfbgCmd_SetQueue: {
        size_t n = 0;
        const u8* p = sendBuf(n);
        if (!p) { reply(base, BadArgs); break; }
        reply(base, engine.setQueue(p, n, s.kind) ? 0 : QueueTooBig);
        break;
    }
    case GfbgCmd_GetQueue: {
        size_t n = 0;
        u8* p = recvBuf(n);
        if (!p) { reply(base, BadArgs); break; }
        engine.getQueue(p, n);
        reply(base, 0);
        break;
    }
    case GfbgCmd_PlayAt: {
        GfbgPlayAt a;
        memcpy(&a, arg, sizeof a);
        engine.playAt(a.pos, a.paused != 0, a.start, s.kind);
        reply(base, 0);
        break;
    }
    case GfbgCmd_Transport: engine.transport(u, s.kind); reply(base, 0); break;
    case GfbgCmd_Seek: {
        double t;
        memcpy(&t, arg, sizeof t);
        engine.seek(t);
        reply(base, 0);
        break;
    }
    case GfbgCmd_SetVolume: {
        float v;
        memcpy(&v, arg, sizeof v);
        engine.setVolume(v);
        reply(base, 0);
        break;
    }
    case GfbgCmd_SetMode: engine.setMode(u, s.kind); reply(base, 0); break;
    case GfbgCmd_SetStopAfter: engine.setStopAfter(u != 0); reply(base, 0); break;
    case GfbgCmd_SetSleep: {
        float v;
        memcpy(&v, arg, sizeof v);
        engine.setSleep(v);
        reply(base, 0);
        break;
    }
    case GfbgCmd_GetPcm: {
        size_t n = 0;
        u8* p = recvBuf(n);
        if (!p || n < GFBG_PCM * sizeof(float)) { reply(base, BadArgs); break; }
        const u32 serial = engine.pcm(reinterpret_cast<float*>(p));
        reply(base, 0, &serial, sizeof serial);
        break;
    }
    case GfbgCmd_GetConfig: {
        const GfbgConfig c = engine.config();
        reply(base, 0, &c, sizeof c);
        break;
    }
    case GfbgCmd_SetConfig: {
        GfbgConfig c;
        memcpy(&c, arg, sizeof c);
        engine.setConfig(c);
        reply(base, 0);
        break;
    }
    case GfbgCmd_GetTrackInfo: {
        size_t n = 0;
        u8* p = recvBuf(n);
        if (!p || n < sizeof(GfbgTrackInfo)) { reply(base, BadArgs); break; }
        engine.trackInfo(reinterpret_cast<GfbgTrackInfo*>(p));
        reply(base, 0);
        break;
    }
    case GfbgCmd_GetGameVolume: {
        const GfbgGameVolume g = engine.gameVolume();
        reply(base, 0, &g, sizeof g);
        break;
    }
    case GfbgCmd_SetGameVolume: {
        float v;
        memcpy(&v, arg, sizeof v);
        engine.setGameVolume(v);
        reply(base, 0);
        break;
    }
    case GfbgCmd_GetEq: {
        const GfbgEq e = engine.eq();
        reply(base, 0, &e, sizeof e);
        break;
    }
    case GfbgCmd_SetEq: {
        GfbgEq e;
        memcpy(&e, arg, sizeof e);
        engine.setEq(e);
        reply(base, 0);
        break;
    }
    default: reply(base, NoSuchCmd); break;
    }
    return true;
}

}  // namespace

int main(int, char**) {
    engine.start();

    constexpr int MaxSessions = 8;
    Handle port = INVALID_HANDLE;
    if (R_FAILED(smRegisterService(&port, smEncodeName(GFBG_SERVICE), false, MaxSessions))) return 0;

    Session sessions[MaxSessions];
    int n = 0;
    auto drop = [&](int i) {
        svcCloseHandle(sessions[i].h);
        sessions[i] = sessions[--n];
    };
    for (;;) {
        Handle handles[MaxSessions + 1];
        handles[0] = port;
        for (int i = 0; i < n; i++) handles[i + 1] = sessions[i].h;
        s32 idx = -1;
        if (R_FAILED(svcWaitSynchronization(&idx, handles, n + 1, UINT64_MAX))) continue;
        if (idx == 0) {
            Handle h;
            if (R_SUCCEEDED(svcAcceptSession(&h, port))) {
                if (n < MaxSessions) sessions[n++] = { h, 0 };
                else svcCloseHandle(h);
            }
            continue;
        }
        const int i = idx - 1;
        s32 got = -1;
        if (R_FAILED(svcReplyAndReceive(&got, &sessions[i].h, 1, INVALID_HANDLE, UINT64_MAX))) { drop(i); continue; }
        if (!handle(sessions[i])) { drop(i); continue; }
        // Reply only (no handles to receive on): the kernel reports a timeout, which is success here.
        const Result rc = svcReplyAndReceive(&got, nullptr, 0, sessions[i].h, 0);
        if (R_FAILED(rc) && R_VALUE(rc) != KERNELRESULT(TimedOut)) drop(i);
    }
}
