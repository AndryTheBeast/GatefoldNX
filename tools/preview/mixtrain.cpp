/*
 * Gatefold NX · tools/preview/mixtrain.cpp (PC only, never in a Switch build)
 * Training Automix V1.1.0 on the user's ratings (the user, 2026-10-29: "the next Automix model will be a trained model":
 * demos from their library, rated 0 to 10, rounds of them):
 *   mixtrain analyse <root> <cache.tsv> [threads]
 *   mixtrain mood <root> <out.tsv> [threads]   (Mood playlists: path, artist, title, genre, the analysis, the mood's features)
 *       every .flac .mp3 .m4a under root: Automix's analysis (mixan, as the console's) and the sung parts from its synced
 *       lyrics (the LYRICS tag), one line each: path, sung parts, mixan::Info::pack. Resumes: files already in it are kept.
 *   mixtrain round <cache.tsv> <out dir> <round> [seed]
 *       each folder ordered as an Automix playlist (V1.0.0); from its joins, two of each kind (on the drop, a bridge between
 *       tempos, a cut on a phrase, a timed fade), each rendered three ways: V1.0.0's numbers and two others (automix::Tune),
 *       shuffled (the user does not know which is which); out dir/round.json says what each demo is, NN.mp3 the demos.
 *       Round 2 on: variants around round 1's winners (variant2), and a plan.tsv: joins to hear again, pairs not to pick, new ones.
 *       Round 3 on: round 2's best (base3) in V1.0.0's place, and variants around it (variant3).
 * Anyone training a model of their own (self-serve, 2026-10-30; Songate's own rounds keep `round` above):
 *   mixtrain build <cache.tsv> <round.spec> <out dir>     a round from a spec (see examples/training/round.spec)
 *   tools/mixtrain-fit.py <round dir>…                     what the ratings say, and a candidate
 *   mixtrain freeze <candidate> <name> <cache.tsv> <out.model>
 *   mixtrain filesame <cache.tsv> <file>.model             a model file against what it stands for
 * Built by tools/preview/build-mixtrain.sh.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "automix.hpp"
#include "ffdecoder.hpp"
#include "loudness.hpp"
#include "metadata.hpp"
#include "mixan.hpp"
#include "mood.hpp"

namespace fs = std::filesystem;

std::string cleanGenre(const std::string& s) { return s; }   // ffdecoder.cpp's tag reading wants it

static std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---------------------------------------------------------------- the sung parts, from synced lyrics

/** "a-b,c-d" in tenths of a second: each timed line sung from its time to the next line's, at most 1 s + 0.09 s a
 *  character (8 s); empty lines (breaks) not sung. "" without synced lyrics; "-" with lyrics but none timed. */
static std::string sungParts(const std::string& lrc) {
    std::vector<std::pair<double, size_t>> lines;   // time, characters
    std::istringstream in(lrc);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) any = true;
        size_t p = 0;
        std::vector<double> stamps;
        while (p < line.size() && line[p] == '[') {
            const size_t q = line.find(']', p);
            if (q == std::string::npos) break;
            int mm = 0;
            double ss = 0;
            if (sscanf(line.substr(p + 1, q - p - 1).c_str(), "%d:%lf", &mm, &ss) == 2) stamps.push_back(mm * 60 + ss);
            p = q + 1;
        }
        std::string text = line.substr(std::min(p, line.size()));
        while (!text.empty() && text.front() == ' ') text.erase(0, 1);
        for (double t : stamps) lines.push_back({ t, text.size() });
    }
    if (lines.empty()) return any ? "-" : "";
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].second == 0) continue;
        double e = lines[i].first + std::min(8.0, 1 + 0.09 * double(lines[i].second));
        if (i + 1 < lines.size()) e = std::min(e, lines[i + 1].first);
        if (e <= lines[i].first) continue;
        char b[48];
        snprintf(b, sizeof b, "%s%d-%d", out.empty() ? "" : ",", int(std::lround(lines[i].first * 10)), int(std::lround(e * 10)));
        out += b;
    }
    return out.empty() ? "-" : out;
}

static std::vector<std::pair<double, double>> parseSung(const std::string& s) {
    std::vector<std::pair<double, double>> v;
    if (s.empty() || s == "-") return v;
    std::istringstream in(s);
    std::string part;
    while (std::getline(in, part, ',')) {
        int a = 0, b = 0;
        if (sscanf(part.c_str(), "%d-%d", &a, &b) == 2) v.push_back({ a / 10.0, b / 10.0 });
    }
    return v;
}

// ---------------------------------------------------------------- analyse

static int analyseAll(const std::string& root, const std::string& cachePath, int threads, bool mood = false) {
    std::set<std::string> done;
    {
        std::ifstream in(cachePath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) done.insert(line.substr(0, line.find('\t')));
    }
    std::vector<std::string> todo;
    for (const auto& e : fs::recursive_directory_iterator(fs::u8path(root))) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(tolower(c));
        if (ext != ".flac" && ext != ".mp3" && ext != ".m4a") continue;
        const std::string p = utf8(e.path());
        if (!done.count(p)) todo.push_back(p);
    }
    std::sort(todo.begin(), todo.end());
    fprintf(stderr, "%zu in the cache, %zu to analyse\n", done.size(), todo.size());
    std::FILE* out = fopen(cachePath.c_str(), "ab");
    if (!out) return 1;
    std::mutex mu;
    std::atomic<size_t> next{ 0 }, finished{ 0 };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < todo.size();) {
                mixan::Analyzer an;
                loud::Meter::Result lr;
                const bool ok = loud::measureFile(todo[i], lr, nullptr, &an);
                const mixan::Info info = ok ? an.finish() : mixan::Info{};
                TrackInfo ti;
                std::string sung;
                const bool tags = ffReadInfo(todo[i], ti, false, true);
                if (tags) sung = sungParts(ti.lyrics);
                // mood: path, artist, title, genre, Automix's analysis, the mood's features (Mood playlists' tests)
                const std::string line = mood ? todo[i] + "\t" + ti.artist + "\t" + ti.title + "\t" + ti.genre + "\t" + info.pack() + "\t" +
                                                    (info.mood.valid ? info.mood.pack() : std::string("-")) + "\n"
                                              : todo[i] + "\t" + sung + "\t" + info.pack() + "\n";
                std::lock_guard<std::mutex> lk(mu);
                fwrite(line.data(), 1, line.size(), out);
                fflush(out);
                const size_t f = ++finished;
                if (f % 100 == 0) fprintf(stderr, "%zu / %zu\n", f, todo.size());
            }
        });
    for (auto& th : pool) th.join();
    fclose(out);
    fprintf(stderr, "done: %zu analysed\n", todo.size());
    return 0;
}

// ---------------------------------------------------------------- a round

struct Track {
    std::string path, folder, name;
    std::vector<std::pair<double, double>> sung;
    bool lyrics = false;   // synced lyrics known (else nothing is known of its voice)
    mixan::Info info;
};

static const char* kindOf(const automix::Plan& p) {
    if (p.drop && p.beatmatched) return "drop";
    if (p.drop) return "bridge";
    if (p.cut) return "cut";
    if (p.beatmatched) return "beat";
    return "fade";
}

/** Two voices at once: seconds where both tracks sing while both are clearly heard in the join (each over 0.3 of its
 *  level), a's sung parts in its own time, b's in its own from the plan's offset. -1 when either track's voice is unknown. */
static double vocalOverlap(const automix::Plan& p, const Track& a, const Track& b) {
    if (!a.lyrics || !b.lyrics) return -1;
    auto sings = [](const std::vector<std::pair<double, double>>& v, double t) {
        for (const auto& s : v) if (t >= s.first && t < s.second) return true;
        return false;
    };
    double both = 0;
    constexpr double Step = 0.05;
    for (double tau = 0; tau < p.length; tau += Step) {
        automix::Gains g;
        automix::gainsAt(p, tau / p.length, g);
        if (g.in < 0.3f || g.out < 0.3f) continue;
        if (sings(a.sung, p.startOut + tau * p.speed) && sings(b.sung, p.offset + tau)) both += Step;
    }
    return both;
}

static bool renderMp3(const automix::Plan& pl, const std::string& fa, const std::string& fb, const std::string& mp3, double pre, double post) {
    constexpr int Rate = 48000;
    FfDecoder a, b;
    if (!a.open(fa, Rate, 2) || !b.open(fb, Rate, 2)) return false;
    std::vector<int16_t> pcm(4096 * 2);
    long long skipA = (long long)std::llround(std::max(0.0, pl.startOut - pre) * Rate);
    while (skipA > 0) {
        const int got = a.read(pcm.data(), int(std::min<long long>(4096, skipA)));
        if (got <= 0) break;
        skipA -= got;
    }
    b.skip(pl.offset);
    std::vector<int16_t> wav;
    automix::render(pl, Rate, pre, post, [&](int16_t* o, int n) { return a.read(o, n); }, [&](int16_t* o, int n) { return b.read(o, n); },
                    [&](const float* x, int n) {
                        for (int i = 0; i < n * 2; i++) wav.push_back(int16_t(std::lrint(std::clamp(x[i] * 32767.f, -32768.f, 32767.f))));
                    });
    // the demo fades in and out over half a second at its ends, as a clip should
    const size_t frames = wav.size() / 2, ramp = Rate / 2;
    for (size_t i = 0; i < std::min(ramp, frames); i++) {
        const float gi = float(i) / ramp;
        wav[i * 2] = int16_t(wav[i * 2] * gi);
        wav[i * 2 + 1] = int16_t(wav[i * 2 + 1] * gi);
        const size_t j = frames - 1 - i;
        wav[j * 2] = int16_t(wav[j * 2] * gi);
        wav[j * 2 + 1] = int16_t(wav[j * 2 + 1] * gi);
    }
    const std::string wavPath = mp3 + ".wav";
    FILE* f = fopen(wavPath.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = uint32_t(wav.size() * 2), riff = 36 + data, fmtLen = 16, rate = Rate, byteRate = Rate * 4;
    const uint16_t pcmFmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcmFmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(wav.data(), 2, wav.size(), f);
    fclose(f);
    const std::string cmd = "ffmpeg -v error -y -i \"" + wavPath + "\" -codec:a libmp3lame -b:a 192k \"" + mp3 + "\"";
    const int rc = system(cmd.c_str());
    fs::remove(fs::u8path(wavPath));
    return rc == 0;
}

static std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

static std::string tuneJson(const automix::Tune& t) {
    char b[768];
    snprintf(b, sizeof b,
             "{\"dropBars\": %d, \"bridgeFrac\": %.3f, \"exitBars\": %d, \"tempoBridgeMax\": %d, \"tempoCutLead\": %d, \"cutFade\": %.3f, "
             "\"cutEcho\": %s, \"fadeScale\": %.2f, \"inLevel\": %.2f, \"lpFrom\": %.0f, \"outPow\": %.1f, \"bridgeOutPow\": %.1f, \"noSkip\": %s, \"noCut\": %s, \"gate\": %d, "
             "\"gateLevel\": %.2f, \"gateDuck\": %.2f, \"gateEvery\": %d, \"clashBeats\": %d, \"clashBars\": %d, \"blendFrom\": %d, \"beatMaxSec\": %.0f}",
             t.dropBars, double(t.bridgeFrac), t.exitBars, t.tempoBridgeMax, t.tempoCutLead, t.cutFade, t.cutEcho ? "true" : "false", t.fadeScale,
             double(t.inLevel), t.lpFrom, double(t.outPow), double(t.bridgeOutPow), t.noSkip ? "true" : "false", t.noCut ? "true" : "false", t.gate,
             double(t.gateLevel), double(t.gateDuck), t.gateEvery, t.clashBeats, t.clashBars, t.blendFrom, t.beatMaxSec);
    return b;
}

/** Another Tune for a kind of join: every number that kind uses, drawn from its range (round 1 explores at random). */
static automix::Tune variant(const std::string& kind, std::mt19937& rng) {
    auto pick = [&](std::initializer_list<double> v) { return *(v.begin() + std::uniform_int_distribution<int>(0, int(v.size()) - 1)(rng)); };
    automix::Tune t;
    if (kind == "drop") {
        t.dropBars = int(pick({ 8, 16 }));
        t.bridgeFrac = float(pick({ 0.125, 0.25, 0.375, 0.5 }));
        t.exitBars = int(pick({ 1, 2, 4 }));
    }
    if (kind == "bridge") {
        t.tempoBridgeMax = int(pick({ 4, 8 }));
        t.tempoCutLead = int(pick({ 0, 1, 2 }));
    }
    if (kind == "drop" || kind == "bridge") {
        t.inLevel = float(pick({ 0.7, 0.8, 0.9, 1.0 }));
        t.lpFrom = pick({ 150, 250, 500, 1000 });
        t.outPow = float(pick({ 1, 2, 3 }));
    }
    if (kind == "cut") {
        t.cutFade = pick({ 0.03, 0.15, 0.5 });
        t.cutEcho = pick({ 0, 1 }) > 0.5;
    }
    if (kind == "fade") t.fadeScale = pick({ 0.5, 0.75, 1.5, 2.5 });   // a 2 s fade: 1 to 5 s
    return t;
}

/**
 * Round 2 on (the user's round 1, 2026-10-29: drops and bridges can be excellent, cuts never are, "too early" was music
 * skipped; the incoming low-pass from 1 kHz, its level 0.8, a short bass-less bridge won): from round 1's winners, no music
 * skipped, no cuts; around them, the outgoing exit (a clean cut on the drop: outPow 12), the low-pass, the level, the bridge.
 */
static automix::Tune variant2(const std::string& kind, std::mt19937& rng) {
    auto pick = [&](std::initializer_list<double> v) { return *(v.begin() + std::uniform_int_distribution<int>(0, int(v.size()) - 1)(rng)); };
    automix::Tune t;
    t.noSkip = t.noCut = true;
    t.lpFrom = pick({ 700, 1000, 1500 });
    t.inLevel = float(pick({ 0.8, 0.9 }));
    t.bridgeFrac = float(pick({ 0.125, 0.25 }));
    t.dropBars = int(pick({ 8, 16 }));
    t.exitBars = int(pick({ 2, 4 }));
    t.outPow = float(pick({ 1, 2, 12 }));
    t.tempoCutLead = int(pick({ 0, 1, 2 }));
    t.fadeScale = pick({ 0.75, 1, 1.5 });
    (void)kind;
    return t;
}

/**
 * Round 3 on (the user's round 2, 2026-10-29: no cuts and no music skipped helped, Hope → Crown and Blue → Summertime got
 * 10s; the complaint left is overlap): round 2's best, fixed, is the reference (version 0, not V1.0.0: its cuts and long
 * overlaps are known worse) ...
 */
static automix::Tune base3() {
    automix::Tune t;
    t.noSkip = t.noCut = true;
    t.lpFrom = 1000;
    t.inLevel = 0.8f;
    t.dropBars = 8;
    t.exitBars = 2;
    t.outPow = 2;
    t.bridgeFrac = 0.25f;
    t.tempoCutLead = 2;
    t.fadeScale = 1;
    t.tempoBridgeMax = 8;
    return t;
}

/** ... and around it, shorter overlaps: the drop's lead-in and the outgoing tail, a clean cut on the drop (outPow 12) with
 *  bridges' own exit (12 left a hole there), a shorter lead-in on bridges ("still too early"), the fade's length (the
 *  former cuts, now fades, were all 1.5 in round 2). */
static automix::Tune variant3(const std::string& kind, std::mt19937& rng) {
    auto pick = [&](std::initializer_list<double> v) { return *(v.begin() + std::uniform_int_distribution<int>(0, int(v.size()) - 1)(rng)); };
    automix::Tune t = base3();
    t.dropBars = int(pick({ 4, 8 }));
    t.exitBars = int(pick({ 1, 2 }));
    t.outPow = float(pick({ 2, 12 }));
    t.bridgeOutPow = float(pick({ 1, 2 }));
    t.tempoBridgeMax = int(pick({ 2, 4, 8 }));
    t.fadeScale = pick({ 0.5, 1, 2 });
    t.lpFrom = pick({ 700, 1000 });
    (void)kind;
    return t;
}

/**
 * Round 4 on (the user's round 3, 2026-10-29: the outgoing song gone a bar after the drop won on drops, "a little too steep"
 * with a clean cut; fades settled; bridges not): round 3's best as the reference (base3 with exitBars 1), and two versions
 * chosen, not drawn, for what is open: on drops the gate (two Spotify mixes the user sent: 1 the next song in stabs, 2 the
 * outgoing one a stab a bar then filtered out) or the outgoing exit's steepness (outPow 5, 12; the reference 2); on the beat
 * the gate; on bridges the exit's shape (bridgeOutPow 1, 2) and the lead-in (tempoBridgeMax 8, 4).
 */
static automix::Tune base4() {
    automix::Tune t = base3();
    t.exitBars = 1;
    return t;
}
static automix::Tune variant4(const std::string& kind, int nth, int version, int attempt) {
    automix::Tune t = base4();
    if (kind == "drop" && nth < 2) t.gate = version;   // the gate: 1, 2
    else if (kind == "drop") t.outPow = version == 1 ? 5.f : 12.f;
    else if (kind == "beat") t.gate = version;
    else if (kind == "bridge") {   // the reference's exit is outPow's 2 and its lead-in 8 bars: 1, the softer exit; 2, a
        // shorter lead-in (4, then 2), or where the lead-in is short already, an exit between (1.5)
        if (version == 1) t.bridgeOutPow = 1.f;
        else if (attempt == 0) t.tempoBridgeMax = 4;
        else if (attempt == 1) t.tempoBridgeMax = 2;
        else t.bridgeOutPow = 1.5f;
    }
    else t.fadeScale = version == 1 ? 1.5 : 0.75;   // (a fade: none are planned in round 4)
    return t;
}

/**
 * Round 5 (the user's round 4, 2026-10-29: "very good batch"; V1.1.0 frozen from it): V1.1.0 itself as the reference (base4
 * with the exits it froze: outPow 5 on drops, 2 on bridges), and for what V1.2.0 is to learn: on drops the gate louder (the
 * user: "it could be higher in volume"): the stabs at full level with the outgoing song dipped 3 dB under each (1), and the
 * same with a stab on every beat (2); on the beat (the weakest kind: 2 and 5, "short, a smart crossfade") a longer, real blend
 * (1: up to 32 s rather than 20, 16 beats even with clashing keys, the incoming harmony muffled, both up and a full EQ swap
 * from 8 beats), and that
 * with the louder stabs over its lead-in (2); where the blend cannot be longer (a join by the structure: its sections decide its
 * length), the drops' two versions.
 */
static automix::Tune base5() {
    automix::Tune t = base4();
    t.outPow = 5;
    t.bridgeOutPow = 2;
    return t;
}
static automix::Tune variant5(const std::string& kind, int version, int attempt) {
    automix::Tune t = base5();
    auto louder = [&] {
        t.gate = 1;
        t.gateLevel = 1;
        t.gateDuck = 0.3f;
    };
    if (kind == "drop" || attempt > 0) {   // (on the beat, where the blend cannot be longer, a join by the structure: as drops)
        louder();
        if (version == 2) t.gateEvery = 1;
    } else {   // on the beat
        t.clashBeats = 16;
        t.clashBars = 4;
        t.blendFrom = 8;
        t.beatMaxSec = 32;   // 32 beats where 16 were the most under 20 s
        if (version == 2) louder();
    }
    return t;
}

static int round(const std::string& cachePath, const std::string& outDir, int roundNo, unsigned seed, const std::string& planPath) {
    std::vector<Track> all;
    {
        std::ifstream in(cachePath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            Track tr;
            tr.path = line.substr(0, t1);
            const std::string sung = line.substr(t1 + 1, t2 - t1 - 1);
            tr.lyrics = !sung.empty();
            tr.sung = parseSung(sung);
            if (!mixan::Info::unpack(line.substr(t2 + 1), tr.info) || !tr.info.valid) continue;
            const fs::path pp = fs::u8path(tr.path);
            tr.folder = utf8(pp.parent_path().filename());
            tr.name = utf8(pp.stem());
            all.push_back(std::move(tr));
        }
    }
    std::map<std::string, std::vector<int>> byFolder;
    for (int i = 0; i < int(all.size()); i++) byFolder[all[size_t(i)].folder].push_back(i);
    // every folder as an Automix playlist (V1.0.0's order), and its joins by kind
    struct Join { int a, b; std::string kind; };
    std::map<std::string, std::vector<Join>> joins;
    for (auto& [folder, idx] : byFolder) {
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return all[size_t(x)].path < all[size_t(y)].path; });
        std::vector<const mixan::Info*> ptr;
        for (int i : idx) ptr.push_back(&all[size_t(i)].info);
        const std::vector<int> ord = automix::order(ptr, -1, automix::Model::V1_0_0);
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const Track &a = all[size_t(idx[size_t(ord[k])])], &b = all[size_t(idx[size_t(ord[k + 1])])];
            const automix::Plan p = automix::plan(&a.info, a.info.seconds, &b.info, 4, nullptr, automix::Model::V1_0_0);
            if (p.off) continue;
            joins[kindOf(p)].push_back({ idx[size_t(ord[k])], idx[size_t(ord[k + 1])], kindOf(p) });
        }
    }
    for (const auto& [k, v] : joins) fprintf(stderr, "%s: %zu joins\n", k.c_str(), v.size());
    std::mt19937 rng(seed);
    std::vector<Join> chosen;
    std::set<std::string> usedFolders;
    // a round's plan (round 2 on: <out dir>'s plan.tsv): "retest<TAB>a<TAB>b" joins heard before, "exclude<TAB>a<TAB>b" pairs
    // not to pick again, "new<TAB>kind<TAB>count" new joins of a kind; without one, two of each kind (round 1)
    std::vector<std::pair<std::string, int>> wanted = { { "drop", 2 }, { "bridge", 2 }, { "cut", 2 }, { "fade", 2 } };
    std::set<std::pair<std::string, std::string>> excluded;
    if (!planPath.empty()) {
        wanted.clear();
        std::map<std::string, int> byPath;
        for (int i = 0; i < int(all.size()); i++) byPath[all[size_t(i)].path] = i;
        std::ifstream in(fs::u8path(planPath), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, '\t');) f.push_back(x);
            if (f.size() < 3) continue;
            if (f[0] == "new") { wanted.push_back({ f[1], atoi(f[2].c_str()) }); continue; }
            excluded.insert({ f[1], f[2] });
            if (f[0] != "retest") continue;
            if (!byPath.count(f[1]) || !byPath.count(f[2])) { fprintf(stderr, "retest: not in the cache: %s\n", f[1].c_str()); continue; }
            const int ia = byPath[f[1]], ib = byPath[f[2]];
            const automix::Plan p = automix::plan(&all[size_t(ia)].info, all[size_t(ia)].info.seconds, &all[size_t(ib)].info, 4, nullptr, automix::Model::V1_0_0);
            chosen.push_back({ ia, ib, kindOf(p) });
            usedFolders.insert(all[size_t(ia)].folder);
        }
    }
    for (const auto& [kind, count] : wanted) {
        auto v = joins[kind];
        std::shuffle(v.begin(), v.end(), rng);
        int got = 0;
        for (const Join& j : v) {   // from different folders where it can, never a pair heard before
            if (got == count) break;
            if (excluded.count({ all[size_t(j.a)].path, all[size_t(j.b)].path })) continue;
            if (usedFolders.count(all[size_t(j.a)].folder) && v.size() > 8) continue;
            usedFolders.insert(all[size_t(j.a)].folder);
            chosen.push_back(j);
            got++;
        }
    }
    struct Demo { int join; int version; automix::Tune tune; };
    std::vector<Demo> demos;
    for (int k = 0; k < int(chosen.size()); k++) {
        const automix::Tune ref = roundNo >= 5 ? base5() : roundNo >= 4 ? base4() : roundNo >= 3 ? base3() : automix::Tune{};   // V1.0.0's numbers (round 3: round 2's best; 4: round 3's; 5: V1.1.0)
        demos.push_back({ k, 0, ref });
        // the three versions all different (a demo heard twice is wasted); round 3 on, different as heard: the plan's timing
        // and exit (numbers its kind of join does not use, or the low-pass alone, made versions that sounded the same)
        const Track &ja = all[size_t(chosen[size_t(k)].a)], &jb = all[size_t(chosen[size_t(k)].b)];
        auto key = [&](const automix::Tune& t) {
            if (roundNo < 3) return tuneJson(t);
            const automix::Plan p = automix::planTuned(&ja.info, ja.info.seconds, &jb.info, 4, t);
            char b[160];
            snprintf(b, sizeof b, "%s %.2f %.2f %.2f %.1f %.3f %.3f %d %d %d %d", kindOf(p), p.startOut, p.length, p.offset, double(p.outPow), p.cross, p.bassCut, p.gate,
                     p.gateEvery, int(p.join.volume), int(p.join.filter));
            return std::string(b);
        };
        std::set<std::string> seen{ key(ref) };
        for (int v = 1; v <= 2; v++) {
            int attempt = 0;
            auto draw = [&] {
                const std::string& kind = chosen[size_t(k)].kind;
                if (roundNo >= 5) return variant5(kind, v, attempt++);
                if (roundNo >= 4) {   // chosen: the how-many-th join of its kind decides what it tests
                    int nth = 0;
                    for (int q = 0; q < k; q++) nth += chosen[size_t(q)].kind == kind;
                    return variant4(kind, nth, v, attempt++);
                }
                return roundNo >= 3 ? variant3(kind, rng) : roundNo >= 2 ? variant2(kind, rng) : variant(kind, rng);
            };
            automix::Tune t = draw();
            for (int tries = 0; seen.count(key(t)) && tries < (roundNo >= 4 ? 3 : 200); tries++) t = draw();
            if (!seen.insert(key(t)).second) fprintf(stderr, "join %d v%d: no different version found\n", k, v);
            demos.push_back({ k, v, t });
        }
    }
    std::shuffle(demos.begin(), demos.end(), rng);
    fs::create_directories(fs::u8path(outDir));
    std::ofstream js(fs::u8path(outDir + "/round.json"), std::ios::binary);
    js << "{\n  \"round\": " << roundNo << ",\n  \"seed\": " << seed << ",\n  \"demos\": [\n";
    for (size_t d = 0; d < demos.size(); d++) {
        const Demo& dm = demos[d];
        const Join& j = chosen[size_t(dm.join)];
        const Track &a = all[size_t(j.a)], &b = all[size_t(j.b)];
        const automix::Plan p = automix::planTuned(&a.info, a.info.seconds, &b.info, 4, dm.tune);
        char file[32];
        snprintf(file, sizeof file, "%02zu.mp3", d + 1);
        const double pre = 10, post = 10;
        const bool ok = renderMp3(p, a.path, b.path, outDir + "/" + file, pre, post);
        double tempo = 0;
        if (a.info.bpm > 0 && b.info.bpm > 0) {
            tempo = 9;
            for (double m : { 1.0, 2.0, 0.5 }) tempo = std::min(tempo, std::fabs(b.info.bpm * m / a.info.bpm - 1));
        }
        const double vo = vocalOverlap(p, a, b);
        fprintf(stderr, "%02zu %s v%d %s -> %s: %.1f s%s, voices together %.1f s\n", d + 1, j.kind.c_str(), dm.version, a.name.c_str(), b.name.c_str(),
                p.length, ok ? "" : " (RENDER FAILED)", vo);
        js << "    {\"file\": " << jsonStr(file) << ", \"join\": " << dm.join << ", \"version\": " << dm.version << ", \"kind\": " << jsonStr(j.kind)
           << ", \"planned\": " << jsonStr(kindOf(p)) << ", \"a\": " << jsonStr(a.path) << ", \"b\": " << jsonStr(b.path)
           << ", \"aName\": " << jsonStr(a.name) << ", \"bName\": " << jsonStr(b.name) << ", \"folder\": " << jsonStr(a.folder)
           << ", \"bpmA\": " << a.info.bpm << ", \"bpmB\": " << b.info.bpm << ", \"tempoGap\": " << tempo << ", \"keyDistance\": " << p.keyDistance
           << ", \"length\": " << p.length << ", \"startOut\": " << p.startOut << ", \"cross\": " << p.cross << ", \"aEnd\": " << a.info.end << ", \"aFadeStart\": " << a.info.fadeStart << ", \"joinAt\": " << pre << ", \"voicesTogether\": " << vo << ", \"lyricsA\": " << (a.lyrics ? "true" : "false")
           << ", \"lyricsB\": " << (b.lyrics ? "true" : "false") << ", \"tune\": " << tuneJson(dm.tune) << "}" << (d + 1 < demos.size() ? "," : "") << "\n";
    }
    js << "  ]\n}\n";
    return 0;
}

/** mixtrain same <cache.tsv>: the frozen model (Automix V1.1.0) against the tunable planner with round 4's best (base4, a
 *  drop's exit cos^5, a bridge's cos^2): every join of every folder in V1.0.0's order, planned both ways, must match. */
/** mixtrain moods <mood.tsv>: the app's own sorting (mood.cpp) of a mood run, each track's mood, to compare with the script. */
static int moodsOf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    std::vector<std::string> names;
    std::vector<mixan::Mood> m;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        size_t a = 0;
        for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
        f.push_back(line.substr(a));
        mixan::Mood x;
        if (f.size() < 6 || !mixan::Mood::unpack(f[5], x)) continue;
        names.push_back(f[1] + " - " + f[2]);
        m.push_back(x);
    }
    std::vector<const mixan::Mood*> p;
    for (const auto& x : m) p.push_back(&x);
    mood::Index idx;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 20; k++) idx.build(p);   // (timed: the app does it on the main thread)
    fprintf(stderr, "build: %.2f ms each\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20);
    int count[mood::Count] = {};
    for (size_t i = 0; i < names.size(); i++) {
        count[idx.best[i]]++;
        printf("%s\t%s\t%.2f\n", mood::name(idx.best[i]), names[i].c_str(), double(idx.score[i]));
    }
    for (int c = 0; c < mood::Count; c++) fprintf(stderr, "%s %d  ", mood::name(c), count[c]);
    fprintf(stderr, "\n");
    return 0;
}

static int same(const std::string& cachePath) {
    std::vector<mixan::Info> all;
    std::vector<std::string> paths;
    std::ifstream in(cachePath, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        mixan::Info i;
        if (!mixan::Info::unpack(line.substr(t2 + 1), i) || !i.valid) continue;
        all.push_back(i);
        paths.push_back(line.substr(0, t1));
    }
    automix::Tune t = base4();
    t.outPow = 5;
    t.bridgeOutPow = 2;
    std::map<std::string, std::vector<int>> byFolder;
    for (int i = 0; i < int(all.size()); i++) byFolder[utf8(fs::u8path(paths[size_t(i)]).parent_path().filename())].push_back(i);
    int n = 0, bad = 0;
    std::map<std::string, int> kinds;
    for (auto& [folder, idx] : byFolder) {
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return paths[size_t(x)] < paths[size_t(y)]; });
        std::vector<const mixan::Info*> ptr;
        for (int i : idx) ptr.push_back(&all[size_t(i)]);
        const std::vector<int> ord = automix::order(ptr, -1, automix::Model::V1_1_0);
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const mixan::Info &a = *ptr[size_t(ord[k])], &b = *ptr[size_t(ord[k + 1])];
            const automix::Plan x = automix::plan(&a, a.seconds, &b, 4, nullptr, automix::Model::V1_1_0);
            const automix::Plan y = automix::planTuned(&a, a.seconds, &b, 4, t);
            n++;
            kinds[kindOf(x)]++;
            const bool eq = x.off == y.off && x.startOut == y.startOut && x.offset == y.offset && x.length == y.length && x.beatmatched == y.beatmatched &&
                            x.drop == y.drop && x.cut == y.cut && x.cross == y.cross && x.bassCut == y.bassCut && x.speed == y.speed && x.easeIn == y.easeIn &&
                            x.stretchFrom == y.stretchFrom && x.outPow == y.outPow && x.inLevel == y.inLevel && x.lpFrom == y.lpFrom && x.cutFade == y.cutFade &&
                            x.gate == y.gate && x.loopFrom == y.loopFrom && x.loopTo == y.loopTo && x.join.preset == y.join.preset &&
                            x.join.volume == y.join.volume && x.join.eq == y.join.eq && x.join.filter == y.join.filter && x.join.effect == y.join.effect;
            if (!eq && bad++ < 5) fprintf(stderr, "differs: %s -> %s\n", paths[size_t(idx[size_t(ord[k])])].c_str(), paths[size_t(idx[size_t(ord[k + 1])])].c_str());
        }
    }
    printf("%d joins, %d differ;", n, bad);
    for (const auto& [k, c] : kinds) printf(" %s %d", k.c_str(), c);
    printf("\n");
    return bad ? 1 : 0;
}

/** Two plans the same in every field the mixing and the show read (the model that made them aside). */
static bool samePlan(const automix::Plan& x, const automix::Plan& y) {
    return x.off == y.off && x.startOut == y.startOut && x.offset == y.offset && x.length == y.length && x.beatmatched == y.beatmatched &&
           x.beatmatchable == y.beatmatchable && x.bpm == y.bpm && x.beats == y.beats && x.join == y.join && x.loopFrom == y.loopFrom &&
           x.loopTo == y.loopTo && x.echoAt == y.echoAt && x.echoDelay == y.echoDelay && x.speed == y.speed && x.easeIn == y.easeIn &&
           x.stretchFrom == y.stretchFrom && x.onBars == y.onBars && x.structured == y.structured && x.drop == y.drop && x.cut == y.cut &&
           x.bassCut == y.bassCut && x.cross == y.cross && x.keyDistance == y.keyDistance && x.keyClash == y.keyClash &&
           x.inLevel == y.inLevel && x.lpFrom == y.lpFrom && x.outPow == y.outPow && x.cutFade == y.cutFade && x.gate == y.gate &&
           (!x.gate || (x.gateLevel == y.gateLevel && x.gateDuck == y.gateDuck && x.gateEvery == y.gateEvery));
}

/**
 * mixtrain filesame <cache.tsv> <model file>: a model file (automix::ModelFile, format 1) against what it stands for, on every
 * join of every folder in its playlist order. A built-in model's file (Automix V1.1.0): the built-in model itself, every field.
 * Automix V1.2.0 (frozen from round 5, 2026-10-30: "freeze from round 5"): what round 5 rendered for each kind of join it
 * plays: drops and joins by the structure the louder stabs on every beat (round 5's v2 there), other joins on the beat the
 * longer blend (v1), the rest V1.1.0 (the reference).
 */
static int fileSame(const std::string& cachePath, const std::string& modelPath) {
    std::ifstream mf(fs::u8path(modelPath), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    automix::ModelFile m;
    std::string why;
    if (!m.parse(text, &why)) { fprintf(stderr, "%s: %s\n", modelPath.c_str(), why.c_str()); return 2; }
    automix::Model builtin;
    const bool isBuiltin = automix::modelFromName(m.name, builtin);
    std::vector<mixan::Info> all;
    std::vector<std::string> paths;
    std::ifstream in(cachePath, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        mixan::Info i;
        if (!mixan::Info::unpack(line.substr(t2 + 1), i) || !i.valid) continue;
        all.push_back(i);
        paths.push_back(line.substr(0, t1));
    }
    std::map<std::string, std::vector<int>> byFolder;
    for (int i = 0; i < int(all.size()); i++) byFolder[utf8(fs::u8path(paths[size_t(i)]).parent_path().filename())].push_back(i);
    int n = 0, bad = 0;
    std::map<std::string, int> kinds, was;
    for (auto& [folder, idx] : byFolder) {
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return paths[size_t(x)] < paths[size_t(y)]; });
        std::vector<const mixan::Info*> ptr;
        for (int i : idx) ptr.push_back(&all[size_t(i)]);
        const std::vector<int> ord = automix::order(ptr, -1, automix::Model::V1_1_0);
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const mixan::Info &a = *ptr[size_t(ord[k])], &b = *ptr[size_t(ord[k + 1])];
            const automix::Plan x = automix::plan(&a, a.seconds, &b, 4, nullptr, m);
            automix::Plan y;
            if (isBuiltin) y = automix::plan(&a, a.seconds, &b, 4, nullptr, builtin);
            else {   // round 5's own version of that kind of join
                const bool stabs = x.beatmatched && (x.drop || x.structured);
                automix::Tune t = x.beatmatched ? variant5(stabs ? "drop" : "beat", stabs ? 2 : 1, 0) : base5();
                y = automix::planTuned(&a, a.seconds, &b, 4, t);
                was[kindOf(automix::plan(&a, a.seconds, &b, 4, nullptr, automix::Model::V1_1_0))]++;
            }
            n++;
            kinds[std::string(kindOf(x)) + (x.gate ? "+stabs" : "")]++;
            if (!samePlan(x, y) && bad++ < 5)
                fprintf(stderr, "differs: %s -> %s\n", paths[size_t(idx[size_t(ord[k])])].c_str(), paths[size_t(idx[size_t(ord[k + 1])])].c_str());
        }
    }
    printf("%s from %s: %d joins, %d differ;", m.name.c_str(), modelPath.c_str(), n, bad);
    for (const auto& [k, c] : kinds) printf(" %s %d", k.c_str(), c);
    if (!was.empty()) {
        printf("\n  V1.1.0 on the same joins:");
        for (const auto& [k, c] : was) printf(" %s %d", k.c_str(), c);
    }
    printf("\n");
    return bad ? 1 : 0;
}

/** mixtrain modelfile <name> <out>: a model's file: Automix V1.1.0 (the built-in one's numbers), or V1.2.0 (round 5). */
static int modelFile(const std::string& name, const std::string& out) {
    automix::ModelFile m;
    automix::Model b;
    if (automix::modelFromName(name, b)) m = automix::builtinFile(b);
    else if (name == "Automix V1.2.0") {
        // round 5's winners: the longer blend on the beat (v1), and on drops and joins by the structure the stabs at full
        // level, the outgoing song dipped under each, on every beat (v2)
        m.format = 1;
        m.name = name;
        m.mixan = mixan::Version;
        m.tune = variant5("beat", 1, 0);
        m.tune.clashBars = 2;   // (joins by the structure were rated with the drops' versions, which keep V1.1.0's)
        const automix::Tune s = variant5("drop", 2, 0);
        m.tune.gate = s.gate;
        m.tune.gateLevel = s.gateLevel;
        m.tune.gateDuck = s.gateDuck;
        m.tune.gateEvery = s.gateEvery;
        m.gateOn = automix::ModelFile::GateDrop | automix::ModelFile::GateStructure;
    } else { fprintf(stderr, "no such model: %s\n", name.c_str()); return 2; }
    std::ofstream(fs::u8path(out), std::ios::binary) << m.text();
    automix::ModelFile back;
    std::string why;
    if (!back.parse(m.text(), &why) || back.text() != m.text()) { fprintf(stderr, "does not read back: %s\n", why.c_str()); return 1; }
    printf("%s", m.text().c_str());
    return 0;
}

// ============================================================ self-serve training (2026-10-30)
// For anyone training an Automix model of their own (the user: "self serve for the end user who wants to train a model like
// ours"; Songate's own rounds keep `mixtrain round`). A round is described by a spec file, not code; tools/mixtrain-fit.py
// reads the ratings; `mixtrain freeze` writes the model file. Specs and candidates are "key value" lines, as model files.

/** A spec or candidate file: each line's words ('#' starts a comment); "retest a | b" keeps its two paths whole. */
static std::vector<std::vector<std::string>> specLines(const std::string& path, std::string& err) {
    std::vector<std::vector<std::string>> out;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) { err = "cannot open " + path; return out; }
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        std::vector<std::string> w;
        if (line.rfind("retest ", 0) == 0) {
            const size_t bar = line.find(" | ");
            if (bar == std::string::npos) { err = "retest needs two paths: retest <a> | <b>"; return {}; }
            w = { "retest", line.substr(7, bar - 7), line.substr(bar + 3) };
            for (auto& s : w) { while (!s.empty() && s.back() == ' ') s.pop_back(); while (!s.empty() && s[0] == ' ') s.erase(0, 1); }
        } else {
            std::stringstream ss(line);
            for (std::string x; ss >> x;) w.push_back(x);
        }
        if (!w.empty()) out.push_back(std::move(w));
    }
    return out;
}

static bool readModelFile(const std::string& path, automix::ModelFile& m, std::string& why) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) { why = "cannot open " + path; return false; }
    return m.parse(std::string(std::istreambuf_iterator<char>(in), {}), &why);
}

/** A model with some of its numbers changed (as a model file's later lines win over its earlier ones). */
static bool withChanges(const automix::ModelFile& base, const std::vector<std::pair<std::string, std::string>>& ch, automix::ModelFile& out,
                        std::string& why) {
    std::string t = base.text();
    for (const auto& [k, v] : ch) t += k + " " + v + "\n";
    return out.parse(t, &why);
}

/** The kinds a self-serve round names: on the drop, a bridge between tempos, by the structure, on the beat, a cut, a fade. */
static const char* kindSpec(const automix::Plan& p) {
    if (p.drop && p.beatmatched) return "drop";
    if (p.drop) return "bridge";
    if (p.cut) return "cut";
    if (p.structured) return "structure";
    if (p.beatmatched) return "beat";
    return "fade";
}

/** What a demo sounds like, for telling versions apart (a number its kind of join never reads changes nothing heard). */
static std::string heard(const automix::Plan& p) {
    char b[200];
    snprintf(b, sizeof b, "%s %.3f %.3f %.3f %.2f %.3f %.3f %d %d %.2f %.2f %d %d %d %.0f %.2f", kindSpec(p), p.startOut, p.length, p.offset,
             double(p.outPow), p.cross, p.bassCut, p.gate, p.gate ? p.gateEvery : 0, p.gate ? double(p.gateLevel) : 0.0,
             p.gate ? double(p.gateDuck) : 0.0, int(p.join.volume), int(p.join.eq), int(p.join.filter), p.lpFrom, double(p.inLevel));
    return b;
}

static std::vector<Track> loadCache(const std::string& cachePath) {
    std::vector<Track> all;
    std::ifstream in(fs::u8path(cachePath), std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        Track tr;
        tr.path = line.substr(0, t1);
        const std::string sung = line.substr(t1 + 1, t2 - t1 - 1);
        tr.lyrics = !sung.empty();
        tr.sung = parseSung(sung);
        if (!mixan::Info::unpack(line.substr(t2 + 1), tr.info) || !tr.info.valid) continue;
        const fs::path pp = fs::u8path(tr.path);
        tr.folder = utf8(pp.parent_path().filename());
        tr.name = utf8(pp.stem());
        all.push_back(std::move(tr));
    }
    return all;
}

static std::string slash(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }

/** Every pair heard in the round.json files under a folder (a round's "a" and "b"). */
static std::set<std::pair<std::string, std::string>> heardPairs(const std::string& folder) {
    std::set<std::pair<std::string, std::string>> out;
    std::error_code ec;
    if (!fs::exists(fs::u8path(folder), ec)) return out;
    for (const auto& e : fs::recursive_directory_iterator(fs::u8path(folder), ec)) {
        if (e.path().filename() != "round.json") continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string t((std::istreambuf_iterator<char>(in)), {});
        auto field = [&](size_t from, const char* key, size_t& end) {
            const std::string k = std::string("\"") + key + "\": \"";
            const size_t a = t.find(k, from);
            if (a == std::string::npos) return std::string();
            std::string v;
            size_t i = a + k.size();
            for (; i < t.size() && t[i] != '"'; i++) {
                if (t[i] == '\\' && i + 1 < t.size()) i++;
                v += t[i];
            }
            end = i;
            return v;
        };
        for (size_t at = 0;;) {
            size_t e1 = 0, e2 = 0;
            const std::string a = field(at, "a", e1);
            if (a.empty()) break;
            const std::string b = field(e1, "b", e2);
            out.insert({ slash(a), slash(b) });
            at = e2;
        }
    }
    return out;
}

/**
 * mixtrain build <cache.tsv> <round.spec> <out folder>: a round from a spec (examples/training/round.spec explains each
 * line): joins of each kind named, picked at random (the seed) from different folders of the library ordered as Automix
 * playlists by the base model, never a pair heard in an earlier round; each rendered three ways: the base model (the
 * reference) and the spec's two versions of that kind. A join where two versions would sound the same is passed over for
 * another. NN.mp3 (needs ffmpeg on the PATH), round.json, and the spec copied beside them; then mixtrain-page.py.
 */
static int build(const std::string& cachePath, const std::string& specPath, const std::string& outDir) {
    std::string err;
    const auto lines = specLines(specPath, err);
    if (!err.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    int roundNo = 1;
    unsigned seed = 1;
    std::string basePath, exclude;
    std::vector<std::pair<std::string, int>> wanted;
    std::vector<std::pair<std::string, std::string>> retest;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> versions[3];   // [1], [2]: kind → changes
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> alts[3];       // … and each one's alternative
    const fs::path specDir = fs::u8path(specPath).parent_path();
    auto rel = [&](const std::string& p) { const fs::path f = fs::u8path(p); return f.is_absolute() || fs::exists(f) ? p : utf8(specDir / f); };
    for (const auto& w : lines) {
        const std::string& k = w[0];
        if (k == "round" && w.size() == 2) roundNo = atoi(w[1].c_str());
        else if (k == "seed" && w.size() == 2) seed = unsigned(strtoul(w[1].c_str(), nullptr, 10));
        else if (k == "base" && w.size() == 2) basePath = rel(w[1]);
        else if (k == "exclude" && w.size() == 2) exclude = rel(w[1]);
        else if (k == "joins" && w.size() == 3) wanted.push_back({ w[1], atoi(w[2].c_str()) });
        else if (k == "retest" && w.size() == 3) retest.push_back({ slash(w[1]), slash(w[2]) });
        else if ((k == "version" || k == "alternative") && w.size() >= 5 && w.size() % 2 == 1 && (w[2] == "1" || w[2] == "2")) {
            auto& ch = (k == "version" ? versions : alts)[atoi(w[2].c_str())][w[1]];
            for (size_t i = 3; i + 1 < w.size(); i += 2) ch.push_back({ w[i], w[i + 1] });
        } else { fprintf(stderr, "%s: a line I do not understand: %s\n", specPath.c_str(), k.c_str()); return 2; }
    }
    automix::ModelFile base;
    if (basePath.empty() || !readModelFile(basePath, base, err)) { fprintf(stderr, "the base model: %s\n", basePath.empty() ? "none (base <file>.model)" : err.c_str()); return 2; }
    // each kind's three models: the base, and its two versions (checked now: a number the format does not know stops here)
    std::map<std::string, automix::ModelFile> model[3], alt[3];
    std::set<std::string> kinds;
    for (const auto& [kind, n] : wanted) kinds.insert(kind);
    for (int v = 1; v <= 2; v++) for (const auto& [kind, ch] : versions[v]) kinds.insert(kind);
    for (const std::string& kind : kinds) {
        for (int v = 1; v <= 2; v++) {
            if (!versions[v].count(kind)) { fprintf(stderr, "%s: no version %d for %s joins\n", specPath.c_str(), v, kind.c_str()); return 2; }
            if (!withChanges(base, versions[v][kind], model[v][kind], err)) { fprintf(stderr, "version %d of %s: %s\n", v, kind.c_str(), err.c_str()); return 2; }
            if (alts[v].count(kind) && !withChanges(base, alts[v][kind], alt[v][kind], err)) { fprintf(stderr, "alternative %d of %s: %s\n", v, kind.c_str(), err.c_str()); return 2; }
        }
        model[0][kind] = base;
    }
    std::vector<Track> all = loadCache(cachePath);
    if (all.empty()) { fprintf(stderr, "%s: no analysed tracks (mixtrain analyse first)\n", cachePath.c_str()); return 2; }
    struct Join { int a, b; std::string kind; bool alt[3] = {}; };   // alt: that version is its alternative (it sounded as one before it)
    std::map<std::string, std::vector<Join>> joins;
    std::map<std::string, std::vector<int>> byFolder;
    for (int i = 0; i < int(all.size()); i++) byFolder[all[size_t(i)].folder].push_back(i);
    for (auto& [folder, idx] : byFolder) {   // every folder as an Automix playlist
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return all[size_t(x)].path < all[size_t(y)].path; });
        std::vector<const mixan::Info*> ptr;
        for (int i : idx) ptr.push_back(&all[size_t(i)].info);
        const std::vector<int> ord = automix::order(ptr, -1, automix::Model::V1_1_0);
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const int ia = idx[size_t(ord[k])], ib = idx[size_t(ord[k + 1])];
            const automix::Plan p = automix::plan(&all[size_t(ia)].info, all[size_t(ia)].info.seconds, &all[size_t(ib)].info, 4, nullptr, base);
            if (!p.off) joins[kindSpec(p)].push_back({ ia, ib, kindSpec(p) });
        }
    }
    fprintf(stderr, "joins in the library by the base model:");
    for (const auto& [k, v] : joins) fprintf(stderr, " %s %zu", k.c_str(), v.size());
    fprintf(stderr, "\n");
    const auto excluded = exclude.empty() ? std::set<std::pair<std::string, std::string>>{} : heardPairs(exclude);
    auto modelOf = [&](const Join& j, int v) -> const automix::ModelFile& {
        if (j.alt[v] && alt[v].count(j.kind)) return alt[v][j.kind];
        return model[v].count(j.kind) ? model[v][j.kind] : base;
    };
    auto planOf = [&](const Join& j, int v) {
        const Track &a = all[size_t(j.a)], &b = all[size_t(j.b)];
        return automix::plan(&a.info, a.info.seconds, &b.info, 4, nullptr, modelOf(j, v));
    };
    // each version as heard; one that sounds as one before it takes its alternative, when the spec has one; true when all
    // three then sound apart
    auto distinct = [&](Join& j) {
        std::set<std::string> seen{ heard(planOf(j, 0)) };
        for (int v = 1; v <= 2; v++) {
            j.alt[v] = false;
            if (seen.count(heard(planOf(j, v))) && alt[v].count(j.kind)) j.alt[v] = true;
            if (!seen.insert(heard(planOf(j, v))).second) return false;
        }
        return true;
    };
    std::vector<Join> chosen;
    std::set<std::string> usedFolders;
    auto ends = [](const std::string& full, const std::string& tail) {
        return full == tail || (full.size() > tail.size() && full.compare(full.size() - tail.size(), tail.size(), tail) == 0 &&
                                full[full.size() - tail.size() - 1] == '/');
    };
    for (const auto& [ra, rb] : retest) {   // joins heard before, again (by their paths, or their ends)
        bool found = false;
        for (auto& [kind, v] : joins)
            for (Join& j : v)
                if (!found && ends(slash(all[size_t(j.a)].path), ra) && ends(slash(all[size_t(j.b)].path), rb)) {
                    if (!model[1].count(kind)) { fprintf(stderr, "retest %s: a %s join, and the spec has no versions for %s\n", ra.c_str(), kind.c_str(), kind.c_str()); return 2; }
                    if (!distinct(j)) fprintf(stderr, "retest %s -> %s: two of its versions sound the same\n", ra.c_str(), rb.c_str());
                    chosen.push_back(j);
                    usedFolders.insert(all[size_t(j.a)].folder);
                    found = true;
                }
        if (!found) fprintf(stderr, "retest: no such join in the library's folders: %s -> %s\n", ra.c_str(), rb.c_str());
    }
    std::mt19937 rng(seed);
    for (const auto& [kind, count] : wanted) {
        auto v = joins[kind];
        std::shuffle(v.begin(), v.end(), rng);
        int got = 0, same = 0;
        for (Join& j : v) {   // from different folders where it can, never a pair heard before, versions that sound apart
            if (got == count) break;
            if (excluded.count({ slash(all[size_t(j.a)].path), slash(all[size_t(j.b)].path) })) continue;
            if (usedFolders.count(all[size_t(j.a)].folder) && v.size() > 8) continue;
            if (!distinct(j)) { same++; continue; }
            usedFolders.insert(all[size_t(j.a)].folder);
            chosen.push_back(j);
            got++;
        }
        if (got < count) fprintf(stderr, "%s: %d of %d joins found (%d passed over: two versions sounded the same)\n", kind.c_str(), got, count, same);
    }
    if (chosen.empty()) { fprintf(stderr, "no joins chosen\n"); return 1; }
    struct Demo { int join, version; };
    std::vector<Demo> demos;
    for (int k = 0; k < int(chosen.size()); k++) for (int v = 0; v < 3; v++) demos.push_back({ k, v });
    std::shuffle(demos.begin(), demos.end(), rng);
    fs::create_directories(fs::u8path(outDir));
    fs::copy_file(fs::u8path(specPath), fs::u8path(outDir) / "round.spec", fs::copy_options::overwrite_existing);
    std::ofstream js(fs::u8path(outDir + "/round.json"), std::ios::binary);
    js << "{\n  \"round\": " << roundNo << ",\n  \"seed\": " << seed << ",\n  \"base\": " << jsonStr(base.name) << ",\n  \"baseFile\": "
       << jsonStr(slash(basePath)) << ",\n  \"demos\": [\n";
    for (size_t d = 0; d < demos.size(); d++) {
        const Demo& dm = demos[d];
        const Join& j = chosen[size_t(dm.join)];
        const Track &a = all[size_t(j.a)], &b = all[size_t(j.b)];
        const automix::Plan p = planOf(j, dm.version);
        char file[32];
        snprintf(file, sizeof file, "%02zu.mp3", d + 1);
        const bool ok = renderMp3(p, a.path, b.path, outDir + "/" + file, 10, 10);
        const double vo = vocalOverlap(p, a, b);
        fprintf(stderr, "%02zu %s v%d %s -> %s: %.1f s%s\n", d + 1, j.kind.c_str(), dm.version, a.name.c_str(), b.name.c_str(), p.length,
                ok ? "" : " (RENDER FAILED: is ffmpeg on the PATH?)");
        std::string changes = "{";
        if (dm.version)
            for (const auto& [k, v] : (j.alt[dm.version] ? alts : versions)[dm.version][j.kind])
                changes += (changes.size() > 1 ? ", " : "") + jsonStr(k) + ": " + jsonStr(v);
        changes += "}";
        js << "    {\"file\": " << jsonStr(file) << ", \"join\": " << dm.join << ", \"version\": " << dm.version << ", \"kind\": " << jsonStr(j.kind)
           << ", \"a\": " << jsonStr(a.path) << ", \"b\": " << jsonStr(b.path) << ", \"aName\": " << jsonStr(a.name) << ", \"bName\": "
           << jsonStr(b.name) << ", \"folder\": " << jsonStr(a.folder) << ", \"bpmA\": " << a.info.bpm << ", \"bpmB\": " << b.info.bpm
           << ", \"keyDistance\": " << p.keyDistance << ", \"length\": " << p.length << ", \"startOut\": " << p.startOut << ", \"cross\": "
           << p.cross << ", \"aEnd\": " << a.info.end << ", \"aFadeStart\": " << a.info.fadeStart << ", \"joinAt\": 10, \"voicesTogether\": " << vo
           << ", \"lyricsA\": " << (a.lyrics ? "true" : "false") << ", \"lyricsB\": " << (b.lyrics ? "true" : "false") << ", \"changes\": "
           << changes << ", \"alternative\": " << (j.alt[dm.version] ? "true" : "false") << ", \"tune\": " << tuneJson(modelOf(j, dm.version).tune)
           << "}" << (d + 1 < demos.size() ? "," : "") << "\n";
    }
    js << "  ]\n}\n";
    printf("round %d: %zu joins, %zu demos in %s. Next: python tools/mixtrain-page.py %s\n", roundNo, chosen.size(), demos.size(), outDir.c_str(), outDir.c_str());
    return 0;
}

/**
 * mixtrain freeze <candidate> <name> <cache.tsv> <out.model>: a new model from a candidate ("base <file>.model" then the
 * numbers it changes, as tools/mixtrain-fit.py writes it), named; never over a file that exists (a model never changes).
 * Plans every join of the library's folders with the base and with the new model and says what changes, by kind.
 */
static int freezeModel(const std::string& candPath, const std::string& name, const std::string& cachePath, const std::string& out) {
    std::string err;
    const auto lines = specLines(candPath, err);
    if (!err.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (fs::exists(fs::u8path(out))) { fprintf(stderr, "%s exists: a model never changes, give the new one a new file\n", out.c_str()); return 2; }
    std::string basePath;
    std::vector<std::pair<std::string, std::string>> ch;
    const fs::path dir = fs::u8path(candPath).parent_path();
    for (const auto& w : lines) {
        if (w[0] == "base" && w.size() == 2) basePath = fs::u8path(w[1]).is_absolute() || fs::exists(fs::u8path(w[1])) ? w[1] : utf8(dir / fs::u8path(w[1]));
        else if (w.size() == 2 && w[0] != "name") ch.push_back({ w[0], w[1] });
        else { fprintf(stderr, "%s: a line I do not understand: %s\n", candPath.c_str(), w[0].c_str()); return 2; }
    }
    automix::ModelFile base, m;
    if (basePath.empty() || !readModelFile(basePath, base, err)) { fprintf(stderr, "the base model: %s\n", basePath.empty() ? "none (base <file>.model)" : err.c_str()); return 2; }
    ch.push_back({ "name", name });
    if (!withChanges(base, ch, m, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    m.format = automix::ModelFile::Format;
    if (m.name == base.name) { fprintf(stderr, "the new model needs a name of its own\n"); return 2; }
    std::vector<Track> all = loadCache(cachePath);
    std::map<std::string, std::vector<int>> byFolder;
    for (int i = 0; i < int(all.size()); i++) byFolder[all[size_t(i)].folder].push_back(i);
    std::map<std::string, int> before, after, changed;
    int n = 0;
    for (auto& [folder, idx] : byFolder) {
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return all[size_t(x)].path < all[size_t(y)].path; });
        std::vector<const mixan::Info*> ptr;
        for (int i : idx) ptr.push_back(&all[size_t(i)].info);
        const std::vector<int> ord = automix::order(ptr, -1, automix::Model::V1_1_0);
        for (size_t k = 0; k + 1 < ord.size(); k++) {
            const mixan::Info &a = *ptr[size_t(ord[k])], &b = *ptr[size_t(ord[k + 1])];
            const automix::Plan x = automix::plan(&a, a.seconds, &b, 4, nullptr, base), y = automix::plan(&a, a.seconds, &b, 4, nullptr, m);
            n++;
            before[std::string(kindSpec(x)) + (x.gate ? "+stabs" : "")]++;
            after[std::string(kindSpec(y)) + (y.gate ? "+stabs" : "")]++;
            if (!samePlan(x, y)) changed[kindSpec(x)]++;
        }
    }
    const std::string text = m.text();
    automix::ModelFile back;
    if (!back.parse(text, &err) || back.text() != text) { fprintf(stderr, "does not read back: %s\n", err.c_str()); return 1; }
    std::ofstream(fs::u8path(out), std::ios::binary) << text;
    printf("%s from %s: %s\n%d joins in the library's folders; changed:", m.name.c_str(), base.name.c_str(), out.c_str(), n);
    if (changed.empty()) printf(" none");
    for (const auto& [k, c] : changed) printf(" %s %d", k.c_str(), c);
    printf("\n  before:");
    for (const auto& [k, c] : before) printf(" %s %d", k.c_str(), c);
    printf("\n  after: ");
    for (const auto& [k, c] : after) printf(" %s %d", k.c_str(), c);
    printf("\nTry it: copy it to the console's config/gatefold-nx/models/ and choose it in Settings › Updates › Automix models.\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "analyse") return analyseAll(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 10);
    if (argc >= 4 && std::string(argv[1]) == "mood") return analyseAll(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 10, true);
    if (argc >= 3 && std::string(argv[1]) == "same") return same(argv[2]);
    if (argc >= 4 && std::string(argv[1]) == "filesame") return fileSame(argv[2], argv[3]);
    if (argc >= 5 && std::string(argv[1]) == "build") return build(argv[2], argv[3], argv[4]);
    if (argc >= 6 && std::string(argv[1]) == "freeze") return freezeModel(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 4 && std::string(argv[1]) == "modelfile") return modelFile(argv[2], argv[3]);
    if (argc >= 3 && std::string(argv[1]) == "moods") return moodsOf(argv[2]);
    if (argc >= 5 && std::string(argv[1]) == "round")
        return round(argv[2], argv[3], atoi(argv[4]), argc > 5 ? unsigned(atoi(argv[5])) : 1u, argc > 6 ? argv[6] : "");
    fprintf(stderr, "mixtrain analyse <root> <cache.tsv> [threads]\n"
                    "mixtrain build <cache.tsv> <round.spec> <out dir>                    a training round from a spec\n"
                    "mixtrain freeze <candidate> <name> <cache.tsv> <out.model>           a new model from a candidate\n"
                    "mixtrain filesame <cache.tsv> <file>.model                           a model file against what it stands for\n"
                    "mixtrain round <cache.tsv> <out dir> <round> [seed] [plan.tsv]       (Songate's own rounds)\n");
    return 2;
}
