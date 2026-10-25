/*
 * Gatefold NX background player · resampler.hpp
 * audout plays 48 kHz only, and most music is 44.1 kHz: a polyphase windowed-sinc resampler
 * (32 taps, Kaiser β 8, 256 phases interpolated) keeps that change inaudible. 48 kHz files
 * pass straight through.
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class Resampler {
public:
    static constexpr int Half = 16, Taps = Half * 2, Phases = 256;

    void reset(int inRate, int outRate) {
        in_ = inRate;
        out_ = outRate;
        step_ = double(inRate) / outRate;
        buf_.assign(size_t(Half - 1) * 2, 0.f);   // history before the first sample
        pos_ = Half - 1;
        if (in_ == out_) { buf_.clear(); pos_ = 0; return; }
        if (tableFor_ == inRate) return;
        tableFor_ = inRate;
        // Cut off a little under the lower Nyquist, so 44.1 → 48 keeps everything up to ~20 kHz.
        const double fc = std::min(1.0, double(outRate) / inRate) * 0.91;
        table_.resize(size_t(Phases + 1) * Taps);
        for (int p = 0; p <= Phases; p++) {
            const double frac = double(p) / Phases;
            double sum = 0;
            for (int k = 0; k < Taps; k++) {
                const double x = (k - (Half - 1)) - frac;   // distance from the output point, in input samples
                const double s = x == 0 ? fc : std::sin(M_PI * fc * x) / (M_PI * x);
                const double w = kaiser(x / Half);
                table_[size_t(p) * Taps + k] = float(s * w);
                sum += s * w;
            }
            for (int k = 0; k < Taps; k++) table_[size_t(p) * Taps + k] = float(table_[size_t(p) * Taps + k] / sum);   // unity gain at DC
        }
    }

    bool passthrough() const { return in_ == out_; }

    /** Adds input frames (stereo s16). */
    void push(const int16_t* in, int frames) {
        const size_t at = buf_.size();
        buf_.resize(at + size_t(frames) * 2);
        for (int i = 0; i < frames * 2; i++) buf_[at + i] = in[i] * (1.f / 32768);
    }

    /** Input frames still needed before pull() can give anything. */
    bool hungry() const { return frames() < size_t(pos_) + Half + 1; }

    /** Up to maxOut stereo float frames; fewer when the input runs out. */
    int pull(float* out, int maxOut) {
        if (passthrough()) {
            const int n = std::min<int>(maxOut, int(frames()));
            memcpy(out, buf_.data(), size_t(n) * 8);
            buf_.erase(buf_.begin(), buf_.begin() + n * 2);
            return n;
        }
        int n = 0;
        while (n < maxOut) {
            const size_t base = size_t(pos_);
            if (base + Half + 1 > frames()) break;
            const double fr = (pos_ - double(base)) * Phases;
            const int ph = int(fr);
            const float t = float(fr - ph);
            const float* h0 = &table_[size_t(ph) * Taps];
            const float* h1 = h0 + Taps;
            const float* x = &buf_[(base - (Half - 1)) * 2];
            float l = 0, r = 0;
            for (int k = 0; k < Taps; k++) {
                const float h = h0[k] + (h1[k] - h0[k]) * t;
                l += x[k * 2] * h;
                r += x[k * 2 + 1] * h;
            }
            out[n * 2] = l;
            out[n * 2 + 1] = r;
            n++;
            pos_ += step_;
        }
        // Drop what no later output point can reach.
        const size_t keepFrom = size_t(pos_) - (Half - 1);
        if (keepFrom > 4096) {
            buf_.erase(buf_.begin(), buf_.begin() + keepFrom * 2);
            pos_ -= double(keepFrom);
        }
        return n;
    }

    /** Input frames held but not played yet: for the position the listener hears. */
    double latencyFrames() const { return passthrough() ? double(frames()) : double(frames()) - pos_; }

private:
    int in_ = 48000, out_ = 48000, tableFor_ = 0;
    double step_ = 1, pos_ = 0;
    std::vector<float> buf_;
    std::vector<float> table_;

    size_t frames() const { return buf_.size() / 2; }

    static double bessel0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 25; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
        return sum;
    }
    static double kaiser(double x) {
        if (x <= -1 || x >= 1) return 0;
        constexpr double beta = 8;
        return bessel0(beta * std::sqrt(1 - x * x)) / bessel0(beta);
    }
};
