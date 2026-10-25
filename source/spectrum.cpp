/*
 * Gatefold NX · spectrum.cpp
 */
#include "spectrum.hpp"

#include <algorithm>
#include <cmath>

Spectrum::Spectrum() : rev_(N), cos_(N / 2), sin_(N / 2), hann_(N), re_(N), im_(N) {
    const int bits = int(std::log2(N));
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev_[i] = r;
    }
    for (int i = 0; i < N / 2; i++) { cos_[i] = std::cos(2 * M_PI * i / N); sin_[i] = -std::sin(2 * M_PI * i / N); }
    for (int i = 0; i < N; i++) hann_[i] = 0.5f - 0.5f * std::cos(2 * M_PI * i / (N - 1));
}

void Spectrum::feed(const float* pcm, int sr) {
    const int nb = std::clamp(params.bands, 8, MaxBands);
    if (nb != bands) {
        bands = nb;
        std::fill(std::begin(bars), std::end(bars), 0.f);
        std::fill(std::begin(peaks), std::end(peaks), 0.f);
        std::fill(std::begin(to_), std::end(to_), 0.f);
        std::fill(std::begin(from_), std::end(from_), 0.f);
    }
    for (int i = 0; i < N; i++) { re_[rev_[i]] = pcm[i] * hann_[i]; im_[rev_[i]] = 0; }
    for (int size = 2; size <= N; size <<= 1) {
        const int half = size >> 1, step = N / size;
        for (int i = 0; i < N; i += size)
            for (int j = 0, k = 0; j < half; j++, k += step) {
                const int a = i + j, b = a + half;
                const float tr = re_[b] * cos_[k] - im_[b] * sin_[k];
                const float ti = re_[b] * sin_[k] + im_[b] * cos_[k];
                re_[b] = re_[a] - tr; im_[b] = im_[a] - ti;
                re_[a] += tr; im_[a] += ti;
            }
    }
    const float lo = 30, hi = std::min(16000.f, sr / 2.f - 1);
    const float norm = 4.f / (float(N) * N);   // Hann coherent gain 0.5, one-sided spectrum
    const float floorDb = 66 + (params.sensitivity - 0.5f) * 24;
    auto pw = [&](int k) { k = std::clamp(k, 1, N / 2 - 1); return (re_[k] * re_[k] + im_[k] * im_[k]) * norm; };
    for (int b = 0; b < bands; b++) {
        // The glide starts from where the bars are heading now, so a burst that comes early does not jump back.
        const float t = std::min(1.f, sinceFeed_ / interval_);
        from_[b] = from_[b] + (to_[b] - from_[b]) * t;
        const float a = lo * std::pow(hi / lo, float(b) / bands) * N / sr;
        const float z = lo * std::pow(hi / lo, float(b + 1) / bands) * N / sr;
        float p = 0;
        if (z - a < 1) {   // narrower than a bin: interpolate
            const float c = (a + z) / 2, f = c - std::floor(c);
            const int k = int(std::floor(c));
            p = pw(k) * (1 - f) + pw(k + 1) * f;
        } else {
            for (int k = int(std::floor(a)); k < int(std::ceil(z)); k++) p = std::max(p, pw(k));
        }
        const float hz = ((a + z) / 2) * sr / N;
        const float db = 10 * std::log10(p + 1e-12f) + 3 * std::log2(std::max(hz, 30.f) / 1000);
        to_[b] = std::clamp((db + floorDb) / 60, 0.f, 1.f);
    }
    // The bursts' spacing, so the glide lasts as long as the wait for the next one.
    if (sinceFeed_ < 0.25f) interval_ = std::clamp(interval_ * 0.8f + sinceFeed_ * 0.2f, 0.012f, 0.1f);
    sinceFeed_ = 0;
}

void Spectrum::step(float dt, bool playing) {
    sinceFeed_ += dt;
    // viz.js: no new audio for 250 ms (paused, a gap) and the bars fall
    const bool live = playing && sinceFeed_ < 0.25f;
    const float t = std::min(1.f, sinceFeed_ / interval_);

    // viz.js steps once per 60 Hz frame: attack 0.55, release 0.86 + 0.14·v. Smoothing bends both; dt keeps it frame-rate free.
    const float s = params.smoothing;
    const float attack = s <= 0.5f ? 0.85f + (0.55f - 0.85f) * (s / 0.5f) : 0.55f + (0.22f - 0.55f) * ((s - 0.5f) / 0.5f);
    const float release = s <= 0.5f ? 0.72f + (0.86f - 0.72f) * (s / 0.5f) : 0.86f + (0.95f - 0.86f) * ((s - 0.5f) / 0.5f);
    const float f = dt * 60;
    const float ka = 1 - std::pow(1 - attack, f), kr = std::pow(release, f);
    const float peakFall = 0.009f * f;
    for (int i = 0; i < bands; i++) {
        const float v = live ? from_[i] + (to_[i] - from_[i]) * t : 0;
        const float b = bars[i];
        bars[i] = v > b ? b + (v - b) * ka : b * kr + v * (1 - kr);   // fast attack, slow release
        peaks[i] = std::max(peaks[i] - peakFall, bars[i]);
    }
    if (!live) {
        std::fill(std::begin(to_), std::end(to_), 0.f);
        std::fill(std::begin(from_), std::end(from_), 0.f);
    }

    // bass onset → beat envelope (cover breathing)
    const int k = std::max(3, int(std::lround(bands * 0.07)));
    float bass = 0;
    for (int i = 0; i < k; i++) bass += bars[i];
    bass /= k;
    const float ka2 = 1 - std::pow(0.94f, f);
    bassAvg_ += (bass - bassAvg_) * ka2;
    beat = std::max(beat * std::pow(0.86f, f), std::max(0.f, bass - bassAvg_ * 1.08f) * 2.4f);
}
