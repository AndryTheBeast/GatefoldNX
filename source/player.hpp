/*
 * Gatefold NX · player.hpp
 * Playback when the background player is not there. Every format goes through FFmpeg (fed through
 * Mix_HookMusic), so crossfade and the equalizer work the same for all of them and the same as in the
 * background player; SDL_mixer's own decoders are only a fallback for a file FFmpeg cannot open.
 * The position is counted from the frames actually delivered.
 *
 * Crossfade: load() with a fade keeps the track playing now as an outgoing voice that fades out while
 * the new one fades in (equal power); the position, the end and the spectrum follow the new one.
 */
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "bgclient.hpp"
#include "ffdecoder.hpp"
#include "gfeq.hpp"

struct _Mix_Music;

class Player {
public:
    bool init();
    void shutdown();

    /**
     * Starts playback, or waits paused at 0. fade > 0 with a track playing: crossfades into it over
     * that many seconds (the track playing fades out meanwhile); otherwise it cuts, as before.
     */
    bool load(const std::string& path, bool paused = false, double fade = 0);
    void stop();
    void togglePause();
    void seek(double seconds);
    /** The main thread, once a frame: closes an outgoing voice whose fade has ended (not on the audio thread). */
    void update();

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
    /** The equalizer for what is played here (the background player has its own, GfbgCmd_SetEq). */
    void setEq(const GfbgEq& e);

    /** Newest n mono samples of what was mixed; false when nothing new since the last call. */
    bool latestPcm(float* out, int n);

    /** True once after a track has played to its end. */
    bool takeFinished() { return finished_.exchange(false); }

private:
    static void postMix(void* self, unsigned char* stream, int len);
    static void musicFinished();
    static void ffHook(void* self, unsigned char* stream, int len);
    void feedRing(const float* stereo, int frames);

    BgClient* bg_ = nullptr;
    _Mix_Music* music_ = nullptr;
    int rate_ = 48000;
    int frameBytes_ = 4;
    std::atomic<long long> frames_{ 0 };
    std::atomic<bool> finished_{ false };
    static Player* instance_;

    // FFmpeg path: the track playing (cur_) and, during a crossfade, the one fading out (out_)
    std::unique_ptr<FfDecoder> cur_, out_, done_;   // done_: an outgoing voice that ended, closed by update()
    std::mutex ffLock_;   // the audio callback decodes; load, seek and stop come from the UI thread
    std::atomic<bool> ffActive_{ false }, ffPaused_{ false }, ffEnded_{ false };
    long long fadeLen_ = 0, fadeAt_ = 0;   // frames: the crossfade's length and how far it is (ffLock_)
    float mixIn_[4096 * 2] = {}, mixOut_[4096 * 2] = {};
    int16_t pcmIn_[4096 * 2] = {};

    // The equalizer (gfeq.hpp), run in the hook. Settings are handed over through eqWant_ (eqLock_), which
    // the hook takes up when it can: a slider dragged never makes the hook wait, nor skip a buffer.
    GfEq eq_{ 48000 };
    GfbgEq eqWant_ = gfEqDefault();
    std::mutex eqLock_;
    std::atomic<bool> eqNew_{ false };

    static constexpr int Ring = 8192;
    float ring_[Ring] = {};
    std::atomic<unsigned> written_{ 0 };
    unsigned lastRead_ = 0;
    int channels_ = 2;
    bool float32_ = false;
    std::atomic<float> volume_{ 1.f }, vol_{ 1.f };   // vol_: volume_ eased in the hook, so a slider drag never crackles
    std::atomic<float> level_{ 1.f }, levelTarget_{ 1.f };
};
