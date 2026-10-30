/*
 * Gatefold NX · mixan.cpp
 */
#include "mixan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mixan {

namespace {

constexpr int N = 1024, H = 128;   // the spectrum's size and the hop, at the analysis rate
constexpr int N4 = 4096;           // the bass's spectrum (v4): 2.7 Hz bins
constexpr double Pi = 3.14159265358979323846;
// The onset strength peaks a little after a beat lands: the window has to take the new sound in first.
// Measured on tracks made with beats at known times (tools/preview/mixtest).
constexpr double OnsetLag = 0.027;   // 27 ms on 96 tracks, spread 23-30 ms

/** Moving mean of a power curve over w frames, centred; returned in dB. */
std::vector<float> smoothDb(const std::vector<float>& db, int w) {
    const int n = int(db.size());
    std::vector<double> pw(size_t(n) + 1, 0);
    for (int i = 0; i < n; i++) pw[size_t(i) + 1] = pw[size_t(i)] + std::pow(10.0, db[size_t(i)] / 10);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const int a = std::max(0, i - w / 2), b = std::min(n, i + w / 2 + 1);
        out[size_t(i)] = float(10 * std::log10((pw[size_t(b)] - pw[size_t(a)]) / (b - a) + 1e-12));
    }
    return out;
}

/** In-place radix-2 FFT of n points whose input was already placed in bit-reversed order. */
void butterflies(std::vector<float>& re, std::vector<float>& im, int n) {
    for (int len = 2; len <= n; len <<= 1) {
        const double a = -2 * Pi / len;
        const float wr = float(std::cos(a)), wi = float(std::sin(a));
        for (int s = 0; s < n; s += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const size_t u = size_t(s + k), v = size_t(s + k + len / 2);
                const float tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] += tr;
                im[u] += ti;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

/**
 * Each spectrum bin's pitch classes over [fLo, fHi) (whole octaves): its lower class, its share there (the rest to the next
 * one up), its weight (1/f: every semitone the same however many bins it spans); and each class's total weight (white
 * noise's chroma), which the chroma is divided by.
 */
void pitchMap(int n, double rate, double fLo, double fHi, std::vector<int8_t>& pc, std::vector<float>& share, std::vector<float>& weight, float* norm) {
    pc.assign(size_t(n) / 2 + 1, -1);
    share.assign(size_t(n) / 2 + 1, 0.f);
    weight.assign(size_t(n) / 2 + 1, 0.f);
    for (int k = 0; k < 12; k++) norm[k] = 0;
    for (int k = 1; k <= n / 2; k++) {
        const double f = k * rate / n;
        if (f < fLo || f >= fHi) continue;
        const double s = 12 * std::log2(f / 440) + 9, lo = std::floor(s);
        const int c = ((int(lo) % 12) + 12) % 12;
        pc[size_t(k)] = int8_t(c);
        share[size_t(k)] = float(1 - (s - lo));
        weight[size_t(k)] = float(fLo / f);
        norm[c] += share[size_t(k)] * weight[size_t(k)];
        norm[(c + 1) % 12] += (1 - share[size_t(k)]) * weight[size_t(k)];
    }
    float mean = 0;
    for (int k = 0; k < 12; k++) mean += norm[k] / 12;
    for (int k = 0; k < 12; k++) norm[k] = norm[k] > 0 ? norm[k] / mean : 1;
}

/** Linear interpolation into a curve at a fractional index. */
float at(const std::vector<float>& v, double x) {
    const int i = int(x);
    if (i < 0 || i + 1 >= int(v.size())) return 0;
    const float f = float(x - i);
    return v[size_t(i)] * (1 - f) + v[size_t(i) + 1] * f;
}

}  // namespace

Analyzer::Analyzer() {
    win_.resize(N);
    for (int i = 0; i < N; i++) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * Pi * i / N));
    re_.resize(N);
    im_.resize(N);
    rev_.resize(N);
    int bits = 0;
    while ((1 << bits) < N) bits++;
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev_[size_t(i)] = r;
    }
    win4_.resize(N4);
    for (int i = 0; i < N4; i++) win4_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * Pi * i / N4));
    re4_.resize(N4);
    im4_.resize(N4);
    rev4_.resize(N4);
    bits = 0;
    while ((1 << bits) < N4) bits++;
    for (int i = 0; i < N4; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev4_[size_t(i)] = r;
    }
}

void Analyzer::begin(int rate, int channels) {
    rate_ = std::max(8000, rate);
    ch_ = std::max(1, channels);
    decim_ = std::max(1, int(std::lround(rate_ / 11025.0)));
    ar_ = double(rate_) / decim_;
    acc_ = 0;
    accN_ = 0;
    buf_.assign(N, 0.f);
    bufAt_ = fill_ = sinceHop_ = 0;
    prevLog_.assign(N / 2 + 1, 0.f);
    flux_.clear();
    db_.clear();
    bass_.clear();
    low_.clear();
    snare_.clear();
    centroid_.clear();
    hf_.clear();
    chroma_.clear();
    chromaN_ = 0;
    for (float& c : chromaAcc_) c = 0;
    const size_t guess = size_t(ar_ / H * 600);   // ten minutes before the first reallocation
    flux_.reserve(guess);
    db_.reserve(guess);
    bass_.reserve(guess);
    low_.reserve(guess);
    snare_.reserve(guess);
    chroma_.reserve(guess * 3);
    // each bin over three whole octaves, G#3 to G#6 (207.7 Hz-1.66 kHz), shared between the two pitch classes it lies
    // between (A = 9), by how near it is to each, and weighted by 1/f, so every semitone weighs the same however many bins
    // it spans; below 200 Hz a bin (10.8 Hz wide here) is wider than a semitone and would smear the key's root.
    // v4 (Automix v3, step 5): it was 200 Hz-2.5 kHz, not whole octaves, bins unweighted: some pitch classes got twice
    // the weight of others (E 0.69 of the mean, D 1.32: the chroma of white noise), and keys leaned to D, B minor and C
    // (10A, 10B, 5A, 4B were 57 % of the FLACs; Enter Sandman, Master of Puppets and Paranoid read B minor)
    const double fLo = 440 * std::pow(2.0, -13.0 / 12);   // G#3
    pitchMap(N, ar_, fLo, fLo * 8, pcOf_, pcW_, binW_, pcNorm_);
    // the bass (v4): G#1 to G#3 in a 4096-point spectrum; only in the PC tools for now (MIXAN_BASSKEY)
    bassKey_ = 0;
#ifndef __SWITCH__
    if (const char* e = getenv("MIXAN_BASSKEY")) bassKey_ = std::max(0.0, atof(e));
#endif
    buf4_.assign(N4, 0.f);
    buf4At_ = hops8_ = 0;
    bassChroma_.clear();
    bassChroma_.reserve(guess * 3 / 2);
    pitchMap(N4, ar_, fLo / 4, fLo, bpcOf_, bpcW_, bbinW_, bpcNorm_);
    hopPow_ = 0;
}

/** Every 8 hops: the bass's pitch classes, from the last N4 samples (zeros until there are that many). */
void Analyzer::bassFrame() {
    if (fill_ < N4) {
        bassChroma_.insert(bassChroma_.end(), 12, 0.f);
        return;
    }
    for (int i = 0; i < N4; i++) {
        re4_[size_t(rev4_[size_t(i)])] = buf4_[size_t((buf4At_ + i) % N4)] * win4_[size_t(i)];
        im4_[size_t(rev4_[size_t(i)])] = 0;
    }
    butterflies(re4_, im4_, N4);
    float acc[12] = {};
    for (int k = 1; k <= N4 / 2; k++) {
        if (bpcOf_[size_t(k)] < 0) continue;
        const float m = 2 * std::sqrt(re4_[size_t(k)] * re4_[size_t(k)] + im4_[size_t(k)] * im4_[size_t(k)]) / N4;
        const int pc = bpcOf_[size_t(k)];
        const float c = std::sqrt(m) * bbinW_[size_t(k)], w = bpcW_[size_t(k)];
        acc[pc] += c * w;
        acc[(pc + 1) % 12] += c * (1 - w);
    }
    bassChroma_.insert(bassChroma_.end(), acc, acc + 12);
}

void Analyzer::push(const float* x, int frames) {
    const float inv = 1.f / float(ch_ * decim_);
    for (int i = 0; i < frames; i++) {
        float m = 0;
        for (int c = 0; c < ch_; c++) m += x[size_t(i) * size_t(ch_) + size_t(c)];
        acc_ += m;
        if (++accN_ < decim_) continue;
        const float s = acc_ * inv;   // a box filter before dropping samples: enough for onsets and levels
        acc_ = 0;
        accN_ = 0;
        buf_[size_t(bufAt_)] = s;
        bufAt_ = (bufAt_ + 1) % N;
        buf4_[size_t(buf4At_)] = s;
        buf4At_ = (buf4At_ + 1) % N4;
        fill_++;
        hopPow_ += double(s) * s;
        if (++sinceHop_ == H) {
            sinceHop_ = 0;
            frame();
        }
    }
}

/** One hop: its level, and once the window is full, the spectrum's onset strength and bass share. */
void Analyzer::frame() {
    if (bassKey_ > 0 && ++hops8_ == 8) {
        hops8_ = 0;
        bassFrame();
    }
    db_.push_back(float(10 * std::log10(hopPow_ / H + 1e-12)));
    hopPow_ = 0;
    if (fill_ < N) {
        flux_.push_back(0);
        bass_.push_back(0);
        low_.push_back(0);
        snare_.push_back(0);
        centroid_.push_back(0);
        hf_.push_back(0);
        if (++chromaN_ == 4) { chroma_.insert(chroma_.end(), 12, 0.f); chromaN_ = 0; }
        return;
    }
    for (int i = 0; i < N; i++) {
        re_[size_t(rev_[size_t(i)])] = buf_[size_t((bufAt_ + i) % N)] * win_[size_t(i)];
        im_[size_t(rev_[size_t(i)])] = 0;
    }
    for (int len = 2; len <= N; len <<= 1) {
        const double a = -2 * Pi / len;
        const float wr = float(std::cos(a)), wi = float(std::sin(a));
        for (int s = 0; s < N; s += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const size_t u = size_t(s + k), v = size_t(s + k + len / 2);
                const float tr = re_[v] * cr - im_[v] * ci, ti = re_[v] * ci + im_[v] * cr;
                re_[v] = re_[u] - tr;
                im_[v] = im_[u] - ti;
                re_[u] += tr;
                im_[u] += ti;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    const int top = std::min(N / 2, int(5000 / (ar_ / N)));   // onsets: up to 5 kHz
    const int low = std::max(1, int(150 / (ar_ / N)));       // bass: below 150 Hz
    double flux = 0, all = 0, bass = 0, kick = 0, snare = 0, cen = 0, hf = 0;
    const int snLo = int(1500 / (ar_ / N)), hfLo = int(3000 / (ar_ / N));
    for (int k = 1; k <= N / 2; k++) {
        const float m = 2 * std::sqrt(re_[size_t(k)] * re_[size_t(k)] + im_[size_t(k)] * im_[size_t(k)]) / N;
        const double p = double(m) * m;
        all += p;
        cen += p * k;   // (Mood: the centroid and the top; nothing Automix reads)
        if (k >= hfLo) hf += p;
        if (k <= low) bass += p;
        if (k <= top) {
            const float l = std::log1p(1000 * m), up = std::max(0.f, l - prevLog_[size_t(k)]);
            flux += up;
            if (k <= low) kick += up;
            if (k >= snLo) snare += up;
            prevLog_[size_t(k)] = l;
        }
        if (pcOf_[size_t(k)] >= 0) {   // (compressed: a few loud partials must not decide the key)
            const int pc = pcOf_[size_t(k)];
            const float c = std::sqrt(m) * binW_[size_t(k)], w = pcW_[size_t(k)];
            chromaAcc_[pc] += c * w;
            chromaAcc_[(pc + 1) % 12] += c * (1 - w);
        }
    }
    flux_.push_back(float(flux));
    bass_.push_back(all > 1e-12 ? float(bass / all) : 0.f);
    low_.push_back(float(kick));
    snare_.push_back(float(snare));
    centroid_.push_back(all > 1e-12 ? float(cen / all * (ar_ / N)) : 0.f);
    hf_.push_back(all > 1e-12 ? float(hf / all) : 0.f);
    if (++chromaN_ == 4) {
        chroma_.insert(chroma_.end(), chromaAcc_, chromaAcc_ + 12);
        for (float& c : chromaAcc_) c = 0;
        chromaN_ = 0;
    }
}

Info Analyzer::finish() {
    Info r = finishMix();
    r.mood = finishMood(r);
    return r;
}

/**
 * Mood's features (mixan.hpp, Mood): over the music (Info::start..end), from what each hop measured. Independent of the
 * beat: an ambient or classical track has its mood too (Automix's analysis stops early for it).
 */
Mood Analyzer::finishMood(const Info& mix) const {
    Mood m;
    const int n = int(db_.size());
    const double fr = ar_ / H;
    if (n < int(fr * 5) || mix.level <= -99) return m;
    const int a = std::clamp(int(mix.start * fr), 0, n - 1), b = std::clamp(int(mix.end * fr), a, n - 1);
    if (b - a < int(fr * 5)) return m;
    m.loud = mix.level;
    // the loudness a second at a time: its spread, and how much of it is quiet
    const std::vector<float> s1 = smoothDb(db_, std::max(1, int(fr)));
    std::vector<float> lv;
    for (int j = a; j <= b; j += std::max(1, int(fr / 4))) lv.push_back(s1[size_t(j)]);
    std::vector<float> sorted = lv;
    std::sort(sorted.begin(), sorted.end());
    m.dyn = sorted[sorted.size() * 9 / 10] - sorted[sorted.size() / 10];
    int quiet = 0;
    for (float v : lv) quiet += v < mix.level - 6 ? 1 : 0;
    m.calm = float(quiet) / float(lv.size());
    // the spectrum: weighted by each hop's power, so the quiet bits do not decide it
    double w = 0, cen = 0, hf = 0, bass = 0, flux = 0, kick = 0, snare = 0;
    for (int j = a; j <= b; j++) {
        const double p = std::pow(10.0, db_[size_t(j)] / 10);
        w += p;
        cen += centroid_[size_t(j)] * p;
        hf += hf_[size_t(j)] * p;
        bass += bass_[size_t(j)] * p;
        flux += flux_[size_t(j)];
        kick += low_[size_t(j)];
        snare += snare_[size_t(j)];
    }
    const int hops = b - a + 1;
    if (w > 0) { m.bright = float(cen / w); m.hf = float(hf / w); m.bass = float(bass / w); }
    m.flux = float(flux / hops);
    m.perc = flux > 0 ? float((kick + snare) / flux) : 0.f;
    // onsets: the flux's local peaks over its running median (a second each side) by half its spread
    int peaks = 0;
    {
        const int half = std::max(2, int(fr * 0.5));
        std::vector<float> win;
        for (int j = a + 1; j < b; j++) {
            const float v = flux_[size_t(j)];
            if (v <= flux_[size_t(j - 1)] || v < flux_[size_t(j + 1)]) continue;
            win.assign(flux_.begin() + std::max(a, j - half), flux_.begin() + std::min(b, j + half) + 1);
            std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
            const float med = win[win.size() / 2];
            if (v > med * 1.5f + 1e-3f) { peaks++; j += std::max(1, int(fr * 0.05)); }   // (50 ms apart at least)
        }
    }
    m.onsets = float(peaks / ((b - a) / fr));
    // the mode: the whole track's chroma against Krumhansl and Kessler's major and minor profiles, every tonic
    {
        float c[12] = {};
        const int nc = int(chroma_.size() / 12);
        const int c0 = std::max(0, int((a - 2.5) / 4)), c1 = std::min(nc, int((b - 2.5) / 4));
        for (int i = c0; i < c1; i++)
            for (int k = 0; k < 12; k++) c[k] += chroma_[size_t(i) * 12 + size_t(k)];
        for (int k = 0; k < 12; k++) c[k] /= pcNorm_[k];
        static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        auto corr = [&](const double* prof, int tonic) {
            double xm = 0, pm = 0;
            for (int i = 0; i < 12; i++) { xm += c[i] / 12.0; pm += prof[i] / 12; }
            double num = 0, dx = 0, dp = 0;
            for (int i = 0; i < 12; i++) {
                const double x = c[(tonic + i) % 12] - xm, y = prof[i] - pm;
                num += x * y;
                dx += x * x;
                dp += y * y;
            }
            return dx > 0 ? num / std::sqrt(dx * dp) : 0.0;
        };
        double bMaj = -1, bMin = -1;
        for (int t = 0; t < 12; t++) { bMaj = std::max(bMaj, corr(maj, t)); bMin = std::max(bMin, corr(mnr, t)); }
        m.minor = float(bMin - bMaj);
    }
    // the pulse: each onset envelope's normalised autocorrelation, its best lag a quarter of a second to a second
    auto pulseOf = [&](const std::vector<float>& v) {
        const int len = b - a + 1;
        double mean = 0;
        for (int j = a; j <= b; j++) mean += v[size_t(j)];
        mean /= len;
        double ac0 = 0;
        for (int j = a; j <= b; j++) ac0 += (v[size_t(j)] - mean) * (v[size_t(j)] - mean);
        if (ac0 <= 0) return 0.f;
        ac0 /= len;
        double best = 0;
        for (int lag = std::max(1, int(fr * 0.25)); lag <= int(fr * 1.0) && lag < len / 2; lag++) {
            double s = 0;
            for (int j = a; j + lag <= b; j++) s += (v[size_t(j)] - mean) * (v[size_t(j + lag)] - mean);
            best = std::max(best, s / (len - lag) / ac0);
        }
        return float(std::clamp(best, 0.0, 1.0));
    };
    m.pulse = pulseOf(flux_);
    m.kickPulse = pulseOf(low_);
    m.bpm = mix.bpm;
    m.beat = mix.confidence;
    m.valid = true;
    return m;
}

Info Analyzer::finishMix() {
    Info r;
    const int n = int(db_.size());
    const double fr = ar_ / H;                       // hops a second
    auto t = [&](double j) { return (j + 1) * H / ar_; };   // the end of hop j, seconds
    r.seconds = float(double(fill_) / ar_);
    if (n < int(fr * 5)) return r;   // under five seconds: nothing to mix

    // ---- where the music is
    const std::vector<float> s1 = smoothDb(db_, std::max(1, int(fr))), s02 = smoothDb(db_, std::max(1, int(fr * 0.2)));
    std::vector<float> loud;
    for (float v : s1) if (v > -70) loud.push_back(v);
    if (loud.size() < size_t(fr * 3)) return r;   // silence
    std::nth_element(loud.begin(), loud.begin() + loud.size() / 2, loud.end());
    const float body = loud[loud.size() / 2];
    r.level = body;
    const float floor = body - 30;
    int a = 0, b = n - 1;
    while (a < n && s02[size_t(a)] < floor) a++;
    while (b > a && s02[size_t(b)] < floor) b--;
    r.start = float(std::max(0.0, t(a) - 1.0 / fr));
    r.end = float(t(b));
    int f = b;
    while (f > a && s1[size_t(f)] < body - 5) f--;
    r.fadeStart = r.end - float(t(f)) < 2 ? r.end : float(t(f));
    int ie = a;
    while (ie < b && s1[size_t(ie)] < body - 5) ie++;
    r.introEnd = float(t(ie));
    auto bassShare = [&](double from, double to) {
        double sb = 0, sp = 0;
        for (int j = std::max(0, int(from * fr)); j < std::min(n, int(to * fr)); j++) {
            const double p = std::pow(10.0, db_[size_t(j)] / 10);
            sb += bass_[size_t(j)] * p;
            sp += p;
        }
        return sp > 0 ? float(sb / sp) : 0.f;
    };
    r.bassIn = bassShare(r.start, r.start + 15);
    r.bassOut = bassShare(std::max(double(r.start), double(r.end) - 15), r.end);
    r.valid = true;

    // ---- the onset strength over the music: minus its local mean, rectified, in units of its spread
    const int m = b - a + 1;
    std::vector<float> o(static_cast<size_t>(m));
    {
        const int w = std::max(1, int(fr * 0.5));
        std::vector<double> c(size_t(m) + 1, 0);
        for (int i = 0; i < m; i++) c[size_t(i) + 1] = c[size_t(i)] + flux_[size_t(a + i)];
        double sum = 0, sq = 0;
        for (int i = 0; i < m; i++) {
            const int lo = std::max(0, i - w), hi = std::min(m, i + w + 1);
            const float v = std::max(0.f, flux_[size_t(a + i)] - float((c[size_t(hi)] - c[size_t(lo)]) / (hi - lo)));
            o[size_t(i)] = v;
            sum += v;
            sq += double(v) * v;
        }
        const double mean = sum / m, sd = std::sqrt(std::max(1e-12, sq / m - mean * mean));
        for (float& v : o) v = float(v / sd);
    }

    // ---- the tempo: autocorrelation, with its harmonics, weighted towards 120 BPM
    const double minLag = fr * 60 / 200, maxLag = fr * 60 / 60;
    const int maxAc = std::min(m - 1, int(maxLag * 4) + 2);
    if (maxAc < int(maxLag)) return r;
    std::vector<float> ac(size_t(maxAc) + 1, 0);
    for (int l = 0; l <= maxAc; l++) {
        double s = 0;
        for (int i = l; i < m; i++) s += double(o[size_t(i)]) * o[size_t(i - l)];
        ac[size_t(l)] = float(s / (m - l));
    }
    if (ac[0] <= 0) return r;
    for (float& v : ac) v /= ac[0];
    auto score = [&](double lag) {
        double s = 0;
        for (int h = 1; h <= 4; h++) s += at(ac, lag * h) / h;
        const double bpm = fr * 60 / lag, oct = std::log2(bpm / 120);
        return s * std::exp(-0.5 * oct * oct);
    };
    double best = 0, bestLag = 0;
    std::vector<float> scores;
    for (double l = minLag; l <= maxLag; l += 0.1) {
        const double s = score(l);
        scores.push_back(float(s));
        if (s > best) { best = s; bestLag = l; }
    }
    if (best <= 0) return r;
    std::vector<float> sorted = scores;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double med = std::max(1e-6f, sorted[sorted.size() / 2]);
    const float tempoConf = float(std::clamp((best / med - 1.5) / 3.0, 0.0, 1.0));

    // ---- the beats: dynamic programming over the onset strength at that period
    const double p = bestLag;
    std::vector<float> sc(static_cast<size_t>(m));
    std::vector<int> from(size_t(m), -1);
    for (int i = 0; i < m; i++) {
        double bestPrev = 0;
        int arg = -1;
        for (int k = i - int(std::lround(2 * p)); k <= i - int(std::lround(p / 2)); k++) {
            if (k < 0) continue;
            const double lg = std::log((i - k) / p);
            const double v = sc[size_t(k)] - 100 * lg * lg;
            if (arg < 0 || v > bestPrev) { bestPrev = v; arg = k; }
        }
        sc[size_t(i)] = o[size_t(i)] + float(arg >= 0 ? std::max(0.0, bestPrev) : 0.0);
        from[size_t(i)] = arg >= 0 && bestPrev > 0 ? arg : -1;
    }
    int end = m - 1;
    for (int i = std::max(0, m - int(p)); i < m; i++) if (sc[size_t(i)] > sc[size_t(end)]) end = i;
    std::vector<double> beats;   // seconds
    for (int i = end; i >= 0; i = from[size_t(i)]) beats.push_back(t(a + i) - OnsetLag);
    std::reverse(beats.begin(), beats.end());
    if (beats.size() < 8) return r;

    // ---- the grids: the period over all the beats, a phase at each end, and how well the beats sit on them
    const double p0 = p / fr;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (double bt : beats) {
        const double k = std::round((bt - beats[0]) / p0);
        sx += k; sy += bt; sxx += k * k; sxy += k * bt;
    }
    const double nb = double(beats.size()), den = nb * sxx - sx * sx;
    const double period = den > 0 ? (nb * sxy - sx * sy) / den : p0;
    // A grid at each end: laid over the onset strength at every phase, for periods within 1 % of that one
    // (0.01 % apart: the tempo's own estimate is 0.2 % coarse, 70 ms of drift over a minute), in the first and
    // the last minute separately (live music's tempo wanders). Kept: where the beats are strongest, and how much
    // stronger than at the average phase (contrast; 1: no beat at all). Fitting the tracked beats instead was
    // thrown by the off-beat hi-hats busy drums have.
    auto scan = [&](double from, double to, double P, int perHop, double& ph, double& contrast) {
        const int steps = std::max(8, int(P * fr * perHop));
        double bestV = -1, sum = 0;
        int bs = 0, beatsIn = 0;
        std::vector<double> sc2(static_cast<size_t>(steps));
        for (int s = 0; s < steps; s++) {
            const double phi = s * P / steps;
            double v = 0;
            int k = 0;
            for (double T = phi + std::ceil((from - phi) / P) * P; T <= to; T += P, k++) v += at(o, (T + OnsetLag) * fr - 1 - a);
            sc2[size_t(s)] = k ? v / k : 0;
            sum += sc2[size_t(s)];
            beatsIn = std::max(beatsIn, k);
            if (sc2[size_t(s)] > bestV) { bestV = sc2[size_t(s)]; bs = s; }
        }
        if (beatsIn < 4 || sum <= 0) return false;
        const double l = sc2[size_t((bs + steps - 1) % steps)], c = sc2[size_t(bs)], rr = sc2[size_t((bs + 1) % steps)];
        const double d = (l - 2 * c + rr) < 0 ? 0.5 * (l - rr) / (l - 2 * c + rr) : 0;   // parabolic, between steps
        ph = (bs + d) * P / steps;
        contrast = c / (sum / steps);
        return true;
    };
    auto grid = [&](double from, double to, double& ph, double& P, double& contrast) {
        double bestC = -1;
        for (int j = -100; j <= 100; j++) {
            const double Pj = period * (1 + j * 1e-4);
            double phj, cj;
            if (scan(from, to, Pj, 2, phj, cj) && cj > bestC) { bestC = cj; P = Pj; }
        }
        return bestC > 0 && scan(from, to, P, 4, ph, contrast);
    };
    double phIn = 0, phOut = 0, pIn = period, pOut = period, cIn = 1, cOut = 1;
    const bool okIn = grid(r.start, r.start + 60, phIn, pIn, cIn), okOut = grid(r.end - 60, r.end, phOut, pOut, cOut);
    if (!okIn || !okOut) return r;
    auto gridConf = [&](double contrast) { return float(std::clamp((contrast - 1.5) / 1.5, 0.0, 1.0)); };
    r.period = float(pOut);
    r.periodIn = float(pIn);
    r.bpm = float(60 / period);
    // the first grid beat at or after the start, the last at or before the end
    r.beatIn = phIn + std::ceil((r.start - phIn) / pIn - 1e-9) * pIn;
    r.beatOut = phOut + std::floor((r.end - phOut) / pOut + 1e-9) * pOut;
    r.confidence = std::min(tempoConf, std::min(gridConf(cIn), gridConf(cOut)));

    // ---- downbeats (v2): beat 1 of each bar. Chords change on it more than on the other beats, and the kick lands on 1
    // (and 3): each beat of an end's grid is scored by how much the harmony moved across it (the chroma of the beat
    // before against the beat after) and how hard the kick hit, then the four places in the bar are compared
    auto chromaOver = [&](double t0, double t1, float* out) {
        for (int k = 0; k < 12; k++) out[k] = 0;
        const int nc = int(chroma_.size() / 12);
        const int c0 = std::max(0, int(std::ceil(((t0 + 0.0464) * fr - 2.5) / 4))), c1 = std::min(nc, int(((t1 + 0.0464) * fr - 2.5) / 4));
        for (int c = c0; c < c1; c++)
            for (int k = 0; k < 12; k++) out[k] += chroma_[size_t(c) * 12 + size_t(k)];
        for (int k = 0; k < 12; k++) out[k] /= pcNorm_[k];
    };
    auto onsetAt = [&](const std::vector<float>& v, double t) {
        const int j = int(std::lround((t + OnsetLag) * fr - 1));
        float m = 0;
        for (int i = std::max(0, j - 1); i <= std::min(n - 1, j + 1); i++) m = std::max(m, v[size_t(i)]);
        return m;
    };
    auto downbeat = [&](double from, double to, double ph, double P, double& db, float& sure) {
        std::vector<double> hc, kk, sn;
        std::vector<int> ix;
        float x[12], y[12];
        for (double T = ph + std::ceil((from + P - ph) / P) * P; T <= to - P; T += P) {
            chromaOver(T - P, T, x);
            chromaOver(T, T + P, y);
            double dot = 0, nx = 0, ny = 0;
            for (int k = 0; k < 12; k++) { dot += double(x[k]) * y[k]; nx += double(x[k]) * x[k]; ny += double(y[k]) * y[k]; }
            hc.push_back(nx > 0 && ny > 0 ? 1 - dot / std::sqrt(nx * ny) : 0);
            kk.push_back(onsetAt(low_, T));
            sn.push_back(onsetAt(snare_, T));
            ix.push_back(int(std::lround((T - ph) / P)));
        }
        if (hc.size() < 16) return false;
        auto z = [](std::vector<double>& v) {
            double m = 0, s = 0;
            for (double a : v) m += a;
            m /= double(v.size());
            for (double a : v) s += (a - m) * (a - m);
            s = std::sqrt(s / double(v.size())) + 1e-9;
            for (double& a : v) a = (a - m) / s;
        };
        z(hc);
        z(kk);
        z(sn);
        double score[4] = {}, cnt[4] = {};
        for (size_t i = 0; i < hc.size(); i++) {
            const int q = ((ix[i] % 4) + 4) % 4;
            score[q] += hc[i] + 0.5 * kk[i] - 0.5 * sn[i];   // the snare: on 2 and 4
            cnt[q]++;
        }
        int best = 0;
        for (int q = 0; q < 4; q++) score[q] = cnt[q] > 0 ? score[q] / cnt[q] : -1e9;
        for (int q = 1; q < 4; q++) if (score[q] > score[best]) best = q;
        double second = -1e9;
        for (int q = 0; q < 4; q++) if (q != best) second = std::max(second, score[q]);
        db = ph + best * P;
        sure = float(std::clamp((score[best] - second) / 0.5, 0.0, 1.0));
        return true;
    };
    double dbIn = 0, dbOut = 0;
    if (downbeat(r.start, r.start + 60, phIn, pIn, dbIn, r.barSureIn) && downbeat(r.end - 60, r.end, phOut, pOut, dbOut, r.barSureOut)) {
        const double barIn = 4 * pIn, barOut = 4 * pOut;
        r.barIn = dbIn + std::ceil((r.start - dbIn) / barIn - 1e-9) * barIn;
        r.barOut = dbOut + std::floor((r.end - dbOut) / barOut + 1e-9) * barOut;
        // each bar's loudness from barIn on (the whole track's period), and where the 8-bar phrases start: the bar
        // offset where the loudness changes most often (sections begin on phrase boundaries)
        const double bar = 4 * period;
        std::vector<int> lv;
        std::vector<double> bassDb;   // each bar's bass (dB), for the bass digits
        for (double t = r.barIn; t + bar <= r.end + 0.01 && lv.size() < 4000; t += bar) {
            double pw = 0, pb = 0;
            int k = 0;
            for (int j = std::max(0, int(t * fr)); j < std::min(n, int((t + bar) * fr)); j++, k++) {
                const double p1 = std::pow(10.0, db_[size_t(j)] / 10);
                pw += p1;
                pb += p1 * bass_[size_t(j)];
            }
            const double dbv = k ? 10 * std::log10(pw / k + 1e-12) : -99;
            lv.push_back(std::clamp(int(std::lround((dbv - body + 12) / 1.5)), 0, 9));
            bassDb.push_back(k ? 10 * std::log10(pb / k + 1e-12) : -99);
        }
        r.bars.clear();
        for (int v : lv) r.bars += char('0' + v);
        // the body's bass: the median of the loud bars' (within 3 dB of the body's level)
        std::vector<double> loudBass;
        for (size_t i = 0; i < lv.size(); i++) if (lv[i] >= 6) loudBass.push_back(bassDb[i]);
        if (!loudBass.empty()) {
            std::nth_element(loudBass.begin(), loudBass.begin() + loudBass.size() / 2, loudBass.end());
            const double bodyBass = loudBass[loudBass.size() / 2];
            r.bass.clear();
            for (double v : bassDb) r.bass += char('0' + std::clamp(int(std::lround((v - bodyBass + 18) / 2)), 0, 9));
            // the drop: the first four bars with full bass after bars without it; its exact beat on the intro's grid (within
            // two bars of where the bars say), where the bass jumps most from the two beats before to the two after. Bass
            // comes in on beat 1, so this is a downbeat even where the downbeat detector was unsure (Calabria, Heads Will Roll)
            // the first beat where the bass jumps by 8 dB or more and stays full (each of the four bars after it within 6 dB
            // of the body's bass), up to two bars past the first four full bars; a build-up rises gradually and is
            // passed over (Calabria: the drop after a bar of silence at bar 30, not where the bass grows from bar 25)
            int full = -1;
            for (int i = 0; i + 3 < int(r.bass.size()) && i <= 64 && full < 0; i++)
                if (std::min({ r.bass[size_t(i)], r.bass[size_t(i) + 1], r.bass[size_t(i) + 2], r.bass[size_t(i) + 3] }) >= '7') full = i;
            if (full > 0) {   // (at 0: full bass from the start, no drop)
                auto bassOver = [&](double t0, double t1) {
                    double pb = 0;
                    int k = 0;
                    for (int j = std::max(0, int(t0 * fr)); j < std::min(n, int(t1 * fr)); j++, k++) pb += std::pow(10.0, db_[size_t(j)] / 10) * bass_[size_t(j)];
                    return k ? 10 * std::log10(pb / k + 1e-12) : -99.0;
                };
                const double upTo = r.barIn + (full + 6) * bar;
                for (double T = phIn + std::ceil((r.start + 2 * pIn - phIn) / pIn) * pIn; T <= upTo && r.dropAt < 0; T += pIn) {
                    const double jump = bassOver(T, T + 2 * pIn) - bassOver(T - 2 * pIn, T);
                    bool stays = jump >= 8;   // each of the four bars after it full (one loud bar would carry an average)
                    for (int q = 0; q < 4 && stays; q++) stays = bassOver(T + q * 4 * pIn, T + (q + 1) * 4 * pIn) >= bodyBass - 6;
                    if (stays) {
                        r.dropAt = T;
                        r.dropJump = float(jump);
                    }
                }
            }
        }
        double nov[8] = {};   // (the phrases)
        for (size_t b = 1; b < lv.size(); b++) nov[b % 8] += std::abs(lv[b] - lv[b - 1]);
        r.phraseAt = 0;
        for (int o = 1; o < 8; o++) if (nov[o] > nov[r.phraseAt] + 1e-9) r.phraseAt = o;
    }

    // ---- the key (v3): the chroma summed over a stretch, against Krumhansl and Kessler's profiles of the 24 keys (each
    // rotated to every tonic); the best correlation wins, its lead over the second is how sure it is
    // v4: the bass's chroma over a stretch (the frames whose 4096-sample window is centred in it)
    auto bassOver = [&](double t0, double t1, float* out) {
        for (int k = 0; k < 12; k++) out[k] = 0;
        const int nb = int(bassChroma_.size() / 12);
        for (int c = 0; c < nb; c++) {
            const double t = ((c + 1) * 8.0 * H - N4 / 2) / ar_;
            if (t < t0 || t >= t1) continue;
            for (int k = 0; k < 12; k++) out[k] += bassChroma_[size_t(c) * 12 + size_t(k)];
        }
        for (int k = 0; k < 12; k++) out[k] /= bpcNorm_[k];
    };
    // the bass's weight against the harmony's: tried for power chords (the root is the lowest note), it did not help on the
    // songs whose keys are known (NEXT_SESSION.md, "Automix v3, step 5"): off, the bass's spectrum not even made, until a
    // larger set of true keys says otherwise. MIXAN_BASSKEY=<weight> (PC tools) turns it on
    const double bassW = bassKey_;
    auto keyOf = [&](double from, double to, float* sure) {
        float c[12], bc[12];
        chromaOver(from, to, c);
        bassOver(from, to, bc);
        static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        // Pearson's correlation of a chroma with key k's profile (rotated to its tonic)
        auto corr = [&](const float* x, int k) {
            const double* prof = k < 12 ? maj : mnr;
            const int tonic = k % 12;
            double xm = 0, pm = 0;
            for (int i = 0; i < 12; i++) { xm += x[i] / 12.0; pm += prof[i] / 12; }
            double num = 0, dx = 0, dp = 0;
            for (int i = 0; i < 12; i++) {
                const double a = x[(tonic + i) % 12] - xm, b = prof[i] - pm;
                num += a * b;
                dx += a * a;
                dp += b * b;
            }
            return dx > 0 ? num / std::sqrt(dx * dp) : 0.0;
        };
        double cm = 0, bm = 0;
        for (int i = 0; i < 12; i++) { cm += c[i]; bm += bc[i]; }
        if (cm <= 0) return -1;
        const double wb = bm > 0 ? bassW : 0;
        double best = -9, second = -9;
        int arg = -1;
        for (int k = 0; k < 24; k++) {
            const double r2 = (corr(c, k) + wb * corr(bc, k)) / (1 + wb);
            if (r2 > best) { second = best; best = r2; arg = k; }
            else if (r2 > second) second = r2;
        }
        if (sure) *sure = float(std::clamp((best - second) / 0.1, 0.0, 1.0));
        return arg;
    };
    r.key = keyOf(r.start, r.end, &r.keySure);
    r.keyIn = keyOf(r.start, std::min(double(r.end), r.start + 60.0), nullptr);
    r.keyOut = keyOf(std::max(double(r.start), r.end - 60.0), r.end, nullptr);
#ifndef __SWITCH__
    if (getenv("MIXAN_DEBUG"))
        fprintf(stderr, "tempo %.2f (peak/median %.2f) contrast in %.2f out %.2f, %zu beats, period %.5f in %.5f out %.5f\n", tempoConf,
                best / med, cIn, cOut, beats.size(), period, pIn, pOut);
#endif
    return r;
}

std::string keyName(int key) {
    if (key < 0 || key > 23) return "";
    static const char* n[12] = { "C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B" };
    return std::string(n[key % 12]) + (key < 12 ? " major" : " minor");
}

/** Camelot: majors B, minors A; C major 8B, A minor 8A; each fifth up one number. */
static int camelotNumber(int key) {
    const int major = key < 12 ? key : (key % 12 + 3) % 12;   // a minor key: its relative major
    return ((7 * major) % 12 + 7) % 12 + 1;
}

std::string camelot(int key) {
    if (key < 0 || key > 23) return "";
    return std::to_string(camelotNumber(key)) + (key < 12 ? "B" : "A");
}

int keyDistance(int a, int b) {
    if (a < 0 || b < 0) return -1;
    const int na = camelotNumber(a), nb = camelotNumber(b);
    int d = std::abs(na - nb);
    d = std::min(d, 12 - d);
    return d + ((a < 12) != (b < 12) ? 1 : 0);   // the other letter: one step more (its relative is 0 + 1)
}

std::string Info::pack() const {
    char s[320];
    snprintf(s, sizeof s, "%d,%.3f,%.3f,%.4f,%.4f,%.6f,%.6f,%.3f,%.3f,%.3f,%.3f,%.2f,%.3f,%.3f,%.3f,%d,%.4f,%.4f,%.2f,%.2f,%d,%d,%d,%d,%.2f,%.4f,%.1f,", Version, bpm,
             confidence, beatIn, beatOut, period, periodIn, start, end, fadeStart, introEnd, level, bassIn, bassOut, seconds, valid ? 1 : 0, barIn,
             barOut, double(barSureIn), double(barSureOut), phraseAt, key, keyIn, keyOut, double(keySure), dropAt, double(dropJump));
    return std::string(s) + (bars.empty() ? "-" : bars) + "/" + (bass.empty() ? "-" : bass);   // the bars last: as long as the track
}

bool Info::unpack(const std::string& s, Info& out) {
    int v = 0, ok = 0, used = 0;
    Info r;
    if (sscanf(s.c_str(), "%d,%f,%f,%lf,%lf,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%lf,%lf,%f,%f,%d,%d,%d,%d,%f,%lf,%f,%n", &v, &r.bpm, &r.confidence, &r.beatIn, &r.beatOut,
               &r.period, &r.periodIn, &r.start, &r.end, &r.fadeStart, &r.introEnd, &r.level, &r.bassIn, &r.bassOut, &r.seconds, &ok, &r.barIn,
               &r.barOut, &r.barSureIn, &r.barSureOut, &r.phraseAt, &r.key, &r.keyIn, &r.keyOut, &r.keySure, &r.dropAt, &r.dropJump, &used) != 27 ||
        v != Version || used <= 0)
        return false;
    r.bars = s.substr(size_t(used));
    const size_t slash = r.bars.find('/');
    if (slash != std::string::npos) {
        r.bass = r.bars.substr(slash + 1);
        r.bars.resize(slash);
    }
    if (r.bars == "-") r.bars.clear();
    if (r.bass == "-") r.bass.clear();
    r.valid = ok != 0;
    out = r;
    return true;
}

std::string Mood::pack() const {
    char s[256];
    snprintf(s, sizeof s, "%d,%.2f,%.2f,%.3f,%.0f,%.4f,%.2f,%.3f,%.3f,%.3f,%.4f,%.2f,%.3f,%.3f,%.3f", Version, double(loud), double(dyn),
             double(calm), double(bright), double(hf), double(onsets), double(perc), double(flux), double(minor), double(bass), double(bpm),
             double(beat), double(pulse), double(kickPulse));
    return s;
}

bool Mood::unpack(const std::string& s, Mood& out) {
    Mood r;
    int v = 0, used = 0;
    if (sscanf(s.c_str(), "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f%n", &v, &r.loud, &r.dyn, &r.calm, &r.bright, &r.hf, &r.onsets,
               &r.perc, &r.flux, &r.minor, &r.bass, &r.bpm, &r.beat, &r.pulse, &r.kickPulse, &used) != 15 || v != Version || used <= 0)
        return false;
    r.valid = true;
    out = r;
    return true;
}

}  // namespace mixan
