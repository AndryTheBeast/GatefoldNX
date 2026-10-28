/*
 * Gatefold NX · metacache.hpp
 * Background reader: tags and durations for every track, and a square thumbnail
 * plus palette for each cover. Covers are per track (embedded art first, then
 * the folder's image, shared by every track in that folder). Requests made while
 * drawing jump the queue, so what is on screen fills in first; a slow full pass
 * covers the rest. Tags persist in sdmc:/config/gatefold-nx/tags.tsv.
 */
#pragma once
#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "library.hpp"
#include "palette.hpp"

struct AlbumArt {
    enum State : uint8_t { None, Pending, Ready, Missing };
    State state = None;
    SDL_Texture* tex = nullptr;
    Palette pal;
    unsigned lastUsed = 0;
};

class MetaCache {
public:
    static constexpr int ThumbSize = 192;
    static constexpr int MaxTextures = 260;   // ~38 MB of thumbnails; the least recently drawn go first

    void start(Library* lib, const std::string& cacheFile);
    void stop();   // joins the worker and saves the tag cache

    /** The cover of a track; queues it when not loaded yet. */
    AlbumArt& art(int trackIdx);
    /** Queues tags for a track that is about to be shown. */
    void wantTags(int trackIdx);

    /** Main thread, once per frame: uploads finished thumbnails, applies tags.
     *  Returns true when tags changed (album/artist indexes need a refresh). */
    bool pump(SDL_Renderer* r);

    /** Called on a track whenever tags land on it (from the file or the cache): your edits go on top. */
    std::function<void(Track&)> overlay;
    /** An image file that replaces a track's cover ("" puts the file's own back). Main thread. */
    void setCoverFile(int trackIdx, const std::string& file);
    /** Reads a track's tags from its file again (after your edits are undone). Main thread. */
    void retag(int trackIdx);

    /** Tracks whose tags have been looked at (read or not), for progress. */
    int taggedCount() const { return tagged_; }
    /** Tracks whose cover is in the disk cache (or known to have none), for Settings → Library. */
    int coversCached() const { return artCount_; }
    /** The cover sweep's place in the library: running while 0 < sweepPos() < tracks. */
    int sweepPos() const { return artSweepPos_; }
    /** Stops (or resumes) the cover sweep; covers shown on screen are still made. Settings and the progress card. */
    void pauseCovers(bool p) { artPaused_ = p; cv_.notify_all(); }
    bool coversPaused() const { return artPaused_; }
    void clearDiskCache();
    /** Settings → Storage and reset: the covers' disk cache (art/) only, the worker stopped first (stop()). */
    void clearCovers();
    const std::string& artDir() const { return artDir_; }

private:
    struct Job { bool art; int idx; bool cacheOnly = false; };   // cacheOnly: the cover sweep, nothing to show
    struct TagResult {
        int idx;
        std::string title, artist, album, date, albumArtist, genre;
        int trackNo;
        double duration;
        bool ok;   // the file could be read; failures are not cached, so a fixed reader tries again next time
    };
    struct ArtResult {
        int idx;
        std::string key;   // "t:<path>" for embedded art, "f:<folder>" for a folder image
        SDL_Surface* thumb;
        Palette pal;
        bool missing;      // no cover at all
        bool shared;       // folder image already decoded for another track
    };

    void run();
    void load();
    void save();
    // The covers' disk cache (art/): one 192 px JPEG per distinct cover, and which track uses which. Worker only.
    bool cachedArt(int idx, const std::string& mine, ArtResult& res);
    void storeArt(int idx, const std::vector<uint8_t>& source, const ArtResult& res);
    void loadArtMap();
    void saveArtMap();
    void cacheArtOnly(int idx, const std::string& mine);
    void queue(Job j);
    int slotFor(const std::string& key);

    Library* lib_ = nullptr;
    std::string cacheFile_;
    std::vector<int> trackSlot_;   // −1 not asked, −2 pending, else index into slots_
    std::vector<AlbumArt> slots_;
    std::unordered_map<std::string, int> slotByKey_;
    std::vector<std::string> slotKey_;
    std::vector<uint8_t> tagQueued_;
    unsigned frame_ = 0;

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> urgent_;
    int sweep_ = 0;   // next track index for the background pass
    std::vector<TagResult> tagDone_;
    std::vector<ArtResult> artDone_;
    std::unordered_set<std::string> folderDecoded_;   // folder images with a live slot (guarded by m_)
    std::atomic<bool> quit_{ false };
    std::atomic<bool> artPaused_{ false };
    int tagged_ = 0;
    bool dirty_ = false;
    std::vector<std::string> paths_, folders_;   // worker-side copies
    std::vector<uint8_t> done_;                  // tags known (guarded by m_)
    std::vector<std::string> coverFile_;         // per track, your cover (guarded by m_)
    struct ArtEntry { std::string file, key; };  // file "-": the track has no cover
    std::string artDir_;
    std::unordered_map<unsigned long long, ArtEntry> artMap_;   // hash of the track's path → its cover
    std::unordered_map<std::string, std::string> artByKey_;     // a cover's key → its file (folder images are shared)
    int artNew_ = 0;
    // After the tags, every track's cover goes into the cache in the background, so lists open with their
    // covers ready, at first launch and for whatever a rescan finds new. It waits a little after launch.
    int artSweep_ = 0;
    std::chrono::steady_clock::time_point artBegin_;
    std::atomic<int> artCount_{ 0 };
    std::atomic<int> artSweepPos_{ 0 };
};
