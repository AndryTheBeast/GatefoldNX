/*
 * Gatefold NX · player.cpp
 */
#include "player.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cores.hpp"

Player* Player::instance_ = nullptr;

namespace {
double clockNow() { return SDL_GetTicks64() / 1000.0; }
}

bool Player::init() {
    Mix_Init(MIX_INIT_FLAC | MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_OPUS);
    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) != 0) {
        SDL_Log("Mix_OpenAudio: %s", Mix_GetError());
        return false;
    }
    Uint16 fmt = 0;
    int ch = 2;
    Mix_QuerySpec(&rate_, &fmt, &ch);
    frameBytes_ = (SDL_AUDIO_BITSIZE(fmt) / 8) * ch;
    channels_ = ch;
    float32_ = SDL_AUDIO_ISFLOAT(fmt);
    instance_ = this;
    Mix_SetPostMix(&Player::postMix, this);
    Mix_HookMusicFinished(&Player::musicFinished);
    return true;
}

void Player::shutdown() {
    bg_ = nullptr;   // the background player keeps playing: that is what it is for
    stop();
    Mix_HookMusicFinished(nullptr);
    Mix_SetPostMix(nullptr, nullptr);
    Mix_CloseAudio();
    Mix_Quit();
    instance_ = nullptr;
}

bool Player::load(const std::string& path, bool paused) {
    stop();
    frames_ = 0;
    if (ffHandles(path)) {
        if (float32_) return false;   // the hook writes 16-bit samples, which is what Mix_OpenAudio asked for
        {
            std::lock_guard<std::mutex> g(ffLock_);
            if (!ff_.open(path, rate_, channels_)) {
                SDL_Log("ffmpeg could not open %s", path.c_str());
                return false;
            }
        }
        ffPaused_ = paused;
        ffEnded_ = false;
        ffActive_ = true;
        Mix_HookMusic(&Player::ffHook, this);
        return true;
    }
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

void Player::stop() {
    if (remote()) { bg_->stop(); return; }
    if (ffActive_) {
        Mix_HookMusic(nullptr, nullptr);   // takes the audio lock, so the hook is not running after this
        ffActive_ = false;
        std::lock_guard<std::mutex> g(ffLock_);
        ff_.close();
    }
    if (music_) {
        Mix_HaltMusic();
        Mix_FreeMusic(music_);
        music_ = nullptr;
    }
    frames_ = 0;
    finished_ = false;   // the SDL_mixer hook also fires on halt; that is not a natural end
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
    if (remote()) { bg_->seek(seconds, clockNow()); return; }
    if (ffActive_) {
        std::lock_guard<std::mutex> g(ffLock_);
        if (ff_.seek(seconds)) { frames_ = (long long)(seconds * rate_); ffEnded_ = false; }
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

bool Player::latestPcm(float* out, int n) {
    if (remote()) return bg_->pcm(out, n);
    const unsigned w = written_.load();
    if (w == lastRead_ || n > Ring) return false;
    lastRead_ = w;
    for (int i = 0; i < n; i++) out[i] = ring_[(w - n + i) % Ring];
    return true;
}

/** Audio thread: FFmpeg-decoded music, with our own pause and volume (SDL_mixer's apply to its own music only). */
void Player::ffHook(void* self, unsigned char* stream, int len) {
    auto* p = static_cast<Player*>(self);
    memset(stream, 0, size_t(len));
    if (!p->ffActive_ || p->ffPaused_ || p->ffEnded_) return;
    std::unique_lock<std::mutex> lk(p->ffLock_, std::try_to_lock);
    if (!lk.owns_lock()) return;   // a seek is in progress: one buffer of silence
    auto* out = reinterpret_cast<int16_t*>(stream);
    const int frames = len / (p->channels_ * 2);
    const int got = p->ff_.read(out, frames);
    const float vol = p->volume_.load();
    if (vol < 0.999f)
        for (int i = 0; i < got * p->channels_; i++) out[i] = int16_t(out[i] * vol);
    p->frames_ += got;
    if (got < frames && p->ff_.atEnd()) {
        p->ffEnded_ = true;
        p->finished_ = true;
    }
}

void Player::postMix(void* self, unsigned char* stream, int len) {
    // SDL's audio thread (decoding included: SDL_mixer and the FFmpeg hook run on it) starts on the main
    // thread's core; it gets one of its own.
    static thread_local bool moved = false;
    if (!moved) { cores::moveHere(cores::Audio); moved = true; }
    auto* p = static_cast<Player*>(self);
    const bool sdlPlaying = p->music_ && Mix_PlayingMusic() && !Mix_PausedMusic();
    const bool ffPlaying = p->ffActive_ && !p->ffPaused_ && !p->ffEnded_;
    if (!sdlPlaying && !ffPlaying) return;
    const int frames = len / p->frameBytes_;
    // Volume levelling: here, after both paths (SDL_mixer's music and the FFmpeg hook) are mixed. Eased
    // per buffer towards the target; a gain above 1 is clamped (the peak limit keeps that rare).
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
    if (sdlPlaying) p->frames_ += frames;   // the FFmpeg hook counts its own frames
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
