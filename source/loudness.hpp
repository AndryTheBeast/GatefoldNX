/*
 * Gatefold NX · loudness.hpp
 * Loudness as ITU-R BS.1770-4 / EBU R128 measure it, ported from Gatefold 1.4.2's
 * loudness-core.js: K-weighting re-derived for the file's own sample rate, 400 ms blocks every
 * 100 ms, gating at −70 LUFS and then −10 LU, the true peak by 4× oversampling, mono as dual
 * mono. Each track's blocks are kept as a 0.1 LU histogram, so an album's loudness is gated over
 * all of its blocks at once (not an average of the tracks) without decoding it again.
 *
 * The music files are never changed: the results live in Gatefold's own folder (loudness.tsv).
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace mixan { class Analyzer; }

namespace loud {

constexpr double Lo = -70, Hi = 10, Bin = 0.1;
constexpr int Bins = 800;   // (Hi − Lo) / Bin

/** Blocks by loudness: how many in each 0.1 LU bin, and their summed power. */
struct Hist {
    std::vector<double> n = std::vector<double>(Bins), p = std::vector<double>(Bins);
    void merge(const Hist& h);
    /** "bin:count:power,…" for loudness.tsv, and back. */
    std::string pack() const;
    static Hist unpack(const std::string& s);
};

/** Gated integrated loudness in LUFS; NaN when nothing is above −70 (silence). */
double integrated(const Hist& h);

class Meter {
public:
    Meter(int rate, int channels);
    /** Interleaved float samples. Nothing is allocated per push: the console measures a whole library. */
    void push(const float* x, int frames);

    struct Result {
        double lufs = 0;       // NaN for silence
        double truePeak = 0;   // linear, 1 = 0 dBFS
        double seconds = 0;
        Hist hist;
    };
    Result result() const;

private:
    struct Biquad { double b0, b1, b2, a1, a2; };
    int rate_, ch_, sub_;
    Biquad shelf_, hp_;
    std::vector<double> w_, z_, acc_;   // per channel: weight, 4 filter states, the sub-block's sum
    int fill_ = 0;
    double ring_[4] = {};
    int ringN_ = 0;
    Hist hist_;
    double peak_ = 0, tp_ = 0;
    std::vector<std::vector<float>> tail_;   // per channel: the last samples, for the true peak across pushes
    std::vector<float> y_;                   // scratch, kept: the tail and one channel of a push
    std::vector<double> sums_;               // scratch, kept: the push's finished 100 ms sub-blocks
    long long frames_ = 0;
    void peaks(int c, const float* x, int frames);
};

/**
 * Decodes the whole file (FFmpeg: every format Gatefold plays) and measures it. Stops early,
 * returning false, when *cancel turns true. Not on the main thread: a track takes a moment.
 * mix, when given, analyses the same samples for Automix (mixan.hpp): one decode for both.
 */
bool measureFile(const std::string& path, Meter::Result& out, const std::atomic<bool>* cancel, mixan::Analyzer* mix = nullptr);

/** dB gain from a loudness to a target, both in LUFS. */
inline double gainTo(double lufs, double target) { return target - lufs; }

}  // namespace loud
