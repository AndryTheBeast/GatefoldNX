/*
 * Gatefold NX · automix.cpp
 */
#include "automix.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace automix {

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr float HalfPi = 1.57079632679f;

// The analysis's per-bar digits read in place: plan() runs for every pair when an Automix playlist is ordered (joinScore),
// so it allocates nothing
struct Digits {
    const std::string& s;
    int operator[](size_t i) const { return s[i] - '0'; }
    size_t size() const { return s.size(); }
};
struct Median3 {   // each bar's digit, the median of it and its two neighbours
    const std::string& s;
    int operator[](size_t i) const {
        const int x = s[i] - '0', y = s[i > 0 ? i - 1 : i] - '0', z = s[i + 1 < s.size() ? i + 1 : i] - '0';
        return std::max(std::min(x, y), std::min(std::max(x, y), z));
    }
    size_t size() const { return s.size(); }
};
}  // namespace

const char* name(Preset p) {
    static const char* n[] = { "Auto", "Fade", "Rise", "Blend", "Echo", "Cut", "Custom", "Off" };
    return n[int(p)];
}
const char* name(Volume v) {
    static const char* n[] = { "Smooth crossfade", "Linear crossfade", "Both up, then out", "Cut on the beat", "Full, then out" };
    return n[int(v)];
}
const char* name(Eq e) {
    static const char* n[] = { "None", "Centre bass swap", "Early bass swap", "Late bass swap", "Full EQ swap", "Bass cut, in on the drop" };
    return n[int(e)];
}
const char* name(Filter f) {
    static const char* n[] = { "None", "High-pass out", "Low-pass in", "Both" };
    return n[int(f)];
}
const char* name(Effect e) {
    static const char* n[] = { "None", "Echo out" };
    return n[int(e)];
}

std::string Join::pack() const {
    char s[128];
    snprintf(s, sizeof s, "%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%d,%.2f", int(preset), int(volume), int(eq), int(filter), int(effect), loopBars,
             beatmatch ? 1 : 0, beats, double(seconds), shiftBeats, double(shiftSec));
    return s;
}

bool Join::unpack(const std::string& s, Join& out) {
    int v[9] = {};
    float sec = 0, shift = 0;
    const int got = sscanf(s.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &sec, &v[8], &shift);
    if (got != 9 && got != 11) return false;   // 9: saved before the editor's drag
    if (v[0] < 0 || v[0] > int(Preset::Off) || v[1] < 0 || v[1] > int(Volume::Drop) || v[2] < 0 || v[2] > int(Eq::BassBridge) || v[3] < 0 ||
        v[3] > int(Filter::Both) || v[4] < 0 || v[4] > int(Effect::Echo))
        return false;
    Join j;
    j.preset = Preset(v[0]);
    j.volume = Volume(v[1]);
    j.eq = Eq(v[2]);
    j.filter = Filter(v[3]);
    j.effect = Effect(v[4]);
    j.loopBars = std::clamp(v[5], 0, 2);
    j.beatmatch = v[6] != 0;
    j.beats = v[7];
    j.seconds = std::clamp(sec, 0.f, 30.f);
    j.shiftBeats = std::clamp(v[8], -64, 64);
    j.shiftSec = std::clamp(shift, -30.f, 30.f);
    out = j;
    return true;
}

Join fromPreset(Preset p) {
    Join j;
    j.preset = p;
    switch (p) {
    case Preset::Fade: j.volume = Volume::Smooth; j.eq = Eq::None; j.beatmatch = false; break;
    case Preset::Rise: j.volume = Volume::Smooth; j.eq = Eq::BassCentre; j.filter = Filter::Both; break;
    case Preset::Blend: j.volume = Volume::BothUp; j.eq = Eq::FullSwap; break;
    case Preset::Echo: j.volume = Volume::Smooth; j.eq = Eq::None; j.effect = Effect::Echo; j.beatmatch = false; break;
    case Preset::Cut: j.volume = Volume::Cut; j.eq = Eq::None; j.beats = 4; j.seconds = 0.2f; break;
    default: break;
    }
    return j;
}

// ==== Automix V1.0.0 (the user, 2026-10-29: "not perfect but works really good"). Frozen: never changed again. A change to
// Automix copies the newest model into a namespace of its own (v1_1_0, …), changes the copy, and adds it to Model
// (automix.hpp) and to the dispatch at the end of this file; the Debug tab chooses between them to compare.
namespace v1_0_0 {

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want = nullptr) {
    Plan p;
    Join j = want ? *want : Join{};
    if (j.preset == Preset::Off) {   // no join: this track plays to its end, then the next one
        p.join = j;
        p.off = true;
        p.startOut = dur;
        return p;
    }
    const bool autoMode = j.preset == Preset::Auto;
    const bool ok = a && b && a->valid && b->valid && a->end > 0 && b->seconds > 0;
    const double endA = ok ? std::min(double(a->end), dur) : dur;
    if (!ok) {   // not analysed: a plain fade
        if (autoMode) {
            j = fromPreset(Preset::Fade);
            j.preset = Preset::Auto;
        }
        p.join = j;
        p.length = j.seconds > 0 ? j.seconds : std::max(fallback, 4.0);
        p.startOut = std::max(0.0, endA - p.length);
        return p;
    }
    const double fade = a->end - a->fadeStart, quietIntro = b->introEnd - b->start;
    const bool fades = fade >= 2;

    // ---- can it be on the beat: both grids sure, and a beat of each within 20 ms over the whole mix as they are, or
    // with the next track's tempo matched to this one's (within 8 %: Stretcher); one may be twice the other
    const double P = a->period;
    double err = 1e9, ratio = 1;   // how far apart a beat of each is per beat of this track (s); the next track's speed to match
    const bool sure = a->confidence >= 0.5f && b->confidence >= 0.5f && a->period > 0 && b->periodIn > 0;
    if (sure)
        for (double m : { 1.0, 2.0, 0.5 })
            if (std::fabs(b->periodIn * m - a->period) < err) {
                err = std::fabs(b->periodIn * m - a->period);
                ratio = b->periodIn * m / a->period;   // < 1: it is faster, slowed to this one's tempo
            }
    // no tempo change (the user, 2026-10-28, after Melatonin: "a lot of transitions create a lot of distortion because you
    // are changing the speed of the music; I don't think automixing should change the speed": 14 of its 17 joins stretched
    // a track by up to 8.4 %). On the beat only where the tempos meet as they are (a beat of each within 20 ms over the
    // whole join); match() below then never stretches. The Stretcher and the player's support for it stay, unused
    const bool stretchable = false;
    auto asIs = [&](int n) { return err * n <= 0.02; };
    // the tempos matched (v3): this track takes the next one's tempo a few beats before the join and keeps it to its end;
    // the next one plays as it is. Its length then counts this track's beats at the next one's tempo (startOut set first)
    auto match = [&](Plan& q, int n) {
        if (asIs(n)) { q.length = n * P; return; }
        q.speed = 1 / ratio;
        q.length = n * P * ratio;
        q.easeIn = 8 * P * ratio;
        q.stretchFrom = std::max(double(a->start), q.startOut - q.easeIn * (1 + q.speed) / 2 - 1.5);
    };
    auto fits = [&](int n) { return sure && n * P <= 20 && (asIs(n) || stretchable); };
    // where a mix of n beats ends on this track's grid: through the middle of a fade-out, or at its last beat
    // bars (v2): where the analysis is sure of the downbeats, a join ends on beat 1 of this track and the next one starts
    // on its own first beat 1, so their bars (and, with whole-bar lengths, their phrases) line up; else on any beat
    const bool barsOut = a->barOut > 0 && a->barSureOut >= 0.5f, barsIn = b->barIn >= 0 && b->barSureIn >= 0.5f;
    const double grid = barsOut ? 4 * P : P, ref = barsOut ? a->barOut : a->beatOut;   // this track's grid, and a point on it
    auto endBeat = [&](int n) {
        double t = fades ? a->fadeStart + 0.5 * fade + 0.5 * n * P : a->beatOut;
        t = std::min(t, a->beatOut);
        return ref - std::round((ref - t) / grid) * grid;
    };
    auto roomFor = [&](int n) { return endBeat(n) - n * P >= a->start + 15; };
    // the last beat at full level: before a fade-out starts, else the last one (where a loop ends and an echo starts)
    auto lastFullBeat = [&]() {
        if (a->period <= 0) return fades ? double(a->fadeStart) : endA - 0.3;
        if (!fades) return ref;
        return ref - std::ceil((ref - a->fadeStart) / grid - 1e-9) * grid;
    };
    p.beatmatchable = fits(4);
    // keys (v2 step 5): both sure (the whole track's), and three or more steps apart on the Camelot wheel: they clash. A
    // key found a fifth off (the commonest error) is one step: it never turns a good pair into a clash
    if (a->keySure >= 0.5f && b->keySure >= 0.5f) p.keyDistance = mixan::keyDistance(a->key, b->key);
    const bool clash = p.keyDistance >= 3;

    // ---- Auto, on the drop (v3, as Spotify's mixes do: automix-demo/reference, NEXT_SESSION.md "Automix v3, step 1"): the
    // next track starts m bars before its drop (its first four bars with full bass), so the drop lands on X, the bar after
    // this track's last bass phrase (on its 4-bar phrase grid). This track stays full; its bass is cut hard on a bar line
    // B bars before X (a bridge with no bass at all); the next one rises from the start under a low-pass that opens by X,
    // its bass held out until it slams in on the drop; this one then goes out over T bars. Whole bars on both grids; the
    // keys left alone (Spotify's 9B into 3A sounds right: the tempo and the phrase make it)
    const bool dropSure = b->dropAt > 0 && b->dropJump >= 6;   // its drop's beat found (a downbeat, even where the bars are unsure)
    if (autoMode && sure && barsOut && (barsIn || dropSure) && a->bass.size() > 16 && b->bass.size() > 8 && a->bpm > 0 && (stretchable || asIs(12))) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int D = -1;   // the next track's drop
        for (int i = 0; i + 3 < int(bb.size()) && i <= 64 && D < 0; i++)
            if (std::min({ bb[size_t(i)], bb[size_t(i) + 1], bb[size_t(i) + 2], bb[size_t(i) + 3] }) >= 7) D = i;
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = std::min(int(ba.size()), int(la.size())) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        // the drop's time, and how many of the next track's bars come before it
        const double barB = 4 * double(b->periodIn);
        double dropT = -1;
        int before = 0;
        if (dropSure) {
            dropT = b->dropAt;
            before = int(std::floor((dropT - b->start) / barB + 0.02));
        } else if (D >= 0) {
            before = D >= 4 ? D : 4;   // a drop sooner: from its first bar (one at bar 0: its bass held to bar 4)
            dropT = b->barIn + before * barB;
        }
        if (dropT > 0 && before >= 4 && lastBass > 8) {
            const double bar = 4 * P;   // this track's bar, its own time
            // (a track with bass to its abrupt end: the last phrase boundary that leaves two bars of it, as Spotify cuts out)
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 2;
            const int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            int m = a->barSureOut >= 0.75f && (dropSure || b->barSureIn >= 0.75f) ? 16 : 8;   // phrase-long: 16 bars where both are sure
            m = std::min(m, before);
            while (m > 4 && X - m * bar < a->start + 15) m -= 4;
            const int T = std::clamp(int(std::floor((endA - X) / bar + 1e-6)), 0, 2);
            while (m > 4 && !asIs((m + T) * 4)) m -= 4;   // shorter, so the beats stay together without a tempo change
            const int n = (m + T) * 4;
            if (T >= 1 && xb - m > 0 && X - m * bar >= a->start + 15 && (asIs(n) || stretchable)) {
                int k = xb;   // the bars before X where this track has no bass of its own already: a bridge it makes itself
                while (k > xb - m && k - 1 < int(ba.size()) && ba[size_t(k) - 1] <= 4) k--;
                const int B = std::clamp(std::max(xb - k, m >= 16 ? 4 : 2), 1, m / 2);
                p.startOut = X - m * bar;
                p.offset = std::max(0.0, dropT - m * barB);
                match(p, n);
                p.beatmatched = p.onBars = p.drop = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + T);
                p.bassCut = double(m - B) / (m + T);
                p.echoAt = p.cross;
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = Join{};
                j.volume = Volume::Drop;
                j.eq = Eq::BassBridge;
                j.filter = Filter::LowPassIn;
                j.beats = n;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto, between different tempos (v3, with no tempo change: the user, 2026-10-28): never two beats at once. The
    // next track comes in on a downbeat of its own (its drop, else its first downbeat) exactly where this one's last bass
    // phrase ends (X, on its phrase grid, as the drop join finds it), so the change lands on a phrase of both. Where the
    // next track's bars just before that downbeat have no bass (no kick: pads, a voice, a riser), up to 8 of them play
    // under this track's last bars, rising under a low-pass that opens by X, this track's bass cut a bar or two before (a
    // bridge, as the drop join); else a clean cut on X (30 ms), as Spotify's S&M into Heads Will Roll
    if (autoMode && a->confidence >= 0.5f && barsOut && b->confidence >= 0.5f && b->periodIn > 0 && !fits(16) && a->bass.size() > 8 &&
        a->bars.size() >= a->bass.size() && a->bpm > 0 && (dropSure || barsIn)) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = int(ba.size()) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        const double bar = 4 * P, barB = 4 * double(b->periodIn);
        if (lastBass > 8) {
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 1;
            const int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            const double entry = dropSure ? b->dropAt : b->barIn;     // the next track's downbeat it comes in on
            const int entryBar = int(std::lround((entry - b->barIn) / barB));   // its bar, in the bass digits
            if (X >= a->start + 20 && X < endA - 0.2 && entry >= 0) {
                int k = 0;   // the next track's bars without bass just before it
                while (k < 8 && entryBar - k - 1 >= 0 && entryBar - k - 1 < int(bb.size()) && bb[size_t(entryBar - k - 1)] <= 3) k++;
                k = std::min({ k, int(std::floor((entry - b->start) / barB + 0.02)), int(std::floor((X - a->start - 15) / barB)) });
                if (k >= 2) {   // a bridge
                    const double over = k * barB, tail = std::clamp(P, 0.3, 0.6);
                    p.startOut = X - over;
                    p.offset = std::max(0.0, entry - over);
                    p.length = over + tail;
                    p.cross = over / p.length;
                    const double cutAt = X - (k >= 4 ? 2 : 1) * bar;   // this track's bass, cut on its own bar line
                    p.bassCut = std::clamp((cutAt - p.startOut) / p.length, 0.02, p.cross);
                    p.drop = p.onBars = true;
                    j = Join{};
                    j.volume = Volume::Drop;
                    j.eq = Eq::BassBridge;
                    j.filter = Filter::LowPassIn;
                    j.beatmatch = false;
                    j.seconds = float(p.length);
                } else {   // a cut on the phrase
                    constexpr double W = 0.3;
                    p.startOut = X - W / 2;
                    p.offset = std::max(0.0, entry - W / 2);
                    p.length = W;
                    p.cross = 0.5;
                    p.cut = p.onBars = true;
                    j = fromPreset(Preset::Cut);
                    j.beatmatch = false;
                    j.seconds = float(W);
                }
                p.echoAt = p.cross;
                p.echoDelay = P >= 0.25 ? P : 2 * P;
                p.bpm = b->bpm;
                j.preset = Preset::Auto;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto, by structure (v2 step 2; a "rolling" transition, as Vande Veire & De Bie's automatic DJ): the outgoing
    // track's outro begins after its last loud bars; the incoming one starts so its intro plays under those bars and its
    // first loud section lands on the bar where that outro begins (the crossover: lows and highs change hands there);
    // the outgoing one then fades through its outro. Up to 8 bars each side, whole bars, on the bars of both.
    if (autoMode && sure && barsOut && barsIn && !a->bars.empty() && !b->bars.empty() && a->bpm > 0 && b->bpm > 0) {
        const Median3 la{ a->bars }, lb{ b->bars };   // each bar's loudness, smoothed over three bars
        constexpr int Loud = 7;   // within 1.5 dB of the body's level
        int lastLoud = -1, firstLoud = -1;
        for (int i = int(la.size()) - 1; i >= 1 && lastLoud < 0; i--)
            if (la[size_t(i)] >= Loud && la[size_t(i) - 1] >= Loud) lastLoud = i;
        for (int i = 0; i + 3 < int(lb.size()) && firstLoud < 0; i++)
            if (lb[size_t(i)] >= Loud && lb[size_t(i) + 1] >= Loud && lb[size_t(i) + 2] >= Loud && lb[size_t(i) + 3] >= Loud) firstLoud = i;
        if (lastLoud > 8 && firstLoud >= 0) {
            const double bar = 4 * P, barB = 4 * 60.0 / b->bpm;
            double X = a->barIn + (lastLoud + 1) * 4 * (60.0 / a->bpm);   // the outro's first bar, on the outgoing grid
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;
            const int outro = int(std::floor((endA - X) / bar + 1e-6));
            const int m = std::clamp(firstLoud, 2, clash ? 2 : 8), f = std::clamp(outro, 1, clash ? 2 : 8);   // clashing keys: short
            const int n = (m + f) * 4;
            const bool asIs = err * n <= 0.02;
            const double start = X - m * bar;
            if ((asIs || stretchable) && n * P <= 40 && start >= a->start + 15 && X < endA) {
                p.startOut = start;
                // the incoming track from where its first loud section is m bars away (its intro's start at the most)
                p.offset = b->barIn + std::max(0, firstLoud - m) * barB;
                match(p, n);
                p.beatmatched = p.onBars = p.structured = true;
                p.bpm = asIs ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + f);
                p.echoAt = p.cross;
                const double Pe = p.length / n;   // a beat as heard
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = fromPreset(Preset::Blend);   // both up to the crossover, a full EQ swap on it
                j.beats = n;
                if (clash) {   // the incoming harmony muffled until the crossover
                    j.filter = Filter::LowPassIn;
                    p.keyClash = true;
                }
                j.preset = Preset::Auto;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto chooses
    if (autoMode) {
        int n = 0;
        for (int c : { 32, 16, 8 })
            if ((!clash || c <= 8) && fits(c) && roomFor(c)) { n = c; break; }   // clashing keys: 8 beats at most
        if (n) {
            j = fromPreset(Preset::Blend);   // both up and a full EQ swap; a short one: a smooth bass swap
            if (clash) {
                j.filter = Filter::LowPassIn;
                p.keyClash = true;
            }
            if (n < 16) {
                j.volume = Volume::Smooth;
                j.eq = Eq::BassCentre;
            }
            j.beats = n;
        } else if (fades) {
            j = fromPreset(quietIntro > 3 ? Preset::Rise : Preset::Fade);
            j.beatmatch = false;
            if (a->bassOut > 0.15f && b->bassIn > 0.15f && j.eq == Eq::None) j.eq = Eq::BassCentre;
            j.seconds = float(std::clamp(fade, quietIntro > 3 ? 4.0 : 3.0, clash ? 3.0 : 10.0));
            p.keyClash = clash;
        } else if (a->bpm > 0 && a->period > 0.2f && a->confidence >= 0.3f) {
            j = fromPreset(Preset::Echo);
        } else {
            j = fromPreset(Preset::Fade);
            j.seconds = 2;
        }
        j.preset = Preset::Auto;
    }

    bool loop = j.loopBars > 0 && a->confidence >= 0.5f && a->period > 0;
    // ---- on the beat
    if (j.beatmatch && p.beatmatchable) {
        int n = j.beats > 0 ? j.beats : 0;
        if (!n)
            for (int c : { 32, 16, 8, 4 })
                if (fits(c) && (loop || roomFor(c))) { n = c; break; }
        n = std::clamp(n - n % 4, 4, 64);   // whole bars
        while (n > 4 && !fits(n)) n -= 4;
        if (loop && !asIs(n)) loop = false;   // a loop repeats this track's own bars: not while it is stretched
        if (n >= 4 && fits(n)) {
            double end = endBeat(n);
            double start = end - n * P;
            if (loop) {   // the bars before the last full-level beat repeat; half the mix before them, half over them
                p.loopTo = lastFullBeat();
                p.loopFrom = p.loopTo - j.loopBars * 4 * P;
                start = p.loopTo - std::min(n / 2, j.loopBars * 4 * 2) * P;
            }
            // the editor's drag: whole beats earlier or later, within the track's music
            if (j.shiftBeats) {
                const double lo = std::ceil((a->start + 2 - start) / P), hi = std::floor((endA - 0.25 * n * P - start) / P);
                if (lo <= hi) start += std::clamp(double(j.shiftBeats), lo, hi) * P;
            }
            if (start >= a->start + 2 || loop) {
                p.startOut = std::max(0.0, start);
                p.offset = barsIn ? b->barIn : b->beatIn;
                p.onBars = barsOut && barsIn;
                match(p, n);   // the tempos drift apart over this mix: this track takes the next one's
                p.beatmatched = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.echoAt = 0.5;   // a beat: the mix has an even number of them
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                if (!loop) j.loopBars = 0;
                p.join = j;
                return p;
            }
            p.loopFrom = p.loopTo = -1;
        }
    }

    // ---- timed to the music
    double secs = j.seconds > 0 ? j.seconds : j.volume == Volume::Cut ? 0.2 : j.effect == Effect::Echo ? 4.0 : fades ? std::clamp(fade, 3.0, 10.0) : 4.0;
    if (j.volume == Volume::Cut) secs = std::min(secs, 1.0);
    p.length = secs;
    p.echoDelay = a->period >= 0.25f ? a->period : a->period > 0 ? 2 * a->period : 0.375;
    if (loop) {
        p.loopTo = lastFullBeat();
        p.loopFrom = p.loopTo - j.loopBars * 4 * P;
        p.startOut = p.loopTo - 0.3 * secs;
    } else if (j.effect == Effect::Echo) {
        const double cut = lastFullBeat();   // the echo repeats its last beat at full level
        p.echoAt = 0.3;
        p.startOut = cut - p.echoAt * secs;
    } else if (j.volume == Volume::Cut) {
        p.startOut = endA - 0.05 - 0.5 * secs;
    } else if (fades) {
        p.startOut = endA - secs;                  // the fade ends with its music
    } else if (quietIntro > 3) {
        p.startOut = endA - secs * 0.5;            // the quiet opening half under this track's last seconds
    } else {
        p.startOut = endA - secs * 0.75;           // an abrupt end: heard nearly to its end
    }
    {   // the editor's drag, within the track's music
        const double lo = std::max(0.0, double(a->start)), hi = std::max(lo, endA - 0.5);
        p.startOut = std::clamp(p.startOut + j.shiftSec, lo, hi);
    }
    p.offset = std::max(0.0, double(b->start) - 0.02);
    p.bpm = b->bpm;
    p.join = j;
    return p;
}

}  // namespace v1_0_0

// ==== the tunable planner (PC tools only: tools/preview/mixtrain.cpp): V1.0.0's plan, its learnable numbers read from a
// Tune (default: V1.0.0's own). Training V1.1.0 on the user's ratings (2026-10-29) renders joins with other Tunes; the
// learned one is then frozen as a model of its own (a copy with its numbers written in), never read from here.
namespace tuned {

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want, const Tune& t) {
    Plan p;
    Join j = want ? *want : Join{};
    if (j.preset == Preset::Off) {   // no join: this track plays to its end, then the next one
        p.join = j;
        p.off = true;
        p.startOut = dur;
        return p;
    }
    const bool autoMode = j.preset == Preset::Auto;
    const bool ok = a && b && a->valid && b->valid && a->end > 0 && b->seconds > 0;
    const double endA = ok ? std::min(double(a->end), dur) : dur;
    if (!ok) {   // not analysed: a plain fade
        if (autoMode) {
            j = fromPreset(Preset::Fade);
            j.preset = Preset::Auto;
        }
        p.join = j;
        p.length = j.seconds > 0 ? j.seconds : std::max(fallback, 4.0);
        p.startOut = std::max(0.0, endA - p.length);
        return p;
    }
    const double fade = a->end - a->fadeStart, quietIntro = b->introEnd - b->start;
    const bool fades = fade >= 2;

    // ---- can it be on the beat: both grids sure, and a beat of each within 20 ms over the whole mix as they are, or
    // with the next track's tempo matched to this one's (within 8 %: Stretcher); one may be twice the other
    const double P = a->period;
    double err = 1e9, ratio = 1;   // how far apart a beat of each is per beat of this track (s); the next track's speed to match
    const bool sure = a->confidence >= 0.5f && b->confidence >= 0.5f && a->period > 0 && b->periodIn > 0;
    if (sure)
        for (double m : { 1.0, 2.0, 0.5 })
            if (std::fabs(b->periodIn * m - a->period) < err) {
                err = std::fabs(b->periodIn * m - a->period);
                ratio = b->periodIn * m / a->period;   // < 1: it is faster, slowed to this one's tempo
            }
    // no tempo change (the user, 2026-10-28, after Melatonin: "a lot of transitions create a lot of distortion because you
    // are changing the speed of the music; I don't think automixing should change the speed": 14 of its 17 joins stretched
    // a track by up to 8.4 %). On the beat only where the tempos meet as they are (a beat of each within 20 ms over the
    // whole join); match() below then never stretches. The Stretcher and the player's support for it stay, unused
    const bool stretchable = false;
    auto asIs = [&](int n) { return err * n <= 0.02; };
    // the tempos matched (v3): this track takes the next one's tempo a few beats before the join and keeps it to its end;
    // the next one plays as it is. Its length then counts this track's beats at the next one's tempo (startOut set first)
    auto match = [&](Plan& q, int n) {
        if (asIs(n)) { q.length = n * P; return; }
        q.speed = 1 / ratio;
        q.length = n * P * ratio;
        q.easeIn = 8 * P * ratio;
        q.stretchFrom = std::max(double(a->start), q.startOut - q.easeIn * (1 + q.speed) / 2 - 1.5);
    };
    auto fits = [&](int n) { return sure && n * P <= t.beatMaxSec && (asIs(n) || stretchable); };
    // where a mix of n beats ends on this track's grid: through the middle of a fade-out, or at its last beat
    // bars (v2): where the analysis is sure of the downbeats, a join ends on beat 1 of this track and the next one starts
    // on its own first beat 1, so their bars (and, with whole-bar lengths, their phrases) line up; else on any beat
    const bool barsOut = a->barOut > 0 && a->barSureOut >= 0.5f, barsIn = b->barIn >= 0 && b->barSureIn >= 0.5f;
    const double grid = barsOut ? 4 * P : P, ref = barsOut ? a->barOut : a->beatOut;   // this track's grid, and a point on it
    auto endBeat = [&](int n) {
        double t = fades ? a->fadeStart + 0.5 * fade + 0.5 * n * P : a->beatOut;
        t = std::min(t, a->beatOut);
        return ref - std::round((ref - t) / grid) * grid;
    };
    auto roomFor = [&](int n) { return endBeat(n) - n * P >= a->start + 15; };
    // the last beat at full level: before a fade-out starts, else the last one (where a loop ends and an echo starts)
    auto lastFullBeat = [&]() {
        if (a->period <= 0) return fades ? double(a->fadeStart) : endA - 0.3;
        if (!fades) return ref;
        return ref - std::ceil((ref - a->fadeStart) / grid - 1e-9) * grid;
    };
    p.beatmatchable = fits(4);
    // keys (v2 step 5): both sure (the whole track's), and three or more steps apart on the Camelot wheel: they clash. A
    // key found a fifth off (the commonest error) is one step: it never turns a good pair into a clash
    if (a->keySure >= 0.5f && b->keySure >= 0.5f) p.keyDistance = mixan::keyDistance(a->key, b->key);
    const bool clash = p.keyDistance >= 3;

    // ---- Auto, on the drop (v3, as Spotify's mixes do: automix-demo/reference, NEXT_SESSION.md "Automix v3, step 1"): the
    // next track starts m bars before its drop (its first four bars with full bass), so the drop lands on X, the bar after
    // this track's last bass phrase (on its 4-bar phrase grid). This track stays full; its bass is cut hard on a bar line
    // B bars before X (a bridge with no bass at all); the next one rises from the start under a low-pass that opens by X,
    // its bass held out until it slams in on the drop; this one then goes out over T bars. Whole bars on both grids; the
    // keys left alone (Spotify's 9B into 3A sounds right: the tempo and the phrase make it)
    const bool dropSure = b->dropAt > 0 && b->dropJump >= 6;   // its drop's beat found (a downbeat, even where the bars are unsure)
    if (autoMode && sure && barsOut && (barsIn || dropSure) && a->bass.size() > 16 && b->bass.size() > 8 && a->bpm > 0 && (stretchable || asIs(12))) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int D = -1;   // the next track's drop
        for (int i = 0; i + 3 < int(bb.size()) && i <= 64 && D < 0; i++)
            if (std::min({ bb[size_t(i)], bb[size_t(i) + 1], bb[size_t(i) + 2], bb[size_t(i) + 3] }) >= 7) D = i;
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = std::min(int(ba.size()), int(la.size())) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        // the drop's time, and how many of the next track's bars come before it
        const double barB = 4 * double(b->periodIn);
        double dropT = -1;
        int before = 0;
        if (dropSure) {
            dropT = b->dropAt;
            before = int(std::floor((dropT - b->start) / barB + 0.02));
        } else if (D >= 0) {
            before = D >= 4 ? D : 4;   // a drop sooner: from its first bar (one at bar 0: its bass held to bar 4)
            dropT = b->barIn + before * barB;
        }
        if (dropT > 0 && before >= 4 && lastBass > 8) {
            const double bar = 4 * P;   // this track's bar, its own time
            // (a track with bass to its abrupt end: the last phrase boundary that leaves two bars of it, as Spotify cuts out)
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 2;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            int m = a->barSureOut >= 0.75f && (dropSure || b->barSureIn >= 0.75f) ? t.dropBars : std::min(8, t.dropBars);   // phrase-long: 16 bars where both are sure
            m = std::min(m, before);
            while (m > 4 && X - m * bar < a->start + 15) m -= 4;
            const int T = std::clamp(int(std::floor((endA - X) / bar + 1e-6)), 0, t.exitBars);
            while (m > 4 && !asIs((m + T) * 4)) m -= 4;   // shorter, so the beats stay together without a tempo change
            const int n = (m + T) * 4;
            if (T >= 1 && xb - m > 0 && X - m * bar >= a->start + 15 && (asIs(n) || stretchable)) {
                int k = xb;   // the bars before X where this track has no bass of its own already: a bridge it makes itself
                while (k > xb - m && k - 1 < int(ba.size()) && ba[size_t(k) - 1] <= 4) k--;
                const int B = std::clamp(std::max(xb - k, int(std::lround(m * t.bridgeFrac))), 1, m / 2);
                p.startOut = X - m * bar;
                p.offset = std::max(0.0, dropT - m * barB);
                match(p, n);
                p.beatmatched = p.onBars = p.drop = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + T);
                p.bassCut = double(m - B) / (m + T);
                p.echoAt = p.cross;
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = Join{};
                j.volume = Volume::Drop;
                j.eq = Eq::BassBridge;
                j.filter = Filter::LowPassIn;
                j.beats = n;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto, between different tempos (v3, with no tempo change: the user, 2026-10-28): never two beats at once. The
    // next track comes in on a downbeat of its own (its drop, else its first downbeat) exactly where this one's last bass
    // phrase ends (X, on its phrase grid, as the drop join finds it), so the change lands on a phrase of both. Where the
    // next track's bars just before that downbeat have no bass (no kick: pads, a voice, a riser), up to 8 of them play
    // under this track's last bars, rising under a low-pass that opens by X, this track's bass cut a bar or two before (a
    // bridge, as the drop join); else a clean cut on X (30 ms), as Spotify's S&M into Heads Will Roll
    if (autoMode && a->confidence >= 0.5f && barsOut && b->confidence >= 0.5f && b->periodIn > 0 && !fits(16) && a->bass.size() > 8 &&
        a->bars.size() >= a->bass.size() && a->bpm > 0 && (dropSure || barsIn)) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = int(ba.size()) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        const double bar = 4 * P, barB = 4 * double(b->periodIn);
        if (lastBass > 8) {
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 1;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            const double entry = dropSure ? b->dropAt : b->barIn;     // the next track's downbeat it comes in on
            const int entryBar = int(std::lround((entry - b->barIn) / barB));   // its bar, in the bass digits
            if (X >= a->start + 20 && X < endA - 0.2 && entry >= 0) {
                int k = 0;   // the next track's bars without bass just before it
                while (k < t.tempoBridgeMax && entryBar - k - 1 >= 0 && entryBar - k - 1 < int(bb.size()) && bb[size_t(entryBar - k - 1)] <= 3) k++;
                k = std::min({ k, int(std::floor((entry - b->start) / barB + 0.02)), int(std::floor((X - a->start - 15) / barB)) });
                if (k < 2 && t.noCut) {
                    // (round 2: no cut; the next kind of join below)
                } else {
                if (k >= 2) {   // a bridge
                    const double over = k * barB, tail = std::clamp(P, 0.3, 0.6);
                    p.startOut = X - over;
                    p.offset = std::max(0.0, entry - over);
                    p.length = over + tail;
                    p.cross = over / p.length;
                    const double cutAt = X - (t.tempoCutLead >= 0 ? t.tempoCutLead : k >= 4 ? 2 : 1) * bar;   // this track's bass, cut on its own bar line
                    p.bassCut = std::clamp((cutAt - p.startOut) / p.length, 0.02, p.cross);
                    p.drop = p.onBars = true;
                    j = Join{};
                    j.volume = Volume::Drop;
                    j.eq = Eq::BassBridge;
                    j.filter = Filter::LowPassIn;
                    j.beatmatch = false;
                    j.seconds = float(p.length);
                } else {   // a cut on the phrase
                    const double W = std::max(0.3, 2.0 * t.cutFade);
                    p.startOut = X - W / 2;
                    p.offset = std::max(0.0, entry - W / 2);
                    p.length = W;
                    p.cross = 0.5;
                    p.cut = p.onBars = true;
                    j = fromPreset(Preset::Cut);
                    if (t.cutEcho) j.effect = Effect::Echo;
                    j.beatmatch = false;
                    j.seconds = float(W);
                }
                p.echoAt = p.cross;
                p.echoDelay = P >= 0.25 ? P : 2 * P;
                p.bpm = b->bpm;
                j.preset = Preset::Auto;
                p.join = j;
                return p;
                }
            }
        }
    }

    // ---- Auto, by structure (v2 step 2; a "rolling" transition, as Vande Veire & De Bie's automatic DJ): the outgoing
    // track's outro begins after its last loud bars; the incoming one starts so its intro plays under those bars and its
    // first loud section lands on the bar where that outro begins (the crossover: lows and highs change hands there);
    // the outgoing one then fades through its outro. Up to 8 bars each side, whole bars, on the bars of both.
    if (autoMode && sure && barsOut && barsIn && !a->bars.empty() && !b->bars.empty() && a->bpm > 0 && b->bpm > 0) {
        const Median3 la{ a->bars }, lb{ b->bars };   // each bar's loudness, smoothed over three bars
        constexpr int Loud = 7;   // within 1.5 dB of the body's level
        int lastLoud = -1, firstLoud = -1;
        for (int i = int(la.size()) - 1; i >= 1 && lastLoud < 0; i--)
            if (la[size_t(i)] >= Loud && la[size_t(i) - 1] >= Loud) lastLoud = i;
        for (int i = 0; i + 3 < int(lb.size()) && firstLoud < 0; i++)
            if (lb[size_t(i)] >= Loud && lb[size_t(i) + 1] >= Loud && lb[size_t(i) + 2] >= Loud && lb[size_t(i) + 3] >= Loud) firstLoud = i;
        if (lastLoud > 8 && firstLoud >= 0) {
            const double bar = 4 * P, barB = 4 * 60.0 / b->bpm;
            double X = a->barIn + (lastLoud + 1) * 4 * (60.0 / a->bpm);   // the outro's first bar, on the outgoing grid
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;
            const int outro = int(std::floor((endA - X) / bar + 1e-6));
            const int m = std::clamp(firstLoud, 2, clash ? t.clashBars : 8), f = std::clamp(outro, 1, clash ? t.clashBars : 8);   // clashing keys: short
            const int n = (m + f) * 4;
            const bool asIs = err * n <= 0.02;
            const double start = X - m * bar;
            if ((asIs || stretchable) && n * P <= 40 && start >= a->start + 15 && X < endA) {
                p.startOut = start;
                // the incoming track from where its first loud section is m bars away (its intro's start at the most)
                p.offset = b->barIn + std::max(0, firstLoud - m) * barB;
                match(p, n);
                p.beatmatched = p.onBars = p.structured = true;
                p.bpm = asIs ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + f);
                p.echoAt = p.cross;
                const double Pe = p.length / n;   // a beat as heard
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = fromPreset(Preset::Blend);   // both up to the crossover, a full EQ swap on it
                j.beats = n;
                if (clash) {   // the incoming harmony muffled until the crossover
                    j.filter = Filter::LowPassIn;
                    p.keyClash = true;
                }
                j.preset = Preset::Auto;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto chooses
    if (autoMode) {
        int n = 0;
        for (int c : { 32, 16, 8 })
            if ((!clash || c <= t.clashBeats) && fits(c) && roomFor(c)) { n = c; break; }   // clashing keys: 8 beats at most
        if (n) {
            j = fromPreset(Preset::Blend);   // both up and a full EQ swap; a short one: a smooth bass swap
            if (clash) {
                j.filter = Filter::LowPassIn;
                p.keyClash = true;
            }
            if (n < t.blendFrom) {
                j.volume = Volume::Smooth;
                j.eq = Eq::BassCentre;
            }
            j.beats = n;
        } else if (fades) {
            j = fromPreset(quietIntro > 3 ? Preset::Rise : Preset::Fade);
            j.beatmatch = false;
            if (a->bassOut > 0.15f && b->bassIn > 0.15f && j.eq == Eq::None) j.eq = Eq::BassCentre;
            j.seconds = float(std::clamp(fade * t.fadeScale, quietIntro > 3 ? 4.0 : 3.0, clash ? 3.0 : 10.0));
            p.keyClash = clash;
        } else if (a->bpm > 0 && a->period > 0.2f && a->confidence >= 0.3f) {
            j = fromPreset(Preset::Echo);
        } else {
            j = fromPreset(Preset::Fade);
            j.seconds = float(2 * t.fadeScale);
        }
        j.preset = Preset::Auto;
    }

    bool loop = j.loopBars > 0 && a->confidence >= 0.5f && a->period > 0;
    // ---- on the beat
    if (j.beatmatch && p.beatmatchable) {
        int n = j.beats > 0 ? j.beats : 0;
        if (!n)
            for (int c : { 32, 16, 8, 4 })
                if (fits(c) && (loop || roomFor(c))) { n = c; break; }
        n = std::clamp(n - n % 4, 4, 64);   // whole bars
        while (n > 4 && !fits(n)) n -= 4;
        if (loop && !asIs(n)) loop = false;   // a loop repeats this track's own bars: not while it is stretched
        if (n >= 4 && fits(n)) {
            double end = endBeat(n);
            double start = end - n * P;
            if (loop) {   // the bars before the last full-level beat repeat; half the mix before them, half over them
                p.loopTo = lastFullBeat();
                p.loopFrom = p.loopTo - j.loopBars * 4 * P;
                start = p.loopTo - std::min(n / 2, j.loopBars * 4 * 2) * P;
            }
            // the editor's drag: whole beats earlier or later, within the track's music
            if (j.shiftBeats) {
                const double lo = std::ceil((a->start + 2 - start) / P), hi = std::floor((endA - 0.25 * n * P - start) / P);
                if (lo <= hi) start += std::clamp(double(j.shiftBeats), lo, hi) * P;
            }
            if (start >= a->start + 2 || loop) {
                p.startOut = std::max(0.0, start);
                p.offset = barsIn ? b->barIn : b->beatIn;
                p.onBars = barsOut && barsIn;
                match(p, n);   // the tempos drift apart over this mix: this track takes the next one's
                p.beatmatched = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.echoAt = 0.5;   // a beat: the mix has an even number of them
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                if (!loop) j.loopBars = 0;
                p.join = j;
                return p;
            }
            p.loopFrom = p.loopTo = -1;
        }
    }

    // ---- timed to the music
    double secs = j.seconds > 0 ? j.seconds : j.volume == Volume::Cut ? 0.2 : j.effect == Effect::Echo ? 4.0 * t.fadeScale : fades ? std::clamp(fade, 3.0, 10.0) : 4.0;
    if (j.volume == Volume::Cut) secs = std::min(secs, 1.0);
    p.length = secs;
    p.echoDelay = a->period >= 0.25f ? a->period : a->period > 0 ? 2 * a->period : 0.375;
    if (loop) {
        p.loopTo = lastFullBeat();
        p.loopFrom = p.loopTo - j.loopBars * 4 * P;
        p.startOut = p.loopTo - 0.3 * secs;
    } else if (j.effect == Effect::Echo) {
        const double cut = lastFullBeat();   // the echo repeats its last beat at full level
        p.echoAt = 0.3;
        p.startOut = cut - p.echoAt * secs;
    } else if (j.volume == Volume::Cut) {
        p.startOut = endA - 0.05 - 0.5 * secs;
    } else if (fades) {
        p.startOut = endA - secs;                  // the fade ends with its music
    } else if (quietIntro > 3) {
        p.startOut = endA - secs * 0.5;            // the quiet opening half under this track's last seconds
    } else {
        p.startOut = endA - secs * 0.75;           // an abrupt end: heard nearly to its end
    }
    {   // the editor's drag, within the track's music
        const double lo = std::max(0.0, double(a->start)), hi = std::max(lo, endA - 0.5);
        p.startOut = std::clamp(p.startOut + j.shiftSec, lo, hi);
    }
    p.offset = std::max(0.0, double(b->start) - 0.02);
    p.bpm = b->bpm;
    p.join = j;
    return p;
}

}  // namespace tuned

Plan planTuned(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Tune& t) {
    Plan p = tuned::plan(a, dur, b, fallback, nullptr, t);
    p.inLevel = t.inLevel;
    p.lpFrom = t.lpFrom;
    p.outPow = !p.beatmatched && p.drop && t.bridgeOutPow > 0 ? t.bridgeOutPow : t.outPow;   // a bridge: drop, not beatmatched
    p.cutFade = t.cutFade;
    p.gate = p.beatmatched && p.bpm > 0 ? t.gate : 0;
    p.gateLevel = t.gateLevel;
    p.gateDuck = t.gateDuck;
    p.gateEvery = t.gateEvery;
    return p;
}

void gainsAt(const Plan& p, double t, Gains& g) {
    const float tt = float(std::clamp(t, 0.0, 1.0));
    switch (p.join.volume) {
    case Volume::Linear: g.in = tt; g.out = 1 - tt; break;
    case Volume::BothUp: {   // as Vande Veire & De Bie's automatic DJ: the incoming one up to 0.8 by the crossover; there it
        // goes to full and the outgoing one to 0.8 (over 15 % of what follows), which then fades out
        const float c = float(std::clamp(p.cross, 0.05, 0.95));
        if (tt < c) {
            g.in = 0.8f * std::sin(tt / c * HalfPi);
            g.out = 1;
        } else {
            const float u = (tt - c) / (1 - c);
            g.in = 0.8f + 0.2f * std::min(1.f, u / 0.15f);
            g.out = u < 0.15f ? 1 - 0.2f * u / 0.15f : 0.8f * std::cos((u - 0.15f) / 0.85f * HalfPi);
        }
        break;
    }
    case Volume::Cut: {   // at the crossover (the middle unless planned), over 30 ms
        const float w = float(std::clamp(p.cutFade / std::max(0.05, p.length), 0.005, 0.2)), c = float(std::clamp(p.cross, 0.05, 0.95));
        const float u = std::clamp((tt - (c - w / 2)) / w, 0.f, 1.f);
        g.in = u * u * (3 - 2 * u);
        g.out = 1 - g.in;
        break;
    }
    case Volume::Drop: {   // v3, as Spotify's editor draws it: the outgoing one full until the crossover, then out (most of it
        // in the first half of what follows); the incoming one up from the start, near full by the bass cut, full on the drop
        const float c = float(std::clamp(p.cross, 0.05, 0.98)), k = float(std::clamp(p.bassCut > 0 ? p.bassCut : p.cross * 0.75, 0.02, double(c)));
        // (the incoming level until the bass cut and the outgoing exit's shape: Plan fields, V1.0.0's 0.9 and a square by default)
        const float lv = p.inLevel, rest = p.inLevel == 0.9f ? 0.1f : 1 - p.inLevel;
        g.in = tt < k ? lv * std::sin(tt / k * HalfPi) : tt < c ? lv + rest * (tt - k) / std::max(1e-3f, c - k) : 1.f;
        const float u = tt < c ? 1.f : std::cos((tt - c) / (1 - c) * HalfPi);
        g.out = p.outPow == 2 ? u * u : std::pow(u, p.outPow);
        break;
    }
    default: g.in = std::sin(tt * HalfPi); g.out = std::cos(tt * HalfPi); break;
    }
    // the EQ, on top of the volume (a DJ mixer's EQ under its faders): where each band (low, mid, high) changes hands;
    // -1: the band is not swapped
    // (around the crossover, 0.5 unless the structure placed it)
    float sp[3] = { -1, -1, -1 };
    const float c = float(std::clamp(p.cross, 0.1, 0.9));
    switch (p.join.eq) {
    case Eq::BassCentre: sp[0] = c; break;
    case Eq::BassEarly: sp[0] = c * 0.5f; break;
    case Eq::BassLate: sp[0] = c + (1 - c) * 0.5f; break;
    case Eq::FullSwap:   // lows and highs (the shelves): the highs a little before the crossover, the lows on it
        sp[0] = c;
        sp[2] = c * 0.85f;
        break;
    default: break;
    }
    g.banded = p.join.eq != Eq::None;
    float sw[3] = { 1, 1, 1 }, so[3] = { 1, 1, 1 };   // each shelf, 0.05..1 (-26 dB at the most)
    for (int k = 0; k < 3; k += 2) {
        if (sp[k] < 0) continue;
        const float u = std::clamp((tt - (sp[k] - 0.04f)) / 0.08f, 0.f, 1.f), s = u * u * (3 - 2 * u);
        sw[k] = 0.05f + 0.95f * s;
        so[k] = 0.05f + 0.95f * (1 - s);
    }
    if (p.join.eq == Eq::BassBridge) {   // v3: the outgoing bass cut hard on a bar line, the incoming one in on the drop: each
        // within 30 ms, ending on its bar line, so the drop's first kick comes through whole; -40 dB, gone as in Spotify's mixes
        const float c = float(std::clamp(p.cross, 0.05, 0.98)), k = float(std::clamp(p.bassCut > 0 ? p.bassCut : p.cross * 0.75, 0.02, double(c)));
        const float w = float(std::clamp(0.03 / std::max(0.1, p.length), 0.001, 0.05));
        auto step = [&](float at) {
            const float u = std::clamp((tt - (at - w)) / w, 0.f, 1.f);
            return u * u * (3 - 2 * u);
        };
        so[0] = 1 - 0.99f * step(k);
        sw[0] = 0.01f + 0.99f * step(c);
    }
    g.lowIn = sw[0];
    g.highIn = sw[2];
    g.lowOut = so[0];
    g.highOut = so[2];
    for (int k = 0; k < 3; k++) {
        g.bandIn[k] = g.in * sw[k];
        g.bandOut[k] = g.out * so[k];
    }
    const bool hp = p.join.filter == Filter::HighPassOut || p.join.filter == Filter::Both;
    const bool lp = p.join.filter == Filter::LowPassIn || p.join.filter == Filter::Both;
    g.hpOut = hp && tt > 0.2f ? float(20 * std::pow(100.0, (tt - 0.2) / 0.8)) : 0;   // 20 Hz to 2 kHz
    const double open = p.structured || p.drop ? std::clamp(p.cross, 0.2, 0.98) : 0.7;   // open by the crossover in a structured join
    g.lpIn = lp && tt < open ? float(p.lpFrom * std::pow(20000.0 / p.lpFrom, tt / open)) : 0;   // 250 Hz (lpFrom) to 20 kHz, then open
    // the balance: both playing at 0.8 and 1 would be louder than either alone (auto-dj: 1 / sqrt(v1² + v2²)); here with
    // the shelves and the low-pass counted too (music's energy roughly 40 % low, 45 % mid, 15 % high), or a join dipped by
    // 2 dB at its swap. At most +3 dB (v3; it was +4 dB, and joins of loud masters peaked at +3 dBFS): the limiter keeps the rest
    float mid = 1, top = 1;   // the share of the incoming mids and highs the low-pass lets through
    if (g.lpIn > 0) {
        mid = float(std::clamp(std::log(g.lpIn / 250.0) / std::log(16.0), 0.0, 1.0));
        top = float(std::clamp(std::log(g.lpIn / 2500.0) / std::log(4.0), 0.0, 1.0));
    }
    auto power = [](float v, float lo, float md, float hi) { return v * v * (0.40f * lo * lo + 0.45f * md * md + 0.15f * hi * hi); };
    const float pw = power(g.in, g.lowIn, mid, g.highIn * top) + power(g.out, g.lowOut, 1, g.highOut);
    g.balance = std::clamp(1.f / std::sqrt(std::max(1e-4f, pw)), 0.5f, 1.41f);
    // the gate (Plan::gate; none in V1.0.0), after the balance so the other voice does not pump with it: 1, the incoming
    // one in stabs of half a beat on beats 1 and 3 until the crossover (a Spotify mix: stabs of the next song over the whole
    // outgoing one, then the switch on the downbeat); 2, the outgoing one in a stab of a beat and a half on each bar's first
    // beat over the first half of the lead-in, then whole but high-passed away, 20 Hz to 1 kHz by the crossover (another:
    // chopped a bar at a time, then filtered out as the next one's bass comes back). The join starts on a bar.
    if (p.gate && p.bpm > 0 && p.length > 0) {
        const double beat = 60.0 / p.bpm, tau = tt * p.length, c = std::clamp(p.cross, 0.05, 0.98);
        auto stab = [&](double phase, double on) {   // in beats: 1 for `on` beats from 0, with 4 ms ramps
            const double r = 0.004 / beat;
            return phase < on ? float(std::clamp(std::min(phase, on - phase) / r, 0.0, 1.0)) : 0.f;
        };
        if (p.gate == 1 && tt < c) {   // the stabs heard at near full level, as in that mix (the incoming one faded under
            // the outgoing one, they were lost); still without the bass where the EQ holds it out
            const float k = stab(std::fmod(tau / beat, double(std::max(1, p.gateEvery))), 0.5), lv = std::max(g.in, p.gateLevel) * k;
            g.in = lv;
            for (int b = 0; b < 3; b++) g.bandIn[b] = lv * sw[b];
            if (p.gateDuck > 0) {   // the outgoing voice dips under each stab, so the stab is heard (round 5)
                const float d = 1 - p.gateDuck * k;
                g.out *= d;
                for (float& v : g.bandOut) v *= d;
            }
        } else if (p.gate == 2) {
            if (tt < c / 2) {
                const float k = stab(std::fmod(tau / beat, 4.0), 1.5);
                g.out *= k;
                for (float& v : g.bandOut) v *= k;
            } else {
                g.hpOut = std::max(g.hpOut, float(20 * std::pow(50.0, std::min(1.0, (tt - c / 2) / (c / 2)))));
            }
        }
    }
    if (p.join.effect == Effect::Echo) {
        const float e = float(p.echoAt);
        g.dry = tt < e ? 1.f : std::max(0.f, 1 - (tt - e) / 0.01f);
        g.wet = tt < e ? 0.f : std::clamp(1 - (tt - e) / std::max(0.05f, 1 - e), 0.f, 1.f);
    } else {
        g.dry = 1;
        g.wet = 0;
    }
}

void Fader::Bq::lowPass(double fc, double q, int rate) {
    const double w0 = 2 * Pi * std::min(fc, rate * 0.45) / rate, c = std::cos(w0), al = std::sin(w0) / (2 * q), a0 = 1 + al;
    b0 = float((1 - c) / 2 / a0); b1 = float((1 - c) / a0); b2 = b0; a1 = float(-2 * c / a0); a2 = float((1 - al) / a0);
}

void Fader::Bq::highPass(double fc, double q, int rate) {
    const double w0 = 2 * Pi * std::min(fc, rate * 0.45) / rate, c = std::cos(w0), al = std::sin(w0) / (2 * q), a0 = 1 + al;
    b0 = float((1 + c) / 2 / a0); b1 = float(-(1 + c) / a0); b2 = b0; a1 = float(-2 * c / a0); a2 = float((1 - al) / a0);
}

void Fader::Bq::shelf(bool high, double fc, double gain, int rate) {
    const double A = std::sqrt(std::max(1e-4, gain)), w0 = 2 * Pi * fc / rate, c = std::cos(w0), al = std::sin(w0) / 2 * std::sqrt(2.0),
                 sa = 2 * std::sqrt(A) * al;
    double nb0, nb1, nb2, na0, na1, na2;
    if (!high) {
        nb0 = A * ((A + 1) - (A - 1) * c + sa); nb1 = 2 * A * ((A - 1) - (A + 1) * c); nb2 = A * ((A + 1) - (A - 1) * c - sa);
        na0 = (A + 1) + (A - 1) * c + sa; na1 = -2 * ((A - 1) + (A + 1) * c); na2 = (A + 1) + (A - 1) * c - sa;
    } else {
        nb0 = A * ((A + 1) + (A - 1) * c + sa); nb1 = -2 * A * ((A - 1) + (A + 1) * c); nb2 = A * ((A + 1) + (A - 1) * c - sa);
        na0 = (A + 1) - (A - 1) * c + sa; na1 = 2 * ((A - 1) - (A + 1) * c); na2 = (A + 1) - (A - 1) * c - sa;
    }
    b0 = float(nb0 / na0); b1 = float(nb1 / na0); b2 = float(nb2 / na0); a1 = float(na1 / na0); a2 = float(na2 / na0);
}

void Fader::init(int rate, double echoSeconds) {
    rate_ = rate;
    echo_.assign(std::max<size_t>(2, size_t(std::lround(rate * echoSeconds)) * 2), 0.f);
}

bool Fader::reserveEcho(double seconds) {
    const size_t want = size_t(std::ceil(seconds * rate_) + 1) * 2;
    if (want <= echo_.size()) return true;
    echo_.assign(want, 0.f);
    return true;
}

void Fader::releaseEcho() { std::vector<float>(2, 0.f).swap(echo_); }

void Fader::begin(const Plan& p) {
    plan_ = p;
    for (Bq* b : { &lsIn_, &hsIn_, &lsOut_, &hsOut_, &filtIn_, &filtOut_ }) b->clear();
    echoLen_ = std::clamp(int(std::lround(p.echoDelay * rate_)), 1, int(echo_.size() / 2));
    echoAt_ = 0;
    std::fill(echo_.begin(), echo_.begin() + echoLen_ * 2, 0.f);
    lim_ = 1;
}

void Fader::mix(float* in, const float* out, int frames, double t, double dt) {
    constexpr int Block = 16;   // the gains (and the filters' cut-offs) move every 16 frames
    Gains g;
    const bool echo = plan_.join.effect == Effect::Echo;
    for (int i0 = 0; i0 < frames; i0 += Block) {
        const int n = std::min(Block, frames - i0);
        const double tb = t + (i0 + n * 0.5) * dt;
        gainsAt(plan_, tb, g);
        if (g.hpOut > 0) filtOut_.highPass(g.hpOut, 0.9, rate_);
        if (g.lpIn > 0) filtIn_.lowPass(g.lpIn, 0.9, rate_);
        if (g.banded) {   // the shelves, at their gains (linear; RBJ's A is its square root)
            lsIn_.shelf(false, 200, g.lowIn, rate_);
            hsIn_.shelf(true, 2500, g.highIn, rate_);
            lsOut_.shelf(false, 200, g.lowOut, rate_);
            hsOut_.shelf(true, 2500, g.highOut, rate_);
        }
        const bool feeding = tb < plan_.echoAt;
        for (int i = i0; i < i0 + n; i++)
            for (int c = 0; c < 2; c++) {
                float x = in[i * 2 + c], o = out[i * 2 + c];
                if (g.lpIn > 0) x = filtIn_.run(c, x);
                if (g.hpOut > 0) o = filtOut_.run(c, o);
                if (g.banded) {   // the EQ
                    x = hsIn_.run(c, lsIn_.run(c, x));
                    o = hsOut_.run(c, lsOut_.run(c, o));
                }
                const float mixIn = x * g.in, mixOut = o * g.out;
                float wet = 0;
                if (echo) {   // the delay line takes the outgoing track until the cut, then only its own repeats
                    float& slot = echo_[size_t(echoAt_) * 2 + size_t(c)];
                    const float y = slot;
                    slot = (feeding ? mixOut : 0.f) + 0.5f * y;
                    wet = y * g.wet;
                }
                in[i * 2 + c] = (mixIn + mixOut * g.dry + wet) * g.balance;
                if (echo && c == 1) echoAt_ = (echoAt_ + 1) % echoLen_;
            }
        // the limiter (v3): each 16-frame block's peak kept under -0.5 dBFS (down at once, back up over ~0.1 s)
        float peak = 0;
        for (int i = i0 * 2; i < (i0 + n) * 2; i++) peak = std::max(peak, std::fabs(in[i]));
        constexpr float Ceiling = 0.944f;
        const float want = peak > Ceiling ? Ceiling / peak : 1.f;
        const bool down = want < lim_;   // down: the whole block at once (no sample over the ceiling); up: eased across it
        const float to = down ? want : lim_ + (1 - lim_) * float(1 - std::exp(-double(n) / (0.1 * rate_)));
        if (to < 1 || lim_ < 1)
            for (int i = 0; i < n; i++) {
                const float gl = down ? to : lim_ + (to - lim_) * float(i + 1) / float(n);
                in[(i0 + i) * 2] = std::clamp(in[(i0 + i) * 2] * gl, -1.f, 1.f);
                in[(i0 + i) * 2 + 1] = std::clamp(in[(i0 + i) * 2 + 1] * gl, -1.f, 1.f);
            }
        lim_ = to;
    }
}

namespace v1_0_0 {   // (Automix V1.0.0, frozen: the order's score and the order)

double joinScore(const mixan::Info* a, const mixan::Info* b) {
    if (!a || !b || !a->valid || !b->valid) return 0;
    // v3: scored from the join Auto would make (Spotify's mixes: the tempo and the phrase make a join, not the keys): on
    // the next track's drop best, then by the structure, then on the beat, then timed; the less stretch the better
    const Plan p = plan(a, a->seconds, b, 4);
    // on the beat: on the drop 6, by the structure 4.5, on the bars 3.5, on the beat 3; between tempos (v3, no tempo
    // change): a bridge into the drop 3, a cut on the phrase 2; a timed fade 0
    double s = p.beatmatched ? (p.drop ? 6 : p.structured ? 4.5 : p.onBars ? 3.5 : 3) : p.drop ? 3 : p.cut ? 2 : 0;
    if (p.beatmatched) s -= 25 * std::fabs(p.speed - 1);
    else if (a->bpm > 0 && b->bpm > 0) {   // off the beat: tempos far apart are a bigger jump, even on a phrase
        double d = 9;
        for (double m : { 1.0, 2.0, 0.5 }) d = std::min(d, std::fabs(b->bpm * m / a->bpm - 1));
        s -= std::min(p.drop || p.cut ? 1.5 : 3.0, 10 * d);
    }
    // keys, when both are sure: a little only (Spotify's 9B into 3A sounds right)
    if (p.keyDistance >= 0) s += p.keyDistance == 0 ? 0.75 : p.keyDistance == 1 ? 0.5 : p.keyDistance == 2 ? 0 : -0.5;
    // a fading or quiet end into a quieter start helps a timed join
    if (!p.beatmatched) {
        if (a->end - a->fadeStart >= 2) s += 0.5;
        if (b->introEnd - b->start >= 4) s += 0.5;
    }
    // ties broken by how near the tempos are (else equal joins are ordered by where they happened to stand)
    if (a->bpm > 0 && b->bpm > 0) s -= 0.01 * std::fabs(double(b->bpm) - a->bpm);
    return s;
}

std::vector<int> order(const std::vector<const mixan::Info*>& t, int first) {
    const int n = int(t.size());
    std::vector<int> best(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) best[size_t(i)] = i;
    if (n < 3) return best;
    std::vector<double> w(size_t(n) * size_t(n));
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) w[size_t(i) * size_t(n) + size_t(j)] = i == j ? -1e9 : joinScore(t[size_t(i)], t[size_t(j)]);
    auto W = [&](int i, int j) { return w[size_t(i) * size_t(n) + size_t(j)]; };
    auto total = [&](const std::vector<int>& p) {
        double s = 0;
        for (size_t k = 0; k + 1 < p.size(); k++) s += W(p[k], p[k + 1]);
        return s;
    };
    double bestScore = -1e18;
    // starts spread over the list: up to 64, fewer for a long one (each is improved at a cost of n² a pass: 559 tracks took
    // 1.6 s on the PC with 64, seconds of a frozen screen on the console). AUTOMIX_TRIES (PC tools) overrides it
    int tries = first >= 0 ? 1 : std::min(n, n <= 100 ? 64 : std::max(4, 4000 / n));
#ifndef __SWITCH__
    if (const char* e = getenv("AUTOMIX_TRIES"); e && first < 0) tries = std::clamp(atoi(e), 1, n);
#endif
    for (int s = 0; s < tries; s++) {
        const int start = first >= 0 ? first : s * n / tries;
        std::vector<int> p{ start };
        std::vector<bool> used(static_cast<size_t>(n), false);
        used[size_t(start)] = true;
        for (int k = 1; k < n; k++) {   // greedy: the best next join
            int arg = -1;
            for (int j = 0; j < n; j++)
                if (!used[size_t(j)] && (arg < 0 || W(p.back(), j) > W(p.back(), arg))) arg = j;
            p.push_back(arg);
            used[size_t(arg)] = true;
        }
        // improve: move one track (never a pinned first) to the place where the path gains most, while it gains
        for (bool better = true; better;) {
            better = false;
            for (int i = first >= 0 ? 1 : 0; i < n; i++) {
                const int x = p[size_t(i)];
                const double out = (i > 0 ? W(p[size_t(i - 1)], x) : 0) + (i + 1 < n ? W(x, p[size_t(i + 1)]) : 0) -
                                   (i > 0 && i + 1 < n ? W(p[size_t(i - 1)], p[size_t(i + 1)]) : 0);
                std::vector<int> q = p;
                q.erase(q.begin() + i);
                double gain = 0;
                int at = -1;
                for (int k = first >= 0 ? 1 : 0; k <= n - 1; k++) {   // insert before q[k] (k == n - 1: at the end)
                    const double in = (k > 0 ? W(q[size_t(k - 1)], x) : 0) + (k < n - 1 ? W(x, q[size_t(k)]) : 0) -
                                      (k > 0 && k < n - 1 ? W(q[size_t(k - 1)], q[size_t(k)]) : 0);
                    if (in - out > gain + 1e-9) { gain = in - out; at = k; }
                }
                if (at >= 0) {
                    q.insert(q.begin() + at, x);
                    p = q;
                    better = true;
                }
            }
        }
        const double sc = total(p);
        if (sc > bestScore) { bestScore = sc; best = p; }
    }
    return best;
}

}  // namespace v1_0_0

// ==== Automix V1.1.0 (frozen 2026-10-29): the first trained model. Four rounds of demos from the user's library, rated 0-10
// (automix-demo/train, NEXT_SESSION.md "Automix V1.1.0: a trained model"; rounds 1 4.4, 2 4.96, 3 5.46, 4 7.0): V1.0.0's
// plan as the tunable planner had it (a copy: it never changes with `tuned`), with the numbers learnt written in (kTune):
// never a cut between tempos (noCut) and no music skipped (noSkip: the change where the outgoing song starts to fade out);
// on the drop 8 bars before it, the outgoing song gone a bar after it at a cos^5 exit (round 4: over 2, and 12 "too
// abrupt"); a bridge between tempos up to 8 bars, its exit cos^2; the incoming level 0.8 until the bass cut, its low-pass
// from 1 kHz; a timed fade as long as V1.0.0's. No gate (round 4: liked in stabs, not settled). Its order is V1.0.0's (the
// order it was trained on).
namespace v1_1_0 {

const Tune kTune = [] {
    Tune t;
    t.noSkip = t.noCut = true;
    t.dropBars = 8;
    t.bridgeFrac = 0.25f;
    t.exitBars = 1;
    t.tempoBridgeMax = 8;
    t.tempoCutLead = 2;
    t.cutFade = 0.03;
    t.cutEcho = false;
    t.fadeScale = 1;
    t.inLevel = 0.8f;
    t.lpFrom = 1000;
    t.outPow = 5;
    t.bridgeOutPow = 2;
    t.gate = 0;
    return t;
}();

Plan planWith(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want, const Tune& t) {
    Plan p;
    Join j = want ? *want : Join{};
    if (j.preset == Preset::Off) {   // no join: this track plays to its end, then the next one
        p.join = j;
        p.off = true;
        p.startOut = dur;
        return p;
    }
    const bool autoMode = j.preset == Preset::Auto;
    const bool ok = a && b && a->valid && b->valid && a->end > 0 && b->seconds > 0;
    const double endA = ok ? std::min(double(a->end), dur) : dur;
    if (!ok) {   // not analysed: a plain fade
        if (autoMode) {
            j = fromPreset(Preset::Fade);
            j.preset = Preset::Auto;
        }
        p.join = j;
        p.length = j.seconds > 0 ? j.seconds : std::max(fallback, 4.0);
        p.startOut = std::max(0.0, endA - p.length);
        return p;
    }
    const double fade = a->end - a->fadeStart, quietIntro = b->introEnd - b->start;
    const bool fades = fade >= 2;

    // ---- can it be on the beat: both grids sure, and a beat of each within 20 ms over the whole mix as they are, or
    // with the next track's tempo matched to this one's (within 8 %: Stretcher); one may be twice the other
    const double P = a->period;
    double err = 1e9, ratio = 1;   // how far apart a beat of each is per beat of this track (s); the next track's speed to match
    const bool sure = a->confidence >= 0.5f && b->confidence >= 0.5f && a->period > 0 && b->periodIn > 0;
    if (sure)
        for (double m : { 1.0, 2.0, 0.5 })
            if (std::fabs(b->periodIn * m - a->period) < err) {
                err = std::fabs(b->periodIn * m - a->period);
                ratio = b->periodIn * m / a->period;   // < 1: it is faster, slowed to this one's tempo
            }
    // no tempo change (the user, 2026-10-28, after Melatonin: "a lot of transitions create a lot of distortion because you
    // are changing the speed of the music; I don't think automixing should change the speed": 14 of its 17 joins stretched
    // a track by up to 8.4 %). On the beat only where the tempos meet as they are (a beat of each within 20 ms over the
    // whole join); match() below then never stretches. The Stretcher and the player's support for it stay, unused
    const bool stretchable = false;
    auto asIs = [&](int n) { return err * n <= 0.02; };
    // the tempos matched (v3): this track takes the next one's tempo a few beats before the join and keeps it to its end;
    // the next one plays as it is. Its length then counts this track's beats at the next one's tempo (startOut set first)
    auto match = [&](Plan& q, int n) {
        if (asIs(n)) { q.length = n * P; return; }
        q.speed = 1 / ratio;
        q.length = n * P * ratio;
        q.easeIn = 8 * P * ratio;
        q.stretchFrom = std::max(double(a->start), q.startOut - q.easeIn * (1 + q.speed) / 2 - 1.5);
    };
    auto fits = [&](int n) { return sure && n * P <= 20 && (asIs(n) || stretchable); };
    // where a mix of n beats ends on this track's grid: through the middle of a fade-out, or at its last beat
    // bars (v2): where the analysis is sure of the downbeats, a join ends on beat 1 of this track and the next one starts
    // on its own first beat 1, so their bars (and, with whole-bar lengths, their phrases) line up; else on any beat
    const bool barsOut = a->barOut > 0 && a->barSureOut >= 0.5f, barsIn = b->barIn >= 0 && b->barSureIn >= 0.5f;
    const double grid = barsOut ? 4 * P : P, ref = barsOut ? a->barOut : a->beatOut;   // this track's grid, and a point on it
    auto endBeat = [&](int n) {
        double t = fades ? a->fadeStart + 0.5 * fade + 0.5 * n * P : a->beatOut;
        t = std::min(t, a->beatOut);
        return ref - std::round((ref - t) / grid) * grid;
    };
    auto roomFor = [&](int n) { return endBeat(n) - n * P >= a->start + 15; };
    // the last beat at full level: before a fade-out starts, else the last one (where a loop ends and an echo starts)
    auto lastFullBeat = [&]() {
        if (a->period <= 0) return fades ? double(a->fadeStart) : endA - 0.3;
        if (!fades) return ref;
        return ref - std::ceil((ref - a->fadeStart) / grid - 1e-9) * grid;
    };
    p.beatmatchable = fits(4);
    // keys (v2 step 5): both sure (the whole track's), and three or more steps apart on the Camelot wheel: they clash. A
    // key found a fifth off (the commonest error) is one step: it never turns a good pair into a clash
    if (a->keySure >= 0.5f && b->keySure >= 0.5f) p.keyDistance = mixan::keyDistance(a->key, b->key);
    const bool clash = p.keyDistance >= 3;

    // ---- Auto, on the drop (v3, as Spotify's mixes do: automix-demo/reference, NEXT_SESSION.md "Automix v3, step 1"): the
    // next track starts m bars before its drop (its first four bars with full bass), so the drop lands on X, the bar after
    // this track's last bass phrase (on its 4-bar phrase grid). This track stays full; its bass is cut hard on a bar line
    // B bars before X (a bridge with no bass at all); the next one rises from the start under a low-pass that opens by X,
    // its bass held out until it slams in on the drop; this one then goes out over T bars. Whole bars on both grids; the
    // keys left alone (Spotify's 9B into 3A sounds right: the tempo and the phrase make it)
    const bool dropSure = b->dropAt > 0 && b->dropJump >= 6;   // its drop's beat found (a downbeat, even where the bars are unsure)
    if (autoMode && sure && barsOut && (barsIn || dropSure) && a->bass.size() > 16 && b->bass.size() > 8 && a->bpm > 0 && (stretchable || asIs(12))) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int D = -1;   // the next track's drop
        for (int i = 0; i + 3 < int(bb.size()) && i <= 64 && D < 0; i++)
            if (std::min({ bb[size_t(i)], bb[size_t(i) + 1], bb[size_t(i) + 2], bb[size_t(i) + 3] }) >= 7) D = i;
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = std::min(int(ba.size()), int(la.size())) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        // the drop's time, and how many of the next track's bars come before it
        const double barB = 4 * double(b->periodIn);
        double dropT = -1;
        int before = 0;
        if (dropSure) {
            dropT = b->dropAt;
            before = int(std::floor((dropT - b->start) / barB + 0.02));
        } else if (D >= 0) {
            before = D >= 4 ? D : 4;   // a drop sooner: from its first bar (one at bar 0: its bass held to bar 4)
            dropT = b->barIn + before * barB;
        }
        if (dropT > 0 && before >= 4 && lastBass > 8) {
            const double bar = 4 * P;   // this track's bar, its own time
            // (a track with bass to its abrupt end: the last phrase boundary that leaves two bars of it, as Spotify cuts out)
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 2;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            int m = a->barSureOut >= 0.75f && (dropSure || b->barSureIn >= 0.75f) ? t.dropBars : std::min(8, t.dropBars);   // phrase-long: 16 bars where both are sure
            m = std::min(m, before);
            while (m > 4 && X - m * bar < a->start + 15) m -= 4;
            const int T = std::clamp(int(std::floor((endA - X) / bar + 1e-6)), 0, t.exitBars);
            while (m > 4 && !asIs((m + T) * 4)) m -= 4;   // shorter, so the beats stay together without a tempo change
            const int n = (m + T) * 4;
            if (T >= 1 && xb - m > 0 && X - m * bar >= a->start + 15 && (asIs(n) || stretchable)) {
                int k = xb;   // the bars before X where this track has no bass of its own already: a bridge it makes itself
                while (k > xb - m && k - 1 < int(ba.size()) && ba[size_t(k) - 1] <= 4) k--;
                const int B = std::clamp(std::max(xb - k, int(std::lround(m * t.bridgeFrac))), 1, m / 2);
                p.startOut = X - m * bar;
                p.offset = std::max(0.0, dropT - m * barB);
                match(p, n);
                p.beatmatched = p.onBars = p.drop = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + T);
                p.bassCut = double(m - B) / (m + T);
                p.echoAt = p.cross;
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = Join{};
                j.volume = Volume::Drop;
                j.eq = Eq::BassBridge;
                j.filter = Filter::LowPassIn;
                j.beats = n;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto, between different tempos (v3, with no tempo change: the user, 2026-10-28): never two beats at once. The
    // next track comes in on a downbeat of its own (its drop, else its first downbeat) exactly where this one's last bass
    // phrase ends (X, on its phrase grid, as the drop join finds it), so the change lands on a phrase of both. Where the
    // next track's bars just before that downbeat have no bass (no kick: pads, a voice, a riser), up to 8 of them play
    // under this track's last bars, rising under a low-pass that opens by X, this track's bass cut a bar or two before (a
    // bridge, as the drop join); else a clean cut on X (30 ms), as Spotify's S&M into Heads Will Roll
    if (autoMode && a->confidence >= 0.5f && barsOut && b->confidence >= 0.5f && b->periodIn > 0 && !fits(16) && a->bass.size() > 8 &&
        a->bars.size() >= a->bass.size() && a->bpm > 0 && (dropSure || barsIn)) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = int(ba.size()) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        const double bar = 4 * P, barB = 4 * double(b->periodIn);
        if (lastBass > 8) {
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 1;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            const double entry = dropSure ? b->dropAt : b->barIn;     // the next track's downbeat it comes in on
            const int entryBar = int(std::lround((entry - b->barIn) / barB));   // its bar, in the bass digits
            if (X >= a->start + 20 && X < endA - 0.2 && entry >= 0) {
                int k = 0;   // the next track's bars without bass just before it
                while (k < t.tempoBridgeMax && entryBar - k - 1 >= 0 && entryBar - k - 1 < int(bb.size()) && bb[size_t(entryBar - k - 1)] <= 3) k++;
                k = std::min({ k, int(std::floor((entry - b->start) / barB + 0.02)), int(std::floor((X - a->start - 15) / barB)) });
                if (k < 2 && t.noCut) {
                    // (round 2: no cut; the next kind of join below)
                } else {
                if (k >= 2) {   // a bridge
                    const double over = k * barB, tail = std::clamp(P, 0.3, 0.6);
                    p.startOut = X - over;
                    p.offset = std::max(0.0, entry - over);
                    p.length = over + tail;
                    p.cross = over / p.length;
                    const double cutAt = X - (t.tempoCutLead >= 0 ? t.tempoCutLead : k >= 4 ? 2 : 1) * bar;   // this track's bass, cut on its own bar line
                    p.bassCut = std::clamp((cutAt - p.startOut) / p.length, 0.02, p.cross);
                    p.drop = p.onBars = true;
                    j = Join{};
                    j.volume = Volume::Drop;
                    j.eq = Eq::BassBridge;
                    j.filter = Filter::LowPassIn;
                    j.beatmatch = false;
                    j.seconds = float(p.length);
                } else {   // a cut on the phrase
                    const double W = std::max(0.3, 2.0 * t.cutFade);
                    p.startOut = X - W / 2;
                    p.offset = std::max(0.0, entry - W / 2);
                    p.length = W;
                    p.cross = 0.5;
                    p.cut = p.onBars = true;
                    j = fromPreset(Preset::Cut);
                    if (t.cutEcho) j.effect = Effect::Echo;
                    j.beatmatch = false;
                    j.seconds = float(W);
                }
                p.echoAt = p.cross;
                p.echoDelay = P >= 0.25 ? P : 2 * P;
                p.bpm = b->bpm;
                j.preset = Preset::Auto;
                p.join = j;
                return p;
                }
            }
        }
    }

    // ---- Auto, by structure (v2 step 2; a "rolling" transition, as Vande Veire & De Bie's automatic DJ): the outgoing
    // track's outro begins after its last loud bars; the incoming one starts so its intro plays under those bars and its
    // first loud section lands on the bar where that outro begins (the crossover: lows and highs change hands there);
    // the outgoing one then fades through its outro. Up to 8 bars each side, whole bars, on the bars of both.
    if (autoMode && sure && barsOut && barsIn && !a->bars.empty() && !b->bars.empty() && a->bpm > 0 && b->bpm > 0) {
        const Median3 la{ a->bars }, lb{ b->bars };   // each bar's loudness, smoothed over three bars
        constexpr int Loud = 7;   // within 1.5 dB of the body's level
        int lastLoud = -1, firstLoud = -1;
        for (int i = int(la.size()) - 1; i >= 1 && lastLoud < 0; i--)
            if (la[size_t(i)] >= Loud && la[size_t(i) - 1] >= Loud) lastLoud = i;
        for (int i = 0; i + 3 < int(lb.size()) && firstLoud < 0; i++)
            if (lb[size_t(i)] >= Loud && lb[size_t(i) + 1] >= Loud && lb[size_t(i) + 2] >= Loud && lb[size_t(i) + 3] >= Loud) firstLoud = i;
        if (lastLoud > 8 && firstLoud >= 0) {
            const double bar = 4 * P, barB = 4 * 60.0 / b->bpm;
            double X = a->barIn + (lastLoud + 1) * 4 * (60.0 / a->bpm);   // the outro's first bar, on the outgoing grid
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;
            const int outro = int(std::floor((endA - X) / bar + 1e-6));
            const int m = std::clamp(firstLoud, 2, clash ? 2 : 8), f = std::clamp(outro, 1, clash ? 2 : 8);   // clashing keys: short
            const int n = (m + f) * 4;
            const bool asIs = err * n <= 0.02;
            const double start = X - m * bar;
            if ((asIs || stretchable) && n * P <= 40 && start >= a->start + 15 && X < endA) {
                p.startOut = start;
                // the incoming track from where its first loud section is m bars away (its intro's start at the most)
                p.offset = b->barIn + std::max(0, firstLoud - m) * barB;
                match(p, n);
                p.beatmatched = p.onBars = p.structured = true;
                p.bpm = asIs ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + f);
                p.echoAt = p.cross;
                const double Pe = p.length / n;   // a beat as heard
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = fromPreset(Preset::Blend);   // both up to the crossover, a full EQ swap on it
                j.beats = n;
                if (clash) {   // the incoming harmony muffled until the crossover
                    j.filter = Filter::LowPassIn;
                    p.keyClash = true;
                }
                j.preset = Preset::Auto;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto chooses
    if (autoMode) {
        int n = 0;
        for (int c : { 32, 16, 8 })
            if ((!clash || c <= 8) && fits(c) && roomFor(c)) { n = c; break; }   // clashing keys: 8 beats at most
        if (n) {
            j = fromPreset(Preset::Blend);   // both up and a full EQ swap; a short one: a smooth bass swap
            if (clash) {
                j.filter = Filter::LowPassIn;
                p.keyClash = true;
            }
            if (n < 16) {
                j.volume = Volume::Smooth;
                j.eq = Eq::BassCentre;
            }
            j.beats = n;
        } else if (fades) {
            j = fromPreset(quietIntro > 3 ? Preset::Rise : Preset::Fade);
            j.beatmatch = false;
            if (a->bassOut > 0.15f && b->bassIn > 0.15f && j.eq == Eq::None) j.eq = Eq::BassCentre;
            j.seconds = float(std::clamp(fade * t.fadeScale, quietIntro > 3 ? 4.0 : 3.0, clash ? 3.0 : 10.0));
            p.keyClash = clash;
        } else if (a->bpm > 0 && a->period > 0.2f && a->confidence >= 0.3f) {
            j = fromPreset(Preset::Echo);
        } else {
            j = fromPreset(Preset::Fade);
            j.seconds = float(2 * t.fadeScale);
        }
        j.preset = Preset::Auto;
    }

    bool loop = j.loopBars > 0 && a->confidence >= 0.5f && a->period > 0;
    // ---- on the beat
    if (j.beatmatch && p.beatmatchable) {
        int n = j.beats > 0 ? j.beats : 0;
        if (!n)
            for (int c : { 32, 16, 8, 4 })
                if (fits(c) && (loop || roomFor(c))) { n = c; break; }
        n = std::clamp(n - n % 4, 4, 64);   // whole bars
        while (n > 4 && !fits(n)) n -= 4;
        if (loop && !asIs(n)) loop = false;   // a loop repeats this track's own bars: not while it is stretched
        if (n >= 4 && fits(n)) {
            double end = endBeat(n);
            double start = end - n * P;
            if (loop) {   // the bars before the last full-level beat repeat; half the mix before them, half over them
                p.loopTo = lastFullBeat();
                p.loopFrom = p.loopTo - j.loopBars * 4 * P;
                start = p.loopTo - std::min(n / 2, j.loopBars * 4 * 2) * P;
            }
            // the editor's drag: whole beats earlier or later, within the track's music
            if (j.shiftBeats) {
                const double lo = std::ceil((a->start + 2 - start) / P), hi = std::floor((endA - 0.25 * n * P - start) / P);
                if (lo <= hi) start += std::clamp(double(j.shiftBeats), lo, hi) * P;
            }
            if (start >= a->start + 2 || loop) {
                p.startOut = std::max(0.0, start);
                p.offset = barsIn ? b->barIn : b->beatIn;
                p.onBars = barsOut && barsIn;
                match(p, n);   // the tempos drift apart over this mix: this track takes the next one's
                p.beatmatched = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.echoAt = 0.5;   // a beat: the mix has an even number of them
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                if (!loop) j.loopBars = 0;
                p.join = j;
                return p;
            }
            p.loopFrom = p.loopTo = -1;
        }
    }

    // ---- timed to the music
    double secs = j.seconds > 0 ? j.seconds : j.volume == Volume::Cut ? 0.2 : j.effect == Effect::Echo ? 4.0 * t.fadeScale : fades ? std::clamp(fade, 3.0, 10.0) : 4.0;
    if (j.volume == Volume::Cut) secs = std::min(secs, 1.0);
    p.length = secs;
    p.echoDelay = a->period >= 0.25f ? a->period : a->period > 0 ? 2 * a->period : 0.375;
    if (loop) {
        p.loopTo = lastFullBeat();
        p.loopFrom = p.loopTo - j.loopBars * 4 * P;
        p.startOut = p.loopTo - 0.3 * secs;
    } else if (j.effect == Effect::Echo) {
        const double cut = lastFullBeat();   // the echo repeats its last beat at full level
        p.echoAt = 0.3;
        p.startOut = cut - p.echoAt * secs;
    } else if (j.volume == Volume::Cut) {
        p.startOut = endA - 0.05 - 0.5 * secs;
    } else if (fades) {
        p.startOut = endA - secs;                  // the fade ends with its music
    } else if (quietIntro > 3) {
        p.startOut = endA - secs * 0.5;            // the quiet opening half under this track's last seconds
    } else {
        p.startOut = endA - secs * 0.75;           // an abrupt end: heard nearly to its end
    }
    {   // the editor's drag, within the track's music
        const double lo = std::max(0.0, double(a->start)), hi = std::max(lo, endA - 0.5);
        p.startOut = std::clamp(p.startOut + j.shiftSec, lo, hi);
    }
    p.offset = std::max(0.0, double(b->start) - 0.02);
    p.bpm = b->bpm;
    p.join = j;
    return p;
}

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want) {
    const Tune& t = kTune;
    Plan p = planWith(a, dur, b, fallback, want, t);
    p.inLevel = t.inLevel;
    p.lpFrom = t.lpFrom;
    p.outPow = !p.beatmatched && p.drop ? t.bridgeOutPow : t.outPow;   // a bridge between tempos: its own exit
    p.cutFade = t.cutFade;
    p.gate = 0;
    return p;
}

}  // namespace v1_1_0

// ==== Model files, format 1 (frozen 2026-10-30 with V1.2.0, the first model sent over the air): the tunable planner as it
// stood after round 5 (a copy: it never changes with `tuned`), every number read from the file's Tune; then the curves'
// numbers and the gate, on the kinds of join on the beat the file names (ModelFile::gateOn).
namespace file1 {

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want, const Tune& t) {
    Plan p;
    Join j = want ? *want : Join{};
    if (j.preset == Preset::Off) {   // no join: this track plays to its end, then the next one
        p.join = j;
        p.off = true;
        p.startOut = dur;
        return p;
    }
    const bool autoMode = j.preset == Preset::Auto;
    const bool ok = a && b && a->valid && b->valid && a->end > 0 && b->seconds > 0;
    const double endA = ok ? std::min(double(a->end), dur) : dur;
    if (!ok) {   // not analysed: a plain fade
        if (autoMode) {
            j = fromPreset(Preset::Fade);
            j.preset = Preset::Auto;
        }
        p.join = j;
        p.length = j.seconds > 0 ? j.seconds : std::max(fallback, 4.0);
        p.startOut = std::max(0.0, endA - p.length);
        return p;
    }
    const double fade = a->end - a->fadeStart, quietIntro = b->introEnd - b->start;
    const bool fades = fade >= 2;

    // ---- can it be on the beat: both grids sure, and a beat of each within 20 ms over the whole mix as they are, or
    // with the next track's tempo matched to this one's (within 8 %: Stretcher); one may be twice the other
    const double P = a->period;
    double err = 1e9, ratio = 1;   // how far apart a beat of each is per beat of this track (s); the next track's speed to match
    const bool sure = a->confidence >= 0.5f && b->confidence >= 0.5f && a->period > 0 && b->periodIn > 0;
    if (sure)
        for (double m : { 1.0, 2.0, 0.5 })
            if (std::fabs(b->periodIn * m - a->period) < err) {
                err = std::fabs(b->periodIn * m - a->period);
                ratio = b->periodIn * m / a->period;   // < 1: it is faster, slowed to this one's tempo
            }
    // no tempo change (the user, 2026-10-28, after Melatonin: "a lot of transitions create a lot of distortion because you
    // are changing the speed of the music; I don't think automixing should change the speed": 14 of its 17 joins stretched
    // a track by up to 8.4 %). On the beat only where the tempos meet as they are (a beat of each within 20 ms over the
    // whole join); match() below then never stretches. The Stretcher and the player's support for it stay, unused
    const bool stretchable = false;
    auto asIs = [&](int n) { return err * n <= 0.02; };
    // the tempos matched (v3): this track takes the next one's tempo a few beats before the join and keeps it to its end;
    // the next one plays as it is. Its length then counts this track's beats at the next one's tempo (startOut set first)
    auto match = [&](Plan& q, int n) {
        if (asIs(n)) { q.length = n * P; return; }
        q.speed = 1 / ratio;
        q.length = n * P * ratio;
        q.easeIn = 8 * P * ratio;
        q.stretchFrom = std::max(double(a->start), q.startOut - q.easeIn * (1 + q.speed) / 2 - 1.5);
    };
    auto fits = [&](int n) { return sure && n * P <= t.beatMaxSec && (asIs(n) || stretchable); };
    // where a mix of n beats ends on this track's grid: through the middle of a fade-out, or at its last beat
    // bars (v2): where the analysis is sure of the downbeats, a join ends on beat 1 of this track and the next one starts
    // on its own first beat 1, so their bars (and, with whole-bar lengths, their phrases) line up; else on any beat
    const bool barsOut = a->barOut > 0 && a->barSureOut >= 0.5f, barsIn = b->barIn >= 0 && b->barSureIn >= 0.5f;
    const double grid = barsOut ? 4 * P : P, ref = barsOut ? a->barOut : a->beatOut;   // this track's grid, and a point on it
    auto endBeat = [&](int n) {
        double t = fades ? a->fadeStart + 0.5 * fade + 0.5 * n * P : a->beatOut;
        t = std::min(t, a->beatOut);
        return ref - std::round((ref - t) / grid) * grid;
    };
    auto roomFor = [&](int n) { return endBeat(n) - n * P >= a->start + 15; };
    // the last beat at full level: before a fade-out starts, else the last one (where a loop ends and an echo starts)
    auto lastFullBeat = [&]() {
        if (a->period <= 0) return fades ? double(a->fadeStart) : endA - 0.3;
        if (!fades) return ref;
        return ref - std::ceil((ref - a->fadeStart) / grid - 1e-9) * grid;
    };
    p.beatmatchable = fits(4);
    // keys (v2 step 5): both sure (the whole track's), and three or more steps apart on the Camelot wheel: they clash. A
    // key found a fifth off (the commonest error) is one step: it never turns a good pair into a clash
    if (a->keySure >= 0.5f && b->keySure >= 0.5f) p.keyDistance = mixan::keyDistance(a->key, b->key);
    const bool clash = p.keyDistance >= 3;

    // ---- Auto, on the drop (v3, as Spotify's mixes do: automix-demo/reference, NEXT_SESSION.md "Automix v3, step 1"): the
    // next track starts m bars before its drop (its first four bars with full bass), so the drop lands on X, the bar after
    // this track's last bass phrase (on its 4-bar phrase grid). This track stays full; its bass is cut hard on a bar line
    // B bars before X (a bridge with no bass at all); the next one rises from the start under a low-pass that opens by X,
    // its bass held out until it slams in on the drop; this one then goes out over T bars. Whole bars on both grids; the
    // keys left alone (Spotify's 9B into 3A sounds right: the tempo and the phrase make it)
    const bool dropSure = b->dropAt > 0 && b->dropJump >= 6;   // its drop's beat found (a downbeat, even where the bars are unsure)
    if (autoMode && sure && barsOut && (barsIn || dropSure) && a->bass.size() > 16 && b->bass.size() > 8 && a->bpm > 0 && (stretchable || asIs(12))) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int D = -1;   // the next track's drop
        for (int i = 0; i + 3 < int(bb.size()) && i <= 64 && D < 0; i++)
            if (std::min({ bb[size_t(i)], bb[size_t(i) + 1], bb[size_t(i) + 2], bb[size_t(i) + 3] }) >= 7) D = i;
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = std::min(int(ba.size()), int(la.size())) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        // the drop's time, and how many of the next track's bars come before it
        const double barB = 4 * double(b->periodIn);
        double dropT = -1;
        int before = 0;
        if (dropSure) {
            dropT = b->dropAt;
            before = int(std::floor((dropT - b->start) / barB + 0.02));
        } else if (D >= 0) {
            before = D >= 4 ? D : 4;   // a drop sooner: from its first bar (one at bar 0: its bass held to bar 4)
            dropT = b->barIn + before * barB;
        }
        if (dropT > 0 && before >= 4 && lastBass > 8) {
            const double bar = 4 * P;   // this track's bar, its own time
            // (a track with bass to its abrupt end: the last phrase boundary that leaves two bars of it, as Spotify cuts out)
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 2;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            int m = a->barSureOut >= 0.75f && (dropSure || b->barSureIn >= 0.75f) ? t.dropBars : std::min(8, t.dropBars);   // phrase-long: 16 bars where both are sure
            m = std::min(m, before);
            while (m > 4 && X - m * bar < a->start + 15) m -= 4;
            const int T = std::clamp(int(std::floor((endA - X) / bar + 1e-6)), 0, t.exitBars);
            while (m > 4 && !asIs((m + T) * 4)) m -= 4;   // shorter, so the beats stay together without a tempo change
            const int n = (m + T) * 4;
            if (T >= 1 && xb - m > 0 && X - m * bar >= a->start + 15 && (asIs(n) || stretchable)) {
                int k = xb;   // the bars before X where this track has no bass of its own already: a bridge it makes itself
                while (k > xb - m && k - 1 < int(ba.size()) && ba[size_t(k) - 1] <= 4) k--;
                const int B = std::clamp(std::max(xb - k, int(std::lround(m * t.bridgeFrac))), 1, m / 2);
                p.startOut = X - m * bar;
                p.offset = std::max(0.0, dropT - m * barB);
                match(p, n);
                p.beatmatched = p.onBars = p.drop = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + T);
                p.bassCut = double(m - B) / (m + T);
                p.echoAt = p.cross;
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = Join{};
                j.volume = Volume::Drop;
                j.eq = Eq::BassBridge;
                j.filter = Filter::LowPassIn;
                j.beats = n;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto, between different tempos (v3, with no tempo change: the user, 2026-10-28): never two beats at once. The
    // next track comes in on a downbeat of its own (its drop, else its first downbeat) exactly where this one's last bass
    // phrase ends (X, on its phrase grid, as the drop join finds it), so the change lands on a phrase of both. Where the
    // next track's bars just before that downbeat have no bass (no kick: pads, a voice, a riser), up to 8 of them play
    // under this track's last bars, rising under a low-pass that opens by X, this track's bass cut a bar or two before (a
    // bridge, as the drop join); else a clean cut on X (30 ms), as Spotify's S&M into Heads Will Roll
    if (autoMode && a->confidence >= 0.5f && barsOut && b->confidence >= 0.5f && b->periodIn > 0 && !fits(16) && a->bass.size() > 8 &&
        a->bars.size() >= a->bass.size() && a->bpm > 0 && (dropSure || barsIn)) {
        const Digits ba{ a->bass }, bb{ b->bass }, la{ a->bars };
        int lastBass = -1;   // this track's last two bars with full bass, loud
        for (int i = int(ba.size()) - 1; i >= 1 && lastBass < 0; i--)
            if (ba[size_t(i)] >= 7 && ba[size_t(i) - 1] >= 7 && la[size_t(i)] >= 6) lastBass = i;
        const double bar = 4 * P, barB = 4 * double(b->periodIn);
        if (lastBass > 8) {
            const int xbEnd = int(std::floor((endA - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)) - 1;
            int xb = a->phraseAt + 4 * int(std::floor((std::min(lastBass + 1, xbEnd) - a->phraseAt) / 4.0));
            if (t.noSkip) {   // (round 2: the change where this track starts to fade out, or at its end: no full music skipped)
                const double fadeAt = a->end - a->fadeStart >= 2 ? double(a->fadeStart) : endA;
                const int xf = std::min(int(std::floor((fadeAt - a->barIn) / (4 * 60.0 / a->bpm) + 1e-6)), xbEnd);
                // on a bar (a phrase's grid of 4 bars left 12 s of an abruptly ending song unplayed: round 2)
                xb = std::max(xb, xf);
            }
            double X = a->barIn + xb * 4 * (60.0 / a->bpm);
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;   // on the outro's grid
            const double entry = dropSure ? b->dropAt : b->barIn;     // the next track's downbeat it comes in on
            const int entryBar = int(std::lround((entry - b->barIn) / barB));   // its bar, in the bass digits
            if (X >= a->start + 20 && X < endA - 0.2 && entry >= 0) {
                int k = 0;   // the next track's bars without bass just before it
                while (k < t.tempoBridgeMax && entryBar - k - 1 >= 0 && entryBar - k - 1 < int(bb.size()) && bb[size_t(entryBar - k - 1)] <= 3) k++;
                k = std::min({ k, int(std::floor((entry - b->start) / barB + 0.02)), int(std::floor((X - a->start - 15) / barB)) });
                if (k < 2 && t.noCut) {
                    // (round 2: no cut; the next kind of join below)
                } else {
                if (k >= 2) {   // a bridge
                    const double over = k * barB, tail = std::clamp(P, 0.3, 0.6);
                    p.startOut = X - over;
                    p.offset = std::max(0.0, entry - over);
                    p.length = over + tail;
                    p.cross = over / p.length;
                    const double cutAt = X - (t.tempoCutLead >= 0 ? t.tempoCutLead : k >= 4 ? 2 : 1) * bar;   // this track's bass, cut on its own bar line
                    p.bassCut = std::clamp((cutAt - p.startOut) / p.length, 0.02, p.cross);
                    p.drop = p.onBars = true;
                    j = Join{};
                    j.volume = Volume::Drop;
                    j.eq = Eq::BassBridge;
                    j.filter = Filter::LowPassIn;
                    j.beatmatch = false;
                    j.seconds = float(p.length);
                } else {   // a cut on the phrase
                    const double W = std::max(0.3, 2.0 * t.cutFade);
                    p.startOut = X - W / 2;
                    p.offset = std::max(0.0, entry - W / 2);
                    p.length = W;
                    p.cross = 0.5;
                    p.cut = p.onBars = true;
                    j = fromPreset(Preset::Cut);
                    if (t.cutEcho) j.effect = Effect::Echo;
                    j.beatmatch = false;
                    j.seconds = float(W);
                }
                p.echoAt = p.cross;
                p.echoDelay = P >= 0.25 ? P : 2 * P;
                p.bpm = b->bpm;
                j.preset = Preset::Auto;
                p.join = j;
                return p;
                }
            }
        }
    }

    // ---- Auto, by structure (v2 step 2; a "rolling" transition, as Vande Veire & De Bie's automatic DJ): the outgoing
    // track's outro begins after its last loud bars; the incoming one starts so its intro plays under those bars and its
    // first loud section lands on the bar where that outro begins (the crossover: lows and highs change hands there);
    // the outgoing one then fades through its outro. Up to 8 bars each side, whole bars, on the bars of both.
    if (autoMode && sure && barsOut && barsIn && !a->bars.empty() && !b->bars.empty() && a->bpm > 0 && b->bpm > 0) {
        const Median3 la{ a->bars }, lb{ b->bars };   // each bar's loudness, smoothed over three bars
        constexpr int Loud = 7;   // within 1.5 dB of the body's level
        int lastLoud = -1, firstLoud = -1;
        for (int i = int(la.size()) - 1; i >= 1 && lastLoud < 0; i--)
            if (la[size_t(i)] >= Loud && la[size_t(i) - 1] >= Loud) lastLoud = i;
        for (int i = 0; i + 3 < int(lb.size()) && firstLoud < 0; i++)
            if (lb[size_t(i)] >= Loud && lb[size_t(i) + 1] >= Loud && lb[size_t(i) + 2] >= Loud && lb[size_t(i) + 3] >= Loud) firstLoud = i;
        if (lastLoud > 8 && firstLoud >= 0) {
            const double bar = 4 * P, barB = 4 * 60.0 / b->bpm;
            double X = a->barIn + (lastLoud + 1) * 4 * (60.0 / a->bpm);   // the outro's first bar, on the outgoing grid
            X = a->barOut - std::round((a->barOut - X) / bar) * bar;
            const int outro = int(std::floor((endA - X) / bar + 1e-6));
            const int m = std::clamp(firstLoud, 2, clash ? t.clashBars : 8), f = std::clamp(outro, 1, clash ? t.clashBars : 8);   // clashing keys: short
            const int n = (m + f) * 4;
            const bool asIs = err * n <= 0.02;
            const double start = X - m * bar;
            if ((asIs || stretchable) && n * P <= 40 && start >= a->start + 15 && X < endA) {
                p.startOut = start;
                // the incoming track from where its first loud section is m bars away (its intro's start at the most)
                p.offset = b->barIn + std::max(0, firstLoud - m) * barB;
                match(p, n);
                p.beatmatched = p.onBars = p.structured = true;
                p.bpm = asIs ? a->bpm : b->bpm;
                p.beats = n;
                p.cross = double(m) / (m + f);
                p.echoAt = p.cross;
                const double Pe = p.length / n;   // a beat as heard
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                j = fromPreset(Preset::Blend);   // both up to the crossover, a full EQ swap on it
                j.beats = n;
                if (clash) {   // the incoming harmony muffled until the crossover
                    j.filter = Filter::LowPassIn;
                    p.keyClash = true;
                }
                j.preset = Preset::Auto;
                p.join = j;
                return p;
            }
        }
    }

    // ---- Auto chooses
    if (autoMode) {
        int n = 0;
        for (int c : { 32, 16, 8 })
            if ((!clash || c <= t.clashBeats) && fits(c) && roomFor(c)) { n = c; break; }   // clashing keys: 8 beats at most
        if (n) {
            j = fromPreset(Preset::Blend);   // both up and a full EQ swap; a short one: a smooth bass swap
            if (clash) {
                j.filter = Filter::LowPassIn;
                p.keyClash = true;
            }
            if (n < t.blendFrom) {
                j.volume = Volume::Smooth;
                j.eq = Eq::BassCentre;
            }
            j.beats = n;
        } else if (fades) {
            j = fromPreset(quietIntro > 3 ? Preset::Rise : Preset::Fade);
            j.beatmatch = false;
            if (a->bassOut > 0.15f && b->bassIn > 0.15f && j.eq == Eq::None) j.eq = Eq::BassCentre;
            j.seconds = float(std::clamp(fade * t.fadeScale, quietIntro > 3 ? 4.0 : 3.0, clash ? 3.0 : 10.0));
            p.keyClash = clash;
        } else if (a->bpm > 0 && a->period > 0.2f && a->confidence >= 0.3f) {
            j = fromPreset(Preset::Echo);
        } else {
            j = fromPreset(Preset::Fade);
            j.seconds = float(2 * t.fadeScale);
        }
        j.preset = Preset::Auto;
    }

    bool loop = j.loopBars > 0 && a->confidence >= 0.5f && a->period > 0;
    // ---- on the beat
    if (j.beatmatch && p.beatmatchable) {
        int n = j.beats > 0 ? j.beats : 0;
        if (!n)
            for (int c : { 32, 16, 8, 4 })
                if (fits(c) && (loop || roomFor(c))) { n = c; break; }
        n = std::clamp(n - n % 4, 4, 64);   // whole bars
        while (n > 4 && !fits(n)) n -= 4;
        if (loop && !asIs(n)) loop = false;   // a loop repeats this track's own bars: not while it is stretched
        if (n >= 4 && fits(n)) {
            double end = endBeat(n);
            double start = end - n * P;
            if (loop) {   // the bars before the last full-level beat repeat; half the mix before them, half over them
                p.loopTo = lastFullBeat();
                p.loopFrom = p.loopTo - j.loopBars * 4 * P;
                start = p.loopTo - std::min(n / 2, j.loopBars * 4 * 2) * P;
            }
            // the editor's drag: whole beats earlier or later, within the track's music
            if (j.shiftBeats) {
                const double lo = std::ceil((a->start + 2 - start) / P), hi = std::floor((endA - 0.25 * n * P - start) / P);
                if (lo <= hi) start += std::clamp(double(j.shiftBeats), lo, hi) * P;
            }
            if (start >= a->start + 2 || loop) {
                p.startOut = std::max(0.0, start);
                p.offset = barsIn ? b->barIn : b->beatIn;
                p.onBars = barsOut && barsIn;
                match(p, n);   // the tempos drift apart over this mix: this track takes the next one's
                p.beatmatched = true;
                p.bpm = asIs(n) ? a->bpm : b->bpm;
                p.beats = n;
                p.echoAt = 0.5;   // a beat: the mix has an even number of them
                const double Pe = p.length / n;
                p.echoDelay = Pe >= 0.25 ? Pe : 2 * Pe;
                if (!loop) j.loopBars = 0;
                p.join = j;
                return p;
            }
            p.loopFrom = p.loopTo = -1;
        }
    }

    // ---- timed to the music
    double secs = j.seconds > 0 ? j.seconds : j.volume == Volume::Cut ? 0.2 : j.effect == Effect::Echo ? 4.0 * t.fadeScale : fades ? std::clamp(fade, 3.0, 10.0) : 4.0;
    if (j.volume == Volume::Cut) secs = std::min(secs, 1.0);
    p.length = secs;
    p.echoDelay = a->period >= 0.25f ? a->period : a->period > 0 ? 2 * a->period : 0.375;
    if (loop) {
        p.loopTo = lastFullBeat();
        p.loopFrom = p.loopTo - j.loopBars * 4 * P;
        p.startOut = p.loopTo - 0.3 * secs;
    } else if (j.effect == Effect::Echo) {
        const double cut = lastFullBeat();   // the echo repeats its last beat at full level
        p.echoAt = 0.3;
        p.startOut = cut - p.echoAt * secs;
    } else if (j.volume == Volume::Cut) {
        p.startOut = endA - 0.05 - 0.5 * secs;
    } else if (fades) {
        p.startOut = endA - secs;                  // the fade ends with its music
    } else if (quietIntro > 3) {
        p.startOut = endA - secs * 0.5;            // the quiet opening half under this track's last seconds
    } else {
        p.startOut = endA - secs * 0.75;           // an abrupt end: heard nearly to its end
    }
    {   // the editor's drag, within the track's music
        const double lo = std::max(0.0, double(a->start)), hi = std::max(lo, endA - 0.5);
        p.startOut = std::clamp(p.startOut + j.shiftSec, lo, hi);
    }
    p.offset = std::max(0.0, double(b->start) - 0.02);
    p.bpm = b->bpm;
    p.join = j;
    return p;
}

}  // namespace file1

namespace {

/** A number written so it reads back exactly (the shortest such). */
template <class T>
std::string exact(T v) {
    char b[40];
    if (v == T(std::llround(double(v))) && std::fabs(double(v)) < 1e9) return std::to_string(std::llround(double(v)));   // 1000, not 1e+03
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, double(v));
        if (T(strtod(b, nullptr)) == v) break;
    }
    return b;
}

struct Field { const char* key; char type; size_t at; };   // i int, f float, d double, b bool
#define AM_F(k, t) { #k, t, offsetof(Tune, k) }
const Field kFields[] = {
    AM_F(dropBars, 'i'), AM_F(bridgeFrac, 'f'), AM_F(exitBars, 'i'), AM_F(tempoBridgeMax, 'i'), AM_F(tempoCutLead, 'i'),
    AM_F(cutFade, 'd'), AM_F(cutEcho, 'b'), AM_F(fadeScale, 'd'), AM_F(inLevel, 'f'), AM_F(lpFrom, 'd'), AM_F(outPow, 'f'),
    AM_F(bridgeOutPow, 'f'), AM_F(noSkip, 'b'), AM_F(noCut, 'b'), AM_F(gate, 'i'), AM_F(gateLevel, 'f'), AM_F(gateDuck, 'f'),
    AM_F(gateEvery, 'i'), AM_F(clashBeats, 'i'), AM_F(clashBars, 'i'), AM_F(blendFrom, 'i'), AM_F(beatMaxSec, 'd'),
};
#undef AM_F

}  // namespace

bool ModelFile::parse(const std::string& textIn, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    ModelFile m;
    bool head = false;
    size_t at = 0;
    while (at < textIn.size()) {
        size_t eol = textIn.find('\n', at);
        if (eol == std::string::npos) eol = textIn.size();
        std::string line = textIn.substr(at, eol - at);
        at = eol + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t k0 = line.find_first_not_of(" \t");
        if (k0 == std::string::npos) continue;
        const size_t k1 = line.find_first_of(" \t", k0);
        const std::string key = line.substr(k0, k1 == std::string::npos ? std::string::npos : k1 - k0);
        std::string val = k1 == std::string::npos ? std::string() : line.substr(line.find_first_not_of(" \t", k1));
        if (!head) {
            if (key != "songate-automix-model") return fail("not an Automix model");
            m.format = atoi(val.c_str());
            if (m.format < 1 || m.format > Format) return fail("a model of format " + val + ": it needs a newer Songate");
            head = true;
            continue;
        }
        if (key == "name") m.name = val;
        else if (key == "app") m.app = val;
        else if (key == "order") m.order = val;
        else if (key == "mixan") m.mixan = atoi(val.c_str());
        else if (key == "gateOn") {
            m.gateOn = 0;
            for (size_t s0 = 0; s0 <= val.size();) {
                size_t s1 = val.find(',', s0);
                if (s1 == std::string::npos) s1 = val.size();
                const std::string kind = val.substr(s0, s1 - s0);
                if (kind == "drop") m.gateOn |= GateDrop;
                else if (kind == "structure") m.gateOn |= GateStructure;
                else if (kind == "beat") m.gateOn |= GateBeat;
                else if (kind != "none" && !kind.empty()) return fail("gateOn: " + kind + "?");
                s0 = s1 + 1;
            }
        } else {
            const Field* f = nullptr;
            for (const Field& x : kFields) if (key == x.key) f = &x;
            if (!f) return fail("an unknown number: " + key);
            char* base = reinterpret_cast<char*>(&m.tune) + f->at;
            const double v = strtod(val.c_str(), nullptr);
            switch (f->type) {
            case 'i': *reinterpret_cast<int*>(base) = int(std::lround(v)); break;
            case 'f': *reinterpret_cast<float*>(base) = float(v); break;
            case 'd': *reinterpret_cast<double*>(base) = v; break;
            case 'b': *reinterpret_cast<bool*>(base) = val == "true" || val == "1"; break;
            }
        }
    }
    if (!head) return fail("not an Automix model");
    if (m.name.empty()) return fail("a model without a name");
    if (m.mixan > mixan::Version) return fail("it needs a newer analysis (a newer Songate)");
    if (m.order != "1.0.0") return fail("it needs a newer Songate (its playlist order)");
    *this = m;
    return true;
}

std::string ModelFile::text() const {
    std::string o = "songate-automix-model " + std::to_string(format ? format : Format) + "\n";
    o += "name " + name + "\napp " + app + "\nmixan " + std::to_string(mixan) + "\norder " + order + "\ngateOn ";
    std::string g;
    if (gateOn & GateDrop) g += "drop,";
    if (gateOn & GateStructure) g += "structure,";
    if (gateOn & GateBeat) g += "beat,";
    o += g.empty() ? "none" : g.substr(0, g.size() - 1);
    o += '\n';
    for (const Field& f : kFields) {
        const char* base = reinterpret_cast<const char*>(&tune) + f.at;
        std::string v;
        switch (f.type) {
        case 'i': v = std::to_string(*reinterpret_cast<const int*>(base)); break;
        case 'f': v = exact(*reinterpret_cast<const float*>(base)); break;
        case 'd': v = exact(*reinterpret_cast<const double*>(base)); break;
        case 'b': v = *reinterpret_cast<const bool*>(base) ? "true" : "false"; break;
        }
        o += std::string(f.key) + " " + v + "\n";
    }
    return o;
}

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want, const ModelFile& m) {
    const Tune& t = m.tune;
    Plan p = file1::plan(a, dur, b, fallback, want, t);
    p.inLevel = t.inLevel;
    p.lpFrom = t.lpFrom;
    p.outPow = !p.beatmatched && p.drop && t.bridgeOutPow > 0 ? t.bridgeOutPow : t.outPow;   // a bridge: drop, not beatmatched
    p.cutFade = t.cutFade;
    const unsigned kind = !p.beatmatched || p.bpm <= 0 ? 0u : p.drop ? ModelFile::GateDrop : p.structured ? ModelFile::GateStructure
                                                                                                             : ModelFile::GateBeat;
    p.gate = kind & m.gateOn ? t.gate : 0;
    p.gateLevel = t.gateLevel;
    p.gateDuck = t.gateDuck;
    p.gateEvery = t.gateEvery;
    return p;
}

// ==== the models

const char* name(Model m) {
    switch (m) {
    case Model::V1_0_0: return "Automix V1.0.0";
    case Model::V1_1_0: return "Automix V1.1.0";
    default: return "Automix";
    }
}

bool modelFromName(const std::string& s, Model& out) {
    for (int i = 0; i < int(Model::Count); i++)
        if (s == name(Model(i))) { out = Model(i); return true; }
    return false;
}

Plan plan(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const Join* want, Model m) {
    Plan p;
    switch (m) {
    case Model::V1_0_0: p = v1_0_0::plan(a, dur, b, fallback, want); break;
    default: m = Model::V1_1_0; [[fallthrough]];
    case Model::V1_1_0: p = v1_1_0::plan(a, dur, b, fallback, want); break;
    }
    p.model = m;
    return p;
}

double joinScore(const mixan::Info* a, const mixan::Info* b, Model m) {
    switch (m) {
    default:   // (V1.1.0 orders as V1.0.0: the order it was trained on)
    case Model::V1_0_0: return v1_0_0::joinScore(a, b);
    }
}

std::vector<int> order(const std::vector<const mixan::Info*>& tracks, int first, Model m) {
    switch (m) {
    default:   // (V1.1.0 too)
    case Model::V1_0_0: return v1_0_0::order(tracks, first);
    }
}

ModelFile builtinFile(Model m) {
    ModelFile f;
    f.format = 1;
    f.name = name(m);
    f.mixan = 4;
    if (m == Model::V1_1_0) f.tune = v1_1_0::kTune;   // no gate (gateOn none)
    return f;   // (V1.0.0's plan is not the tunable planner's: its file would differ in its cuts and skips)
}

double speedAt(const Plan& p, double tau) {
    if (p.speed == 1 || tau <= 0) return 1;
    if (p.easeIn <= 0 || tau >= p.easeIn) return p.speed;
    return 1 + (p.speed - 1) * tau / p.easeIn;
}

double inTime(const Plan& p, double tau) {
    return p.speed == 1 ? tau : tau / p.speed;   // this track is at its join tempo by then: its seconds go by faster or slower
}

void Stretcher::init() {
    win_.resize(N);
    for (int i = 0; i < N; i++) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * Pi * i / N));   // periodic: half-overlapped, they add up to 1
    in_.assign(size_t(16384 + N + 2 * Search) * 2, 0.f);
    ola_.assign(size_t(Hs) * 2, 0.f);
    tail_.assign(size_t(Hs) * 2, 0.f);
    scratch_.assign(2048 * 2, 0);
}

void Stretcher::begin(const Plan& p, int rate, double delay) {
    plan_ = p;
    rate_ = rate;
    delay_ = delay;
    primed_ = false;
    on_ = p.speed != 1;
    ended_ = false;
    base_ = have_ = 0;
    prev_ = -Hs;
    next_ = 0;
    ideal_ = 0;
    outFrames_ = 0;
    outAt_ = Hs;
    std::fill(tail_.begin(), tail_.end(), 0.f);
}

void OutLoop::init(int rate) {
    rate_ = rate;
    buf_.assign(size_t(rate) * 8 * 2, 0);
}

void OutLoop::begin(const Plan& p, long long posFrames) {
    pos_ = posFrames;
    loopAt_ = 0;
    to_ = 0;
    if (p.loopTo <= 0 || p.loopFrom < 0) return;
    const long long from = std::llround(p.loopFrom * rate_), to = std::llround(p.loopTo * rate_);
    if (to - from < rate_ / 10 || size_t(to - from) * 2 > buf_.size() || posFrames > from) return;   // too short, too long, or already past
    from_ = from;
    to_ = to;
    len_ = to - from;
}

}  // namespace automix
