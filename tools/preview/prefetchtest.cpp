/*
 * Gatefold NX · tools/preview/prefetchtest.cpp (PC only, never in a Switch build)
 * The decoder's read-ahead (ffdecoder.hpp: prefetch and the feeder) against decoding in place: the same file read both
 * ways in uneven pieces, with pauses that let the feeder run and pieces taken faster than it fills, a seek midway and
 * a skip at the start; every sample must match, and both must end together.
 * Build: clang++ -std=c++20 -O2 -DGF_PREVIEW -DGF_FFMPEG -Isource -I<ffmpeg>/include tools/preview/prefetchtest.cpp source/ffdecoder.cpp
 *        -L<ffmpeg>/lib -lavformat -lavcodec -lswresample -lavutil -o prefetchtest.exe
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ffdecoder.hpp"

std::string cleanGenre(const std::string& s) { return s; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    ffFeederStart();
    int bad = 0;
    for (int run = 0; run < 3; run++) {
        FfDecoder a, b;
        if (!a.open(argv[1], 48000, 2) || !b.open(argv[1], 48000, 2)) return 1;
        const double skip = run == 1 ? 7.3 : run == 2 ? 41.0 : 0;   // in place, and past 30 s (a seek)
        a.skip(skip);
        b.skip(skip);
        b.prefetch(48000 * 3, 24000);
        std::vector<int16_t> x(8192 * 2), y(8192 * 2);
        long long frames = 0;
        unsigned seed = 12345 + run;
        bool sought = false;
        for (;;) {
            seed = seed * 1103515245 + 12345;
            const int n = 1 + int((seed >> 8) % 8192);
            if (!sought && frames > 48000 * 20) {   // a seek midway, as the player's (under its lock: no read meanwhile)
                a.seek(90);
                b.seek(90);
                sought = true;
            }
            const int ga = a.read(x.data(), n), gb = b.read(y.data(), n);
            if (ga != gb) { printf("run %d: %d vs %d frames at %lld\n", run, ga, gb, frames); bad++; break; }
            for (int i = 0; i < ga * 2; i++)
                if (x[size_t(i)] != y[size_t(i)]) { printf("run %d: differs at frame %lld\n", run, frames + i / 2); bad++; break; }
            frames += ga;
            if (ga < n) break;
            if ((seed >> 20) % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        printf("run %d (skip %.1f s): %lld frames, ends %d/%d\n", run, skip, frames, a.atEnd() ? 1 : 0, b.atEnd() ? 1 : 0);
    }
    printf("reads that found the ring dry: %u\n", ffStarved());
    ffFeederStop();
    printf(bad ? "FAIL\n" : "ALL MATCH\n");
    return bad ? 1 : 0;
}
