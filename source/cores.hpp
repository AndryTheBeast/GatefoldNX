/*
 * Gatefold NX · cores.hpp
 * Where each thread runs, and how urgently. Threads made with std::thread start on the process's
 * default core at the lowest priority (0x3B), and Horizon does not share a core between threads of
 * the same priority: whichever runs keeps the core until it waits. With every worker on one core at
 * one priority, the track loader queued behind the tag reader and the overlay covers, and things
 * appeared one at a time. So each worker has a core and a priority of its own (lower is sooner;
 * the main thread is 0x2C on core 0, which an application may share with cores 1 and 2):
 *
 *   core 0: the overlay's covers (0x30): only the main thread's idle time, it waits for vsync most of a frame
 *   core 1: audio (0x2A), its read-ahead (0x2B: ffdecoder.hpp), the track loader (0x2C), file writing and sorting (0x2D), the waveform (0x2E)
 *   core 2: tags and cover thumbnails (0x2E), on their own: a whole-track waveform no longer holds them up;
 *           below them, measuring loudness (0x31, levels.hpp), and a second measurer on core 1 at 0x31: whole
 *           tracks one after another, only in time nothing else on those cores wants
 */
#pragma once
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace cores {
struct Place { int core, priority; };
constexpr Place Audio{ 1, 0x2A }, Feed{ 1, 0x2B }, Loader{ 1, 0x2C }, Files{ 1, 0x2D }, Wave{ 1, 0x2E }, Meta{ 2, 0x2E }, Thumbs{ 0, 0x30 }, Loud{ 2, 0x31 }, Loud2{ 1, 0x31 };
// Files' copying and deleting, and the updater's downloads: mostly waiting on the SD card or the network, below tags
constexpr Place FileOps{ 2, 0x30 }, Net{ 2, 0x30 };

/** Moves the calling thread there. Where that is not allowed it simply stays where it is. */
inline void moveHere(Place p) {
#ifdef __SWITCH__
    svcSetThreadPriority(CUR_THREAD_HANDLE, u32(p.priority));
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, p.core, 1u << p.core);
#else
    (void)p;
#endif
}
}  // namespace cores
