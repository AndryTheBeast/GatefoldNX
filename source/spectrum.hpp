/*
 * Gatefold NX · spectrum.hpp
 * The Now Playing spectrum: port of Gatefold's spectrum-worker.js (Hann window,
 * radix-2 FFT, log-spaced bands 30 Hz–16 kHz, +3 dB/octave tilt) and the
 * bar/peak smoothing from viz.js.
 *
 * On the console the mixer hands over audio in bursts of 2048 frames (~43 ms),
 * three display frames apart, where Gatefold gets a fresh block every frame.
 * So a new FFT only runs when a burst arrived, and the bars glide from the
 * previous result to the new one over the burst interval instead of jumping.
 */
#pragma once
#include <vector>

class Spectrum {
public:
    static constexpr int MaxBands = 128;
    static constexpr int N = 4096;

    /** Settings → Visualizer. The defaults reproduce Gatefold. */
    struct Params {
        int bands = 56;
        float sensitivity = 0.5f;   // 0..1, ±12 dB around Gatefold's (db + 66) / 60 floor
        float smoothing = 0.5f;     // 0 snappy … 0.5 Gatefold … 1 slow
    };

    Spectrum();
    /** pcm: N mono samples of a burst that just arrived. */
    void feed(const float* pcm, int sampleRate);
    /** Once per display frame. playing = false lets the bars fall. */
    void step(float dt, bool playing);

    Params params;
    int bands = 56;
    float bars[MaxBands] = {};
    float peaks[MaxBands] = {};
    float beat = 0;

private:
    std::vector<int> rev_;
    std::vector<float> cos_, sin_, hann_, re_, im_;
    float from_[MaxBands] = {}, to_[MaxBands] = {};
    float sinceFeed_ = 1, interval_ = 0.043f;
    float bassAvg_ = 0;
};
