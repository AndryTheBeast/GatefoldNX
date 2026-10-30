/* Gatefold NX · tools/bgtest/join/switch.h
 * The slice of libnx the background player's engine uses, for running engine.cpp on the PC (bgjoin.cpp): no threads are
 * started (the test drives the audio and watch work itself), the clock is the test's, and nothing reaches a console. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Result;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
#define MAKERESULT(m, d) (((m) & 0x1FF) | ((d) << 9))
enum { Module_Libnx = 345 };
enum { LibnxError_OutOfMemory = 2, LibnxError_NotFound = 41, LibnxError_IncompatSysVer = 33 };

struct Mutex { std::recursive_mutex m; };
inline void mutexInit(Mutex*) {}
inline void mutexLock(Mutex* m) { m->m.lock(); }
inline void mutexUnlock(Mutex* m) { m->m.unlock(); }

struct Thread { int unused; };
typedef void (*ThreadFunc)(void*);
inline Result threadCreate(Thread*, ThreadFunc, void*, void*, size_t, int, int) { return 0; }
inline Result threadStart(Thread*) { return 0; }
inline void svcSleepThread(s64) {}

extern u64 g_ticks;   // bgjoin.cpp: the test's clock, 19.2 MHz as the console's
inline u64 armGetSystemTick() { return g_ticks; }
inline u64 armTicksToNs(u64 t) { return t * 625 / 12; }
inline void armDCacheFlush(void*, size_t) {}

enum TimeType { TimeType_UserSystemClock = 0 };
inline Result timeGetCurrentTime(TimeType, u64* t) { *t = 0; return 1; }
inline Result pmdmntGetApplicationProcessId(u64*) { return 1; }
inline Result pmdmntGetProgramId(u64*, u64) { return 1; }
inline bool hosversionAtLeast(int, int, int) { return true; }
inline Result audaSetAudioOutputProcessMasterVolume(u64, u64, float) { return 0; }
inline Result audoutaSetProcessMasterVolume(u64, u64, float) { return 0; }

enum { PdmPlayEventType_Applet = 0 };
enum { AppletId_application = 1 };
enum { PdmAppletEventType_Launch = 0, PdmAppletEventType_InFocus = 4, PdmAppletEventType_OutOfFocus = 5, PdmAppletEventType_OutOfFocus4 = 6 };
struct PdmPlayEvent {
    u8 play_event_type;
    struct { struct { u8 applet_id; u8 event_type; } applet; } event_data;
};
inline Result pdmqryGetAvailablePlayEventRange(s32*, s32*, s32*) { return 1; }
inline Result pdmqryQueryPlayEvent(s32, PdmPlayEvent*, s32, s32*) { return 1; }

struct AudioOutBuffer { AudioOutBuffer* next; void* buffer; u64 buffer_size; u64 data_size; u64 data_offset; };
inline Result audoutInitialize() { return 0; }
inline void audoutExit() {}
inline Result audoutStartAudioOut() { return 0; }
inline Result audoutStopAudioOut() { return 0; }
inline Result audoutAppendAudioOutBuffer(AudioOutBuffer*) { return 0; }
inline Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer**, u32* n) { *n = 0; return 0; }
inline Result audoutWaitPlayFinish(AudioOutBuffer**, u32* n, u64) { *n = 0; return 0; }
inline Result audoutFlushAudioOutBuffers(bool* b) { if (b) *b = false; return 0; }
#define aligned_alloc(a, s) _aligned_malloc((s), (a))
