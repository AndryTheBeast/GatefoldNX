/*
 * Gatefold NX background player · engine.cpp
 * See engine.hpp. The queue rules follow Gatefold's App::next/prev/setOrder line for line, so
 * the music carries on exactly as it would have inside the app.
 */
#include "engine.hpp"

#include <dirent.h>
#include <malloc.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" size_t gfHeapSize;   // main.cpp

namespace {

constexpr const char* Dir = "sdmc:/config/gatefold-nx";
constexpr int BufFrames = 1536, BufCount = 5;   // 5 × 32 ms in flight: room to open the next track without a gap
constexpr size_t BufBytes = (BufFrames * 4 + 0xFFF) & ~size_t(0xFFF);

u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000; }

std::string cfgPath(const char* f) { return std::string(Dir) + "/" + f; }

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/** cover/folder/front .jpg/.png next to the track, the names Gatefold looks for too. */
std::string folderCover(const std::string& track) {
    const size_t slash = track.rfind('/');
    if (slash == std::string::npos) return {};
    const std::string dir = track.substr(0, slash + 1);
    for (const char* n : { "cover", "folder", "front", "Cover", "Folder", "Front" })
        for (const char* e : { ".jpg", ".jpeg", ".png" })
            if (exists(dir + n + e)) return dir + n + e;
    return {};
}

struct Lock {
    Mutex* m;
    explicit Lock(Mutex* x) : m(x) { mutexLock(m); }
    ~Lock() { mutexUnlock(m); }
};

}  // namespace

namespace {

std::vector<std::string> readLines(const std::string& p) {
    std::vector<std::string> out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        out.emplace_back(line, n);
    }
    fclose(f);
    return out;
}

}  // namespace

// ============================================================ start

void Engine::start() {
    mutexInit(&m_);
    loadFiles();
    static Thread audio, watch;
    // The audio thread sits just under the IPC thread's priority, on the same core.
    threadCreate(&audio, audioMain, this, nullptr, 0x8000, 0x2B, -2);
    threadCreate(&watch, watchMain, this, nullptr, 0x4000, 0x2E, -2);
    threadStart(&audio);
    threadStart(&watch);
}

// ============================================================ IPC

void Engine::hello(u32 kind, u64 pid) {
    Lock l(&m_);
    if (kind == GfbgClient_Gatefold) gatefoldPid_ = pid;
}

GfbgState Engine::state() {
    Lock l(&m_);
    GfbgState s = {};
    s.protocol = GFBG_PROTOCOL;
    // A pause asked for reads as paused at once, though the 40 ms fade is still running.
    // The sleep timer's fade is still playing: Gatefold keeps its spectrum and play button going until it ends.
    s.status = status_ == GfbgStatus_Playing && pauseAtSilence_ && !autoPausing_ && !sleepFading_ ? GfbgStatus_Paused : status_;
    s.pos = pos_;
    s.track = current();
    s.trackGen = trackGen_;
    s.queueGen = queueGen_;
    s.queueCount = u32(count());
    s.queueBytes = u32(sizeof(GfbgQueueHeader) + order_.size() * 4 + blob_.size());
    s.mode = mode_;
    s.stopAfter = stopAfter_;
    if (status_ == GfbgStatus_Stopped) s.position = resumeAt_;
    else s.position = std::max(0.0, base_ + (double(outFrames_) - double(inFlight_)) / GFBG_RATE);
    if (dec_) {
        s.duration = dec_->duration;
        s.position = std::min(s.position, s.duration > 0 ? s.duration : s.position);
        s.sampleRate = u32(dec_->rate);
        s.channels = u32(dec_->channels);
        s.codec = u32(dec_->codec);
        s.bitrate = u32(dec_->bitrate);
    }
    s.volume = volume_;
    s.gain = gain_;
    s.errorGen = errorGen_;
    s.appState = appState_;
    s.autoPaused = autoPaused_ || autoPausing_;
    s.trackBy = trackBy_;
    s.queueBy = queueBy_;
    const struct mallinfo mi = mallinfo();
    s.heapFree = u32(gfHeapSize > size_t(mi.uordblks) ? gfHeapSize - size_t(mi.uordblks) : 0);
    if (sleepAt_) s.sleepLeft = float(double(sleepAt_ > nowMs() ? sleepAt_ - nowMs() : 0) / 1000.0);
    s.sleepGen = sleepGen_;
    return s;
}

void Engine::setQueue(const u8* data, size_t size, u32 by) {
    if (size < sizeof(GfbgQueueHeader)) return;
    GfbgQueueHeader h;
    memcpy(&h, data, sizeof h);
    const size_t orderBytes = size_t(h.count) * 4;
    if (h.count > 100000 || sizeof h + orderBytes + h.pathBytes > size) return;
    const char* paths = reinterpret_cast<const char*>(data + sizeof h + orderBytes);

    // Built outside the lock: a big library is a few hundred KB to copy.
    std::string blob(paths, h.pathBytes);
    std::vector<u32> offs;
    offs.reserve(h.count);
    for (size_t at = 0; at < blob.size() && offs.size() < h.count;) {
        offs.push_back(u32(at));
        const size_t end = blob.find('\0', at);
        if (end == std::string::npos) { blob.push_back('\0'); break; }
        at = end + 1;
    }
    if (offs.size() != h.count) return;
    std::vector<s32> order(h.count);
    memcpy(order.data(), data + sizeof h, orderBytes);
    for (s32 o : order) if (o < 0 || o >= s32(h.count)) return;

    Lock l(&m_);
    blob_ = std::move(blob);
    offs_ = std::move(offs);
    order_ = std::move(order);
    pos_ = h.count ? std::clamp(h.pos, 0, s32(h.count) - 1) : -1;
    mode_ = h.mode <= GfbgMode_Shuffle ? h.mode : GfbgMode_Default;
    name_.assign(h.name, strnlen(h.name, sizeof h.name));
    // Whatever plays now carries on: Gatefold follows a new queue with PlayAt when it wants
    // another track. KeepCurrent only says the track is the one at pos already.
    if (count() == 0) cmds_.push_front({ Cmd::Stop, 0, false, 0, 0 });   // on the audio thread: only it touches the decoder
    queueGen_++;
    queueBy_ = by;
    markDirty();
}

size_t Engine::queueBytes() {
    Lock l(&m_);
    return sizeof(GfbgQueueHeader) + order_.size() * 4 + blob_.size();
}

void Engine::getQueue(u8* out, size_t cap) {
    Lock l(&m_);
    GfbgQueueHeader h = {};
    h.count = u32(count());
    h.pos = pos_;
    h.mode = mode_;
    h.pathBytes = u32(blob_.size());
    memcpy(h.name, name_.c_str(), std::min(name_.size(), sizeof h.name - 1));
    if (sizeof h + order_.size() * 4 + blob_.size() > cap) return;
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, order_.data(), order_.size() * 4);
    memcpy(out + sizeof h + order_.size() * 4, blob_.data(), blob_.size());
}

void Engine::playAt(s32 pos, bool paused, double start, u32 by) {
    Lock l(&m_);
    cmds_.push_back({ Cmd::PlayAt, pos, paused, start, by });
}

void Engine::transport(u32 op, u32 by) {
    static const Cmd::Kind kinds[] = { Cmd::Pause, Cmd::Resume, Cmd::Toggle, Cmd::Stop, Cmd::Next, Cmd::Prev };
    if (op >= sizeof kinds / sizeof *kinds) return;
    Lock l(&m_);
    // Played again by hand during the sleep fade: awake after all, so the timer is off.
    if (sleepFading_ && (kinds[op] == Cmd::Resume || kinds[op] == Cmd::Toggle)) { sleepFading_ = false; sleepAt_ = 0; }
    cmds_.push_back({ kinds[op], 0, false, 0, by });
}

void Engine::seek(double t) {
    Lock l(&m_);
    cmds_.push_back({ Cmd::Seek, 0, false, std::max(0.0, t), 0 });
}

void Engine::setVolume(float v) {
    Lock l(&m_);
    volume_ = std::clamp(v, 0.f, 1.f);
    markDirty();
}

/** App::setOrder: the order starts over from the queue as listed, shuffled with the current track first. */
void Engine::setMode(u32 mode, u32 by) {
    if (mode > GfbgMode_Shuffle) return;
    Lock l(&m_);
    const s32 cur = current();
    mode_ = mode;
    lvlTarget_ = levelFor(playing_);   // Auto levels by track while shuffling, by album otherwise
    identityOrder();
    if (mode == GfbgMode_Shuffle) {
        shuffle();
        if (cur >= 0) std::iter_swap(order_.begin(), std::find(order_.begin(), order_.end(), cur));
        pos_ = order_.empty() ? -1 : 0;
    } else {
        pos_ = order_.empty() ? -1 : std::max(0, cur);
    }
    queueGen_++;
    queueBy_ = by;
    markDirty();
}

void Engine::setStopAfter(bool on) {
    Lock l(&m_);
    stopAfter_ = on;
}

/** The levelling gain for a path (linear), as the mode asks: Auto takes the album's but the track's while shuffling. */
float Engine::levelFor(const std::string& path) const {
    const u32 mode = cfg_.reserved[0];
    if (mode == GfbgLevel_Off || gains_.empty() || path.empty()) return 1;
    const u64 h = gfbgHash(path.data(), path.size());
    const auto it = std::lower_bound(gains_.begin(), gains_.end(), h, [](const GfbgGain& g, u64 v) { return g.hash < v; });
    if (it == gains_.end() || it->hash != h) return 1;   // not measured yet: as it is
    const bool album = mode == GfbgLevel_Album || (mode == GfbgLevel_Auto && mode_ != GfbgMode_Shuffle);
    return std::pow(10.f, (album ? it->album : it->track) / 20.f);
}

/** Gatefold rewrites GFBG_GAINS as it measures: its header's generation says when to read it again. */
void Engine::watchGains() {
    FILE* f = fopen(GFBG_GAINS, "rb");
    if (!f) return;
    GfbgGainsHeader h{};
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != GFBG_GAINS_MAGIC || h.count > 100000) { fclose(f); return; }
    {
        Lock l(&m_);
        if (h.gen == gainsGen_) { fclose(f); return; }
    }
    std::vector<GfbgGain> g(h.count);
    const bool ok = fread(g.data(), sizeof(GfbgGain), h.count, f) == h.count;
    fclose(f);
    if (!ok) return;
    Lock l(&m_);
    gains_.swap(g);
    gainsGen_ = h.gen;
    lvlTarget_ = levelFor(playing_);   // the track playing may have its value now: eased in
}

void Engine::setSleep(float seconds) {
    Lock l(&m_);
    // Turned off or moved during the fade: the music comes back up.
    if (sleepFading_) cmds_.push_back({ Cmd::Resume, 0, false, 0, 0 });
    sleepFading_ = false;
    sleepAt_ = seconds > 0 ? nowMs() + u64(double(seconds) * 1000) : 0;
}

/** Every watch tick: the fade starts SleepFade seconds before the end, then the pause; then the timer is off. */
void Engine::watchSleep(u64 now) {
    Lock l(&m_);
    if (!sleepAt_) return;
    if (now >= sleepAt_) {
        sleepAt_ = 0;
        sleepFading_ = false;
        sleepGen_++;
        if (status_ == GfbgStatus_Playing && !pauseAtSilence_) cmds_.push_back({ Cmd::Pause, 0, false, 0.3, 0 });
        return;
    }
    if (!sleepFading_ && status_ == GfbgStatus_Playing && !pauseAtSilence_ && now + u64(SleepFade * 1000) >= sleepAt_) {
        sleepFading_ = true;
        cmds_.push_back({ Cmd::Pause, 0, false, double(sleepAt_ - now) / 1000.0, 0 });
    }
}

u32 Engine::pcm(float* out) {
    Lock l(&m_);
    for (int i = 0; i < GFBG_PCM; i++) out[i] = ring_[(ringAt_ - GFBG_PCM + i) % Ring];
    return serial_;
}

GfbgConfig Engine::config() {
    Lock l(&m_);
    return cfg_;
}

void Engine::setConfig(const GfbgConfig& c) {
    {
        Lock l(&m_);
        cfg_ = c;
        cfg_.onOpen = std::min<u8>(cfg_.onOpen, GfbgAct_Pause);
        cfg_.onHome = std::min<u8>(cfg_.onHome, GfbgAct_Pause);
        cfg_.onClose = std::min<u8>(cfg_.onClose, GfbgAct_Pause);
        cfg_.onReturn = std::min<u8>(cfg_.onReturn, GfbgAct_Pause);
        cfg_.fadeOut = std::clamp(cfg_.fadeOut, 0.f, 10.f);
        cfg_.fadeIn = std::clamp(cfg_.fadeIn, 0.f, 10.f);
        cfg_.gameVolume = std::clamp(cfg_.gameVolume, 0.f, 1.f);
        cfg_.reserved[0] = std::min<u32>(cfg_.reserved[0], GfbgLevel_Auto);
        lvlTarget_ = levelFor(playing_);   // the levelling mode may have changed: eased to the new gain
        applyGameVolume();   // the default may be the one the game in front uses
    }
    saveConfig();
}

// ============================================================ game volume

float Engine::gameVolumeFor(u64 program) const {
    for (const auto& [p, v] : gameVolumes_) if (p == program) return v;
    return cfg_.gameVolume;
}

/** The game's own audio, through aud:a (11.0.0+) or audout:a before it. m_ held. */
void Engine::applyGameVolume() {
    if (!appPid_) return;
    const float v = gameVolumeFor(appProgram_);
    if (hosversionAtLeast(11, 0, 0)) audaSetAudioOutputProcessMasterVolume(appPid_, 0, v);
    else audoutaSetProcessMasterVolume(appPid_, 0, v);
}

GfbgGameVolume Engine::gameVolume() {
    Lock l(&m_);
    GfbgGameVolume g = {};
    g.programId = appPid_ ? appProgram_ : 0;
    g.volume = appPid_ ? gameVolumeFor(appProgram_) : cfg_.gameVolume;
    if (appPid_) for (const auto& e : gameVolumes_) if (e.first == appProgram_) g.own = 1;
    return g;
}

void Engine::setGameVolume(float v) {
    {
        Lock l(&m_);
        if (!appPid_) return;
        v = std::clamp(v, 0.f, 1.f);
        bool found = false;
        for (auto& e : gameVolumes_) if (e.first == appProgram_) { e.second = v; found = true; }
        if (!found) gameVolumes_.push_back({ appProgram_, v });
        applyGameVolume();
    }
    saveGameVolumes();
}

void Engine::saveGameVolumes() {
    std::vector<std::pair<u64, float>> copy;
    {
        Lock l(&m_);
        copy = gameVolumes_;
    }
    mkdir("sdmc:/config", 0777);
    mkdir(Dir, 0777);
    if (FILE* f = fopen(cfgPath("bg-games.txt").c_str(), "wb")) {
        for (const auto& [p, v] : copy) fprintf(f, "%016llx=%.3f\n", (unsigned long long)p, double(v));
        fclose(f);
    }
}

void Engine::trackInfo(GfbgTrackInfo* out) {
    Lock l(&m_);
    memset(out, 0, sizeof *out);
    out->trackGen = trackGen_;
    const s32 t = current();
    const std::string p = dec_ ? playing_ : t >= 0 ? path(t) : "";
    snprintf(out->path, sizeof out->path, "%s", p.c_str());
    if (dec_) {
        snprintf(out->title, sizeof out->title, "%s", dec_->tags.title.c_str());
        snprintf(out->artist, sizeof out->artist, "%s", dec_->tags.artist.c_str());
        snprintf(out->album, sizeof out->album, "%s", dec_->tags.album.c_str());
        out->coverKind = u32(dec_->tags.coverKind);
        out->coverOffset = dec_->tags.coverOffset;
        out->coverSize = dec_->tags.coverSize;
    }
    if (!out->coverKind && !coverPath_.empty()) {
        out->coverKind = 3;
        snprintf(out->coverPath, sizeof out->coverPath, "%s", coverPath_.c_str());
    }
}

// ============================================================ queue helpers

void Engine::identityOrder() {
    order_.resize(offs_.size());
    for (size_t i = 0; i < order_.size(); i++) order_[i] = s32(i);
}

void Engine::shuffle() {
    for (size_t i = order_.size(); i > 1; i--) {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;   // xorshift64
        std::swap(order_[i - 1], order_[size_t(rng_ % i)]);
    }
}

void Engine::markDirty() {
    if (!dirty_) dirtyAt_ = nowMs();
    dirty_ = true;
}

// ============================================================ transport (audio thread, m_ held)

void Engine::runCommands() {
    while (!cmds_.empty()) {
        const Cmd c = cmds_.front();
        cmds_.pop_front();
        switch (c.kind) {
        case Cmd::PlayAt:
            failed_ = 0;
            autoPaused_ = autoPausing_ = false;
            playAtPos(c.pos, c.paused, c.t);
            trackBy_ = c.by;
            break;
        case Cmd::Toggle:
        case Cmd::Pause:
        case Cmd::Resume: {
            const bool pausing = status_ == GfbgStatus_Playing && !pauseAtSilence_;
            const bool wantPause = c.kind == Cmd::Pause || (c.kind == Cmd::Toggle && pausing);
            autoPaused_ = autoPausing_ = false;
            if (wantPause) {
                // c.t: a longer fade asked for (the sleep timer); a pause by hand takes 40 ms
                if (status_ == GfbgStatus_Playing) rampTo(0, float(std::max(0.04, c.t)), true);
            } else if (status_ == GfbgStatus_Stopped) {
                if (!order_.empty()) { playAtPos(std::max(0, pos_), false, resumeAt_); trackBy_ = c.by; }
            } else {
                if (status_ == GfbgStatus_Paused) gain_ = 0;
                status_ = GfbgStatus_Playing;
                rampTo(1, 0.04f, false);
            }
            markDirty();
            break;
        }
        case Cmd::AutoResume:
            autoPaused_ = autoPausing_ = false;
            if (status_ == GfbgStatus_Stopped) {
                if (order_.empty()) break;
                playAtPos(std::max(0, pos_), false, resumeAt_);
                gain_ = 0;
            } else if (status_ == GfbgStatus_Paused) {
                gain_ = 0;
                status_ = GfbgStatus_Playing;
            }
            rampTo(1, float(std::max(0.04, c.t)), false);
            markDirty();
            break;
        case Cmd::PlayFade:
            autoPaused_ = autoPausing_ = false;
            failed_ = 0;
            playAtPos(c.pos, false, 0);
            gain_ = 0;
            rampTo(1, float(std::max(0.04, c.t)), false);
            trackBy_ = 0;
            break;
        case Cmd::Stop: stop(); break;
        case Cmd::Next: next(false); trackBy_ = c.by; break;
        case Cmd::Prev: prev(); trackBy_ = c.by; break;
        case Cmd::Seek:
            if (!dec_) { resumeAt_ = c.t; break; }
            dec_->seek(c.t);
            rs_.reset(dec_->rate, GFBG_RATE);
            tailFlushed_ = false;
            base_ = c.t;
            outFrames_ = 0;
            flush_ = true;
            if (status_ == GfbgStatus_Playing && !pauseAtSilence_) { gain_ = 0; rampTo(1, 0.015f, false); }
            break;
        }
    }
}

/** Opens the track at pos, outside the lock: parsing a big file's tables takes a moment. */
bool Engine::load(s32 pos, double start) {
    if (pos < 0 || pos >= s32(order_.size())) return false;
    const std::string p = path(order_[size_t(pos)]);
    mutexUnlock(&m_);
    std::unique_ptr<Decoder> d = openDecoder(p.c_str());
    if (d && start > 0.5) d->seek(start);
    const std::string cover = d && !d->tags.coverKind ? folderCover(p) : std::string();
    mutexLock(&m_);
    pos_ = pos;   // even when it failed: the skip carries on from here
    trackGen_++;
    if (!d) return false;
    dec_ = std::move(d);
    rs_.reset(dec_->rate, GFBG_RATE);
    tailFlushed_ = false;
    playing_ = p;
    coverPath_ = cover;
    lvl_ = lvlTarget_ = levelFor(p);   // levelled from the first sample, also across a gapless join
    heardFrames_ = 0;
    counted_ = false;
    base_ = start > 0.5 ? start : 0;
    outFrames_ = 0;
    resumeAt_ = 0;
    markDirty();
    return true;
}

/** App::playAt: an unplayable file is skipped, but a list of broken files never loops forever. */
void Engine::playAtPos(s32 pos, bool paused, double start) {
    const s32 n = s32(order_.size());
    if (n == 0) return;
    pos = std::clamp(pos, 0, n - 1);
    for (s32 tries = 0; tries < n; tries++) {
        if (load(pos, start)) {
            failed_ = 0;
            if (paused) {
                status_ = GfbgStatus_Paused;
                gain_ = 0;
                gainTarget_ = 0;
                pauseAtSilence_ = false;
            } else {
                // A track picked by hand starts on a 10 ms fade, so the cut never clicks.
                flush_ = true;
                status_ = GfbgStatus_Playing;
                gain_ = 0;
                rampTo(1, 0.01f, false);
            }
            return;
        }
        errorGen_++;
        start = 0;
        if (paused) break;
        if (++pos >= n) {
            if (mode_ == GfbgMode_Shuffle) { shuffle(); queueGen_++; queueBy_ = 0; }
            pos = 0;
        }
    }
    stop();
}

void Engine::next(bool natural) {
    const s32 n = s32(order_.size());
    if (n == 0) return stop();
    if (natural && mode_ == GfbgMode_RepeatOne) {
        if (!load(pos_, 0)) { errorGen_++; stop(false); }
        trackBy_ = 0;
        return;
    }
    if (natural && stopAfter_) {
        stopAfter_ = false;
        stop(false);
        return;
    }
    s32 p = pos_ + 1;
    if (p >= n) {
        if (mode_ == GfbgMode_Default && natural) { stop(false); return; }
        if (mode_ == GfbgMode_Shuffle) { shuffle(); queueGen_++; queueBy_ = 0; }
        p = 0;
    }
    if (natural) {
        // Gapless: the next track follows in the same buffer, no flush and no fade.
        for (s32 tries = 0; tries < n; tries++) {
            if (load(p, 0)) { trackBy_ = 0; return; }
            errorGen_++;
            if (++p >= n) {
                if (mode_ == GfbgMode_Default) break;
                p = 0;
            }
        }
        stop(false);
        return;
    }
    playAtPos(p, false, 0);
}

void Engine::prev() {
    if (order_.empty()) return;
    const double heard = base_ + (double(outFrames_) - double(inFlight_)) / GFBG_RATE;
    if (dec_ && heard > 3) {   // first back to the start, then the track before
        cmds_.push_front({ Cmd::Seek, 0, false, 0, 0 });
        return;
    }
    playAtPos(pos_ > 0 ? pos_ - 1 : s32(order_.size()) - 1, status_ == GfbgStatus_Paused, 0);
}

void Engine::stop(bool cut) {
    dec_.reset();
    playing_.clear();
    status_ = GfbgStatus_Stopped;
    pauseAtSilence_ = autoPausing_ = false;
    resumeAt_ = 0;
    flush_ = flush_ || cut;
    markDirty();
}

void Engine::rampTo(float target, float seconds, bool pauseAfter) {
    gainTarget_ = target;
    gainStep_ = seconds > 0 ? 1.f / (seconds * GFBG_RATE) : 1.f;
    pauseAtSilence_ = pauseAfter;
}

// ============================================================ audio

/** Fills out with up to frames of 48 kHz stereo; the track changes inside the buffer when one ends. */
/**
 * Called without the lock. Decoding and resampling run unlocked: only the audio thread replaces or
 * moves the decoder (commands, load and next all run on it), and Gatefold asks for the state and
 * the PCM every frame, which used to wait out a whole decode and stall its frame. The lock is taken
 * for the shared counters, a track change, and the mix at the end.
 */
int Engine::produce(s16* out, int frames) {
    int n = 0;
    while (n < frames && dec_) {
        const int got = rs_.pull(mix_ + n * 2, frames - n);
        n += got;
        if (got > 0) {
            Lock l(&m_);
            outFrames_ += u64(got);
            heardFrames_ += u64(got);   // what was heard, seeks or not: foo_playcount counts listening time
            const double dur = dec_->duration;
            const double need = dur > 0 ? std::min(60.0, dur * 0.9) : 60.0;   // Gatefold's Stats::threshold
            if (!counted_ && double(heardFrames_) >= need * GFBG_RATE) {
                counted_ = true;
                if (plays_.size() < 256) plays_.push_back(playing_);
            }
        }
        if (n >= frames) break;
        const int r = dec_->read(decBuf_, 1024);
        if (r > 0) { rs_.push(decBuf_, r); continue; }
        if (!tailFlushed_) {
            // The resampler holds back half its filter: push silence so the last notes come out.
            static const s16 zeros[(Resampler::Half + 1) * 2] = {};
            rs_.push(zeros, Resampler::Half + 1);
            tailFlushed_ = true;
            continue;
        }
        Lock l(&m_);
        if (r < 0) errorGen_++;
        next(true);
        if (status_ != GfbgStatus_Playing) break;
    }
    if (n == 0) return 0;
    Lock l(&m_);
    feedRing(mix_, n);

    for (int i = 0; i < n; i++) {
        if (gain_ < gainTarget_) gain_ = std::min(gainTarget_, gain_ + gainStep_);
        else if (gain_ > gainTarget_) gain_ = std::max(gainTarget_, gain_ - gainStep_);
        vol_ += (volume_ - vol_) * 0.0005f;   // ~40 ms to settle
        lvl_ += (lvlTarget_ - lvl_) * 0.00005f;   // ~0.4 s: a gain measured mid-track comes in gently
        const float g = gain_ * gain_ * (3 - 2 * gain_) * vol_ * lvl_;   // smoothstep: fades start and end softly
        for (int c = 0; c < 2; c++) {
            const float v = mix_[i * 2 + c] * g * 32767.f;
            out[i * 2 + c] = s16(std::lrint(std::clamp(v, -32768.f, 32767.f)));
        }
    }
    if (pauseAtSilence_ && gain_ <= 0) {
        status_ = GfbgStatus_Paused;
        pauseAtSilence_ = false;
        if (autoPausing_) { autoPausing_ = false; autoPaused_ = true; }
        markDirty();
    }
    return n;
}

/** The mono mix before volume and fades, for Gatefold's spectrum. */
void Engine::feedRing(const float* stereo, int frames) {
    for (int i = 0; i < frames; i++) ring_[ringAt_++ % Ring] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
    serial_++;
}

void Engine::audioMain(void* self) {
    auto* e = static_cast<Engine*>(self);
    AudioOutBuffer bufs[BufCount] = {};
    bool busy[BufCount] = {};
    for (auto& b : bufs) {
        b.buffer = aligned_alloc(0x1000, BufBytes);
        b.buffer_size = BufBytes;
    }
    bool open = false;
    auto release = [&](AudioOutBuffer* rel) {
        for (int i = 0; i < BufCount; i++)
            if (rel == &bufs[i] && busy[i]) {
                busy[i] = false;
                Lock l(&e->m_);
                e->inFlight_ -= std::min<u64>(e->inFlight_, bufs[i].data_size / 4);
            }
    };
    auto reclaim = [&] {
        for (int k = 0; k < BufCount; k++) {
            AudioOutBuffer* rel = nullptr;
            u32 cnt = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &cnt)) || cnt == 0 || !rel) break;
            release(rel);
        }
    };
    auto reset = [&] {
        for (auto& b : busy) b = false;
        Lock l(&e->m_);
        e->inFlight_ = 0;
    };

    for (;;) {
        // audout may not be up yet at boot, and may need opening again after sleep mode.
        if (!open) {
            open = R_SUCCEEDED(audoutInitialize());
            if (open && R_FAILED(audoutStartAudioOut())) { audoutExit(); open = false; }
            if (!open) {
                mutexLock(&e->m_);
                e->runCommands();
                mutexUnlock(&e->m_);
                svcSleepThread(500'000'000);
                continue;
            }
            reset();
        }

        mutexLock(&e->m_);
        e->runCommands();
        const bool playing = e->status_ == GfbgStatus_Playing && e->dec_;
        const bool flush = e->flush_;
        e->flush_ = false;
        mutexUnlock(&e->m_);
        if (flush) {
            bool flushed = false;
            audoutFlushAudioOutBuffers(&flushed);
        }
        reclaim();

        int free = -1;
        for (int i = 0; i < BufCount; i++) if (!busy[i]) { free = i; break; }
        if (playing && free >= 0) {
            AudioOutBuffer& b = bufs[free];
            const int n = e->produce(static_cast<s16*>(b.buffer), BufFrames);   // takes the lock itself, around the decode
            if (n > 0) {
                b.data_size = u64(n) * 4;
                b.data_offset = 0;
                armDCacheFlush(b.buffer, b.data_size);
                if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&b))) {
                    busy[free] = true;
                    Lock l(&e->m_);
                    e->inFlight_ += u64(n);
                } else {
                    audoutStopAudioOut();
                    audoutExit();
                    open = false;
                }
            }
            continue;
        }
        bool any = false;
        for (bool b : busy) any |= b;
        if (any) {
            AudioOutBuffer* rel = nullptr;
            u32 cnt = 0;
            if (R_SUCCEEDED(audoutWaitPlayFinish(&rel, &cnt, 20'000'000)) && cnt && rel) release(rel);
        } else {
            svcSleepThread(10'000'000);
        }
    }
}

// ============================================================ pause-for-apps

/**
 * The action set for a game event. Gatefold itself never counts as a game (watchMain). In front is
 * either a fresh launch (onOpen) or the same game back from the HOME menu (onReturn): with Play on
 * launch, going HOME and back no longer shuffles another folder.
 */
void Engine::gameEvent(u32 state, bool fresh) {
    u8 action;
    {
        Lock l(&m_);
        appState_ = state;
        action = state == GfbgApp_Foreground ? (fresh ? cfg_.onOpen : cfg_.onReturn) : state == GfbgApp_Suspended ? cfg_.onHome : cfg_.onClose;
        if (action != GfbgAct_Play) { act(action); return; }
    }
    // Play: a folder is picked and listed outside the lock (SD card), then started like the others.
    if (!playRandomFolder()) {
        Lock l(&m_);
        act(GfbgAct_Resume);   // no music folder found: the next best thing
    }
}

void Engine::act(u8 action) {
    if (action == GfbgAct_Pause) {
        if (status_ != GfbgStatus_Playing || pauseAtSilence_) return;
        autoPausing_ = true;
        rampTo(0, std::max(0.04f, cfg_.fadeOut), true);
    } else if (action == GfbgAct_Resume) {
        if (status_ == GfbgStatus_Playing && !pauseAtSilence_) return;
        if (status_ == GfbgStatus_Playing && pauseAtSilence_) {   // still fading out: turn round
            autoPausing_ = false;
            rampTo(1, std::max(0.04f, cfg_.fadeIn), false);
            return;
        }
        cmds_.push_back({ Cmd::AutoResume, 0, false, double(cfg_.fadeIn), 0 });
    }
}

namespace {

bool isAudio(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    for (const char* e : { ".m4a", ".mp4", ".m4b", ".mp3", ".flac", ".wav" })
        if (!strcasecmp(dot, e)) return true;
    return false;
}

/** Folders under dir (to depth 4) that hold music themselves. */
void musicFolders(const std::string& dir, int depth, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    bool music = false;
    std::vector<std::string> subs;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type == DT_DIR) subs.push_back(dir + "/" + e->d_name);
        else if (isAudio(e->d_name)) music = true;
    }
    closedir(d);
    if (music) out.push_back(dir);
    if (depth < 4)
        for (const auto& s : subs) musicFolders(s, depth + 1, out);
}

}  // namespace

/** GfbgAct_Play: one folder of Gatefold's music folder, at random, shuffled, faded in. */
bool Engine::playRandomFolder() {
    std::string root;
    for (const auto& l : readLines(cfgPath("settings.txt")))
        if (!l.compare(0, 10, "musicRoot=")) root = l.substr(10);
    if (root.empty()) root = "sdmc:/Music";
    while (root.size() > 6 && root.back() == '/') root.pop_back();
    std::vector<std::string> folders;
    musicFolders(root, 0, folders);
    if (folders.empty()) return false;
    std::vector<std::string> files;
    std::string dir;
    {
        Lock l(&m_);
        rng_ ^= armGetSystemTick();
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
        dir = folders[size_t(rng_ % folders.size())];
    }
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && e->d_type != DT_DIR && isAudio(e->d_name)) files.push_back(dir + "/" + e->d_name);
        closedir(d);
    }
    if (files.empty()) return false;
    std::sort(files.begin(), files.end());

    Lock l(&m_);
    blob_.clear();
    offs_.clear();
    for (const auto& f : files) {
        offs_.push_back(u32(blob_.size()));
        blob_ += f;
        blob_.push_back(char(0));
    }
    name_ = dir.substr(dir.rfind('/') + 1);
    mode_ = GfbgMode_Shuffle;
    identityOrder();
    shuffle();
    pos_ = 0;
    queueGen_++;
    queueBy_ = 0;
    markDirty();
    cmds_.push_back({ Cmd::PlayFade, 0, false, double(cfg_.fadeIn), 0 });
    return true;
}

/**
 * Watches the running application: pm says whether there is one, pdm's play log says whether it
 * is in front (in_focus) or suspended on the HOME menu (out_of_focus). Gatefold itself, when it
 * runs as the application, is told apart by the pid it sent in Hello; since it says hello a
 * moment after it starts, a new application's pause waits a few seconds for that.
 */
void Engine::watchMain(void* self) {
    auto* e = static_cast<Engine*>(self);
    u64 lastPid = 0, seenAt = 0;
    s32 lastEnd = -1;
    u32 shown = GfbgApp_None;     // the state pause-for-apps last acted on
    u64 launchedPid = 0;          // the game whose launch was acted on: in front again, it is a return from HOME
    u64 gainsAt = 0;              // the gains file's header is read every 2 s
    int pdmFocus = -1;            // the newest application applet event: 1 in front, 0 not, −1 unknown
    for (;;) {
        svcSleepThread(250'000'000);
        const u64 now = nowMs();
        u64 pid = 0;
        const bool app = R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
        if (!app) pid = 0;
        if (pid != lastPid) { lastPid = pid; seenAt = now; pdmFocus = -1; }

        s32 total = 0, first = 0, end = 0;
        if (app && R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total, &first, &end)) && end != lastEnd) {
            lastEnd = end;
            PdmPlayEvent ev[16];
            s32 got = 0;
            if (R_SUCCEEDED(pdmqryQueryPlayEvent(std::max(first, end - 15), ev, 16, &got)))
                for (s32 i = 0; i < got; i++) {
                    if (ev[i].play_event_type != PdmPlayEventType_Applet || ev[i].event_data.applet.applet_id != AppletId_application) continue;
                    const u8 t = ev[i].event_data.applet.event_type;
                    if (t == PdmAppletEventType_Launch || t == PdmAppletEventType_InFocus) pdmFocus = 1;
                    else if (t == PdmAppletEventType_OutOfFocus || t == PdmAppletEventType_OutOfFocus4) pdmFocus = 0;
                }
        }

        mutexLock(&e->m_);
        const bool isGatefold = app && pid == e->gatefoldPid_;
        // A new game (not Gatefold) gets its volume as soon as it is known.
        const u64 gamePid = app && !isGatefold ? pid : 0;
        if (gamePid != e->appPid_) {
            e->appPid_ = gamePid;
            u64 program = 0;
            if (gamePid) pmdmntGetProgramId(&program, gamePid);
            e->appProgram_ = program;
            e->applyGameVolume();
        }
        mutexUnlock(&e->m_);
        u32 want = !app || isGatefold ? GfbgApp_None : pdmFocus == 0 ? GfbgApp_Suspended : GfbgApp_Foreground;
        // Give a fresh application a moment to turn out to be Gatefold.
        if (want == GfbgApp_Foreground && shown == GfbgApp_None && now - seenAt < 3500) want = shown;
        if (want != shown) {
            // A launch is the first time a game is in front (a new pid); after that it is a return. A game sent
            // to HOME within the grace period never had its launch acted on, so its first time in front still is one.
            const bool fresh = want == GfbgApp_Foreground && pid != launchedPid;
            if (fresh) launchedPid = pid;
            e->gameEvent(want, fresh);
            shown = want;
        }
        if (!app) launchedPid = 0;

        // Saving: two seconds after a change settles, and every 30 s while playing (the position).
        mutexLock(&e->m_);
        const bool due = (e->dirty_ && now - e->dirtyAt_ > 2000) || (e->status_ == GfbgStatus_Playing && now - e->savedAt_ > 30000);
        mutexUnlock(&e->m_);
        if (due) e->saveSession();
        e->writePlays();
        e->watchSleep(now);
        if (now - gainsAt >= 2000) { gainsAt = now; e->watchGains(); }
    }
}

// ============================================================ files


/**
 * The queue as the player left it (bg-session.txt), or else Gatefold's own session.txt and its
 * order and volume from settings.txt: either way it waits, stopped, at the saved position.
 */
void Engine::loadFiles() {
    for (const auto& l : readLines(cfgPath("bg-config.txt"))) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq);
        const char* v = l.c_str() + eq + 1;
        if (k == "onOpen") cfg_.onOpen = u8(std::clamp(atoi(v), 0, 3));
        else if (k == "onHome") cfg_.onHome = u8(std::clamp(atoi(v), 0, 3));
        else if (k == "onClose") cfg_.onClose = u8(std::clamp(atoi(v), 0, 3));
        else if (k == "onReturn") cfg_.onReturn = u8(std::clamp(atoi(v), 0, 3));   // 0.6 had none: Resume until set
        // 0.5's on/off switches, as the actions they were
        else if (k == "pauseForApps") cfg_.onOpen = atoi(v) ? GfbgAct_Pause : GfbgAct_Nothing;
        else if (k == "resumeOnHome") cfg_.onHome = atoi(v) ? GfbgAct_Resume : GfbgAct_Nothing;
        else if (k == "resumeOnExit") cfg_.onClose = atoi(v) ? GfbgAct_Resume : GfbgAct_Nothing;
        else if (k == "fadeOut") cfg_.fadeOut = std::clamp(float(atof(v)), 0.f, 10.f);
        else if (k == "fadeIn") cfg_.fadeIn = std::clamp(float(atof(v)), 0.f, 10.f);
        else if (k == "gameVolume") cfg_.gameVolume = std::clamp(float(atof(v)), 0.f, 1.f);
        else if (k == "levelling") cfg_.reserved[0] = u32(std::clamp(atoi(v), 0, 3));
    }
    for (const auto& l : readLines(cfgPath("bg-games.txt"))) {
        const size_t eq = l.find('=');
        if (eq != std::string::npos) gameVolumes_.push_back({ strtoull(l.c_str(), nullptr, 16), std::clamp(float(atof(l.c_str() + eq + 1)), 0.f, 1.f) });
    }

    bool own = true;
    std::vector<std::string> lines = readLines(cfgPath("bg-session.txt"));
    if (lines.empty()) {
        own = false;
        lines = readLines(cfgPath("session.txt"));
        for (const auto& l : readLines(cfgPath("settings.txt"))) {
            if (!l.compare(0, 6, "order=")) mode_ = u32(std::clamp(atoi(l.c_str() + 6), 0, 3));
            else if (!l.compare(0, 7, "volume=")) volume_ = vol_ = std::clamp(float(atof(l.c_str() + 7)), 0.f, 1.f);
        }
    }
    std::vector<s32> order;
    for (const auto& l : lines) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq);
        const char* v = l.c_str() + eq + 1;
        if (k == "track") { offs_.push_back(u32(blob_.size())); blob_.append(v); blob_.push_back('\0'); }
        else if (k == "name") name_ = v;
        else if (k == "pos") pos_ = atoi(v);
        else if (k == "position") resumeAt_ = std::max(0.0, atof(v));
        else if (own && k == "mode") mode_ = u32(std::clamp(atoi(v), 0, 3));
        else if (own && k == "volume") volume_ = vol_ = std::clamp(float(atof(v)), 0.f, 1.f);
        else if (k == "order") {
            char* endp = nullptr;
            for (long n = strtol(v, &endp, 10); endp != v; n = strtol(v, &endp, 10)) { order.push_back(s32(n)); v = endp; }
        }
    }
    bool ok = order.size() == offs_.size();
    for (s32 o : order) ok = ok && o >= 0 && o < count();
    if (ok) order_ = std::move(order);
    else identityOrder();
    pos_ = order_.empty() ? -1 : std::clamp(pos_, 0, s32(order_.size()) - 1);
    rng_ ^= armGetSystemTick();
}

/** "time<TAB>path" lines for Gatefold (Stats, stats.cpp), which renames the file away to read it. */
void Engine::writePlays() {
    mutexLock(&m_);
    if (plays_.empty()) { mutexUnlock(&m_); return; }
    std::vector<std::string> plays = std::move(plays_);
    plays_.clear();
    mutexUnlock(&m_);
    u64 t = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &t))) t = 0;   // 0: Gatefold dates it when it reads it
    // Opened and wound to its end rather than with "a": the plays must not depend on append mode.
    const std::string path = cfgPath("bg-plays.txt");
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) f = fopen(path.c_str(), "wb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    for (const auto& p : plays) fprintf(f, "%llu\t%s\n", (unsigned long long)t, p.c_str());
    fclose(f);
}

void Engine::saveSession() {
    std::string out;
    {
        Lock l(&m_);
        dirty_ = false;
        savedAt_ = nowMs();
        char head[256];
        const double position = status_ == GfbgStatus_Stopped ? resumeAt_ : std::max(0.0, base_ + (double(outFrames_) - double(inFlight_)) / GFBG_RATE);
        snprintf(head, sizeof head, "pos=%d\nposition=%.2f\nmode=%u\nvolume=%.3f\norder=", pos_, position, mode_, volume_);
        out = "name=" + name_ + "\n" + head;
        for (size_t i = 0; i < order_.size(); i++) {
            char n[16];
            snprintf(n, sizeof n, i ? " %d" : "%d", order_[i]);
            out += n;
        }
        out += "\n";
        for (int t = 0; t < count(); t++) { out += "track="; out += path(t); out += "\n"; }
    }
    mkdir("sdmc:/config", 0777);
    mkdir(Dir, 0777);
    // Next to the old one and renamed over it: a power cut mid-write never loses the queue.
    const std::string tmp = cfgPath("bg-session.tmp"), dst = cfgPath("bg-session.txt");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!ok) return;
    remove(dst.c_str());
    rename(tmp.c_str(), dst.c_str());
}

void Engine::saveConfig() {
    GfbgConfig c = config();
    mkdir("sdmc:/config", 0777);
    mkdir(Dir, 0777);
    if (FILE* f = fopen(cfgPath("bg-config.txt").c_str(), "wb")) {
        fprintf(f, "onOpen=%d\nonHome=%d\nonClose=%d\nonReturn=%d\nfadeOut=%.2f\nfadeIn=%.2f\ngameVolume=%.3f\n", c.onOpen, c.onHome,
                c.onClose, c.onReturn, double(c.fadeOut), double(c.fadeIn), double(c.gameVolume));
        fprintf(f, "levelling=%u\n", unsigned(c.reserved[0]));
        fclose(f);
    }
}
