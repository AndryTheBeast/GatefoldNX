/*
 * Gatefold NX · player.hpp
 * Playback through SDL_mixer: its own decoders for FLAC/MP3/Ogg/Opus/WAV, and
 * FFmpeg (fed through Mix_HookMusic) for M4A/AAC/ALAC, which SDL_mixer 2.0
 * cannot open. SDL_mixer 2.0 has no position API, so the position is counted
 * from the frames actually delivered.
 */
#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "bgclient.hpp"
#include "ffdecoder.hpp"

struct _Mix_Music;

class Player {
public:
    bool init();
    void shutdown();

    bool load(const std::string& path, bool paused = false);   // starts playback, or waits paused at 0
    void stop();
    void togglePause();
    void seek(double seconds);

    bool loaded() const;
    bool paused() const;
    double position() const;
    int sampleRate() const { return remote() ? 48000 : rate_; }

    void setRemote(BgClient* bg) { bg_ = bg; }
    bool remote() const { return bg_ && bg_->connected(); }

    /** 0..1, applied to music playback. */
    void setVolume(float v);
    /** Volume levelling (levels.hpp): a linear gain on everything played here, eased over ~0.4 s, or at once. */
    void setLevel(float gain, bool now = false) { levelTarget_ = gain; if (now) level_ = gain; }

    /** Newest n mono samples of what was mixed; false when nothing new since the last call. */
    bool latestPcm(float* out, int n);

    /** True once after a track has played to its end. */
    bool takeFinished() { return finished_.exchange(false); }

private:
    static void postMix(void* self, unsigned char* stream, int len);
    static void musicFinished();
    static void ffHook(void* self, unsigned char* stream, int len);

    BgClient* bg_ = nullptr;
    _Mix_Music* music_ = nullptr;
    int rate_ = 48000;
    int frameBytes_ = 4;
    std::atomic<long long> frames_{ 0 };
    std::atomic<bool> finished_{ false };
    static Player* instance_;

    // FFmpeg path
    FfDecoder ff_;
    std::mutex ffLock_;   // the audio callback decodes; seek and stop come from the UI thread
    std::atomic<bool> ffActive_{ false }, ffPaused_{ false }, ffEnded_{ false };

    static constexpr int Ring = 8192;
    float ring_[Ring] = {};
    std::atomic<unsigned> written_{ 0 };
    unsigned lastRead_ = 0;
    int channels_ = 2;
    bool float32_ = false;
    std::atomic<float> volume_{ 1.f };
    std::atomic<float> level_{ 1.f }, levelTarget_{ 1.f };
};
