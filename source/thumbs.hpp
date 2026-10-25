/*
 * Gatefold NX · thumbs.hpp
 * Covers for the background player's overlay. An overlay has a few MB of heap and cannot decode
 * the large progressive JPEGs music usually carries, so Gatefold makes a small copy of the cover
 * of every track the player may play, at the overlay's own cover size in full RGB, one file per
 * distinct cover (an album's tracks share it), and a map from track to file. Format: GFBG_ART
 * and GFBG_THUMBS in gfbg.h.
 *
 * Runs on a thread of its own: after a queue reaches the player, once per track shown (so a cover
 * you set in the track menu reaches the overlay too), and for the whole library when asked
 * (Settings → Playback → Background player).
 */
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

class ThumbWorker {
public:
    struct Job {
        std::string path;     // the track
        std::string folder;   // for cover.jpg and friends
        std::string mine;     // your cover for it (Store::covers), if any
        bool redo = false;    // even when the map has it already (the shown track: its cover may have changed)
        bool fresh = false;   // set by want(): this track had no cover yet (what progress counts)
    };

    void start(const std::string& dir);
    void stop();
    /** Queues j; first puts it ahead of the rest (the track playing). */
    void want(Job j, bool first = false);
    /** Covers still to make (for the progress in Settings), and how many were made since start. */
    int waiting();
    int done();
    /** Covers that were missing: made since start, and still to make. Redoing the shown track's does not count,
     *  so nothing reads as work when every cover exists. */
    void progress(int& made, int& left);
    bool has(const std::string& track);
    bool running() const { return started_; }

private:
    void run();
    void loadMap();
    void saveMap();

    std::string dir_;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool quit_ = false, started_ = false;
    std::unordered_map<unsigned long long, std::string> map_;   // gfbgHash(track path) → file name
    bool mapDirty_ = false;
    bool busy_ = false;
    int done_ = 0;
    int fresh_ = 0, freshDone_ = 0;                // missing covers queued, and made (guarded by m_)
    std::unordered_set<unsigned long long> queued_;   // their tracks, so one queued twice counts once
};
