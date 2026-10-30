/*
 * Gatefold NX · player.cpp
 */
#include "player.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "cores.hpp"

Player* Player::instance_ = nullptr;

namespace {
double clockNow() { return SDL_GetTicks64() / 1000.0; }
}

bool Player::init() {
    Mix_Init(MIX_INIT_FLAC | MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_OPUS);
#ifdef GF_WINDOWED
    // Exactly this, not what the device would rather have: the mix, its joins and its equalizer are 48 kHz 16-bit stereo; SDL converts
    if (Mix_OpenAudioDevice(48000, AUDIO_S16SYS, 2, 2048, nullptr, 0) != 0) {
#else
    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) != 0) {
#endif
        SDL_Log("Mix_OpenAudio: %s", Mix_GetError());
        return false;
    }
    Uint16 fmt = 0;
    int ch = 2;
    Mix_QuerySpec(&rate_, &fmt, &ch);
    frameBytes_ = (SDL_AUDIO_BITSIZE(fmt) / 8) * ch;
    channels_ = ch;
    float32_ = SDL_AUDIO_ISFLOAT(fmt);
    eq_ = GfEq(float(rate_));
    fader_.init(rate_);
    outLoop_.init(rate_);
    stretch_.init();
    stretchOut_.init();
    ffFeederStart();   // the decoders' read-ahead (ended in shutdown)
    instance_ = this;
    Mix_SetPostMix(&Player::postMix, this);
    Mix_HookMusicFinished(&Player::musicFinished);
    return true;
}

void Player::shutdown() {
    bg_ = nullptr;   // the background player keeps playing: that is what it is for
    stop();
    prepJoin();
    prepDec_.reset();   // (before the feeder stops: a prepared decoder reads ahead on it)
    ffFeederStop();
    Mix_HookMusicFinished(nullptr);
    Mix_SetPostMix(nullptr, nullptr);
    Mix_CloseAudio();
    Mix_Quit();
    instance_ = nullptr;
}

bool Player::load(const std::string& path, bool paused, double fade, const automix::Plan* mix) {
    // Opened before anything stops: the file's tables are read while the music still plays. Automix's next track was
    // usually opened and moved to its offset already, on a worker (prepare)
#ifdef GF_PREVIEW
    const auto loadT0 = std::chrono::steady_clock::now();
#endif
    prepJoin();
    std::unique_ptr<FfDecoder> d;
    bool ff = false;
    double offset = 0;
    const bool prepared = prepDec_ && mix && prepPath_ == path && std::fabs(prepOffset_ - mix->offset) < 1e-3;
    if (prepared) {
        d = std::move(prepDec_);
        ff = true;
        offset = prepOffsetUsed_;
    } else {
        prepDec_.reset();
        d = std::make_unique<FfDecoder>();
        ff = !float32_ && channels_ == 2 && d->open(path, rate_, channels_);   // the hook writes 16-bit stereo, as asked for
        if (ff && mix && mix->offset > 0.001 && d->skip(mix->offset)) offset = mix->offset;   // Automix: from its first beat
        if (ff) d->prefetch(rate_ * 3, rate_ / 2);   // read ahead (ffdecoder.hpp): half a second now, the rest on the feeder
    }
#ifdef GF_PREVIEW
    if (mix)   // the PC preview checks it with this
        SDL_Log("load: %s, %.1f ms on the main thread", prepared ? "prepared ahead" : "opened here",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadT0).count());
#endif
    if (!ff) {
        // SDL_mixer's own decoders, for a file FFmpeg cannot open: no crossfade or equalizer there.
        stop();
        frames_ = 0;
        music_ = Mix_LoadMUS(path.c_str());
        if (!music_) {
            SDL_Log("Mix_LoadMUS(%s): %s", path.c_str(), Mix_GetError());
            return false;
        }
        // Holding the audio lock, so not one buffer plays before a paused start takes effect.
        SDL_LockAudio();
        const bool ok = Mix_PlayMusic(music_, 1) == 0;
        if (ok && paused) Mix_PauseMusic();
        SDL_UnlockAudio();
        if (!ok) {
            SDL_Log("Mix_PlayMusic: %s", Mix_GetError());
            stop();
            return false;
        }
        return true;
    }
    if (music_) {
        Mix_HaltMusic();
        Mix_FreeMusic(music_);
        music_ = nullptr;
    }
    const bool cross = fade > 0.05 && !paused && ffActive_ && !ffPaused_ && !ffEnded_ && cur_;
    std::unique_ptr<FfDecoder> old1, old2;   // closed after the lock is let go
    {
        std::lock_guard<std::mutex> g(ffLock_);
        if (cross) {
#ifdef GF_PREVIEW
            SDL_Log("crossfade %.2f s into %s", fade, path.c_str());   // the PC preview checks it with this
#endif
            old1 = std::move(out_);   // a crossfade still running: that outgoing track just stops
            out_ = std::move(cur_);
            fadeLen_ = std::max(1LL, (long long)(fade * rate_));
            fadeAt_ = 0;
            // Automix: the incoming track waits for its sample of the outgoing one (frames_ is where that one is, in its own
            // time; stretched, its frames go by at its speed). A seek undid its stretch: taken up at once, to stay on the beat
            if (mix && mix->speed != 1 && !stretch_.active()) {
                automix::Plan now = *mix;
                now.easeIn = 0;
                stretch_.begin(now, rate_);
            }
            const double sp = mix && stretch_.active() ? stretch_.speedNow() : 1;
            delay_ = mix ? std::max(0LL, (long long)std::llround((mix->startOut * rate_ - double(frames_.load())) / sp)) : 0;
            outLevel_ = level_.load();
            automix::Plan plain;   // a plain crossfade: equal power, nothing else
            plain.join = automix::fromPreset(automix::Preset::Fade);
            fader_.begin(mix ? *mix : plain);
            outEcho_ = mix && mix->join.effect == automix::Effect::Echo;
            outLoop_.begin(mix ? *mix : plain, frames_.load());
        } else {
            old1 = std::move(cur_);
            old2 = std::move(out_);
            fadeLen_ = fadeAt_ = delay_ = 0;
        }
        cur_ = std::move(d);
        frames_ = (long long)std::llround(offset * rate_);
        // tempo matching (v3): the outgoing track keeps its stretcher, at the new one's tempo to its end; the new one plays as it is
        if (cross) std::swap(stretch_, stretchOut_);
        else stretchOut_.stop();
        stretch_.stop();
        ffPaused_ = paused;
        ffEnded_ = false;
        finished_ = false;
    }
    if (!ffActive_) {
        ffActive_ = true;
        Mix_HookMusic(&Player::ffHook, this);
    }
    return true;
}

void Player::preview(std::vector<int16_t>&& pcm) {
    std::vector<int16_t> old;
    {
        std::lock_guard<std::mutex> g(ffLock_);
        old.swap(pv_);
        pv_ = std::move(pcm);
        pvLen_ = pv_.size() / 2;
        pvAt_ = 0;
        pvOn_ = true;
    }
    Mix_HookMusic(&Player::ffHook, this);   // also when nothing played here (the background player had the music)
}

void Player::stopPreview() {
    if (!pvOn_) return;
    std::vector<int16_t> old;
    {
        std::lock_guard<std::mutex> g(ffLock_);
        pvOn_ = false;
        old.swap(pv_);
        pvLen_ = pvAt_ = 0;
    }
    if (!ffActive_) Mix_HookMusic(nullptr, nullptr);
}

void Player::stop() {
    if (remote()) { bg_->stop(); return; }
    if (ffActive_) {
        Mix_HookMusic(nullptr, nullptr);   // takes the audio lock, so the hook is not running after this
        ffActive_ = false;
        std::lock_guard<std::mutex> g(ffLock_);
        cur_.reset();
        out_.reset();
        done_.reset();
    }
    if (music_) {
        Mix_HaltMusic();
        Mix_FreeMusic(music_);
        music_ = nullptr;
    }
    frames_ = 0;
    finished_ = false;   // the SDL_mixer hook also fires on halt; that is not a natural end
}

void Player::update() {
    if (!doneSet_.load()) return;   // taking the lock every frame cost a buffer of silence whenever the callback came then
    std::unique_ptr<FfDecoder> done;
    {
        std::lock_guard<std::mutex> g(ffLock_);
        done = std::move(done_);
        doneSet_ = false;
    }
}

void Player::prepare(const std::string& path, double offset) {
    if (remote() || float32_ || channels_ != 2) return;
    prepJoin();
    prepDec_.reset();
    prepPath_ = path;
    prepOffset_ = offset;
    prepRunning_ = true;
    prepThread_ = std::thread([this, path, offset] {
        cores::moveHere(cores::Loader);
        auto d = std::make_unique<FfDecoder>();
        if (d->open(path, rate_, channels_)) {
            double used = 0;
            if (offset > 0.001 && d->skip(offset)) used = offset;
            d->prefetch(rate_ * 3, rate_ / 2);
            prepOffsetUsed_ = used;
            prepDec_ = std::move(d);   // read by load() only after joining this thread
        }
        prepRunning_ = false;
    });
}

void Player::beginStretch(const automix::Plan& p) {
    if (remote() || p.speed == 1) return;
    std::lock_guard<std::mutex> g(ffLock_);
    if (ffActive_ && cur_ && !stretch_.active()) stretch_.begin(p, rate_);
}

Player::Trace Player::takeTrace() {
    Trace t;
    t.worstMs = float(worstUs_.exchange(0)) / 1000.f;
    t.late = late_.exchange(0);
    t.lockMiss = lockMiss_.exchange(0);
    t.starved = ffStarved();
    return t;
}

void Player::togglePause() {
    if (remote()) { bg_->toggle(clockNow()); return; }
    if (ffActive_) { ffPaused_ = !ffPaused_; return; }
    if (!music_) return;
    if (Mix_PausedMusic()) Mix_ResumeMusic();
    else Mix_PauseMusic();
}

void Player::seek(double seconds) {
    if (seconds < 0) seconds = 0;
    seeks_++;
    if (remote()) { bg_->seek(seconds, clockNow()); return; }
    if (ffActive_) {
        std::unique_ptr<FfDecoder> old;
        std::lock_guard<std::mutex> g(ffLock_);
        old = std::move(out_);   // a seek ends a crossfade: the outgoing track stops
        fadeLen_ = fadeAt_ = delay_ = 0;
        stretch_.stop();   // a seek: its own tempo, straight from the decoder
        if (cur_ && cur_->seek(seconds)) { frames_ = (long long)(seconds * rate_); ffEnded_ = false; }
        return;
    }
    if (!music_) return;
    // MP3, Ogg, Opus and FLAC all take an absolute position in SDL_mixer 2.0.
    if (Mix_SetMusicPosition(seconds) == 0) frames_ = (long long)(seconds * rate_);
}

bool Player::loaded() const {
    if (remote()) return bg_->state().pos >= 0 && bg_->state().queueCount > 0;
    return music_ != nullptr || ffActive_;
}

bool Player::paused() const {
    if (remote()) return bg_->state().status != BgState::Playing;
    return ffActive_ ? ffPaused_.load() : (music_ && Mix_PausedMusic());
}

double Player::position() const { return remote() ? bg_->position(clockNow()) : double(frames_.load()) / rate_; }

void Player::setVolume(float v) {
    v = std::clamp(v, 0.f, 1.f);
    volume_ = v;
    if (remote()) bg_->setVolume(v);
    Mix_VolumeMusic(int(std::lround(v * MIX_MAX_VOLUME)));
}

void Player::setEq(const GfbgEq& e) {
    std::lock_guard<std::mutex> g(eqLock_);
    eqWant_ = e;
    eqNew_ = true;
}

bool Player::latestPcm(float* out, int n) {
    if (remote()) return bg_->pcm(out, n);
    const unsigned w = written_.load();
    if (w == lastRead_ || n > Ring) return false;
    lastRead_ = w;
    for (int i = 0; i < n; i++) out[i] = ring_[(w - n + i) % Ring];
    return true;
}

/** The mono mix before volume and the equalizer, for the spectrum (as the background player's). */
void Player::feedRing(const float* stereo, int frames) {
    unsigned w = written_.load();
    for (int i = 0; i < frames; i++) ring_[w++ % Ring] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
    written_ = w;
}

/**
 * Audio thread: the FFmpeg voices, mixed. The track playing, plus during a crossfade the one fading out
 * (equal power: sin in, cos out); then volume, levelling and the equalizer (whose headroom the volume
 * gives back, gfeq.hpp), as the background player does. SDL_mixer's volume applies to its own music only.
 */
void Player::ffHook(void* self, unsigned char* stream, int len) {
    auto* p = static_cast<Player*>(self);
    memset(stream, 0, size_t(len));
    std::unique_lock<std::mutex> lk(p->ffLock_, std::try_to_lock);
    if (!lk.owns_lock()) { p->lockMiss_++; return; }   // a load or seek is being made: one buffer of silence
    const auto t0 = std::chrono::steady_clock::now();
    if (p->pvOn_) {   // the join editor's Listen, at the volume set (no levelling or equalizer: as the two tracks are)
        auto* out = reinterpret_cast<int16_t*>(stream);
        const size_t at = p->pvAt_, n = std::min(size_t(len / 4), p->pv_.size() / 2 - std::min(at, p->pv_.size() / 2));
        const float v = p->volume_.load();
        for (size_t i = 0; i < n * 2; i++) out[i] = int16_t(std::lrint(p->pv_[at * 2 + i] * v));
        p->pvAt_ = at + n;
        return;
    }
    if (!p->ffActive_ || p->ffPaused_) return;
    if (p->eqNew_) {
        std::unique_lock<std::mutex> el(p->eqLock_, std::try_to_lock);
        if (el.owns_lock()) { p->eq_.set(p->eqWant_); p->eqNew_ = false; }
    }
    auto* out = reinterpret_cast<int16_t*>(stream);
    const int frames = len / 4;
    constexpr int Chunk = 4096;
    for (int at = 0; at < frames; at += Chunk) {
        const int n = std::min(Chunk, frames - at);
        // the track playing (with Automix it may start some frames into this buffer: dd)
        const int dd = p->out_ ? int(std::min<long long>(p->delay_, n)) : 0;
        int got = 0;
        long long used = 0;   // the track's own frames those were (fewer or more than got while its tempo is matched)
        if (!p->ffEnded_ && p->cur_ && dd < n) {
            if (p->stretch_.active()) got = p->stretch_.read([p](int16_t* b, int k) { return p->cur_->read(b, k); }, p->pcmIn_, n - dd, used);
            else used = got = p->cur_->read(p->pcmIn_, n - dd);
        }
        std::fill(p->mixIn_, p->mixIn_ + dd * 2, 0.f);
        for (int i = 0; i < got * 2; i++) p->mixIn_[dd * 2 + i] = p->pcmIn_[i] * (1.f / 32768.f);
        std::fill(p->mixIn_ + (dd + got) * 2, p->mixIn_ + n * 2, 0.f);
        p->frames_ += used;
        if (dd < n && got < n - dd && !p->ffEnded_ && p->cur_ && p->cur_->atEnd()) {
            p->ffEnded_ = true;
            p->finished_ = true;
        }
        // the one fading out (automix::Fader: equal power, and the bass swap when Automix asks for one); with
        // Automix, before the incoming track starts (dd frames) it plays on alone
        if (p->out_) {
            const int og = p->outLoop_.read(
                [p](int16_t* b, int k) {
                    long long u;
                    return p->stretchOut_.active() ? p->stretchOut_.read([p](int16_t* b2, int k2) { return p->out_->read(b2, k2); }, b, k, u)
                                                   : p->out_->read(b, k);
                },
                p->pcmIn_, n);
            const float keepOut = p->outLevel_ / std::max(1e-4f, p->level_.load());   // the levelling below then gives it its own gain
            for (int i = 0; i < n * 2; i++) p->outF_[i] = i < og * 2 ? p->pcmIn_[i] * (1.f / 32768.f) * keepOut : 0.f;
            for (int i = 0; i < dd * 2; i++) p->mixIn_[i] = p->outF_[i];
            if (dd < n)
                p->fader_.mix(p->mixIn_ + dd * 2, p->outF_ + dd * 2, n - dd, double(p->fadeAt_) / double(p->fadeLen_),
                              1.0 / double(p->fadeLen_));
            p->delay_ -= dd;
            p->fadeAt_ += n - dd;
            if (og < n && p->delay_ > 0) p->delay_ = 0;   // the outgoing track ended before its mix point: no gap
            // the fade's end; or the outgoing track's, unless its echo still rings on
            if (p->fadeAt_ >= p->fadeLen_ || (og < n && !p->outEcho_)) {
                if (!p->done_) { p->done_ = std::move(p->out_); p->doneSet_ = true; }   // closed by update(), off the audio thread
                else p->out_.reset();
                p->fadeLen_ = p->fadeAt_ = p->delay_ = 0;
            }
        }
        p->feedRing(p->mixIn_, n);
        // levelling, the equalizer, then the volume: last, so it works as with the EQ off (gfeq.hpp)
        float vol = p->vol_.load(), lv = p->level_.load();
        const float want = p->volume_.load(), wantLv = p->levelTarget_.load();
        for (int i = 0; i < n; i++) {
            lv += (wantLv - lv) * 0.00005f;    // ~0.4 s
            p->mixIn_[i * 2] *= lv;
            p->mixIn_[i * 2 + 1] *= lv;
        }
        p->level_ = lv;
        p->eq_.process(p->mixIn_, n);
        for (int i = 0; i < n; i++) {
            vol += (want - vol) * 0.0005f;     // ~40 ms to settle
            p->mixIn_[i * 2] *= vol;
            p->mixIn_[i * 2 + 1] *= vol;
        }
        p->vol_ = vol;
        for (int i = 0; i < n * 2; i++) out[at * 2 + i] = int16_t(std::lrint(std::clamp(p->mixIn_[i] * 32767.f, -32768.f, 32767.f)));
    }
    // the trace: how long this took, against how long the buffer plays
    const unsigned us = unsigned(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (us > p->worstUs_.load()) p->worstUs_ = us;
    if (double(us) > 1e6 * frames / p->rate_) p->late_++;
}

void Player::postMix(void* self, unsigned char* stream, int len) {
    // SDL's audio thread (decoding included: SDL_mixer and the FFmpeg hook run on it) starts on the main
    // thread's core; it gets one of its own.
    static thread_local bool moved = false;
    if (!moved) { cores::moveHere(cores::Audio); moved = true; }
    auto* p = static_cast<Player*>(self);
    // The FFmpeg hook does its own levelling, equalizer and spectrum; this is for SDL_mixer's music only.
    const bool sdlPlaying = !p->ffActive_ && p->music_ && Mix_PlayingMusic() && !Mix_PausedMusic();
    if (!sdlPlaying) return;
    const int frames = len / p->frameBytes_;
    // Volume levelling: eased per buffer towards the target; a gain above 1 is clamped (the peak limit keeps that rare).
    {
        float lv = p->level_.load();
        const float want = p->levelTarget_.load();
        lv += (want - lv) * std::min(1.f, frames / (0.4f * p->rate_));
        p->level_ = lv;
        if (std::fabs(lv - 1.f) > 0.001f) {
            const int n = frames * p->channels_;
            if (p->float32_) {
                auto* s = reinterpret_cast<float*>(stream);
                for (int i = 0; i < n; i++) s[i] = std::clamp(s[i] * lv, -1.f, 1.f);
            } else {
                auto* s = reinterpret_cast<int16_t*>(stream);
                for (int i = 0; i < n; i++) s[i] = int16_t(std::clamp(int(std::lround(s[i] * lv)), -32768, 32767));
            }
        }
    }
    p->frames_ += frames;
    // Mono copy for the spectrum. Mix_OpenAudio asked for S16, but read what was granted.
    unsigned w = p->written_.load();
    const int ch = p->channels_;
    // The post-mix buffer is after the volume; undo it so the spectrum does not shrink with it.
    const float gain = 1.f / (ch * std::max(0.05f, p->volume_.load()));
    for (int i = 0; i < frames; i++) {
        float v = 0;
        for (int c = 0; c < ch; c++)
            v += p->float32_ ? reinterpret_cast<const float*>(stream)[i * ch + c]
                             : reinterpret_cast<const int16_t*>(stream)[i * ch + c] / 32768.f;
        p->ring_[w++ % Ring] = v * gain;
    }
    p->written_ = w;
}

void Player::musicFinished() {
    if (instance_) instance_->finished_ = true;
}
