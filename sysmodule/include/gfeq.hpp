/*
 * Gatefold NX · gfeq.hpp
 * The equalizer, after Poweramp's: shared by Gatefold's own player and the background player, so it
 * sounds the same with or without the sysmodule. Float, stereo, any rate (both use 48 kHz).
 *
 * Why a bass boost here does not muffle or crackle (Poweramp's reasons):
 *  - the bass is a low shelf at 100 Hz (its half-way point), under the 200–500 Hz "mud", and a 20 Hz high-pass keeps the
 *    boost off what no speaker plays;
 *  - headroom: the largest boost of the curve, less 3 dB the limiter may catch, is taken off before the filters;
 *    the preamp is a plain gain on top (louder is louder, the limiter catching the peaks), as Poweramp's is;
 *  - what still goes over is caught by a soft limiter, not clipped.
 * Every change glides (about 0.15 s for 12 dB), so moving a slider never zips.
 *
 * The volume comes after all of it, so it works exactly as with the EQ off. (Giving the headroom back through the
 * volume, as Poweramp's DVC does, left the volume flat above ~20 % with a strong curve: each step up was cancelled.)
 *
 * Use: set() from any thread is not safe; the players call it on their audio thread (or under the
 * lock that thread holds). Per block: the fades and levelling, process(), then the volume.
 */
#pragma once
#include <algorithm>
#include <cmath>

#include "gfeqcfg.h"

constexpr double kPi = 3.14159265358979323846;

class GfEq {
public:
    explicit GfEq(float rate = 48000) : rate_(rate) { reset(); }

    /** New settings: the filters glide to them. */
    void set(const GfbgEq& e) {
        GfbgEq c = e;
        gfEqClamp(&c);
        on_ = c.on != 0;
        for (int i = 0; i < GFEQ_BANDS; i++) target_[i] = on_ ? c.bands[i] : 0.f;
        target_[Bass] = on_ ? c.bass : 0.f;
        target_[Treble] = on_ ? c.treble : 0.f;
        target_[Pre] = on_ && c.advanced ? c.preamp : 0.f;
    }

    /** Silence the filters' memory (a seek, a new track after a stop). */
    void reset() {
        for (auto& f : f_) f.z[0][0] = f.z[0][1] = f.z[1][0] = f.z[1][1] = 0;
        hp_.z[0][0] = hp_.z[0][1] = hp_.z[1][0] = hp_.z[1][1] = 0;
        env_ = 0;
    }

    /** True while anything is to be done: on, or still gliding back to flat after being turned off. */
    bool active() const { return on_ || !flat_; }

    /** In place, interleaved stereo; the volume goes on after it. */
    void process(float* lr, int frames) {
        if (!active()) return;
        for (int at = 0; at < frames; at += Block) {
            glide();
            const int n = std::min(Block, frames - at);
            float* s = lr + at * 2;
            for (int i = 0; i < n * 2; i++) s[i] *= pre_;
            hp_.run(s, n);
            for (int k = 0; k < Filters; k++)
                if (std::fabs(cur_[k]) > 0.01f) f_[k].run(s, n);
            limit(s, n);
        }
    }

private:
    enum { Bass = GFEQ_BANDS, Treble, Filters, Pre = Filters, Values };
    static constexpr int Block = 32;
    static constexpr float Glide = 12.f / (0.15f * 48000 / Block);   // dB a block
    static constexpr float Allow = 3.f;   // dB of the curve's peak left to the limiter

    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};   // transposed direct form II, per channel
        void run(float* s, int n) {
            for (int i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) {
                    const float x = s[i * 2 + c];
                    const float y = b0 * x + z[c][0];
                    z[c][0] = b1 * x - a1 * y + z[c][1];
                    z[c][1] = b2 * x - a2 * y;
                    s[i * 2 + c] = y;
                }
        }
        /** |H| in dB at f. */
        float db(float f, float rate) const {
            const double w = 2 * kPi * f / rate, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
            const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
            const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
            return float(10 * std::log10((nr * nr + ni * ni) / std::max(1e-12, dr * dr + di * di)));
        }
    };

    /** RBJ's cookbook: k 0..9 a peak (Q 1.41), Bass a low shelf at 100 Hz, Treble a high shelf at 10 kHz. */
    void design(int k, float gain) {
        Biquad& b = f_[k];
        const bool low = k == Bass, high = k == Treble;
        const double f0 = low ? 100 : high ? 10000 : kGfEqFreq[k];
        const double A = std::pow(10.0, gain / 40), w = 2 * kPi * std::min(f0, rate_ * 0.45) / rate_;
        const double cw = std::cos(w), sw = std::sin(w);
        double b0, b1, b2, a0, a1, a2;
        if (low || high) {
            const double al = sw / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;   // shelf slope 1
            const double sg = low ? 1 : -1;
            b0 = A * ((A + 1) - sg * (A - 1) * cw + sa);
            b1 = sg * 2 * A * ((A - 1) - sg * (A + 1) * cw);
            b2 = A * ((A + 1) - sg * (A - 1) * cw - sa);
            a0 = (A + 1) + sg * (A - 1) * cw + sa;
            a1 = -sg * 2 * ((A - 1) + sg * (A + 1) * cw);
            a2 = (A + 1) + sg * (A - 1) * cw - sa;
        } else {
            const double al = sw / (2 * 1.41);
            b0 = 1 + al * A;
            b1 = -2 * cw;
            b2 = 1 - al * A;
            a0 = 1 + al / A;
            a1 = -2 * cw;
            a2 = 1 - al / A;
        }
        b.b0 = float(b0 / a0); b.b1 = float(b1 / a0); b.b2 = float(b2 / a0);
        b.a1 = float(a1 / a0); b.a2 = float(a2 / a0);
    }

    /** Moves the gains towards their targets and, when anything moved, redesigns and measures the curve. */
    void glide() {
        bool moved = !designed_;
        for (int k = 0; k < Values; k++) {
            const float d = target_[k] - cur_[k];
            if (d == 0) continue;
            cur_[k] = std::fabs(d) <= Glide ? target_[k] : cur_[k] + (d > 0 ? Glide : -Glide);
            moved = true;
        }
        if (!moved) return;
        if (!designed_) {
            // 20 Hz high-pass, Butterworth (Q 0.707)
            const double w = 2 * kPi * 20 / rate_, cw = std::cos(w), al = std::sin(w) / (2 * 0.7071);
            const double a0 = 1 + al;
            hp_.b0 = float((1 + cw) / 2 / a0); hp_.b1 = float(-(1 + cw) / a0); hp_.b2 = hp_.b0;
            hp_.a1 = float(-2 * cw / a0); hp_.a2 = float((1 - al) / a0);
            designed_ = true;
        }
        flat_ = true;
        for (int k = 0; k < Values; k++) flat_ = flat_ && cur_[k] == 0;
        for (int k = 0; k < Filters; k++) design(k, cur_[k]);
        // The curve's highest point, over 48 frequencies from 20 Hz to 20 kHz: that much, less Allow, is taken off first.
        float peak = 0;
        for (int i = 0; i < 48; i++) {
            const float f = 20.f * std::pow(1000.f, i / 47.f);
            float db = 0;   // the bands, bass and treble only: the preamp is not given room, it is a plain gain
            for (int k = 0; k < Filters; k++)
                if (std::fabs(cur_[k]) > 0.01f) db += f_[k].db(f, rate_);
            peak = std::max(peak, db);
        }
        // The preamp goes on top in full, as Poweramp's: raised, the music is that much louder and the limiter takes the peaks.
        room_ = std::pow(10.f, -std::max(0.f, peak - Allow) / 20.f);
        pre_ = std::pow(10.f, cur_[Pre] / 20.f) * room_;
    }

    /**
     * A soft limiter at −1 dBFS: the peak envelope rises at once and falls back over ~30 ms (150 ms held a raised preamp
     * down to nothing on loud masters; faster than this starts to distort the bass); above the
     * threshold the level is bent smoothly (tanh) towards full scale instead of being cut off.
     */
    void limit(float* s, int n) {
        constexpr float T = 0.89f;
        const float rel = std::exp(-1.f / (0.03f * rate_));
        for (int i = 0; i < n; i++) {
            const float e = std::max(std::fabs(s[i * 2]), std::fabs(s[i * 2 + 1]));
            env_ = e > env_ ? e : env_ * rel + e * (1 - rel);
            if (env_ <= T) continue;
            const float out = T + (1 - T) * std::tanh((env_ - T) / (1 - T));
            const float g = out / env_;
            s[i * 2] *= g;
            s[i * 2 + 1] *= g;
        }
    }

    float rate_;
    Biquad f_[Filters], hp_;
    float cur_[Values] = {}, target_[Values] = {};
    float pre_ = 1, room_ = 1, env_ = 0;   // pre_: what goes in before the filters (preamp × room_, the headroom taken off)
    bool on_ = false, flat_ = true, designed_ = false;
};
