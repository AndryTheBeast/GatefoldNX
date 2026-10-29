/*
 * Gatefold NX · loudness.cpp
 */
#include "loudness.hpp"
#include "mixan.hpp"

#if defined(__SWITCH__) || defined(GF_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#define GF_LOUDNESS_DECODE 1
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace loud {

namespace {

double lufsOf(double power) { return -0.691 + 10 * std::log10(power); }

void histAdd(Hist& h, double power) {
    const double L = lufsOf(power);
    if (!(L >= Lo)) return;   // the absolute gate (and silence, NaN)
    const int i = std::min(Bins - 1, int(std::floor((L - Lo) / Bin)));
    h.n[size_t(i)] += 1;
    h.p[size_t(i)] += power;
}

/** Channel weights in WAVE order (L R C LFE Ls Rs …): mono counts double (it plays from both speakers). */
std::vector<double> weights(int n) {
    if (n == 1) return { 2 };
    std::vector<double> w(size_t(n), 1);
    if (n <= 3) return w;
    if (n >= 6) { w[3] = 0; for (int c = 4; c < n; c++) w[size_t(c)] = 1.41; }   // 5.1 / 7.1: LFE out, surrounds 1.41
    else if (n == 4) { w[2] = 1.41; w[3] = 1.41; }                               // quad
    else if (n == 5) { w[3] = 1.41; w[4] = 1.41; }                               // 5.0
    return w;
}

// The true peak's 4× interpolator: phases 1…3, the value at n + p/4 from x[n−5 … n+6] (12 taps,
// a Kaiser-windowed sinc, β 6, normalised to unity DC gain).
constexpr int Taps = 12;
struct TpFilters { double h[3][Taps]; };
const TpFilters& tpFilters() {
    static const TpFilters f = [] {
        TpFilters t{};
        auto i0 = [](double x) { double s = 1, a = 1; for (int k = 1; k < 30; k++) { a *= (x / (2 * k)) * (x / (2 * k)); s += a; } return s; };
        const double beta = 6, half = Taps / 2;
        for (int p = 1; p < 4; p++) {
            double sum = 0;
            for (int k = 0; k < Taps; k++) {
                const double d = (k - (half - 1)) - p / 4.0;
                const double sinc = d == 0 ? 1 : std::sin(M_PI * d) / (M_PI * d);
                const double r = d / half;
                const double v = std::fabs(r) >= 1 ? 0 : sinc * i0(beta * std::sqrt(1 - r * r)) / i0(beta);
                t.h[p - 1][k] = v;
                sum += v;
            }
            for (int k = 0; k < Taps; k++) t.h[p - 1][k] /= sum;
        }
        return t;
    }();
    return f;
}

}  // namespace

// ---------------------------------------------------------------- the histogram

void Hist::merge(const Hist& h) {
    for (int i = 0; i < Bins; i++) { n[size_t(i)] += h.n[size_t(i)]; p[size_t(i)] += h.p[size_t(i)]; }
}

std::string Hist::pack() const {
    std::string out;
    char b[64];
    for (int i = 0; i < Bins; i++)
        if (n[size_t(i)] > 0) {
            snprintf(b, sizeof b, "%s%d:%.0f:%.9g", out.empty() ? "" : ",", i, n[size_t(i)], p[size_t(i)]);
            out += b;
        }
    return out;
}

Hist Hist::unpack(const std::string& s) {
    Hist h;
    const char* c = s.c_str();
    while (*c) {
        char* e = nullptr;
        const long i = strtol(c, &e, 10);
        if (*e != ':') break;
        const double n = strtod(e + 1, &e);
        if (*e != ':') break;
        const double p = strtod(e + 1, &e);
        if (i >= 0 && i < Bins) { h.n[size_t(i)] = n; h.p[size_t(i)] = p; }
        c = *e == ',' ? e + 1 : e;
        if (!*e) break;
    }
    return h;
}

double integrated(const Hist& h) {
    double n = 0, p = 0;
    for (int i = 0; i < Bins; i++) { n += h.n[size_t(i)]; p += h.p[size_t(i)]; }
    if (!n) return NAN;
    const double rel = lufsOf(p / n) - 10;   // the relative gate
    const int from = std::max(0, int(std::ceil((rel - Lo) / Bin - 1e-9)));
    double n2 = 0, p2 = 0;
    for (int i = from; i < Bins; i++) { n2 += h.n[size_t(i)]; p2 += h.p[size_t(i)]; }
    return n2 ? lufsOf(p2 / n2) : NAN;
}

// ---------------------------------------------------------------- the meter

Meter::Meter(int rate, int channels) : rate_(rate), ch_(channels), sub_(int(std::lround(rate / 10.0))) {
    // The BS.1770 filters re-derived for the rate (the constants of libebur128 / pyloudnorm).
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(M_PI * f0 / rate);
    const double Vh = std::pow(10, G / 20), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1 + K / Q + K * K;
    shelf_ = { (Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(M_PI * f0 / rate);
    a0 = 1 + K / Q + K * K;
    hp_ = { 1, -2, 1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    w_ = weights(channels);
    z_.assign(size_t(channels) * 4, 0);
    acc_.assign(size_t(channels), 0);
    tail_.assign(size_t(channels), {});
}

void Meter::push(const float* x, int frames) {
    std::vector<double>& sums = sums_;
    sums.clear();
    int end = fill_;
    if (ch_ == 2) {
        // Stereo, nearly every file: both channels in one loop. Each filter is a chain where every sample
        // waits for the one before; two chains side by side keep the CPU busy (the same sums: weights 1, 1).
        peaks(0, x, frames);
        peaks(1, x, frames);
        const Biquad &s = shelf_, &h = hp_;
        double l1 = z_[0], l2 = z_[1], lh1 = z_[2], lh2 = z_[3];
        double r1 = z_[4], r2 = z_[5], rh1 = z_[6], rh2 = z_[7];
        double accL = acc_[0], accR = acc_[1];
        int pos = fill_;
        for (int i = 0; i < frames; i++) {
            const double vl = x[2 * size_t(i)], vr = x[2 * size_t(i) + 1];
            const double yl = s.b0 * vl + l1, yr = s.b0 * vr + r1;
            l1 = s.b1 * vl - s.a1 * yl + l2;
            r1 = s.b1 * vr - s.a1 * yr + r2;
            l2 = s.b2 * vl - s.a2 * yl;
            r2 = s.b2 * vr - s.a2 * yr;
            const double ol = h.b0 * yl + lh1, orr = h.b0 * yr + rh1;
            lh1 = h.b1 * yl - h.a1 * ol + lh2;
            rh1 = h.b1 * yr - h.a1 * orr + rh2;
            lh2 = h.b2 * yl - h.a2 * ol;
            rh2 = h.b2 * yr - h.a2 * orr;
            accL += ol * ol;
            accR += orr * orr;
            if (++pos == sub_) {
                sums.push_back(accL / sub_ + accR / sub_);
                accL = accR = 0;
                pos = 0;
            }
        }
        z_[0] = l1; z_[1] = l2; z_[2] = lh1; z_[3] = lh2;
        z_[4] = r1; z_[5] = r2; z_[6] = rh1; z_[7] = rh2;
        acc_[0] = accL;
        acc_[1] = accR;
        end = pos;
    } else for (int c = 0; c < ch_; c++) {
        peaks(c, x, frames);
        const double wc = w_[size_t(c)];
        if (!wc) continue;
        double* z = &z_[size_t(c) * 4];
        double s1 = z[0], s2 = z[1], h1 = z[2], h2 = z[3], acc = acc_[size_t(c)];
        int pos = fill_;
        size_t bi = 0;
        const Biquad &s = shelf_, &h = hp_;
        const float* xc = x + c;
        for (int i = 0; i < frames; i++) {   // two biquads, transposed direct form II
            const double v = xc[size_t(i) * ch_];
            const double y1 = s.b0 * v + s1;
            s1 = s.b1 * v - s.a1 * y1 + s2;
            s2 = s.b2 * v - s.a2 * y1;
            const double y2 = h.b0 * y1 + h1;
            h1 = h.b1 * y1 - h.a1 * y2 + h2;
            h2 = h.b2 * y1 - h.a2 * y2;
            acc += y2 * y2;
            if (++pos == sub_) {
                if (sums.size() <= bi) sums.push_back(0);
                sums[bi++] += acc / sub_ * wc;
                acc = 0;
                pos = 0;
            }
        }
        z[0] = s1; z[1] = s2; z[2] = h1; z[3] = h2;
        acc_[size_t(c)] = acc;
        end = pos;
    }
    fill_ = end;
    // Each finished 100 ms sub-block closes a 400 ms block (75 % overlap).
    for (double v : sums) {
        if (ringN_ == 4) { ring_[0] = ring_[1]; ring_[1] = ring_[2]; ring_[2] = ring_[3]; ringN_ = 3; }
        ring_[ringN_++] = v;
        if (ringN_ == 4) histAdd(hist_, (ring_[0] + ring_[1] + ring_[2] + ring_[3]) / 4);
    }
    frames_ += frames;
}

/* The sample peak, and the true peak between samples: the 4× interpolation evaluated only next to
   the loudest samples (where an inter-sample peak can be), continuous across pushes. */
void Meter::peaks(int c, const float* x, int frames) {
    // x is interleaved: channel c, with the last samples of the previous push in front of it
    std::vector<float>& tail = tail_[size_t(c)];
    std::vector<float>& y = y_;
    y.resize(tail.size() + size_t(frames));
    std::copy(tail.begin(), tail.end(), y.begin());
    float pkf = float(peak_);
    float* yo = y.data() + tail.size();
    for (int i = 0; i < frames; i++) {
        const float v = x[size_t(i) * ch_ + c];
        yo[i] = v;
        pkf = std::max(pkf, std::fabs(v));
    }
    const double pk = pkf;
    peak_ = pk;
    const int n = int(y.size());
    // Candidates reach 75 % of the sample peak (not of the true peak found so far, which on a clipped
    // master can exceed every sample and would shut every candidate out).
    double tp = std::max(tp_, pk);
    const float thr = pkf * 0.75f;
    const TpFilters& f = tpFilters();
    for (int i = 5; i + 6 < n; i++) {
        const float u = y[size_t(i)], w = y[size_t(i) + 1];
        const float a = std::fabs(u), b = std::fabs(w);
        if (a < thr && b < thr) continue;   // the cheap test first: it rules out nearly every sample
        if ((u > 0) != (w > 0)) continue;
        // an over between two samples sits at a local maximum of the waveform: skip the slopes
        const float m = std::max(a, b);
        if (m < std::fabs(y[size_t(i) - 1]) || m < std::fabs(y[size_t(i) + 2])) continue;
        for (int ph = 0; ph < 3; ph++) {
            double v = 0;
            for (int k = 0; k < Taps; k++) v += f.h[ph][k] * y[size_t(i - 5 + k)];
            tp = std::max(tp, std::fabs(v));
        }
    }
    tp_ = tp;
    tail.assign(y.begin() + std::max(0, n - 11), y.end());
}

Meter::Result Meter::result() const {
    Result r;
    r.lufs = integrated(hist_);
    r.truePeak = std::max(tp_, peak_);
    r.seconds = double(frames_) / rate_;
    r.hist = hist_;
    return r;
}

// ---------------------------------------------------------------- a whole file

bool measureFile(const std::string& path, Meter::Result& out, const std::atomic<bool>* cancel, mixan::Analyzer* mix) {
#ifdef GF_LOUDNESS_DECODE
    AVFormatContext* fmt = nullptr;
    const std::string url = "file:" + path;   // FFmpeg reads "sdmc:/…" as a URL otherwise
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) != 0) return false;
    bool ok = false;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const AVCodec* codec = nullptr;
    int stream = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0 && (stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) >= 0 && codec &&
        (dec = avcodec_alloc_context3(codec)) && avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0 && pkt && frame) {
        // Float at the file's own rate: a 16-bit conversion would clip the overs the true peak is for.
        // Mono stays mono (dual mono, weighted); more than two channels are folded to stereo.
        const int ch = dec->ch_layout.nb_channels == 1 ? 1 : 2;
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, ch);
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, dec->sample_rate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr) >= 0 &&
            swr_init(swr) >= 0) {
            Meter m(dec->sample_rate, ch);
            if (mix) mix->begin(dec->sample_rate, ch);
            std::vector<float> buf;
            bool eof = false, cancelled = false;
            while (!eof) {
                if (cancel && cancel->load()) { cancelled = true; break; }
                int r = avcodec_receive_frame(dec, frame);
                if (r == 0) {
                    const int cap = swr_get_out_samples(swr, frame->nb_samples);
                    buf.resize(size_t(cap) * ch);
                    uint8_t* outp = reinterpret_cast<uint8_t*>(buf.data());
                    const int got = swr_convert(swr, &outp, cap, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                    if (got > 0) {
                        m.push(buf.data(), got);
                        if (mix) mix->push(buf.data(), got);
                    }
                    av_frame_unref(frame);
                    continue;
                }
                if (r == AVERROR_EOF) { eof = true; break; }
                r = av_read_frame(fmt, pkt);
                if (r < 0) { avcodec_send_packet(dec, nullptr); continue; }   // the end: flush the decoder
                if (pkt->stream_index == stream) avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
            }
            if (!cancelled) {
                out = m.result();
                ok = out.seconds > 0;
            }
        }
    }
    if (swr) swr_free(&swr);
    if (dec) avcodec_free_context(&dec);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    avformat_close_input(&fmt);
    return ok;
#else
    (void)path; (void)out; (void)cancel; (void)mix;
    return false;
#endif
}

}  // namespace loud
