/*
 * Gatefold NX · waveform.hpp
 * Decodes a whole track on a worker thread into bar heights for the seek bar
 * (what foo_ui_webview2's audio.generateFullWaveform gives Gatefold on the PC).
 *
 * Heights follow loudness (RMS) in dB against the track's own range, not the
 * sample peak: a modern master touches full scale in nearly every slice, so
 * peaks drew a flat wall while the music clearly rises and falls.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/** Mean squares of short blocks → `resolution` heights in 0..1 (shared by the console and preview readers). */
inline std::vector<float> shapeWaveform(const std::vector<float>& ms, int resolution) {
    std::vector<float> db(resolution, -120.f);
    const size_t n = ms.size();
    if (!n) return {};
    for (int i = 0; i < resolution; i++) {
        const size_t a = size_t(i) * n / resolution, b = std::max(a + 1, size_t(i + 1) * n / resolution);
        double sum = 0;
        size_t k = 0;
        for (size_t j = a; j < b && j < n; j++, k++) sum += ms[j];
        db[i] = k ? 10.f * std::log10(float(sum / k) + 1e-12f) : -120.f;
    }
    // The loud end is the track's loudest slice; the quiet end its 8th percentile, ignoring silence. The window is
    // at least 10 dB (a flat master still shows its sections) and at most 36 dB (fades do not squash the rest).
    std::vector<float> sorted;
    for (float d : db) if (d > -70) sorted.push_back(d);
    if (sorted.empty()) return std::vector<float>(resolution, 0.f);
    std::sort(sorted.begin(), sorted.end());
    const float hi = sorted.back(), lo = sorted[sorted.size() * 8 / 100];
    const float window = std::clamp(hi - lo + 3, 10.f, 36.f);
    std::vector<float> out(resolution);
    for (int i = 0; i < resolution; i++) {
        const float v = std::clamp((db[i] - (hi - window)) / window, 0.f, 1.f);
        out[i] = v * v * (1.5f - 0.5f * v) * 0.92f + (v > 0 ? 0.08f : 0);   // eased so the top keeps detail; anything audible shows
    }
    return out;
}

class Waveform {
public:
    static constexpr int Resolution = 1600;

    ~Waveform() { cancel(); }
    void request(const std::string& path);
    void cancel();

    /** Bar heights 0..1 (shapeWaveform); empty until ready. */
    bool ready() const { return ready_; }
    const std::vector<float>& peaks() const { return peaks_; }
    const std::string& path() const { return path_; }

private:
    void run(std::string path, unsigned gen);

    std::thread thread_;
    std::atomic<unsigned> gen_{ 0 };
    std::atomic<bool> ready_{ false };
    std::vector<float> peaks_;
    std::string path_;
};
