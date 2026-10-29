/*
 * Gatefold NX · tools/preview/fadertest.cpp (PC only, never in a Switch build)
 * automix::Fader on two independent noises of equal loudness (pink-ish: white through a gentle low-pass): the level of
 * the mix through the fade, every 5 %, for each volume curve and EQ. A good join stays near 0 dB (the level of either alone).
 *   clang++ -std=c++20 -O2 -Isource tools/preview/fadertest.cpp source/automix.cpp source/mixan.cpp -o fadertest.exe
 */
#include <cmath>
#include <random>
#include <cstdio>
#include <vector>

#include "automix.hpp"

int main() {
    constexpr int Rate = 48000, Len = Rate * 10;
    std::mt19937 s1(1), s2(424242);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    auto noise = [&](std::mt19937& s, float& z) {
        z = 0.9f * z + 0.1f * uni(s);   // more bass than treble, as music
        return z * 1.5f;
    };
    struct Case { const char* name; automix::Volume v; automix::Eq e; double cross; automix::Filter f = automix::Filter::None; double cut = 0; };
    const Case cases[] = { { "Smooth, no EQ", automix::Volume::Smooth, automix::Eq::None, 0.5 },
                           { "Smooth, bass swap", automix::Volume::Smooth, automix::Eq::BassCentre, 0.5 },
                           { "Both up, full swap", automix::Volume::BothUp, automix::Eq::FullSwap, 0.5 },
                           { "Both up, full swap, cross 0.8", automix::Volume::BothUp, automix::Eq::FullSwap, 0.8 },
                           { "Linear, no EQ", automix::Volume::Linear, automix::Eq::None, 0.5 },
                           // v3: the drop join, 16 bars in and 2 out (cross 0.89), the bass cut 4 bars before; and 8 + 2
                           { "Drop, 16 + 2 bars", automix::Volume::Drop, automix::Eq::BassBridge, 16.0 / 18, automix::Filter::LowPassIn, 12.0 / 18 },
                           { "Drop, 8 + 2 bars", automix::Volume::Drop, automix::Eq::BassBridge, 8.0 / 10, automix::Filter::LowPassIn, 6.0 / 10 } };
    for (const Case& c : cases) {
        automix::Plan p;
        p.join.volume = c.v;
        p.join.eq = c.e;
        p.join.filter = c.f;
        p.cross = c.cross;
        p.bassCut = c.cut;
        p.drop = c.v == automix::Volume::Drop;
        p.length = 10;
        automix::Fader f;
        f.init(Rate);
        f.begin(p);
        std::vector<float> in(4096 * 2), out(4096 * 2);
        float z1 = 0, z2 = 0, z1r = 0, z2r = 0;
        double ref = 0;
        std::vector<double> e(21, 0), cnt(21, 0);
        for (int at = 0; at < Len; at += 4096) {
            const int n = std::min(4096, Len - at);
            for (int i = 0; i < n; i++) {
                in[i * 2] = noise(s1, z1); in[i * 2 + 1] = noise(s1, z1r);
                out[i * 2] = noise(s2, z2); out[i * 2 + 1] = noise(s2, z2r);
                ref += double(out[i * 2]) * out[i * 2];
            }
            f.mix(in.data(), out.data(), n, double(at) / Len, 1.0 / Len);
            for (int i = 0; i < n; i++) {
                const int k = std::min(20, int(double(at + i) / Len * 20 + 0.5));
                e[size_t(k)] += double(in[i * 2]) * in[i * 2];
                cnt[size_t(k)]++;
            }
        }
        ref /= Len;
        printf("%-30s", c.name);
        double lo = 99, hi = -99;
        for (int k = 0; k <= 20; k++) {
            const double db = 10 * std::log10(e[size_t(k)] / cnt[size_t(k)] / ref);
            lo = std::min(lo, db);
            hi = std::max(hi, db);
            printf(" %+.1f", db);
        }
        printf("   (range %+.1f to %+.1f dB)\n", lo, hi);
    }
}
