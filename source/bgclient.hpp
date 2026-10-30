/*
 * Gatefold NX · bgclient.hpp
 * Gatefold's side of the background player (sysmodule/, protocol in sysmodule/include/gfbg.h).
 * While the sysmodule runs it does all the playback, so the song carries on when Gatefold
 * closes; Gatefold sends it the queue and transport and draws what it reports.
 *
 * The header keeps libnx's IPC types out of the views: BgState mirrors GfbgState.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfeqcfg.h"

struct BgState {
    enum Status { Stopped = 0, Playing = 1, Paused = 2 };
    int status = Stopped;
    int pos = -1, track = -1;
    unsigned trackGen = 0, queueGen = 0, errorGen = 0;
    unsigned trackBy = 0, queueBy = 0;   // 0 the player itself, 1 Gatefold, 2 the overlay
    unsigned queueCount = 0, queueBytes = 0;
    int mode = 0;
    bool stopAfter = false, autoPaused = false;
    double position = 0, duration = 0;
    float volume = 1, gain = 1;
    int sampleRate = 0, channels = 0, codec = 0, bitrate = 0;
    int appState = 0;
    unsigned heapFree = 0;
    float sleepLeft = 0;     // seconds until the sleep timer pauses the music, 0 off
    unsigned sleepGen = 0;   // bumps when it runs out
    unsigned eqGen = 0;      // bumps on every SetEq, whoever sent it (the overlay too)
    // Automix's join in the player (protocol 6): joinGen bumps as each starts; joining 0 none, 1 the next one planned, 2 the
    // two tracks playing together; joinAt where what is heard is in it (negative before it)
    unsigned joinGen = 0;
    int joining = 0;
    float joinAt = 0, joinLength = 0, joinBpm = 0;
    unsigned joinFlags = 0;   // 1 on the beat, 2 on the drop (GfbgJoin_*)
};

struct BgConfig {
    // What the music does when a game opens, waits on the HOME menu, comes back from it, or closes:
    // GfbgAct (0 nothing, 1 play a random folder, 2 resume, 3 pause).
    // First-run defaults (the user, 2026-10-28): the music does nothing by itself on game events; as Engine::cfg_
    int onOpen = 0, onHome = 0, onClose = 0, onReturn = 0;
    float fadeOut = 2.f, fadeIn = 2.f;
    // 4 (GfbgAct_Playlist): the playlist for each event, gfbgHash of its name, in GfbgConfig's order (onOpen, onHome,
    // onClose, onReturn); and how such a playlist starts: 0 where it was left, 1 shuffled afresh (GfbgListStart)
    uint64_t lists[4] = {};
    int listStart = 0;
};

struct BgQueue {
    std::string name;
    std::vector<std::string> paths;
    std::vector<int> order;
    int pos = -1, mode = 0;
    bool automix = false;   // an Automix playlist (GfbgQueue_Automix): joined by Automix in the player, never shuffled
};

class BgClient {
public:
    static constexpr unsigned ByGatefold = 1, ByOverlay = 2;   // GfbgClient

    /** True when the sysmodule runs and speaks our protocol. */
    bool connect();
    void disconnect();
    bool connected() const { return connected_; }

    /** Reads the state; false (and disconnected) when the sysmodule went away. */
    bool poll(double now);
    const BgState& state() const { return st_; }
    /** The position now, carried forward from the last poll while playing. */
    double position(double now) const;

    bool setQueue(const BgQueue& q);
    bool queueRefused() const { return refused_; }   // the last setQueue: too big for the player's heap (still connected)
    bool getQueue(BgQueue& q);
    void playAt(int pos, bool paused, double start);
    void toggle(double now);
    void pause(double now);
    void stop();
    void seek(double t, double now);
    void setVolume(float v);
    void setStopAfter(bool on);
    /** Volume levelling's mode for the player (GfbgLevel, in GfbgConfig.reserved[0]); the gains are a file. */
    void setLevelMode(int mode);
    /** Crossfade for the player (GfbgConfig.crossfade, xfadeFlags): seconds, 0 off. */
    void setCrossfade(float seconds, bool notInAlbum);
    /** Automix's model for the player's joins (GfbgConfig.automixModel: automix::Model; file: a downloaded model's, or ""). */
    void setAutomixModel(int model, const std::string& file);
    bool eq(GfbgEq& out);
    void setEq(const GfbgEq& e);
    void setSleep(float seconds);
    /** Newest n mono samples at 48 kHz; false when nothing new since the last call. */
    bool pcm(float* out, int n);
    bool config(BgConfig& out);
    void setConfig(const BgConfig& c);


private:
    bool connected_ = false;
    bool refused_ = false;
    BgState st_;
    double polledAt_ = 0;
    // What Gatefold just asked for wins over a poll that may predate it (commands are async).
    int expectStatus_ = -1;
    double expectUntil_ = 0, expectPos_ = -1;
    unsigned pcmSerial_ = 0;
    std::vector<float> pcm_;
    float sentVolume_ = -1;
};
