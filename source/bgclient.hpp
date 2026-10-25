/*
 * Gatefold NX · bgclient.hpp
 * Gatefold's side of the background player (sysmodule/, protocol in sysmodule/include/gfbg.h).
 * While the sysmodule runs it does all the playback, so the song carries on when Gatefold
 * closes; Gatefold sends it the queue and transport and draws what it reports.
 *
 * The header keeps libnx's IPC types out of the views: BgState mirrors GfbgState.
 */
#pragma once
#include <string>
#include <vector>

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
};

struct BgConfig {
    // What the music does when a game opens, waits on the HOME menu, comes back from it, or closes:
    // GfbgAct (0 nothing, 1 play a random folder, 2 resume, 3 pause).
    int onOpen = 3, onHome = 2, onClose = 2, onReturn = 2;
    float fadeOut = 1.5f, fadeIn = 2.f;
};

struct BgQueue {
    std::string name;
    std::vector<std::string> paths;
    std::vector<int> order;
    int pos = -1, mode = 0;
};

class BgClient {
public:
    static constexpr unsigned ByGatefold = 1;

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
    bool getQueue(BgQueue& q);
    void playAt(int pos, bool paused, double start);
    void toggle(double now);
    void pause(double now);
    void stop();
    void seek(double t, double now);
    void setVolume(float v);
    void setStopAfter(bool on);
    /** Newest n mono samples at 48 kHz; false when nothing new since the last call. */
    bool pcm(float* out, int n);
    bool config(BgConfig& out);
    void setConfig(const BgConfig& c);


private:
    bool connected_ = false;
    BgState st_;
    double polledAt_ = 0;
    // What Gatefold just asked for wins over a poll that may predate it (commands are async).
    int expectStatus_ = -1;
    double expectUntil_ = 0, expectPos_ = -1;
    unsigned pcmSerial_ = 0;
    std::vector<float> pcm_;
    float sentVolume_ = -1;
};
