/*
 * Gatefold NX overlay · tune_gfbg.cpp
 * sys-tune's client API (tune.h), implemented on Gatefold's background player (gfbg.h), so the
 * sys-tune 2.1 overlay this one is built from runs unchanged on top of it.
 *
 * - The "queue" sys-tune pages index is our play order: item i is paths[order[i]].
 * - Queue edits (clear, enqueue, remove, move) change a local copy and go to the player as one
 *   SetQueue just before the next call that needs it: sys-tune clears and re-adds a folder one
 *   file at a time, which would otherwise send the whole queue once per file.
 * - sys-tune has repeat and shuffle apart; Gatefold has one order (Default, Repeat list, Repeat
 *   track, Shuffle, where Shuffle repeats the list). The two map onto it as closely as they can.
 * - Positions are passed as milliseconds (sample_rate 1000).
 * - Paths: sys-tune's have no mount name ("/Music/…"), Gatefold's start with "sdmc:".
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gfbg.h"
#include "tune.h"
#include "tune_gfbg.hpp"

namespace {

Service srv;
bool open = false;

struct Queue {
    std::vector<std::string> paths;
    std::vector<s32> order;
    s32 pos = -1;
    u32 mode = GfbgMode_Default;
    std::string name;
    u32 gen = 0;
    bool valid = false;   // matches the player's queueGen
    bool dirty = false;   // changed here, not sent yet
    bool fresh = false;   // emptied here and filled again (a folder, Add all, the saved playlist), not played yet
    bool sent = false;    // … and already sent to the player (shown in the overlay's queue) since it was emptied
} q;

/** strnlen, which the overlay's strict -std=c++26 hides. */
size_t boundedLen(const char* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) n++;
    return n;
}

GfbgState state() {
    GfbgState s{};
    if (open) gfbgGetState(&srv, &s);
    return s;
}

/** The player's queue, unless ours has edits it has not had yet. */
void ensureQueue() {
    if (!open || q.dirty) return;
    const GfbgState s = state();
    if (q.valid && q.gen == s.queueGen) { q.pos = s.pos; q.mode = s.mode; return; }
    if (s.queueBy != GfbgClient_Overlay) q.fresh = false;   // someone else's queue now (Gatefold's, the player's)
    std::vector<u8> buf(s.queueBytes + 4096);
    if (s.queueBytes < sizeof(GfbgQueueHeader) || R_FAILED(gfbgGetQueue(&srv, buf.data(), buf.size()))) return;
    GfbgQueueHeader h;
    memcpy(&h, buf.data(), sizeof h);
    if (sizeof h + size_t(h.count) * 4 + h.pathBytes > buf.size()) return;
    q.order.resize(h.count);
    memcpy(q.order.data(), buf.data() + sizeof h, size_t(h.count) * 4);
    q.paths.clear();
    const char* p = reinterpret_cast<const char*>(buf.data() + sizeof h + size_t(h.count) * 4);
    const char* end = p + h.pathBytes;
    while (p < end && q.paths.size() < h.count) {
        const size_t n = boundedLen(p, size_t(end - p));
        q.paths.emplace_back(p, n);
        p += n + 1;
    }
    q.pos = h.pos;
    q.mode = h.mode;
    q.name.assign(h.name, boundedLen(h.name, sizeof h.name));
    q.gen = s.queueGen;
    q.valid = q.paths.size() == h.count;
}

bool shuffleOn(u32 mode) { return mode == GfbgMode_Shuffle; }

u64 rnd(u64 n) {
    static u64 seed = armGetSystemTick() | 1;
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed % n;
}

/**
 * The player takes the order it is sent, and a queue built here was in the order it was added: with
 * Shuffle on, a folder (Browse, or Add all) played in order until Shuffle was switched off and on.
 * So with Shuffle on every track added goes in at a random place (tuneEnqueue): anywhere in a queue
 * being filled from empty, after the current track in one that plays. The queue shows shuffled at
 * once. A fresh queue's first start may put the track picked in front (path index; −1 none).
 */
void startFresh(s32 first) {
    if (!q.fresh) return;
    q.fresh = false;
    if (first < 0 || q.order.size() < 2 || !shuffleOn(state().mode)) return;
    const auto it = std::find(q.order.begin(), q.order.end(), first);
    if (it == q.order.end() || it == q.order.begin()) return;
    q.order.erase(it);
    q.order.insert(q.order.begin(), first);
    q.pos = 0;
    q.dirty = true;
}

/** Sends the local edits as one SetQueue. */
void flush() {
    if (!open || !q.dirty) return;
    if (q.fresh) q.sent = true;
    q.dirty = false;
    q.valid = false;
    size_t bytes = 0;
    for (const auto& p : q.paths) bytes += p.size() + 1;
    std::vector<u8> buf(sizeof(GfbgQueueHeader) + q.order.size() * 4 + bytes);
    GfbgQueueHeader h{};
    h.count = u32(q.paths.size());
    h.pos = q.order.empty() ? -1 : std::clamp(q.pos, 0, s32(q.order.size()) - 1);
    h.mode = q.mode = state().mode;   // the player's own: ours may be from before a Shuffle switch
    h.flags = GfbgQueue_KeepCurrent;
    h.pathBytes = u32(bytes);
    // A list built here (a folder from Browse) is named after its folder, as Gatefold names its lists.
    std::string name = q.name;
    if (name.empty() && !q.paths.empty()) {
        const std::string& f = q.paths[0];
        const size_t s = f.rfind('/'), s2 = s == std::string::npos ? s : f.rfind('/', s - 1);
        name = s2 == std::string::npos ? "Overlay" : f.substr(s2 + 1, s - s2 - 1);
    }
    memcpy(h.name, name.c_str(), std::min(name.size(), sizeof h.name - 1));
    memcpy(buf.data(), &h, sizeof h);
    memcpy(buf.data() + sizeof h, q.order.data(), q.order.size() * 4);
    u8* p = buf.data() + sizeof h + q.order.size() * 4;
    for (const auto& s : q.paths) { memcpy(p, s.c_str(), s.size() + 1); p += s.size() + 1; }
    gfbgSetQueue(&srv, buf.data(), buf.size());
}

/** Gatefold's path as sys-tune pages expect it, without "sdmc:". */
const char* bare(const std::string& p) { return p.c_str() + (p.rfind("sdmc:/", 0) == 0 ? 5 : 0); }

/** And back: what the player opens and Gatefold finds in its library. */
std::string mounted(const char* p) { return p[0] == '/' ? std::string("sdmc:") + p : std::string(p); }

TuneRepeatMode repeatOf(u32 mode) {
    if (mode == GfbgMode_RepeatOne) return TuneRepeatMode_One;
    if (mode == GfbgMode_RepeatAll || mode == GfbgMode_Shuffle) return TuneRepeatMode_All;
    return TuneRepeatMode_Off;
}

}  // namespace

// ---------------------------------------------------------------- Gatefold's extras (tune_gfbg.hpp)

bool gfTrackInfo(GfbgTrackInfo* out) { return open && R_SUCCEEDED(gfbgGetTrackInfo(&srv, out)); }
bool gfConfig(GfbgConfig* out) { return open && R_SUCCEEDED(gfbgGetConfig(&srv, out)); }
void gfSetConfig(const GfbgConfig& c) { if (open) gfbgSetConfig(&srv, &c); }
bool gfGameVolume(GfbgGameVolume* out) { return open && R_SUCCEEDED(gfbgGetGameVolume(&srv, out)); }
bool gfState(GfbgState* out) { if (!open) return false; flush(); return R_SUCCEEDED(gfbgGetState(&srv, out)); }
void gfSetStopAfter(bool on) { if (open) gfbgSetStopAfter(&srv, on); }
void gfSetSleep(float seconds) { if (open) gfbgSetSleep(&srv, seconds); }
void gfFlush() { flush(); }
bool gfEq(GfbgEq* out) { return open && R_SUCCEEDED(gfbgGetEq(&srv, out)); }
void gfSetEq(const GfbgEq& e) { if (open) gfbgSetEq(&srv, &e); }

std::vector<std::string> gfPlaylistNames() {
    std::vector<std::string> out;
    FILE* f = fopen(GFBG_PLAYLISTS, "r");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != '#') continue;   // a track: only the names are wanted here
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > 1) out.emplace_back(line + 1, n - 1);
    }
    fclose(f);
    return out;
}

std::string gfPlaylistName(u64 hash) {
    if (!hash) return {};
    for (const auto& n : gfPlaylistNames())
        if (gfbgHash(n.data(), n.size()) == hash) return n;
    return {};
}

bool gfLoadThumb(const char* trackPath, std::vector<u8>& rgb) {
    // map.tsv changes while Gatefold works through a queue, so it is read again each time: a few
    // KB, once per track change.
    const std::string key = mounted(trackPath);
    char want[20];
    snprintf(want, sizeof want, "%016llx", (unsigned long long)gfbgHash(key.data(), key.size()));
    FILE* f = fopen(GFBG_THUMBS "/map.tsv", "r");
    if (!f) return false;
    char line[128];
    std::string file;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, 16) && line[16] == '\t') {
            file = line + 17;
            while (!file.empty() && (file.back() == '\n' || file.back() == '\r')) file.pop_back();
            break;
        }
    fclose(f);
    if (file.empty()) return false;
    f = fopen((std::string(GFBG_THUMBS "/") + file).c_str(), "rb");
    if (!f) return false;
    rgb.resize(size_t(GFBG_ART) * GFBG_ART * 3);
    const bool ok = fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- tune.h

Result tuneInitialize() {
    if (open) return 0;
    const Result rc = gfbgOpen(&srv, GfbgClient_Overlay);
    open = R_SUCCEEDED(rc);
    q = Queue{};
    // "Not running" reads as sys-tune's NotFound, so main.cpp starts the sysmodule.
    if (R_VALUE(rc) == MAKERESULT(Module_Libnx, LibnxError_NotFound)) return KERNELRESULT(NotFound);
    return rc;
}

void tuneExit() {
    flush();
    if (open) serviceClose(&srv);
    open = false;
}

Result tuneGetStatus(bool* status) {
    flush();
    *status = state().status == GfbgStatus_Playing;
    return 0;
}

Result tunePlay() { startFresh(-1); flush(); return gfbgTransport(&srv, GfbgOp_Resume); }
Result tunePause() { flush(); return gfbgTransport(&srv, GfbgOp_Pause); }
Result tuneNext() { startFresh(-1); flush(); return gfbgTransport(&srv, GfbgOp_Next); }
Result tunePrev() { startFresh(-1); flush(); return gfbgTransport(&srv, GfbgOp_Prev); }

Result tuneGetVolume(float* out) { *out = state().volume; return 0; }
Result tuneSetVolume(float volume) { return gfbgSetVolume(&srv, volume); }

Result tuneGetTitleVolume(float* out) {
    GfbgGameVolume g{};
    gfGameVolume(&g);
    *out = g.programId ? g.volume : 1.f;
    return 0;
}
Result tuneSetTitleVolume(float volume) { return gfbgSetGameVolume(&srv, volume); }

Result tuneGetDefaultTitleVolume(float* out) {
    GfbgConfig c{};
    *out = gfConfig(&c) ? c.gameVolume : 1.f;
    return 0;
}
Result tuneSetDefaultTitleVolume(float volume) {
    GfbgConfig c{};
    if (!gfConfig(&c)) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    c.gameVolume = volume;
    gfSetConfig(c);
    return 0;
}

Result tuneGetRepeatMode(TuneRepeatMode* out) { *out = repeatOf(state().mode); return 0; }

Result tuneSetRepeatMode(TuneRepeatMode r) {
    flush();
    const bool shuffle = shuffleOn(state().mode);
    // Shuffle already repeats the list; repeating one track has to leave shuffle.
    const u32 mode = r == TuneRepeatMode_One ? GfbgMode_RepeatOne : shuffle ? GfbgMode_Shuffle : r == TuneRepeatMode_All ? GfbgMode_RepeatAll : GfbgMode_Default;
    return gfbgSetMode(&srv, mode);
}

Result tuneGetShuffleMode(TuneShuffleMode* out) {
    *out = shuffleOn(state().mode) ? TuneShuffleMode_On : TuneShuffleMode_Off;
    return 0;
}

Result tuneSetShuffleMode(TuneShuffleMode s) {
    flush();
    const u32 cur = state().mode;
    if ((s == TuneShuffleMode_On) == shuffleOn(cur)) return 0;
    return gfbgSetMode(&srv, s == TuneShuffleMode_On ? GfbgMode_Shuffle : GfbgMode_RepeatAll);
}

Result tuneGetPlaylistSize(u32* count) {
    ensureQueue();
    *count = u32(q.order.size());
    return 0;
}

Result tuneGetPlaylistItem(u32 index, char* out_path, size_t len) {
    ensureQueue();
    if (index >= q.order.size()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    snprintf(out_path, len, "%s", bare(q.paths[size_t(q.order[index])]));
    return 0;
}

Result tuneGetCurrentQueueItem(char* out_path, size_t len, TuneCurrentStats* out) {
    flush();
    const GfbgState s = state();
    if (s.pos < 0 || s.queueCount == 0) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    ensureQueue();
    if (s.pos >= s32(q.order.size())) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    snprintf(out_path, len, "%s", bare(q.paths[size_t(q.order[size_t(s.pos)])]));
    out->sample_rate = 1000;
    out->current_frame = u32(std::max(0.0, s.position) * 1000);
    out->total_frames = std::max<u32>(1, u32(s.duration * 1000));   // a stopped queue has no duration yet
    return 0;
}

Result tuneClearQueue() {
    ensureQueue();
    q.paths.clear();
    q.order.clear();
    q.pos = -1;
    q.name.clear();
    q.dirty = true;
    q.fresh = true;
    q.sent = false;
    return 0;
}

Result tuneMoveQueueItem(u32 src, u32 dst) {
    ensureQueue();
    if (src >= q.order.size() || dst >= q.order.size()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const s32 cur = q.pos >= 0 && q.pos < s32(q.order.size()) ? q.order[size_t(q.pos)] : -1;
    const s32 v = q.order[src];
    q.order.erase(q.order.begin() + src);
    q.order.insert(q.order.begin() + dst, v);
    for (size_t i = 0; i < q.order.size(); i++) if (q.order[i] == cur) q.pos = s32(i);
    q.dirty = true;
    return 0;
}

Result tuneSelect(u32 index) {
    // A fresh queue: picked before it was ever sent (Browse plays a folder: clear, add each file, then the
    // file's place in the folder), the index is a track as added, and it goes first. Picked from the queue
    // as shown, it is the place shown.
    if (q.fresh && index < q.order.size()) {
        if (q.sent) startFresh(-1);
        else {
            const s32 picked = s32(index);
            startFresh(picked);
            index = u32(std::find(q.order.begin(), q.order.end(), picked) - q.order.begin());
        }
    }
    flush();
    const GfbgState s = state();
    if (index >= s.queueCount) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return gfbgPlayAt(&srv, s32(index), s.status != GfbgStatus_Playing, 0);
}

Result tuneSeek(u32 position) { flush(); return gfbgSeek(&srv, position / 1000.0); }

Result tuneEnqueue(const char* path, TuneEnqueueType type) {
    ensureQueue();
    const s32 idx = s32(q.paths.size());
    q.paths.push_back(mounted(path));
    // Front: right after the track playing now (Gatefold's "Play next"). With Shuffle on, a random place
    // (startFresh says why): anywhere while the queue is filled from empty, else among those still to come.
    if (type == TuneEnqueueType_Front && q.pos >= 0) q.order.insert(q.order.begin() + q.pos + 1, idx);
    else if (shuffleOn(state().mode) && (q.fresh || q.pos >= 0)) {
        const size_t lo = q.fresh ? 0 : std::min(q.order.size(), size_t(q.pos) + 1);
        q.order.insert(q.order.begin() + lo + size_t(rnd(q.order.size() - lo + 1)), idx);
    } else q.order.push_back(idx);
    if (q.pos < 0) q.pos = 0;
    q.dirty = true;
    return 0;
}

Result tuneRemove(u32 index) {
    ensureQueue();
    if (index >= q.order.size()) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    // The track at pos is the one playing: the player would carry on with it while pos moved on (Gatefold keeps it too)
    if (s32(index) == q.pos) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const s32 gone = q.order[index];
    q.order.erase(q.order.begin() + index);
    q.paths.erase(q.paths.begin() + gone);
    for (auto& o : q.order) if (o > gone) o--;
    if (s32(index) < q.pos) q.pos--;
    if (q.pos >= s32(q.order.size())) q.pos = s32(q.order.size()) - 1;
    q.dirty = true;
    return 0;
}

Result tuneQuit() { flush(); return gfbgTransport(&srv, GfbgOp_Stop); }

Result tuneGetApiVersion(u32* version) {
    *version = TUNE_API_VERSION;
    return 0;
}
