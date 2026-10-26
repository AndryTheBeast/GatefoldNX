/*
 * Gatefold NX · levels.hpp
 * Volume levelling: every track's loudness, measured on the console (loudness.hpp) because the
 * music carries no ReplayGain tags, and kept by path in loudness.tsv. A worker measures what is
 * asked for first (the track playing and the next ones), then the rest of the library; the main
 * thread merges its results in poll(), as with the play statistics (stats.hpp).
 *
 * Per track only what the gains need is kept: the loudness, the true peak, and how many blocks
 * passed the gate. An album's loudness is the power mean of its tracks weighted by those blocks:
 * close to gating the album's blocks at once (Gatefold 1.4.2's histograms), without 10 MB of them.
 */
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Level {
    float lufs = NAN;   // NaN: could not be measured (silence, or a file FFmpeg cannot read)
    float peak = 0;     // true peak, linear
    float blocks = 0;   // 400 ms blocks above the gates: the track's weight in its album
};

class Levels {
public:
    void start(const std::string& dir);
    void stop();

    const Level* find(const std::string& path) const;
    /** first: measured before anything else (the track playing, the next ones); all: the rest, in order. */
    void want(std::vector<std::string> first, std::vector<std::string> all);

    /** Main thread, every frame: merges the worker's results. True when anything changed. */
    bool poll();
    unsigned gen() const { return gen_; }
    bool loaded() const { return loaded_; }
    bool dirty() const { return dirty_; }
    std::string serialise();   // loudness.tsv; clears dirty
    size_t count() const { return map_.size(); }
    /** Measuring the library right now (the progress card); and, this run, the seconds a track takes with two at once. */
    bool busy() const { return busy_.load() > 0; }
    double secondsPerTrack() const { return measuredN_ ? measuredS_ / measuredN_ / 2 : 0; }

private:
    void run(int worker);
    void loadFile();

    std::unordered_map<std::string, Level> map_;
    unsigned gen_ = 0;
    bool dirty_ = false, loaded_ = false;
    double measuredS_ = 0;
    int measuredN_ = 0;
    std::string dir_;

    // Two workers, on cores 2 and 1 at the lowest priority there (cores.hpp): two tracks at once, so
    // a whole library takes about half as long; the audio, the loader and the interface always come first.
    std::thread threads_[2];   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    std::atomic<bool> quit_{ false };
    std::atomic<int> busy_{ 0 };   // workers on the library (not on the tracks asked for first)
    std::vector<std::string> first_, all_;
    size_t allAt_ = 0;
    std::unordered_set<std::string> known_;   // the worker's: measured or tried
    bool fileRead_ = false;
    std::unordered_map<std::string, Level> fromFile_;
    std::vector<std::pair<std::string, Level>> found_;
    std::vector<double> took_;
};
