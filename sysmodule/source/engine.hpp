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

#include "../../source/automix.hpp"   // Gatefold's own Automix (automix_shared.cpp builds it in)
#include "decoder.hpp"
#include "gfbg.h"
#include "gfeq.hpp"
#include "resampler.hpp"

class Engine {
public:
    void start();

    // ---- IPC (any thread)
    void hello(u32 kind, u64 pid);
    GfbgState state();
    bool setQueue(const u8* data, size_t size, u32 by);   // false: no room on the heap for it
    size_t queueBytes();
    void getQueue(u8* out, size_t cap);
    void playAt(s32 pos, bool paused, double start, u32 by);
    void transport(u32 op, u32 by);
    void seek(double t);
    void setVolume(float v);
    void setMode(u32 mode, u32 by);
    void setStopAfter(bool on);
    void setSleep(float seconds);
    u32 pcm(float* out);
    GfbgConfig config();
    void setConfig(const GfbgConfig& c);
    void trackInfo(GfbgTrackInfo* out);
    GfbgGameVolume gameVolume();
    void setGameVolume(float v);
    GfbgEq eq();
    void setEq(const GfbgEq& e);

private:
    // AutoResume and PlayFade come from the game events: t is their fade-in.
    struct Cmd { enum Kind { PlayAt, Toggle, Pause, Resume, Stop, Next, Prev, Seek, AutoResume, PlayFade } kind; s32 pos; bool paused; double t; u32 by; double start = 0; };

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
    float volume_ = 0.5f;      // first run: half, as Gatefold's
    float gain_ = 0, gainTarget_ = 1, gainStep_ = 1;   // per output frame
    float vol_ = 0.5f;          // volume_ as applied, eased so a slider drag never crackles
    bool pauseAtSilence_ = false, autoPausing_ = false, autoPaused_ = false;
    bool tailFlushed_ = false;  // the resampler's tail is out: the track is really over
    bool flush_ = false;        // drop the buffers audout holds (a seek, a track picked by hand)
    std::string playing_;      // the path loaded now
    // ---- crossfade (cfg_.crossfade): the track fading out (out*) while the one loaded fades in, equal power
    std::unique_ptr<Decoder> outDec_;
    Resampler outRs_;
    bool outTail_ = false;
    u64 xfLen_ = 0, xfAt_ = 0;   // 48 kHz frames: the fade's length, and how far it is
    bool xfChecked_ = false;     // this track's end was looked at
    float outMix_[2048 * 2];
    // Play statistics: a track heard long enough (Gatefold's Stats::threshold) is counted once per
    // play, and the watch thread appends it to bg-plays.txt for Gatefold to take in.
    u64 heardFrames_ = 0;
    bool counted_ = false;
    std::vector<std::string> plays_;
    std::string coverPath_;    // the folder's cover image, when the file has none
    s16 decBuf_[1024 * 2];
    float mix_[2048 * 2];

    // ---- spectrum feed for Gatefold
    static constexpr int Ring = 8192;
    float ring_[Ring] = {};
    u32 ringAt_ = 0, serial_ = 0;

    // ---- pause-for-apps
    // First-run defaults, as Gatefold's BgConfig (the user, 2026-10-28): nothing on game events, a 2 s fade out
    GfbgConfig cfg_{ GfbgAct_Nothing, GfbgAct_Nothing, GfbgAct_Nothing, GfbgAct_Nothing, 2.f, 2.f, 1.f, {}, 0.f, GfbgXfade_NotInAlbum, GfbgListStart_Continue, 0, {} };
    // Game volume (aud:a): per program id, applied when a game comes up.
    std::vector<std::pair<u64, float>> gameVolumes_;
    u64 appPid_ = 0, appProgram_ = 0;   // the application now, when it is not Gatefold
    float gameVolumeFor(u64 program) const;
    void applyGameVolume();             // m_ held
    void saveGameVolumes();
    u64 gatefoldPid_ = 0;
    u32 appState_ = GfbgApp_None;

    // ---- volume levelling: Gatefold's measured gains (GFBG_GAINS), the mode in cfg_.reserved[0]
    std::vector<GfbgGain> gains_;   // sorted by hash
    u32 gainsGen_ = 0;
    float lvl_ = 1, lvlTarget_ = 1;   // linear; eased towards the target, set at once when a track loads
    float levelFor(const std::string& path) const;   // m_ held
    void watchGains();                                // the watch thread, without the lock

    // ---- the equalizer (gfeq.hpp): set under m_, run by produce under m_
    GfbgEq eqCfg_ = gfEqDefault();
    GfEq eqDsp_{ float(GFBG_RATE) };
    u32 eqGen_ = 0;
    u64 eqSaveAt_ = 0;   // nowMs() when bg-config.txt is written: a slider dragged sends many, the card gets one

    // ---- the sleep timer: the music fades out over its last seconds (SleepFade) and pauses
    static constexpr double SleepFade = 10;
    u64 sleepAt_ = 0;          // nowMs() when it runs out, 0 off
    bool sleepFading_ = false;
    u32 sleepGen_ = 0;
    void watchSleep(u64 now);  // the watch thread, without the lock

    // ---- Automix (protocol 6): an Automix playlist's queue joins its tracks as Gatefold does, with Gatefold's model, from
    // Gatefold's analysis (GFBG_MIX: the player never analyses). The watch thread plans the join to the next track and opens
    // that one at the plan's offset ahead of time; the audio thread starts it on its sample (delay_ frames into a buffer) and
    // the two go through automix::Fader, the very mixer of Gatefold's player. A track not analysed yet: a plain fade.
    bool automix_ = false;            // the queue is an Automix playlist (GfbgQueue_Automix): in its order, joined by Automix
    struct MixJoin {
        u32 gen = 0;                  // trackGen_ it was planned for (0: none)
        s32 pos = -1;                 // the next track's position in the order
        std::string next;             // … its path: a queue edit that changes it plans again
        automix::Plan plan;
        bool off = false;             // no join (Off, or a track shorter than two of it): this one plays whole
        std::unique_ptr<Decoder> dec; // the next track, at the plan's offset (handed over by the watch thread)
        std::string cover;
    };
    MixJoin mj_;                      // m_ held
    // the watch thread's own: the next track being opened and decoded to its offset, a few seconds a tick
    std::unique_ptr<Decoder> prepDec_;
    std::string prepPath_, prepCover_;
    u32 prepGen_ = 0;
    u64 prepDone_ = 0, prepWant_ = 0; // its frames decoded so far, and the offset's
    // the two analyses last read (mix.tsv is long: the next track's is this one's at the next join)
    std::string infoPath_[2];
    mixan::Info info_[2];
    bool infoOk_[2] = {};
    bool mixInfo(const std::string& path, mixan::Info& out);   // the watch thread, without the lock
    // a downloaded Automix model (GfbgConfig.automixFile: config/gatefold-nx/models/<file>), read once it is named
    std::string modelName_;           // the file read (the watch thread's)
    automix::ModelFile modelFile_;
    bool modelOk_ = false;
    // the join playing (audio thread, m_ held)
    automix::Fader fader_;
    automix::Plan joinPlan_;
    bool joining_ = false;            // outDec_ is a join's outgoing track (not a plain crossfade)
    u64 joinLen_ = 0, joinAt_ = 0, delay_ = 0;   // 48 kHz frames: its length, how far it is, and before the incoming one starts
    bool joinEcho_ = false;           // its echo rings on past the outgoing track's end
    float outLvl_ = 1;                // the outgoing track's own levelling gain
    u32 joinGen_ = 0;
    void watchAutomix();              // the watch thread, without the lock
    bool startJoin();                 // the planned join begins (audio thread, m_ held)
    void endJoin();                   // … and its outgoing track goes (m_ held)
    static char kindOf(const std::string& name);   // a playlist's kind from GFBG_PLAYLISTS' "!kinds" line (0: unknown)

    // ---- persistence
    bool dirty_ = false;
    u64 dirtyAt_ = 0, savedAt_ = 0;
    u32 edits_ = 0;            // bumps when the queue's paths or order change: a save that sees it move starts again later

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
    /** The bookkeeping of a track loaded (load, crossfadeInto); false, with pos_ moved on, when d is null. */
    bool adopt(s32 pos, std::unique_ptr<Decoder> d, const std::string& p, const std::string& cover, double start);
    /** The next track's position if this track's end may crossfade into it (next(true)'s rules), else −1. */
    s32 crossfadeNextPos(double left) const;
    /** Starts fading into pos over fade seconds; false (nothing changed) when it cannot: no memory to spare,
     *  the file did not open, or (albumRule) it is the next track of the same album. Unlocks m_ to open it. */
    bool crossfadeInto(s32 pos, double fade, bool albumRule);
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
    bool playPlaylist(u64 hash);     // GfbgAct_Playlist: that one of Gatefold's playlists (GFBG_PLAYLISTS); m_ not held
    // Each action playlist's place (its track and time) when another queue took over, for GfbgListStart_Continue.
    struct Place { u64 hash; double at; std::string path; };
    std::vector<Place> places_;
    u64 listHash_ = 0;               // the queue now is this action playlist (0: anything else)
    void rememberPlace();            // m_ held
    void savePlaces();

    // files
    void loadFiles();
    void saveSession();
    void writePlays();   // the watch thread, without the lock
    void saveConfig();
};
