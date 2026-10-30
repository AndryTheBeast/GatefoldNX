/*
 * Gatefold NX · tools/bgtest/join/bgjoin.cpp
 * Automix in the background player (protocol 6), checked on the PC: the sysmodule's own engine.cpp, decoders and resampler
 * play an Automix playlist of two tracks through their join, as the console would, into a WAV. The audio thread's work
 * (commands, produce) and the watch thread's (watchAutomix, 4 a second) are run in turn on the test's clock.
 *
 *   bgjoin <music root> <a> <b> <out.wav> <analysis.tsv> [model 0|1] [pre seconds]
 * a and b are paths under the music root (queued as PC paths); analysis.tsv has "path<TAB>…<TAB>mixan::Info::pack()" lines (mixtrain's cache)
 * or "path<TAB>pack" (mix.tsv) for them. The card (sdmc:/) is a scratch folder next to out.wav; the music is read where it
 * is. The output starts `pre` seconds (12) before the join, as `mixtest --render` does, so the two can be compared.
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <switch.h>

#include "engine.hpp"   // (pre.h opens its private part to the test)

namespace fs = std::filesystem;

extern "C" size_t gfHeapSize;
size_t gfHeapSize = 0x180000;
u64 g_ticks = 0;
static std::string g_card, g_music;
static size_t g_used = 0;

struct mallinfo mallinfo_() { return { g_used }; }

struct Lock {   // (engine.cpp's own is in its unnamed namespace)
    Mutex* m;
    explicit Lock(Mutex* x) : m(x) { mutexLock(m); }
    ~Lock() { mutexUnlock(m); }
};

static std::string host(const char* p) {
    std::string s = p;
    if (s.rfind("sdmc:/Music/", 0) == 0) return g_music + "/" + s.substr(12);
    if (s.rfind("sdmc:/", 0) == 0) return g_card + "/" + s.substr(6);
    return s;
}
FILE* hostFopen(const char* p, const char* m) {
    const std::string h = host(p);
    return _wfopen(fs::u8path(h).wstring().c_str(), fs::u8path(m).wstring().c_str());
}
int hostStat(const char* p, struct stat* s) {
    std::error_code e;
    const fs::path f = fs::u8path(host(p));
    const auto st = fs::status(f, e);
    if (e || !fs::exists(st)) return -1;
    memset(s, 0, sizeof *s);
    s->st_mode = fs::is_directory(st) ? _S_IFDIR : _S_IFREG;
    if (!fs::is_directory(st)) s->st_size = long(fs::file_size(f, e));
    return 0;
}
int hostMkdir(const char* p, int) { std::error_code e; fs::create_directories(fs::u8path(host(p)), e); return e ? -1 : 0; }
int hostRemove(const char* p) { std::error_code e; return (fs::remove)(fs::u8path(host(p)), e) ? 0 : -1; }
int hostRename(const char* a, const char* b) { std::error_code e; (fs::rename)(fs::u8path(host(a)), fs::u8path(host(b)), e); return e ? -1 : 0; }

static void wav(const std::string& path, const std::vector<int16_t>& s, int rate) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    const uint32_t data = uint32_t(s.size() * 2), riff = 36 + data, fmt = 16, br = uint32_t(rate) * 4;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    f.write("RIFF", 4); f.write((const char*)&riff, 4); f.write("WAVEfmt ", 8); f.write((const char*)&fmt, 4);
    f.write((const char*)&pcm, 2); f.write((const char*)&ch, 2); f.write((const char*)&rate, 4); f.write((const char*)&br, 4);
    f.write((const char*)&align, 2); f.write((const char*)&bits, 2); f.write("data", 4); f.write((const char*)&data, 4);
    f.write((const char*)s.data(), std::streamsize(s.size() * 2));
}

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: bgjoin <music root> <a> <b> <out.wav> <analysis.tsv> [model] [pre]\n"); return 2; }
    g_music = argv[1];
    const std::string a = argv[2], b = argv[3], out = argv[4];
    // the model: a built-in one's number, or a .model file (copied to the card's models folder, as Gatefold downloads it)
    const std::string modelArg = argc > 6 ? argv[6] : "1";
    const bool fileModel = modelArg.find(".model") != std::string::npos;
    const int model = fileModel ? 1 : atoi(modelArg.c_str());
    const double pre = argc > 7 ? atof(argv[7]) : 12;
    {
        const std::u8string u = (fs::u8path(out).parent_path() / "bgjoin-card").u8string();
        g_card.assign(u.begin(), u.end());
    }
    fs::remove_all(fs::u8path(g_card));
    fs::create_directories(fs::u8path(g_card) / "config" / "gatefold-nx");
    // the card's mix.tsv: the two tracks' analyses, under their console paths
    // the queue holds the PC paths themselves (dr_flac opens its file on its own, past the sdmc:/ mapping)
    const std::string pa = g_music + "/" + a, pb = g_music + "/" + b;
    {
        std::ifstream in(fs::u8path(argv[5]), std::ios::binary);
        std::ofstream mix(fs::u8path(g_card + "/config/gatefold-nx/mix.tsv"), std::ios::binary);
        auto norm = [](std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; };
        const std::string ka = norm(g_music + "/" + a), kb = norm(g_music + "/" + b);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t t = line.find('\t');
            if (t == std::string::npos) continue;
            const std::string p = norm(line.substr(0, t)), pack = line.substr(line.rfind('\t') + 1);
            if (p == ka) mix << pa << '\t' << pack << '\n';
            if (p == kb) mix << pb << '\t' << pack << '\n';
        }
    }

    Engine e;
    mutexInit(&e.m_);
    e.fader_.init(GFBG_RATE, 0);
    GfbgConfig c = e.config();
    c.automixModel = u32(model);
    automix::ModelFile mf;
    if (fileModel) {
        std::ifstream in(fs::u8path(modelArg), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!mf.parse(text)) { fprintf(stderr, "not a model: %s\n", modelArg.c_str()); return 2; }
        fs::create_directories(fs::u8path(g_card) / "config" / "gatefold-nx" / "models");
        std::ofstream(fs::u8path(g_card + "/config/gatefold-nx/models/test.model"), std::ios::binary) << text;
        snprintf(c.automixFile, sizeof c.automixFile, "test.model");
    }
    c.crossfade = 4;
    e.setConfig(c);
    GfbgEq eq = e.eq();
    eq.on = 0;
    e.setEq(eq);
    e.setVolume(1);
    e.vol_ = 1;

    // the queue, as Gatefold sends an Automix playlist's
    std::vector<u8> q(sizeof(GfbgQueueHeader) + 8);
    GfbgQueueHeader h = {};
    h.count = 2;
    h.flags = GfbgQueue_Automix;
    snprintf(h.name, sizeof h.name, "Test Mix");
    std::string paths = pa + '\0' + pb + '\0';
    h.pathBytes = u32(paths.size());
    memcpy(q.data(), &h, sizeof h);
    const s32 order[2] = { 0, 1 };
    memcpy(q.data() + sizeof h, order, 8);
    q.insert(q.end(), paths.begin(), paths.end());
    if (!e.setQueue(q.data(), q.size(), 1)) { fprintf(stderr, "queue refused\n"); return 1; }

    // where the join will be: the plan the player makes, from the same analyses
    mixan::Info ia, ib;
    const bool ka = e.mixInfo(pa, ia), kb = e.mixInfo(pb, ib);
    std::unique_ptr<Decoder> da = openDecoder(pa.c_str());
    if (!da) { fprintf(stderr, "cannot open %s\n", pa.c_str()); return 1; }
    const automix::Plan pl = fileModel ? automix::plan(ka ? &ia : nullptr, da->duration, kb ? &ib : nullptr, 4, nullptr, mf)
                                       : automix::plan(ka ? &ia : nullptr, da->duration, kb ? &ib : nullptr, 4, nullptr, automix::Model(model));
    printf("analysis: a %s, b %s; plan: at %.3f s of a, b from %.3f s, %.2f s%s, effect %s\n", ka ? "yes" : "NO", kb ? "yes" : "NO",
           pl.startOut, pl.offset, pl.length, pl.beatmatched ? " on the beat" : "", automix::name(pl.join.effect));
    da.reset();
    const double from = std::max(0.0, pl.startOut - pre);
    e.playAt(0, false, from, 1);

    std::vector<int16_t> pcm;
    s16 buf[1536 * 2];
    u64 frames = 0, nextWatch = 0;
    u32 joinGen = e.joinGen_;
    double joinedAt = -1, watchMs = 0, worstWatch = 0;
    const u64 total = u64((pre + pl.length + 12) * GFBG_RATE);
    while (frames < total) {
        g_ticks = frames * 19200000ull / GFBG_RATE;
        {
            Lock l(&e.m_);
            e.runCommands();
        }
        if (frames >= nextWatch) {   // the watch thread: every 250 ms
            const auto t0 = std::chrono::steady_clock::now();
            e.watchAutomix();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            watchMs += ms;
            worstWatch = std::max(worstWatch, ms);
            nextWatch += GFBG_RATE / 4;
        }
        const int n = e.produce(buf, 1536);
        if (n <= 0) { fprintf(stderr, "silence at %.2f s\n", double(frames) / GFBG_RATE); break; }
        if (e.joinGen_ != joinGen && joinedAt < 0) {
            joinedAt = double(frames) / GFBG_RATE;
            printf("join started %.3f s into the output (delay %.3f s, echo line %zu floats)\n", joinedAt, double(e.delay_) / GFBG_RATE,
                   e.fader_.echo_.size());
        }
        pcm.insert(pcm.end(), buf, buf + n * 2);
        frames += u64(n);
    }
    printf("now playing: %s at %.2f s; watch thread %.1f ms in all, worst tick %.1f ms\n", e.playing_.c_str(),
           e.base_ + double(e.outFrames_) / GFBG_RATE, watchMs, worstWatch);
    wav(out, pcm, GFBG_RATE);
    printf("wrote %s: %.2f s\n", out.c_str(), double(pcm.size() / 2) / GFBG_RATE);
    return 0;
}
