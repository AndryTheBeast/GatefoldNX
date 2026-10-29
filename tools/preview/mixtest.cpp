/*
 * Gatefold NX · tools/preview/mixtest.cpp (PC only, never in a Switch build)
 * Runs Automix's analysis (source/mixan.cpp) through the same decode as the console's levelling worker
 * (loudness.cpp's measureFile) and prints one line per file:
 *   mixtest <file or folder>...      folders are walked; .m4a .mp3 .flac .wav .ogg .opus
 *   mixtest --render a b out.wav [preset] [loop1|loop2]
 *                                    Automix's join from a into b, as the console plays it (preset: auto fade rise blend echo cut)
 *   mixtest --plans <folder>         Auto's plan for each track into the next, name order vs Automix order
 *   mixtest --bands file [window]    the file's level per window in four bands (a mix's envelopes)
 * Built by tools/preview/build-mixtest.sh.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "automix.hpp"
#include "ffdecoder.hpp"
#include "loudness.hpp"
#include "mixan.hpp"

namespace fs = std::filesystem;

// ffdecoder.cpp's tag reading wants it; the tool reads no tags
std::string cleanGenre(const std::string& s) { return s; }

// AUTOMIX_MODEL=1.0.0 (or "Automix V1.0.0"): the model to measure (automix.hpp); else the newest. Its name is printed first
static automix::Model model() {
    static const automix::Model m = [] {
        automix::Model r = automix::Latest;
        if (const char* e = getenv("AUTOMIX_MODEL"); e && *e) {
            std::string n = e;
            if (n.rfind("Automix ", 0) != 0) n = "Automix " + std::string(n[0] == 'V' ? "" : "V") + n;
            if (!automix::modelFromName(n, r)) { fprintf(stderr, "no model %s\n", e); exit(2); }
        }
        fprintf(stderr, "model: %s\n", automix::name(r));
        return r;
    }();
    return m;
}

static std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static mixan::Info analyse(const std::string& f) {
    mixan::Analyzer an;
    loud::Meter::Result lr;
    return loud::measureFile(f, lr, nullptr, &an) ? an.finish() : mixan::Info{};
}

/**
 * --render a b out.wav: Automix's join from a into b, as Gatefold's player makes it (automix::plan, FfDecoder's skip,
 * automix::Fader), with 12 s before and after, to a 48 kHz WAV. Levelling is left out (both at their own volume).
 */
static int render(const std::string& fa, const std::string& fb, const std::string& outPath, const std::string& preset, int loopBars) {
    constexpr int Rate = 48000, Pre = 12, Post = 12;
    const mixan::Info ia = analyse(fa), ib = analyse(fb);
    automix::Join want;
    for (int k = 0; k <= int(automix::Preset::Cut); k++) {
        std::string n = automix::name(automix::Preset(k));
        for (char& c : n) c = char(tolower(c));
        if (n == preset) want = automix::fromPreset(automix::Preset(k));
    }
    if (loopBars && want.preset == automix::Preset::Auto) want = automix::fromPreset(automix::Preset::Blend);
    want.loopBars = loopBars;
    const automix::Plan pl = automix::plan(&ia, ia.seconds, &ib, 4, &want, model());
    printf("a: %.1f BPM (sure %.2f), music %.2f-%.2f s, fade-out from %.2f\nb: %.1f BPM (sure %.2f), music from %.2f, first beat %.3f\n",
           ia.bpm, ia.confidence, ia.start, ia.end, ia.fadeStart, ib.bpm, ib.confidence, ib.start, ib.beatIn);
    printf("plan: at %.3f s of a, b from %.3f s, %.2f s%s (%d beats); %s: %s, %s, filter %s, effect %s, loop %d (%.2f-%.2f)\n", pl.startOut,
           pl.offset, pl.length, pl.drop && pl.beatmatched ? " on the drop, on the bars" : pl.drop ? " a bridge into the drop, tempos apart" : pl.cut ? " a cut on the phrase, tempos apart" : pl.structured ? " by the structure, on the bars" : pl.beatmatched ? (pl.onBars ? " on the beat and the bars" : " on the beat") : "", pl.beats, automix::name(pl.join.preset), automix::name(pl.join.volume),
           automix::name(pl.join.eq), automix::name(pl.join.filter), automix::name(pl.join.effect), pl.join.loopBars, pl.loopFrom, pl.loopTo);
    if (ia.key >= 0 && ib.key >= 0)
        printf("keys: %s (%s, sure %.2f) into %s (%s, sure %.2f): %d apart%s\n", mixan::keyName(ia.key).c_str(), mixan::camelot(ia.key).c_str(),
               double(ia.keySure), mixan::keyName(ib.key).c_str(), mixan::camelot(ib.key).c_str(), double(ib.keySure), pl.keyDistance,
               pl.keyClash ? ": a clash, kept short and the incoming muffled" : "");
    if (pl.structured) printf("structure: crossover at %.0f %% of the join (%.3f s of a), a's outro after it; incoming %s\n", pl.cross * 100, pl.startOut + pl.cross * pl.length * pl.speed, pl.join.filter == automix::Filter::LowPassIn ? "low-passed until then (two melodies)" : "clear");
    if (pl.drop)
        printf("drop: a's bass cut at %.3f s of a, b's in at %.3f s of a (the crossover, %.0f %% of the join); b from its bar %.0f\n",
               pl.startOut + pl.bassCut * pl.length * pl.speed, pl.startOut + pl.cross * pl.length * pl.speed, pl.cross * 100,
               ib.bpm > 0 ? (pl.offset - ib.barIn) / (4 * ib.periodIn) : 0.0);
    if (pl.speed != 1) printf("tempo: a at %+.2f %% from %.2f s (reached over %.1f s) to its end; b as it is\n", (pl.speed - 1) * 100, pl.stretchFrom, pl.easeIn);
    FfDecoder a, b;
    if (!a.open(fa, Rate, 2) || !b.open(fb, Rate, 2)) return 1;
    std::vector<int16_t> pcm(4096 * 2);
    long long skipA = (long long)std::llround(std::max(0.0, pl.startOut - Pre) * Rate);
    while (skipA > 0) {
        const int got = a.read(pcm.data(), int(std::min<long long>(4096, skipA)));
        if (got <= 0) break;
        skipA -= got;
    }
    b.skip(pl.offset);
    std::vector<int16_t> wav;
    // what the player would clip (its output is 16-bit too): samples past full scale, and the peak, before / in / after the join
    long long clipped[3] = {};
    float peak[3] = {};
    int part = 0;
    auto put = [&](const float* x, int n) {
        for (int i = 0; i < n * 2; i++) {
            if (std::fabs(x[i]) >= 1.f) clipped[part]++;
            peak[part] = std::max(peak[part], std::fabs(x[i]));
            wav.push_back(int16_t(std::lrint(std::clamp(x[i] * 32767.f, -32768.f, 32767.f))));
        }
    };
    long long joinAt = -1;   // where the join starts in the render (frames)
    automix::render(pl, Rate, Pre, Post, [&](int16_t* o, int n) { return a.read(o, n); }, [&](int16_t* o, int n) { return b.read(o, n); },
                    [&](const float* x, int n) {
                        if (part == 1 && joinAt < 0) joinAt = (long long)wav.size() / 2;
                        put(x, n);
                    },
                    &part);
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) return 1;
    const uint32_t data = uint32_t(wav.size() * 2), riff = 36 + data, fmtLen = 16, rate = Rate, byteRate = Rate * 4;
    const uint16_t pcmFmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcmFmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(wav.data(), 2, wav.size(), f);
    fclose(f);
    printf("wrote %s: %.1f s, the join starts at %.2f s and lasts %.2f s\n", outPath.c_str(), wav.size() / 2.0 / Rate, double(joinAt) / Rate, pl.length);
    printf("clipped samples before / in / after the join: %lld / %lld / %lld; peaks %.2f / %.2f / %.2f dBFS\n", clipped[0], clipped[1], clipped[2],
           20 * std::log10(std::max(1e-6f, peak[0])), 20 * std::log10(std::max(1e-6f, peak[1])), 20 * std::log10(std::max(1e-6f, peak[2])));
    return 0;
}

/**
 * --bands file [window s]: the file's level per window (0.25 s by default) in four bands, dB: bass 30-150 Hz, low mids
 * 150-300 Hz, mids 300 Hz-3 kHz, highs above 5 kHz; and the whole, and the mids' side (L - R) share, dB under their centre.
 * Each band through two 2nd-order Butterworth sections at each edge. To read a mix's envelopes (a reference, or a render).
 */
static int bands(const std::string& file, double window) {
    constexpr int Rate = 48000;
    struct Bq {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set(bool high, double fc, double q) {
            const double w0 = 2 * 3.14159265358979 * fc / Rate, c = std::cos(w0), al = std::sin(w0) / (2 * q), a0 = 1 + al;
            b0 = (high ? (1 + c) : (1 - c)) / 2 / a0;
            b1 = (high ? -(1 + c) : (1 - c)) / a0;
            b2 = b0;
            a1 = -2 * c / a0;
            a2 = (1 - al) / a0;
        }
        double run(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    // 4th-order Butterworth: Q 0.5412 and 1.3066
    struct Band {
        std::vector<Bq> f;
        double acc = 0;
        Band(double lo, double hi) {
            for (double q : { 0.5412, 1.3066 }) {
                if (lo > 0) { f.emplace_back(); f.back().set(true, lo, q); }
                if (hi > 0) { f.emplace_back(); f.back().set(false, hi, q); }
            }
        }
        double run(double x) {
            for (Bq& b : f) x = b.run(x);
            return x;
        }
    };
    const mixan::Info info = analyse(file);
    printf("# %s\n# bpm %.2f (sure %.2f), first beat %.3f s, first downbeat %.3f s (sure %.2f), bar %.3f s\n", file.c_str(), info.bpm,
           info.confidence, info.beatIn, info.barIn, double(info.barSureIn), info.bpm > 0 ? 240.0 / info.bpm : 0.0);
    FfDecoder d;
    if (!d.open(file, Rate, 2)) return 1;
    std::vector<Band> bs{ Band(30, 150), Band(150, 300), Band(300, 3000), Band(5000, 0) };
    Band sideMid(300, 3000);
    double all = 0;
    const long long per = std::max(1LL, (long long)std::llround(window * Rate));
    long long n = 0, k = 0;
    auto db = [&](double e) { return 10 * std::log10(std::max(1e-12, e / double(per))); };
    printf("t\tall\tbass\tlowmid\tmid\thigh\tside\n");
    std::vector<int16_t> pcm(4096 * 2);
    for (int got; (got = d.read(pcm.data(), 4096)) > 0;) {
        for (int i = 0; i < got; i++) {
            const double l = pcm[size_t(i) * 2] / 32768.0, r = pcm[size_t(i) * 2 + 1] / 32768.0, m = (l + r) / 2, s = (l - r) / 2;
            all += m * m;
            for (Band& b : bs) {
                const double y = b.run(m);
                b.acc += y * y;
            }
            const double y = sideMid.run(s);
            sideMid.acc += y * y;
            if (++n == per) {
                printf("%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", double(k) * window, db(all), db(bs[0].acc), db(bs[1].acc), db(bs[2].acc),
                       db(bs[3].acc), db(sideMid.acc) - db(bs[2].acc));
                all = 0;
                for (Band& b : bs) b.acc = 0;
                sideMid.acc = 0;
                n = 0;
                k++;
            }
        }
    }
    return 0;
}

/** --plans <folder>: every track analysed, then Auto's plan for each one into the next (in name order): what it chose. */
static int plans(const std::string& dir) {
    std::vector<std::string> files;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(dir))) {
        std::string ext = utf8(e.path().extension());
        for (char& c : ext) c = char(tolower(c));
        if (e.is_regular_file() && (ext == ".m4a" || ext == ".mp3" || ext == ".flac")) files.push_back(utf8(e.path()));
    }
    std::sort(files.begin(), files.end());
    std::vector<mixan::Info> info;
    for (const auto& f : files) info.push_back(analyse(f));
    // the joins of one order: what Auto plans for each
    auto stats = [&](const std::vector<int>& ord, const char* what, bool list) {
        int drops = 0, bridges = 0, cuts = 0, structured = 0, hidden = 0, onBars = 0, onBeat = 0, timed = 0, stretched = 0, keyed = 0, clashes = 0, n = 0;
        double score = 0;
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const mixan::Info& a = info[size_t(ord[k])];
            const mixan::Info& b = info[size_t(ord[k + 1])];
            const automix::Plan pl = automix::plan(&a, a.seconds, &b, 4, nullptr, model());
            n++;
            score += automix::joinScore(&a, &b, model());
            if (pl.drop && pl.beatmatched) drops++;
            if (pl.drop && !pl.beatmatched) bridges++;
            if (pl.cut) cuts++;
            if (pl.structured) structured++;
            if (pl.structured && pl.join.filter == automix::Filter::LowPassIn) hidden++;
            if (pl.beatmatched && pl.onBars) onBars++;
            if (pl.beatmatched) onBeat++; else timed++;
            if (pl.speed != 1) stretched++;
            if (pl.keyDistance >= 0) keyed++;
            if (pl.keyClash) clashes++;
            if (list)
                printf("%s%s%s\t%d beats\t%.1f s\tcross %.2f\t%s\n", pl.structured ? "structure" : pl.beatmatched ? "beat" : "timed",
                       pl.keyClash ? "+keys clash" : "", pl.speed != 1 ? "+tempo" : "", pl.beats, pl.length, pl.cross,
                       utf8(fs::path(files[size_t(ord[k])]).filename()).c_str());
        }
        printf("%s: %d joins, score %.0f: %d on the drop, %d by the structure, %d on the bars, %d on the beat, %d timed (%d bridges, %d cuts on a phrase); %d tempo-matched; keys known on both "
               "sides %d, clashing %d (%d structured joins kept short)\n",
               what, n, score, drops, structured, onBars, onBeat, timed, bridges, cuts, stretched, keyed, clashes, hidden);
    };
    std::vector<int> byName(files.size());
    for (size_t i = 0; i < files.size(); i++) byName[i] = int(i);
    stats(byName, "in name order", false);
    std::vector<const mixan::Info*> ptr;
    for (const auto& x : info) ptr.push_back(&x);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<int> best = automix::order(ptr, -1, model());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char label[64];
    snprintf(label, sizeof label, "Automix order (%.0f ms)", ms);
    stats(best, label, false);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--plans") return plans(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "--bands") return bands(argv[2], argc > 3 ? atof(argv[3]) : 0.25);
    if (argc >= 5 && std::string(argv[1]) == "--render")
        return render(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : "auto", argc > 6 ? atoi(argv[6] + 4) : 0);
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        const fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p)) {
                if (!e.is_regular_file()) continue;
                std::string ext = utf8(e.path().extension());
                for (char& c : ext) c = char(tolower(c));
                if (ext == ".m4a" || ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".opus")
                    files.push_back(utf8(e.path()));
            }
        } else {
            files.push_back(utf8(p));
        }
    }
    std::sort(files.begin(), files.end());
    printf("bpm\tconf\tbeatIn\tbeatOut\tstart\tend\tfade\tintro\tlevel\tbassIn\tbassOut\tsecs\tms\tbarIn\tbarSureIn\tbarOut\tbarSureOut\tphrase\tkey\tcamelot\tkeySure\tkeyIn\tkeyOut\tbars\tdropAt\tdropJump\tbass\tfile\n");
    for (const auto& f : files) {
        mixan::Analyzer an;
        loud::Meter::Result lr;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = loud::measureFile(f, lr, nullptr, &an);
        const mixan::Info r = ok ? an.finish() : mixan::Info{};
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%.2f\t%.2f\t%.3f\t%.3f\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%.2f\t%.2f\t%.1f\t%.0f\t%.3f\t%.2f\t%.3f\t%.2f\t%d\t%d\t%s\t%.2f\t%d\t%d\t%s\t%.3f\t%.1f\t%s\t%s\n", r.bpm,
               r.confidence, r.beatIn, r.beatOut, r.start, r.end, r.fadeStart, r.introEnd, r.level, r.bassIn, r.bassOut, r.seconds, ms, r.barIn,
               double(r.barSureIn), r.barOut, double(r.barSureOut), r.phraseAt, r.key, mixan::camelot(r.key).c_str(), double(r.keySure), r.keyIn, r.keyOut, r.bars.empty() ? "-" : r.bars.c_str(), r.dropAt, double(r.dropJump), r.bass.empty() ? "-" : r.bass.c_str(), f.c_str());
        fflush(stdout);
    }
}
