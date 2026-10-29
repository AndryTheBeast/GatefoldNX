/*
 * Gatefold NX · automix.hpp
 * Automix: how one track joins the next, from both tracks' analyses (mixan.hpp). Gatefold's own player
 * only for now; the background player keeps its plain crossfade. Written to move there later: plain C++,
 * nothing allocated while playing.
 *
 * A join (Join) is a preset or a set of choices, as in Spotify's transition editor: the volume curve, an EQ
 * swap (bass, or bass, mids and treble each at its own moment), a filter sweep, an echo out, a loop of the
 * outgoing track's last bars, beatmatching, and a length. plan() turns it and the two analyses into times;
 * Auto lets plan() choose:
 * - both beat grids sure and the tempos within reach (the same, or one twice the other, drifting under 20 ms
 *   over the whole mix): the next track starts on its first beat, on a beat of this one, over 32, 16 or 8
 *   beats; both up with a full EQ swap for 16 and more, else a smooth crossfade with a bass swap. The EQ works
 *   under the volume curve, as a DJ mixer's EQ under its faders;
 * - otherwise timed to the music: through a fade-out (a filter rise when the next one opens quietly), or
 *   after an abrupt end an echo of its last beat (a short fade when it has no tempo); its silence skipped.
 * - not analysed yet: a plain fade of the crossfade setting's length (at least 4 s).
 * Joins edited in a playlist are kept by the app (mixes.tsv) and passed in as `want`.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mixan.hpp"

namespace automix {

enum class Preset : uint8_t { Auto, Fade, Rise, Blend, Echo, Cut, Custom, Off };   // Off: no Automix for this join (the user, 2026-10-28)
enum class Volume : uint8_t { Smooth, Linear, BothUp, Cut, Drop };   // Drop (v3): the outgoing full until the crossover, then out
enum class Eq : uint8_t { None, BassCentre, BassEarly, BassLate, FullSwap, BassBridge };   // BassBridge (v3): no bass between the cut and the drop
enum class Filter : uint8_t { None, HighPassOut, LowPassIn, Both };
enum class Effect : uint8_t { None, Echo };

const char* name(Preset p);
const char* name(Volume v);
const char* name(Eq e);
const char* name(Filter f);
const char* name(Effect e);

/**
 * Automix's models (the user, 2026-10-29): each a frozen way of planning the joins and ordering an Automix playlist, so
 * models can be compared on the console (Settings › Debug, dev builds; a release plays Latest). V1.0.0 is Automix as it
 * stood on 2026-10-29. A model is never changed once it exists: a change copies the newest into a new one (automix.cpp).
 * The analysis (mixan, mix.tsv) is shared: a model that needs a new analysis raises mixan's Version, and the older
 * models then run on it too. A Plan carries the model that made it, for the mixing (Fader) to follow it later.
 */
enum class Model : uint8_t { V1_0_0, Count };
constexpr Model Latest = Model::V1_0_0;
const char* name(Model m);   // "Automix V1.0.0"
bool modelFromName(const std::string& s, Model& out);

struct Join {
    Preset preset = Preset::Auto;   // Auto: the rows below are plan()'s to choose
    Volume volume = Volume::Smooth;
    Eq eq = Eq::BassCentre;
    Filter filter = Filter::None;
    Effect effect = Effect::None;
    int loopBars = 0;               // 0, 1 or 2: the outgoing track's last bars repeat under the next one
    bool beatmatch = true;          // when both grids allow it
    int beats = 0;                  // the length on the beat: 4, 8, 16 or 32; 0: chosen
    float seconds = 0;              // the length off the beat; 0: chosen
    int shiftBeats = 0;             // the editor's drag: the next track comes in this many beats later (on the beat)
    float shiftSec = 0;             // … or seconds later (off the beat)

    std::string pack() const;       // "preset,volume,eq,filter,effect,loop,beatmatch,beats,seconds,shiftBeats,shiftSec"
    static bool unpack(const std::string& s, Join& out);
    bool operator==(const Join& o) const { return pack() == o.pack(); }
};

/** The rows a preset sets (Auto and Custom: as given). */
Join fromPreset(Preset p);

struct Plan {
    double startOut = 0;   // where in this track the next one starts (s)
    double offset = 0;     // where the next one starts (s): its first beat, or where its music begins
    double length = 0;     // the fade (s)
    bool beatmatched = false, beatmatchable = false;   // done on the beat; could be (the editor's switch)
    float bpm = 0;
    int beats = 0;
    Join join;             // what it does, Auto resolved
    double loopFrom = -1, loopTo = -1;   // this track's bars that repeat from loopTo on (s); -1: none
    double echoAt = 0.3, echoDelay = 0.375;   // the echo: where the dry track stops (0..1 of the fade), its delay (s)
    // Tempo matching (v3): this track's speed (its pitch kept: Stretcher), so its beats fall on the next one's: reached over
    // `easeIn` seconds from `stretchFrom` (in its own time, before the join) and kept to its end. The next track plays as it
    // is from its first beat: nothing to ease back after the join (the user, 2026-10-28: "after Automix ends the music
    // takes a while to get back to normal"; it was the next track stretched, eased back over 16 beats). 1: as it is
    double speed = 1, easeIn = 0, stretchFrom = 0;
    bool off = false;   // the join is Off: the tracks play one after the other, as without Automix
    bool onBars = false;   // on the beat and on the bars: this track's beat 1 meets the next one's
    bool structured = false;   // planned from the two tracks' sections (v2 step 2)
    bool drop = false;         // planned on the next track's drop (v3: Spotify's joins); off the beat: a bridge between tempos
    bool cut = false;          // between different tempos: a clean cut on a phrase of both (v3)
    double bassCut = 0;        // Eq::BassBridge, Volume::Drop: where this track's bass is cut (0..1 of the fade; the crossover brings the next one's)
    double cross = 0.5;        // the crossover (0..1 of the fade): where the volumes meet and the EQ swaps
    int keyDistance = -1;      // the two keys on the Camelot wheel (v2 step 5; -1: not both sure); 3 and more: they clash
    bool keyClash = false;     // … and the join was kept short, the incoming track muffled until the crossover
    Model model = Latest;      // the model that made it
};

/** a: this track (dur: its length as the player knows it); b: the next one; fallback: the plain fade's length; want: an edited join. */
Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want = nullptr, Model m = Latest);

/**
 * Automix playlists (the user's idea, 2026-10-28): how well a flows into b (the higher the better; 0 when either is not
 * analysed), and the order of a set of tracks with the best joins overall, as Vande Veire & De Bie's track lister: tempos
 * on the beat as they are, else within the stretch (8 %), keys near on the Camelot wheel (when both sure), a fading or
 * quiet end into a quieter start. The order: a greedy path from every start (or from `first`), each improved by moving
 * one track at a time to where it joins best while that helps; the best found.
 */
double joinScore(const mixan::Info* a, const mixan::Info* b, Model m = Latest);
std::vector<int> order(const std::vector<const mixan::Info*>& tracks, int first = -1, Model m = Latest);

/** This track's speed `tau` seconds (output) after its stretch began; and how far into the next track (s, from the plan's
 *  offset) it is when this one has played `tau` seconds of its own past the join's start. */
double speedAt(const Plan& p, double tau);
double inTime(const Plan& p, double tau);

/**
 * Tempo without pitch: WSOLA. Grains of 1024 frames (Hann, half overlapping) taken from the input every 512 × speed
 * frames, each moved by up to ±256 frames to where it best continues the grain before (a cross-correlation on a mono,
 * 4× decimated copy), then added up. At speed 1 the grain that naturally follows is taken without searching: the
 * input comes out exactly as it went in, so it stays in the chain for the rest of the track at no cost. Buffers made
 * in init; read() pulls 16-bit stereo from the decoder as it needs it.
 */
class Stretcher {
public:
    static constexpr int N = 1024, Hs = N / 2, Search = 256;
    void init();
    void begin(const Plan& p, int rate, double delay = 0);   // this track's decoder where it is; the ramp `delay` s (output) later
    bool active() const { return on_; }
    double speedNow() const { return on_ ? speedAt(plan_, double(outFrames_) / rate_ - delay_) : 1; }
    void stop() { on_ = false; }   // a seek, or a track loaded without a join: read straight from the decoder again
    /** Output frames (16-bit stereo); `consumed` gets the input frames used (the track's position moves by them). */
    template <class Read>
    int read(Read&& src, int16_t* out, int frames, long long& consumed) {
        consumed = 0;
        int done = 0;
        while (done < frames) {
            if (outAt_ >= Hs) {
                const long long before = next_;
                if (!hop(src)) break;
                consumed += next_ - before;
            }
            const int n = std::min(frames - done, Hs - outAt_);
            for (int i = 0; i < n; i++) {
                // × 32768, as read in: at its own tempo the samples come out exactly as they went in
                out[(done + i) * 2] = int16_t(std::lrint(std::clamp(ola_[size_t(outAt_ + i) * 2] * 32768.f, -32768.f, 32767.f)));
                out[(done + i) * 2 + 1] = int16_t(std::lrint(std::clamp(ola_[size_t(outAt_ + i) * 2 + 1] * 32768.f, -32768.f, 32767.f)));
            }
            outAt_ += n;
            done += n;
        }
        return done;
    }

private:
    template <class Read>
    bool fill(Read&& src, long long upTo);   // the input buffered up to absolute frame upTo (false: the track ended before)
    template <class Read>
    bool hop(Read&& src);
    Plan plan_;
    int rate_ = 48000;
    double delay_ = 0;
    bool on_ = false, ended_ = false, primed_ = false;
    std::vector<float> in_, win_, ola_, tail_, mono_;
    std::vector<int16_t> scratch_;
    long long base_ = 0, have_ = 0;   // absolute input frame of in_[0]; frames buffered
    long long prev_ = 0, next_ = 0;   // the last grain's start; the input position consumed so far
    double ideal_ = 0;                // where the next grain should start, by the speed
    long long outFrames_ = 0;         // output so far (the speed's clock)
    int outAt_ = Hs;                  // read position in ola_ (Hs: empty)
};

template <class Read>
bool Stretcher::fill(Read&& src, long long upTo) {
    // what can no longer be read (before the next grains' search) leaves the buffer, in large steps
    const long long keep = std::min(prev_ + Hs, (long long)ideal_) - Search - 8;
    if (keep - base_ > 8192 && keep - base_ < have_) {
        const long long drop = keep - base_;
        std::copy(in_.begin() + drop * 2, in_.begin() + have_ * 2, in_.begin());
        base_ += drop;
        have_ -= drop;
    }
    const long long cap = (long long)(in_.size() / 2);
    while (base_ + have_ < upTo && !ended_ && have_ < cap) {
        const int want = int(std::min<long long>({ 2048, cap - have_, std::max<long long>(512, upTo - base_ - have_) }));
        const int got = src(scratch_.data(), want);
        for (int i = 0; i < got * 2; i++) in_[size_t(have_ * 2 + i)] = scratch_[size_t(i)] * (1.f / 32768.f);
        have_ += std::max(0, got);
        if (got < want) ended_ = true;
    }
    return base_ + have_ >= upTo;
}

template <class Read>
bool Stretcher::hop(Read&& src) {
    const double sp = speedAt(plan_, double(outFrames_) / rate_ - delay_);
    const long long natural = prev_ + Hs;
    long long start = natural;
    if (std::fabs(sp - 1) < 1e-6) {
        ideal_ = double(natural);   // its own tempo again: the grain that follows, the input as it was
    } else {
        const long long target = std::max(natural - Hs, (long long)std::llround(ideal_));
        if (fill(src, target + Search + N) && fill(src, natural + Hs)) {
            // where the new grain's first half best matches what naturally follows the last one
            double best = -1e30;
            for (int k = -Search; k <= Search; k += 4) {
                const long long c = target + k - base_, r = natural - base_;
                if (c < 0) continue;
                double s = 0;
                for (int i = 0; i < Hs; i += 4)
                    s += double(in_[size_t((c + i) * 2)] + in_[size_t((c + i) * 2 + 1)]) * double(in_[size_t((r + i) * 2)] + in_[size_t((r + i) * 2 + 1)]);
                if (s > best) { best = s; start = target + k; }
            }
        }
    }
    fill(src, start + N);
    if (start - base_ >= have_ && ended_) return false;   // the track's end
    for (int i = 0; i < Hs; i++)
        for (int c = 0; c < 2; c++) {
            const long long a = (start - base_ + i) * 2 + c, b = (start - base_ + Hs + i) * 2 + c;
            const float x = a >= 0 && a < have_ * 2 ? in_[size_t(a)] : 0.f, y = b >= 0 && b < have_ * 2 ? in_[size_t(b)] : 0.f;
            // the first grain as it is: started in the middle of a track (v3), a rising window would dip the sound
            ola_[size_t(i * 2 + c)] = primed_ ? tail_[size_t(i * 2 + c)] + x * win_[size_t(i)] : x;
            tail_[size_t(i * 2 + c)] = y * win_[size_t(Hs + i)];
        }
    primed_ = true;
    prev_ = start;
    next_ = start + Hs;
    ideal_ += Hs * sp;
    outFrames_ += Hs;
    outAt_ = 0;
    return true;
}

/** The fade's gains at t (0..1): Fader's and the editor's curves. */
struct Gains {
    float in = 0, out = 1;            // the volume curves
    float bandIn[3] = {}, bandOut[3] = { 1, 1, 1 };   // low (< 200 Hz), mid, high (> 2.5 kHz): volume × EQ (the editor draws them)
    bool banded = false;
    // the EQ itself (v2 step 4): each voice's low shelf (200 Hz) and high shelf (2.5 kHz), as gains 0.05..1 (-26 dB at the
    // most, as a DJ mixer's EQ: the incoming track thin and back, never gone), under the volume
    float lowIn = 1, highIn = 1, lowOut = 1, highOut = 1;
    float balance = 1;                // the loudness balance over the overlap: 1 / sqrt(in² + out²), 0.5..1.41 (v3, with the limiter)
    float hpOut = 0, lpIn = 0;        // filter cut-offs, Hz (0: none)
    float dry = 1, wet = 0;           // the outgoing track itself, and its echo
};
void gainsAt(const Plan& p, double t, Gains& g);

/**
 * The fade itself, on interleaved stereo floats. Each voice through a low and a high shelf (the EQ), its volume, the
 * filter sweeps, and the echo (a delay line of at most 2 s, made in init); the sum kept even by the balance. Gatefold's player runs it on the audio thread; tools/preview/mixtest renders
 * joins with it, so what is heard on the PC is what the console plays.
 */
class Fader {
public:
    void init(int rate);
    void begin(const Plan& p);   // at the start of each fade
    /** in: the incoming voice, mixed in place; out: the outgoing one. t: the fade's progress at the first frame, dt per frame. */
    void mix(float* in, const float* out, int frames, double t, double dt);

private:
    struct Bq {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};
        void lowPass(double fc, double q, int rate);
        void highPass(double fc, double q, int rate);
        void shelf(bool high, double fc, double gain, int rate);   // gain linear (0.05..1); RBJ's shelf, S = 1
        float run(int c, float x) {
            const float y = b0 * x + z[c][0];
            z[c][0] = b1 * x - a1 * y + z[c][1];
            z[c][1] = b2 * x - a2 * y;
            return y;
        }
        void clear() { z[0][0] = z[0][1] = z[1][0] = z[1][1] = 0; }
    };
    int rate_ = 48000;
    Plan plan_;
    Bq lsIn_, hsIn_, lsOut_, hsOut_, filtIn_, filtOut_;
    std::vector<float> echo_;   // stereo, 2 s
    int echoLen_ = 1, echoAt_ = 0;
    float lim_ = 1;   // the limiter's gain: the sum kept under -0.5 dBFS (v3: two loud masters summed to +3 dBFS and clipped)
};

/**
 * The outgoing voice, read through: up to the plan's loopTo as it plays (its bars from loopFrom captured on
 * the way), then those bars again and again. Without a loop, or when it started past loopFrom, it just reads.
 * The buffer (up to 8 s of 16-bit stereo) is made in init.
 */
class OutLoop {
public:
    void init(int rate);
    void begin(const Plan& p, long long posFrames);   // where the outgoing voice is now
    /** read(buf, frames) -> frames read: the outgoing voice's decoder. */
    template <class Read>
    int read(Read&& src, int16_t* out, int frames) {
        if (to_ <= 0) return src(out, frames);
        int done = 0;
        while (done < frames) {
            if (pos_ < to_) {
                const int want = int(std::min<long long>(frames - done, to_ - pos_));
                const int got = src(out + done * 2, want);
                for (int i = 0; i < got; i++) {
                    const long long at = pos_ + i - from_;
                    if (at >= 0 && at < len_) {
                        buf_[size_t(at) * 2] = out[(done + i) * 2];
                        buf_[size_t(at) * 2 + 1] = out[(done + i) * 2 + 1];
                    }
                }
                pos_ += got;
                done += got;
                if (got < want) break;   // the track ended before its loop
                continue;
            }
            const int n = int(std::min<long long>(frames - done, len_ - loopAt_));
            for (int i = 0; i < n * 2; i++) out[done * 2 + i] = buf_[size_t(loopAt_) * 2 + size_t(i)];
            loopAt_ = (loopAt_ + n) % len_;
            done += n;
        }
        return done;
    }

private:
    int rate_ = 48000;
    std::vector<int16_t> buf_;
    long long pos_ = 0, from_ = 0, to_ = 0, len_ = 1, loopAt_ = 0;
};

/**
 * A join rendered as Gatefold's player plays it (the PC tool's --render and the join editor's Listen): `pre` seconds of
 * this track before the join (readA positioned there: at startOut - pre in its own time), stretched from the plan's
 * stretchFrom as the player does; the join through OutLoop and the Fader; then `post` seconds of the next one (readB at
 * the plan's offset). read(buf, frames) -> frames; put(floats, frames); `part` says which part put is given (0, 1, 2).
 */
template <class RA, class RB, class Put>
void render(const Plan& pl, int rate, double pre, double post, RA&& readA, RB&& readB, Put&& put, int* part = nullptr,
            const std::atomic<bool>* cancel = nullptr) {
    pre = std::min(pre, pl.startOut);
    Stretcher st;
    st.init();
    if (pl.speed != 1) st.begin(pl, rate, std::max(0.0, pl.stretchFrom - (pl.startOut - pre)));
    long long own = 0;   // this track's frames played, its own time
    std::vector<int16_t> s(4096 * 2);
    auto readOwn = [&](int16_t* o, int n) {
        long long u = 0;
        const int got = st.active() ? st.read(readA, o, n, u) : readA(o, n);
        own += st.active() ? u : got;
        return got;
    };
    std::vector<float> fi(4096 * 2), fo(4096 * 2);
    auto toF = [](const int16_t* x, float* y, int got, int n) {
        for (int i = 0; i < n * 2; i++) y[i] = i < got * 2 ? x[i] / 32768.f : 0.f;
    };
    if (part) *part = 0;
    for (const long long target = (long long)std::llround(pre * rate); own < target && !(cancel && *cancel);) {   // this track alone
        const int n = int(std::clamp<long long>((long long)((target - own) / std::max(0.5, st.speedNow())), 1, 1024));
        const int got = readOwn(s.data(), n);
        toF(s.data(), fo.data(), got, n);
        put(fo.data(), n);
        if (got < n) break;
    }
    Fader fader;
    fader.init(rate);
    fader.begin(pl);
    OutLoop loop;
    loop.init(rate);
    loop.begin(pl, (long long)std::llround(pl.startOut * rate));
    const long long len = std::max(1LL, (long long)std::llround(pl.length * rate));
    if (part) *part = 1;
    for (long long at = 0; at < len && !(cancel && *cancel);) {   // the join
        const int n = int(std::min<long long>(4096, len - at));
        toF(s.data(), fo.data(), loop.read(readOwn, s.data(), n), n);
        toF(s.data(), fi.data(), readB(s.data(), n), n);
        fader.mix(fi.data(), fo.data(), n, double(at) / len, 1.0 / len);
        put(fi.data(), n);
        at += n;
    }
    if (part) *part = 2;
    for (long long left = (long long)std::llround(post * rate); left > 0 && !(cancel && *cancel);) {   // the next one alone
        const int n = int(std::min<long long>(4096, left)), got = readB(s.data(), n);
        toF(s.data(), fi.data(), got, n);
        put(fi.data(), got);
        if (got < n) break;
        left -= n;
    }
}

}  // namespace automix
