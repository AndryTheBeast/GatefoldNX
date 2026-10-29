/*
 * Gatefold NX · tools/preview/stretchtest.cpp (PC only, never in a Switch build)
 * Checks automix::Stretcher as Automix v3 uses it: the outgoing track (a click track at 112 BPM) is taken up in the middle
 * of playing, plays as it is for 5 s, then eases over 8 beats to the next track's tempo (120 BPM) and keeps it to its end.
 * Prints where the clicks land; checks that until the ramp the output is the input exactly, sample for sample (started
 * mid-track, the first grain must not dip), and that after it the clicks are 0.500 s apart, on a grid.
 *   clang++ -std=c++20 -O2 -Isource tools/preview/stretchtest.cpp source/automix.cpp source/mixan.cpp -o stretchtest.exe
 */
#include <cmath>
#include <cstdio>
#include <vector>

#include "automix.hpp"

int main() {
    constexpr int Rate = 48000;
    const double Pin = 60.0 / 112, Pout = 60.0 / 120;
    // the input: 60 s, a short click on every beat over quiet noise (so the correlation has something everywhere)
    const size_t n = size_t(60 * Rate);
    std::vector<int16_t> in(n * 2);
    unsigned seed = 1;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        float v = ((seed >> 16) % 2000 - 1000) / 1000.f * 0.02f;
        const double t = double(i) / Rate, ph = std::fmod(t, Pin);
        if (ph < 0.004) v += float(0.8 * std::sin(2 * 3.14159265 * 1000 * ph) * (1 - ph / 0.004));
        in[i * 2] = in[i * 2 + 1] = int16_t(v * 32767);
    }
    automix::Plan p;
    p.speed = Pin / Pout;   // > 1: the slower track sped up to the next one's tempo
    p.easeIn = 8 * Pout;
    const double delay = 5;
    automix::Stretcher st;
    st.init();
    const size_t startAt = size_t(10.3 * Rate);   // taken up in the middle of the track, off a beat
    size_t at = startAt;
    st.begin(p, Rate, delay);
    auto src = [&](int16_t* o, int k) {
        const int got = int(std::min<size_t>(size_t(k), n - at));
        for (int i = 0; i < got * 2; i++) o[i] = in[at * 2 + size_t(i)];
        at += size_t(got);
        return got;
    };
    std::vector<int16_t> out;
    std::vector<int16_t> buf(1000 * 2);
    long long consumed = 0, used;
    while (true) {
        const int got = st.read(src, buf.data(), 1000, used);
        consumed += used;
        out.insert(out.end(), buf.begin(), buf.begin() + got * 2);
        if (got < 1000) break;
    }
    // until the ramp: the input, exactly
    size_t same = 0;
    while (same < size_t(delay * Rate) && same * 2 + 1 < out.size() && out[same * 2] == in[(startAt + same) * 2] && out[same * 2 + 1] == in[(startAt + same) * 2 + 1]) same++;
    printf("as it is for its first %.3f s (the ramp from %.3f s)%s\n", double(same) / Rate, delay, same + 600 >= size_t(delay * Rate) ? "" : ": NOT IDENTICAL");
    // the clicks: where the output first goes over half of a click's peak, after a gap
    std::vector<double> clicks;
    for (size_t i = 1, last = 0; i < out.size() / 2; i++)
        if (std::abs(out[i * 2]) > 0.4 * 32767 && (clicks.empty() || i - last > size_t(0.2 * Rate))) {
            clicks.push_back(double(i) / Rate);
            last = i;
        }
    const double settled = delay + p.easeIn + 0.5;
    double worst = 0, drift = 0, first = -1;
    for (size_t k = 1; k < clicks.size(); k++) {
        const double gap = clicks[k] - clicks[k - 1], t = clicks[k];
        if (t > settled) {
            worst = std::max(worst, std::fabs(gap - std::max(1.0, std::round(gap / Pout)) * Pout));   // (a click missed: two beats)
            if (first < 0) first = t;
            drift = std::max(drift, std::fabs(t - first - std::round((t - first) / Pout) * Pout));
        }
        if (k < 3 || (t > delay - 0.6 && t < settled + 1) || k + 2 > clicks.size()) printf("  click %3zu at %8.4f s, gap %.4f\n", k, t, gap);
    }
    printf("speed %.4f reached over %.2f s; %zu clicks; after it: gaps within %.1f ms of %.4f, positions within %.1f ms of the grid\n", p.speed,
           p.easeIn, clicks.size(), worst * 1000, Pout, drift * 1000);
    printf("time: the track's own frames used %.4f s for %.4f s of output\n", double(consumed) / Rate, double(out.size() / 2) / Rate);
    return same + 600 >= size_t(delay * Rate) && worst < 0.012 && drift < 0.012 ? 0 : 1;
}
