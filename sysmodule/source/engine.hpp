/*
 * Gatefold NX background player · engine.hpp
 * The player itself: the queue and its order (the same rules as Gatefold's App::next), the
 * audio thread that decodes, resamples and feeds audout, the fades, and pause-for-apps.
 *
 * Threads: the IPC thread calls the public methods; anything that opens or seeks a file is
 * queued for the audio thread, so a slow SD card never stalls a reply.
 */
#pragma once
#include <switch.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "gfbg.h"
#include "resampler.hpp"

class Engine {
public:
    void start();

    // ---- IPC (any thread)
    void hello(u32 kind, u64 pid);
    GfbgState state();
    void setQueue(const u8* data, size_t size, u32 by);
    size_t queueBytes();
    void getQueue(u8* out, size_t cap);
    void playAt(s32 pos, bool paused, double start, u32 by);
    void transport(u32 op, u32 by);
    void seek(double t);
    void setVolume(float v);
    void setMode(u32 mode, u32 by);
    void setStopAfter(bool on);
    u32 pcm(float* out);
    GfbgConfig config();
    void setConfig(const GfbgConfig& c);
    void trackInfo(GfbgTrackInfo* out);
    GfbgGameVolume gameVolume();
    void setGameVolume(float v);

private:
    // AutoResume and PlayFade come from the game events: t is their fade-in.
    struct Cmd { enum Kind { PlayAt, Toggle, Pause, Resume, Stop, Next, Prev, Seek, AutoResume, PlayFade } kind; s32 pos; bool paused; double t; u32 by; };

    Mutex m_;
    std::deque<Cmd> cmds_;

    // ---- the queue: paths packed in one string (a sysmodule cannot afford a std::string each)
    std::string name_;
    std::string blob_;
    std::vector<u32> offs_;
    std::vector<s32> order_;
    s32 pos_ = -1;
    u32 mode_ = GfbgMode_Default;
    bool stopAfter_ = false;
    u32 queueGen_ = 1, trackGen_ = 1, errorGen_ = 0, trackBy_ = 0, queueBy_ = 0;
    int failed_ = 0;
    u64 rng_ = 0x9E3779B97F4A7C15ull;

    // ---- playback (audio thread, read under m_)
    std::unique_ptr<Decoder> dec_;
    Resampler rs_;
    u32 status_ = GfbgStatus_Stopped;
    double base_ = 0;          // the track time the output count starts from
    u64 outFrames_ = 0;        // 48 kHz frames made since base_
    u64 inFlight_ = 0;         // frames handed to audout and not played yet
    double resumeAt_ = 0;      // where a restored, stopped queue starts
    float volume_ = 1;
    float gain_ = 0, gainTarget_ = 1, gainStep_ = 1;   // per output frame
    float vol_ = 1;            // volume_ as applied, eased so a slider drag never crackles
    bool pauseAtSilence_ = false, autoPausing_ = false, autoPaused_ = false;
    bool tailFlushed_ = false;  // the resampler's tail is out: the track is really over
    bool flush_ = false;        // drop the buffers audout holds (a seek, a track picked by hand)
    std::string playing_;      // the path loaded now
    std::string coverPath_;    // the folder's cover image, when the file has none
    s16 decBuf_[1024 * 2];
    float mix_[2048 * 2];

    // ---- spectrum feed for Gatefold
    static constexpr int Ring = 8192;
    float ring_[Ring] = {};
    u32 ringAt_ = 0, serial_ = 0;

    // ---- pause-for-apps
    GfbgConfig cfg_{ GfbgAct_Pause, GfbgAct_Resume, GfbgAct_Resume, GfbgAct_Resume, 1.5f, 2.f, 1.f, {} };
    // Game volume (aud:a): per program id, applied when a game comes up.
    std::vector<std::pair<u64, float>> gameVolumes_;
    u64 appPid_ = 0, appProgram_ = 0;   // the application now, when it is not Gatefold
    float gameVolumeFor(u64 program) const;
    void applyGameVolume();             // m_ held
    void saveGameVolumes();
    u64 gatefoldPid_ = 0;
    u32 appState_ = GfbgApp_None;

    // ---- persistence
    bool dirty_ = false;
    u64 dirtyAt_ = 0, savedAt_ = 0;

    // queue helpers (m_ held)
    int count() const { return int(offs_.size()); }
    const char* path(int track) const { return blob_.c_str() + offs_[size_t(track)]; }
    s32 current() const { return pos_ >= 0 && pos_ < s32(order_.size()) ? order_[size_t(pos_)] : -1; }
    void shuffle();
    void identityOrder();
    void markDirty();

    // audio thread (m_ held unless noted)
    static void audioMain(void* self);
    static void watchMain(void* self);
    void runCommands();
    bool load(s32 pos, double start);
    void playAtPos(s32 pos, bool paused, double start);
    void next(bool natural);
    void prev();
    void stop(bool cut = true);   // cut: drop what audout still holds
    void rampTo(float target, float seconds, bool pauseAfter);
    int produce(s16* out, int frames);
    void feedRing(const float* stereo, int frames);

    // pause-for-apps (watch thread)
    void gameEvent(u32 state, bool fresh);   // the game opened (fresh) or came back, went to HOME, or quit: cfg_'s action
    void act(u8 action);             // m_ held
    bool playRandomFolder();         // GfbgAct_Play; scans the SD card, so m_ is not held

    // files
    void loadFiles();
    void saveSession();
    void saveConfig();
};
