/*
 * Gatefold NX · app.hpp
 * Application state and the views. Layout values are Gatefold's CSS (app.css)
 * in CSS pixels, which map 1:1 onto the Switch's 1280×720 screen.
 */
#pragma once
#include <SDL2/SDL.h>
#include <switch.h>

#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "bgclient.hpp"
#include "gfx.hpp"
#include "levels.hpp"
#include "library.hpp"
#include "loader.hpp"
#include "metacache.hpp"
#include "metadata.hpp"
#include "palette.hpp"
#include "player.hpp"
#include "spectrum.hpp"
#include "stats.hpp"
#include "store.hpp"
#include "thumbs.hpp"
#include "ui.hpp"
#include "updater.hpp"
#include "waveform.hpp"

namespace css {
constexpr int W = 1280, H = 720;
constexpr float TbH = 46, BarH = 92, SideW = 236, SideWCollapsed = 68;
// --fs-* scale, one pixel up from the desktop values for a handheld screen
constexpr int Xs = 12, Sm = 13, Base = 15, Md = 16, Lg = 19, Xl = 25, Xxl = 34, Hero = 52;
}  // namespace css

struct TourStep;   // tour.cpp

enum class View { Now, Queue, Artists, Artist, Genres, Genre, Library, Album, Folders, List, Search, Settings, PickFolder, Albums, PickPlaylist, PickExport,
                  Files, Playlists };

/** Gatefold 1.4.2's four list views (.pl-modes): by album, with covers, as tiles, as a plain list. */
enum class ListMode { Albums = 0, Covers = 1, Grid = 2, List = 3 };

struct LyricLine { double t; std::string text; };

/** An artist or a genre: its name, the albums under it and every track, albums or not. */
struct Artist {
    std::string name;
    std::vector<int> albums;
    std::vector<int> tracks;
};

/** Files: one entry of a folder (view_files.cpp). */
struct FileEntry { std::string name; bool dir = false; uint64_t size = 0; int64_t mtime = 0; };

/** A row of the sheets without a track (view_smart.cpp's card): an action, a heading, a choice with a check, … */
struct SheetRow {
    enum Kind { Action, Group, Setting, Count, Preview, Name, Choice, Grid, Text, Toggle } kind;   // Text: wrapped lines (sub), an icon before them; Toggle: label, sub, a switch (on)
    const char* icon = nullptr;
    std::string label, sub, detail;
    std::function<void()> act;
    float h = 58;
    bool on = false;
};

/** The track menu (+ on a track): its page and what it is working on. */
struct Sheet {
    // NewList, Smart and SmartForm (view_smart.cpp) belong to no track: + in the sidebar opens them.
    enum Page { Closed, Menu, Tags, Cover, Delete, Info, Lists, NewList, Smart, SmartForm, Sleep, Job, Jobs, Colour, PlFile, Confirm, EqPreset, ActList, ListActions,
                PlMenu, PlIcon,     // PlMenu / PlIcon: a playlist page's ⋯ and its icons (recipe: the playlist, name: its name)
                FileMenu, FileInfo, FileLink,     // Files: a file's menu, its details, the track an .lrc goes with (dir: the file)
                Release,                          // Settings → Updates: a release's changelog and Install (recipe: its index)
                Factory };                        // Settings → Reset → Factory reset: what to erase (App::factoryPick)
    Page page = Closed;
    int track = -1;
    int queuePos = -1;      // opened on a queue row: its position in play order; Jobs: the row to focus
    bool upNext = false;    // … in Now playing's Up next
    Id returnFocus = 0;
    std::map<std::string, std::string> draft;   // Tags: the fields as they are being edited
    std::string dir;                            // Cover: the folder being browsed; PlFile: the playlist file
    std::vector<std::string> entries;           // its sub-folders ("/" at the end) and images
    bool wholeAlbum = true;
    std::string previewPath;                    // Cover: the image shown in the header
    SDL_Texture* preview = nullptr;
    TrackInfo info;                             // Info
    int recipe = -1;                            // SmartForm: the recipe (view_smart.cpp), its setting, the name; Job: the card's slot
    std::string arg, name;
    bool nameTouched = false;
    bool fromList = false;                      // Job: opened from the Jobs list, where B goes back to
    std::vector<int> fileTracks;                // PlFile: its lines found in the library
    int fileMissing = 0;                        // ... and those that were not
    // Confirm (App::confirm): a question with Cancel (focused) and a red button, as the track menu's Delete
    std::string askKicker, askTitle, askSub, askQuestion, askText, askButton;
    const char* askIcon = nullptr;              // nullptr: the list icon
    std::function<void()> askYes;
    // … a safe question (askSafe: the button in the accent, not red), and a second choice beside it (askAlt)
    bool askSafe = false;
    bool askNoCancel = false;                   // no Cancel: the first launch's music folder card (askAlt is the other way)
    const char* askYesIcon = nullptr;
    std::string askAlt;
    std::function<void()> askAltYes;
    std::string askAlt2;                        // a third choice, left of askAlt (the welcome card)
    std::function<void()> askAlt2Yes;
    std::string askWarn;                        // a warning under the text, apart from it: bold capitals
    // What a page shows of files on the SD card (sizes, whether .lrc lyrics exist, a file's kind): read once, not every
    // frame (the sheet's rows are built each frame, and each read waits on the SD card)
    std::unordered_map<std::string, std::string> memo;
};

class App {
public:
    bool init();
    void run();
    void shutdown();

private:
    // ---------------- systems
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    Gfx g;
    Ui ui;
    Player player;
    BgClient bg;     // the background player (sysmodule), when it runs: it then does the playback
    ThumbWorker thumbs;   // covers for its overlay
    TrackLoader loader;   // a track's tags, lyrics and cover, read and decoded on a worker core
    unsigned loadGen = 0;
    Library lib;
    MetaCache meta;
    Waveform wave;
    Spectrum spec;
    Store store;
    PadState pad_;

    // ---------------- palette
    Palette pal = paletteDefault();
    Palette target = paletteDefault();
    Palette coverPal = paletteDefault();   // the playing cover's own colours, whatever the theme
    std::vector<uint8_t> coverPx;          // its 64×64 sample, so Brightness and Saturation can rebuild coverPal
    PaletteOpts paletteOpts() const;
    Palette customPalette() const;

    // ---------------- now playing
    struct Now {
        int track = -1;
        TrackInfo info;
        SDL_Texture* cover = nullptr;
        SDL_Texture* prevCover = nullptr;
        SDL_Texture* blur = nullptr;
        SDL_Texture* prevBlur = nullptr;
        float fade = 1;
        std::vector<LyricLine> lyrics;
        bool synced = false;
        std::string lyricsSource;
        double lrcOffset = 0;   // the .lrc's own [offset:], in seconds (already taken off the lines' times)
        bool lyricsFromFile = false;   // from an .lrc file beside the track, rather than its tags
        bool loading = false;   // its tags, lyrics and cover are on their way (TrackLoader)
    } now;
    double lyricsHold = 0;      // while loading: where the previous lyrics stay
    /** Where the lyrics are: the music's position, less the track's nudge (lyrics moved later show later). */
    double lyricsPos() const { return (now.loading ? lyricsHold : player.position()) - lyricNudge(); }
    // The lyrics nudge: per track, in 0.1 s steps, kept by Gatefold (Store::lyricNudge); + later, − earlier.
    float lyricNudge() const;
    void nudgeLyrics(float by);   // 0: back in time with the file
    void seekLyric(int line);     // a tapped line: the music goes to where that line now shows
    /** The timing control shows: synced lyrics, the setting on, and from a source it is wanted for (Settings → Lyrics). */
    bool lyricTimingShown() const {
        return store.lyricTiming && now.synced && !now.lyrics.empty() &&
               (store.lyricTimingFor == 2 || (store.lyricTimingFor == 1) == now.lyricsFromFile);
    }
    /** "Earlier · 0.3 s · Later": the nudge's control (the Lyrics tab, the Canvas). Returns its width. */
    float lyricSyncControl(const char* key, float x, float cy, float h, const Font& f, float alpha, bool live);

    // ---------------- play queue (Gatefold's "active playlist")
    struct Queue {
        std::string name;
        std::vector<int> tracks;
        std::vector<int> order;   // positions into tracks, in play order
        int pos = -1;             // index into order
        int failed = 0;
    } q;
    bool stopAfterCurrent = false;

    // ---------------- volume levelling (levels.hpp): measured here, applied here and by the background player
    Levels levels;
    std::vector<std::string> levelFirst, levelAll;   // what was last asked to be measured first, and the library
    unsigned levelEpoch = ~0u, levelGenUsed = ~0u;
    int levelModeSent = -1, levelTargetUsed = 0;
    // Crossfade (Gatefold's own player; the background player does its own): this track's was looked at
    bool xfadeChecked = false;
    // The equalizer: a change to send to the background player (at most every 0.1 s while a slider moves)
    bool eqDirty = false;
    double eqSentAt = 0;
    double levelsSavedAt = 0, gainsWrittenAt = -100;
    bool gainsDirty = false;
    unsigned gainsGen = 0;
    std::unordered_set<std::string> gainsHave;   // the paths in gains.bin as last written
    /** The gain for a track in dB, for a mode (GfbgLevel; −1: the one set) and the target (0: not measured, or off). */
    double levelDb(int track, int mode = -1) const;
    void updateLevels();
    void writeGains();

    // ---------------- the sleep timer: the background player keeps it when it plays, else Gatefold (sleepUntil)
    double sleepUntil = 0;        // App::time it runs out, 0 off (Gatefold's own playback)
    bool sleepFading = false;
    unsigned sleepGenSeen = 0;    // the background player's sleepGen last seen
    int sleepChoice = 0;          // the choice last made (Settings' segmented control), 0 off
    bool sleepReleased = false;   // the timer ran out: the console may dim and sleep again until music plays
    bool awakeHeld = false;       // keepAwake as applied (the idle-time extension and auto-sleep off)
    int mediaState = -1;          // appletSetMediaPlaybackState as last set: playing, or kept awake
    double sleepLeft() const;     // seconds, 0 off
    void setSleep(double seconds, int choice);
    void updateSleep();
    /** seen: it ran out while Gatefold watched, so the card that puts the console to sleep may follow. */
    void sleepEnded(bool seen);
    double sleepSeenAt = 0;       // steady-clock seconds the background player's timer was last seen running
    // The card after the timer: the console goes to sleep (appletRequestToSleep) unless you stay awake.
    double sleepPromptAt = 0;     // App::time it appeared, 0 none
    void drawSleepPrompt();
    void closeSleepPrompt();

    // ---------------- Keep the console awake → Dim the screen: after store.dimAfter minutes without input
    double idleFor = 0;           // seconds without input
    int dimmed = 0;               // 0 no, 1 the backlight lowered (handheld), 2 a veil (docked: the TV is not ours)
    float dimSaved = 1;           // the brightness to put back
    float veilA = 0;              // the docked veil, eased
    void setDimmed(bool on);

    // ---------------- the battery: charging, and how long to full or to empty
    struct Battery {
        std::vector<std::pair<double, double>> samples;   // App::time, raw percentage
        double lastAt = -100;
        long long lastWall = 0;   // std::time of the last sample: a gap means the app was suspended
        int charging = -1;
        std::string est;          // "2 h 10", "full in 45 min", "" while unknown
    } battery;
    void sampleBattery();

    // ---------------- play statistics (stats.hpp): Gatefold counts what it plays itself; the background player its own
    Stats stats;
    double statHeard = 0;       // seconds of the current track heard here
    bool statCounted = false;   // … and already counted
    double statsSavedAt = 0;
    unsigned statsEpoch = ~0u;  // the scan whose files were asked for their dates (libEpoch)
    void saveStats();

    // ---------------- the background player: what Gatefold sent it last, and what it has seen of it
    struct BgSync {
        std::vector<int> tracks, order;
        std::string name;
        int pos = -2, mode = -1;
        bool stopAfter = false, seenStopAfter = false;
        unsigned trackGen = 0, queueGen = 0, errorGen = 0;
        float volume = -1;
        double awaitUntil = 0;   // a track Gatefold asked for is on its way: the state may still show the old one
        BgConfig cfg;
        bool cfgRead = false;
        unsigned eqGen = 0;
        float xfadeSent = -1;
        int xfadeFlagSent = -1;
        int coverCount = 0;        // library tracks with an overlay cover (Settings), counted once a second
        double coverCountAt = -10;
    } bgs;

    // ---------------- navigation
    struct Route { View view; int ref; std::string query; Id focus = 0; };
    Route route{ View::Now, -1, {}, 0 };
    std::vector<Route> history;
    std::unordered_map<uint64_t, ScrollArea> areas;
    int npTab = 0;
    Id listJump = 0;   // "Current track": the row to focus once the list has scrolled to it
    View libFrom = View::Library;   // which tab an album page was opened from, for the sidebar's highlight
    std::string playlistPickerAt;      // + → Import a playlist file…: the folder of the last file picked (this session)
    std::string exportPickerAt;        // Export: the folder last exported to (this session)
    std::string exportName;            // Export: the list being exported, while its folder is chosen (View::PickExport)
    std::vector<int> exportTracks;
    View folderFrom = View::Folders;   // … and a folder page: Library when opened from its Folders tab (App::navigate)
    int librarySort = 0;   // 0 artist, 1 title, 2 year, 3 folder
    bool canvas = false;
    float canvasIdle = 0;    // seconds without input; the Canvas controls fade away
    float canvasCtl = 1;     // their opacity
    float canvasViz = 0;     // 0 = seek hairline, 1 = the idle spectrum it turns into
    // the Canvas layout switch: weights it started from, the layout it is heading to, and when it began
    float cvFrom[3] = { 1, 0, 0 };
    int cvTo = 0;
    double cvStart = -10;

    // ---------------- derived indexes
    std::vector<int> albumOrder;
    std::vector<Artist> artists;
    std::vector<int> searchResults;
    std::unordered_map<std::string, int> pathIndex;
    std::vector<Artist> genres;
    std::unordered_map<std::string, int> albumByKey, artistByKey, genreByKey;   // album and artist pages are routed by key: indexes move as tags arrive
    bool indexDirty = true;
    double indexAt = 0;

    // ---------------- the left stick's pointer
    struct Pointer {
        static constexpr float Hide = 4;   // seconds still before it fades away
        float x = 640, y = 360;
        float idle = 99;                   // seconds since it last moved
        bool down = false;                 // A held: pressing under it
    } pointer;

    // ---------------- misc
    std::string toastText;
    float toastT = 0;
    double time = 0;
    float dt = 0;
    bool running = true;
    float sideW = css::SideW;
    int marqueeTrack = -2;   // the player bar's long title and artist scroll from marqueeAt, again for each track
    double marqueeAt = 0;
    float volumeShown = 1;
    float coverBeat = 0;     // the cover's eased bass pulse
    double persistAt = 0;    // when the debounced save is due (0: nothing pending)
    double sessionAt = 0;    // last time the session (queue, position) was written
    int savedTrack = -1;
    bool savedPlaying = false;
    AppletHookCookie appletCookie_{};
    bool ready_ = false;     // init finished: before that there is nothing worth saving over the files
    std::vector<float> pcm_;
    // Settings → About → Frame times: the last 120 frames, interval and the work before the present
    float frameMs_[120] = {}, workMs_[120] = {};
    int frameAt_ = 0;
    std::string frameStats_, profStats_;
    std::string startStats_;   // how long launch took, by step (Frame times): what the console spends before the first frame
    Uint64 launchAt_ = 0;      // when init began: "frame1" and "cover" are counted from it
    bool firstFrame_ = true, firstCover_ = true;
    void launchAt(const char* what) {
        char b[32];
        snprintf(b, sizeof b, "  %s @%.0f", what, double(SDL_GetPerformanceCounter() - launchAt_) / double(SDL_GetPerformanceFrequency()) * 1000);
        startStats_ += b;
    }
    void startMark(const char* step) {
        const Uint64 t = SDL_GetPerformanceCounter();
        char b[40];
        snprintf(b, sizeof b, "%s%s %.0f", startStats_.empty() ? "launch: " : "  ", step, double(t - profAt_) / double(SDL_GetPerformanceFrequency()) * 1000);
        startStats_ += b;
        profAt_ = t;
    }
    // ... and where the work goes: input+update, background, view, sidebar+title bar, player bar, the rest, the Canvas warm-up, present
    double profAcc_[8] = {};
    int profFrames_ = 0;
    Uint64 profAt_ = 0;
    void profMark(int section) {
        const Uint64 t = SDL_GetPerformanceCounter();
        profAcc_[section] += double(t - profAt_) / double(SDL_GetPerformanceFrequency()) * 1000;
        profAt_ = t;
    }
    double frameStatsAt_ = 0;
    unsigned warmTick_ = 0;
    // Once a launch: many tracks have no cover for the overlay yet. 0 not looked yet, 1 showing, 2 done
    // The app's own covers (the covers cache): asked about first when many are missing, the sweep held until then
    // (coversHeld; this launch only, never saved). 0 not looked at yet, 1 showing, 2 answered or not needed.
    int appCoverNotice = 0;
    bool coversHeld = false;
    void releaseCovers();
    int coverNotice = 0;
    int coverMissing = 0;
    double coverNoticeAt = 0;
    Id noticeFocus_ = 0;   // where focus was before it, to go back to
    void closeCoverNotice();
    float textShift = 0;          // the Canvas's own moving offset, under any scroll (Gfx::setShift)
    float settingsPaneX = 1e9f;   // where Settings' right pane begins: B there goes back to the categories

    // ================= app.cpp: lifecycle, input, playback
    void input();
    void update();
    void rebuildIndex();
    /** Reads the music folder again; files: a walk already made (the launch check's), else it walks now. */
    void rescan(const std::vector<std::string>* files = nullptr);
    // The music folder's file list, remembered (library.tsv): launch builds from it instead of walking the
    // card, then a walk on another core checks it, and a rescan follows only when something changed.
    std::vector<std::string> libFiles_;    // the list the library was built from, sorted
    std::thread libCheck_;
    std::vector<std::string> libChecked_;
    std::atomic<bool> libCheckDone_{ false };

    void playTracks(const std::vector<int>& tracks, int startPos, const std::string& name, bool shuffle = false);
    /** fade: crossfade into it over that many seconds (Gatefold's own player), 0 cuts or, when a crossfade is set, 0.3 s. */
    void playAt(int orderPos, double fade = 0);
    void loadTrack(int trackIdx, bool paused = false, bool start = true, double fade = 0);
    /** The order position a crossfade at the end of this track goes to, or −1 when this join takes none. */
    int crossfadeNext(double left) const;
    /** Settings → Equalizer: kept, applied here, and sent to the background player. */
    void setEq(const GfbgEq& e);
    void bgConnect();
    void bgUpdate();
    bool bgSyncQueue(bool reload = true);
    void forgetNowPlaying();   // nothing plays any more (the overlay emptied the queue): Now playing as at a first launch
    bool bgAdoptQueue();
    void setBackground(bool on);
    /** Asks a name with the keyboard and makes a playlist of your own; track ≥ 0 goes into it. */
    void newPlaylist(int track);
    void addToPlaylist(int playlist, int track);
    void thumbFor(int track, bool now);
    void applyTheme();
    void persistSoon(float delay);
    void persistNow(bool sessionOnly = false);
    void restoreSession();
    static void appletEvent(AppletHookType type, void* self);
    void next(bool natural);
    void prev();
    void togglePlay();
    void seekBy(double d);
    void setOrder(Order o);
    std::vector<int> upNext(int n) const;
    int currentQueuePos() const;
    // Editing the queue, by position in play order (q.order). Without Shuffle the list itself is
    // kept in play order (queueNormalize), so turning Shuffle off and on never loses an edit.
    std::vector<int> queueInPlayOrder() const;
    void queueNormalize();
    void queueEdited();
    void queueAdd(int track, bool next);
    void queueRemove(int pos);
    void queueMove(int from, int to);
    void queuePlayNext(int pos);
    void queueClear();
    // A queue row picked up with the track menu's Move: the D-pad moves it, A or B puts it down.
    // Also the Playlists page's playlists (+ → Move) and the sidebar's tabs (Settings → Appearance → Move): same keys.
    struct Grab { enum Kind { Queue, Playlists, Tabs } kind = Queue; int pos = -1; bool upNext = false; int cols = 1; } grab;
    void grabStep(int d);
    void grabDrop();
    void applyLoaded(TrackLoader::Result& r);
    void applyCover(TrackLoader::Result& r);
    TrackLoader::Result coverWait_;   // a loaded cover kept for the frame after its tags (one thing at a time)
    bool coverWaiting_ = false;
    void parseLyrics(const std::string& text);

    void navigate(View v, int ref = -1, const std::string& query = {}, bool push = true);
    unsigned pickerGen = 0;   // the folder pickers list a folder again when it bumps (opened anew, a file written there)
    void back();
    void openSearch();
    void openCanvas(bool on);
    void toast(const std::string& s);
    ScrollArea& area(const char* key, int ref = 0);

    std::vector<int> listTracks(View v, int ref, const std::string& query) const;
    std::string trackSub(int t) const;
    std::string fmtTime(double s) const;
    std::string fmtTotal(const std::vector<int>& tracks) const;
    bool isPlaying() const;

    // ================= shell.cpp: background, title bar, sidebar, player bar
    void draw();
    void drawBackground();
    void drawTitlebar();
    void drawSidebar();
    void drawPlayerbar();
    void drawToast();
    void drawFocusRing();
    void drawPointer();
    void drawFrameTimes();
    void drawCoverNotice();
    // The sidebar's progress cards, stacked above Settings: one per kind of work running in the background at once.
    // Slot 0: the music folder check, then tags, then the cover cache (one after another); 1: the overlay's covers; 2: levelling.
    enum class Stoppable { None, Covers, Thumbs, Levels };
    struct Job {
        std::string label;
        int done = 0, total = 0;   // total 0: just busy
        int settingsCat = 3;   // SetLibrary
        Stoppable stop = Stoppable::None;
        const char* row = nullptr;   // its row in that Settings category (showSetting)
        bool kb = false;             // done and total are kilobytes: shown as "4.1 of 12.9 MB" (a download)
    };
    static constexpr int JobSlots = 5;   // 3: Files' copying, moving and deleting (fileWork); 4: an update (updater)
    struct JobCard { Job job; float shown = 0, since = 0; };
    JobCard jobCards[JobSlots];
    /** What runs in each slot now; false where nothing does (or it was stopped). */
    void backgroundJobs(Job (&jobs)[JobSlots], bool (&busy)[JobSlots]);
    struct JobStack {
        float weight[JobSlots] = {};   // each job's share of the front card's contents
        float body = 0;                // the front card's opacity (the stack's presence)
        float behind = 0;              // cards peeking out behind it, in fractions
        int front = 0;                 // the job with the largest share: where A on the card goes
        int running = 0;               // jobs mostly shown: more than one opens the list
    };
    JobStack jobStack() const;
    std::string jobCount(const Job& j) const;   // "120 of 3074", "4.1 of 12.9 MB"; "" without a total
    float jobsHeight(bool collapsed) const;
    void drawJobCard(const JobStack& st, float x, float bottom, float w, bool collapsed);
    /** Stops a job until it is resumed (the card's ×, Settings), kept in settings.txt; or resumes it. */
    void setJobStopped(Stoppable which, bool stopped);
    void applyJobStops();
    void openJobMenu(int slot, bool fromList);   // a job that can be stopped: Stop, or Open in Settings (view_smart.cpp)
    void openJobList();                          // A on the stack with more than one job: all of them
    // Playlist files (m3u.hpp): .m3u / .m3u8 in the music folder, played or imported; any playlist exported as one.
    std::vector<int> resolvePlaylistFile(const std::string& file, int& missing);
    void openPlaylistFile(const std::string& file);   // its card: Play, Shuffle, Import as a playlist
    void openPlaylistPicker();                         // + → Import a playlist file…: the SD card's folders (drawPicker)
    void leavePlaylistPicker();                        // back to the page it was opened from (either picker)
    void openExportPicker(const std::string& name, const std::vector<int>& tracks);   // Export: the folder to write it in
    void exportPlaylist(const std::string& dir);       // … "Export here": <dir>/<name>.m3u8, asking before replacing one
    /** A question in the sheet's card, as the track menu's Delete: Cancel is focused, `button` (red) runs yes. */
    void confirm(const char* icon, const std::string& kicker, const std::string& title, const std::string& sub, const std::string& question,
                 const std::string& text, const std::string& button, std::function<void()> yes);
    /** Settings at a job's row: the category, scrolled to it, focused and lit for a moment. */
    void showSetting(const Job& j);
    std::string settingsJump;   // the row label showSetting is after, and when it asked (drawSettings)
    double settingsJumpAt = -10;
    void drawSeek(float x, float y, float w, float h, Id id);

    // ================= widgets.cpp: Gatefold's components
    bool iconButton(Id id, const char* icon, float cx, float cy, bool on = false, float size = 34, float iconSize = 18, uint8_t flags = 0);
    float button(Id id, const char* icon, const std::string& label, float x, float y, bool primary = false, bool* clicked = nullptr,
                 bool toggled = false, bool danger = false, uint8_t flags = 0, float minW = 0);   // minW: at least this wide, its content centred
    float chip(const std::string& label, float x, float cy, bool accent);
    void kicker(const std::string& s, float x, float cy, const Rgb& c, float spacing = 0.12f);
    void eqGlyph(float x, float cy, const Rgb& c);
    void lyricDots(float x, float cy, float d, const Rgb& c, float alpha, bool current);
    void coverBox(int track, float x, float y, float size, float rad, float alpha = 1);
    bool trackRow(Id id, int track, int number, float x, float y, float w, float h, bool showAlbum);
    void listHead(float x, float y, float w, bool showAlbum);
    float viewHead(const std::string& kick, const std::string& title, const std::string& sub, float x, float y, float w);
    /** Red for what deletes: Gatefold's .btn.danger, oklch(0.72 0.17 25), and on a light palette its darker
     *  oklch(0.48 0.17 25) (app.css's light error colour): the light one was too pale to read on white. */
    Rgb dangerInk() const { return pal.dark ? Rgb{ 0.93f, 0.45f, 0.42f } : Rgb{ 0.66f, 0.13f, 0.15f }; }
    void slider(Id id, float x, float cy, float w, float value, float* out);
    /** Upright, value 0..1 from the bottom, filled from the middle (the equalizer's bands); ←/→ while picked up go to left/right. */
    void vslider(Id id, float cx, float top, float h, float value, float* out, Id left, Id right);
    void openEqPresets();
    /** Your saved equalizer presets: the sound now under a name (an existing name is replaced), renamed, deleted. */
    void saveEqPreset(int into = -1);   // into: your preset k updated with the sound now, else a new one (named)
    void renameEqPreset(int k);
    void deleteEqPreset(int k);
    std::string eqPresetName(int preset) const;   // the preset button's text: a preset's name, or Custom
    // ---- game actions: a playlist (GfbgAct_Playlist); event 0 opens, 1 HOME, 2 closes, 3 back to the game (GfbgConfig.lists)
    static uint64_t listHash(const std::string& name);   // gfbgHash of a playlist's name
    std::string listName(uint64_t hash) const;           // "" when no playlist has it any more
    void openActionList(int event);                       // Choose: the playlist for that event
    void openListActions(const std::string& name);        // a playlist page's Games: the events that play it
    /** The playlists for the background player (GFBG_PLAYLISTS), rewritten when they changed (every few seconds). */
    void writeBgPlaylists();
    /** The background player is connected and its settings read (the preview pretends so with GF_BGCFG=1). */
    bool bgShown() const;
    double bgListsAt = -100;
    uint64_t bgListsSig = 0;
    void openEqualizer();   // a long press on the equalizer buttons (player bar, Canvas): Settings → Equalizer
    void toggleEq();        // ... a short press: on or off
    bool toggle(Id id, float x, float cy, bool on);

    // ================= view_now.cpp
    void drawNow(float x, float y, float w, float h);
    void drawSpectrum(float x, float y, float w, float h);
    void drawUpNext(float x, float y, float w, float h);
    void drawLyrics(float x, float y, float w, float h);
    void drawPaletteTab(float x, float y, float w, float h);
    void drawCanvas();
    void warmCanvas();
    void drawCanvasLyrics(float x, float y, float w, float h, int size, int layout, float alpha);
    int canvasLayoutShown() const;
    unsigned canvasLayoutsOff() const;   // bit i: Canvas layout i cannot show right now

    // ================= view_lists.cpp
    void drawTrackList(float x, float y, float w, float h);
    void drawAlbum(float x, float y, float w, float h);
    void drawFolders(float x, float y, float w, float h);
    float drawSubfolders(const char* idp, const FolderNode& node, float x, float cy, float w, float y, float h, ListMode mode);
    void addSubfoldersAsPlaylists(int folder);   // asks first
    void drawPicker(float x, float y, float w, float h);
    void confirmRemoveFolderPlaylist(int ref, int count);   // Delete on a folder playlist: asks first (App::confirm)
    void useMusicFolder(const std::string& path);
    void welcomeCard(const std::string& found);
    void plusMenu(bool held);                     // + on the focused row; held: a long touch (quiet when there is no menu)
    // The guided tour (tour.cpp): offered on the first launch's welcome card, and in Settings › About
    void startTour();
    void offerTour();     // a first launch that chose its folder in the picker: asks
    void tourGo(int step);
    void tourBack();      // B: the chapter list closes, else the step before
    void endTour();       // back to the page it started on
    void drawTour();
    int tourCount() const;
    static const std::vector<TourStep>& tourSteps();
    int tourStep = -1;                  // the step shown; −1: no tour
    bool tourChapters = false, tourFocus = false, tourLit = false;
    bool tourPending = false;           // the welcome card asked for it: it starts once the music is read and the covers asked
    bool tourAsk = false;               // a first launch that chose another folder: asked once the folder is in use
    SDL_FRect tourRect{};               // the part lit, gliding to the step's
    SDL_FRect tourWant{}, tourCard{};   // where the light is heading (kept a moment while a page draws its part); the card, gliding
    double tourAt = 0;                  // when the step (or the chapter list) came: its words fade in
    float tourDim = 0, tourMiss = 0;    // the dimming, coming up; how long the part has not been found
    bool tourCardSet = false;
    Route tourFrom{ View::Now, -1, {}, 0 };
    int plusAfterHold = 0;                        // frames until a long touch opens it   // the first launch, over the folder picker: use the card's Music folder?
    int routeAlbum() const;
    int routeArtist() const;
    void openAlbum(int a);
    void goToAlbum(int a);                       // from outside the grids: lights Albums, or Library when Albums is hidden
    bool tabFor(int tab, const char* page);      // false: its tab and Library are both hidden (a toast says so)
    void openArtist(const std::string& name);
    int routeGenre() const;
    void openGenre(const std::string& name);

    // ================= view_grid.cpp
    void drawLibrary(float x, float y, float w, float h);
    void drawArtists(float x, float y, float w, float h, bool genre = false);
    void drawArtist(float x, float y, float w, float h, bool genre = false);
    // A genre's page shows one of Albums, Artists or Tracks at a time (kept while the app runs); its artists are
    // worked out once per genre and index: (artist, tracks of theirs in the genre), by name
    int genreTab = 0;    // 0 Albums, 1 Artists, 2 Tracks
    int artistTab = 0;   // an artist's page: 0 Albums, 2 Tracks (the same numbers, no Artists)
    std::vector<std::pair<int, int>> genreArtists_;
    int genreArtistsFor_ = -1;
    unsigned genreArtistsGen_ = ~0u;
    const std::vector<std::pair<int, int>>& genreArtists(int genre);
    /** quad: the four covers of these tracks' first albums (a genre), as Folders shows a folder. */
    bool tile(Id id, int coverTrack, float x, float y, float w, bool round, const std::string& title, const std::string& sub,
              const std::vector<int>* quad = nullptr, uint8_t flags = 0);

    // ================= view_tracks.cpp: the four list views, shared by every list of tracks
    struct ListEntry {
        enum Kind { ColumnHead, GroupHead, Row, TileRow } kind;
        int index;    // the track's place in the list (the first of a tile row)
        float y, h;   // from the top of the list
    };
    std::vector<ListEntry> listLayout(const std::vector<int>& tracks, float w, ListMode mode) const;
    float listRowY(const std::vector<ListEntry>& layout, int index) const;
    float drawTrackBody(const char* key, const std::vector<int>& tracks, float x, float top, float w, float clipY, float clipH, ListMode mode,
                        bool extended, const std::function<void(int)>& play);
    int bodyFocus = -1;   // drawTrackBody: the index of the row or tile under focus, −1 when none
    int bodyGrab = -1;    // drawTrackBody: the index drawn picked up (the queue's Move), −1 when none
    int bodyCols = 1;     // drawTrackBody: tiles in a row last drawn (1 in the row views)
    float viewSwitch(const char* key, float rx, float cy, int& mode, unsigned hidden = 0);   // hidden: bit i, view i left out
    void coverQuad(const std::vector<int>& tracks, float x, float y, float size, float rad, float alpha = 1);
    void listColumns(float x, float y, float w, ListMode mode, bool extended);
    bool coverRow(Id id, int track, int number, float x, float y, float w, float h, bool extended);
    bool trackTile(Id id, int track, float x, float y, float w, float cover);
    void groupHead(const char* key, int index, int track, float x, float y, float w);
    // Library → Tracks: every song, sorted and filtered (cached: sorting thousands each frame is not free)
    struct TracksView {
        int sort = -1;
        bool desc = false;
        std::string filter;
        unsigned gen = ~0u;
        std::vector<int> list;
        std::vector<std::pair<char, int>> letters;   // the letter index: first character, first row with it
        unsigned epoch = 0;                          // the scan its indexes belong to (libEpoch)
        std::shared_ptr<struct TracksJob> job;       // a sort running on a worker core
        std::thread thread;                          // its thread
    } tracksView;
    unsigned libEpoch = 0;   // bumps with every rescan: track indexes from before mean other tracks
    unsigned indexGen = 0;   // bumps with every rebuildIndex
    const std::vector<int>& sortedTracks();
    void drawTracksTab(float x, float y, float w, float h, float top);
    void drawFoldersTab(float x, float y, float w, float h, float top);
    void drawAlbumGrid(float x, float y, float w, float h, float top);
    void drawAlbums(float x, float y, float w, float h);
    bool folderTile(Id id, int folder, float x, float y, float w, float cover);
    std::string tracksFilter;   // Library → Tracks: what the filter keeps

    // ================= view_playlists.cpp: every playlist on a page, and their order
    void drawPlaylists(float x, float y, float w, float h);
    std::vector<int> playlistTracks(int ref) const;   // −1 Liked
    void movePlaylist(int from, int to);              // the sidebar follows; open pages of playlists move with them
    void erasePlaylist(int ref);                      // Delete: pages of it leave the history, later ones keep theirs
    int plHover = -2, plHoverShown = -2;              // the page's playlist under focus (−1 Liked, −2 none): + opens its menu

    // ================= view_files.cpp: Files, a browser of the whole SD card
    struct FilesState {
        std::string listed;            // the folder listed, with or without hidden files, and when (gen bumps to list again)
        bool listedHidden = false;
        unsigned gen = 0, listedGen = ~0u;
        std::vector<FileEntry> entries;
        std::vector<std::string> clip; // Copy / Move: what Paste here puts in the folder shown (one, or a selection)
        bool clipMove = false;
        std::string hover, hoverShown; // the row under focus: + opens its menu
        std::string focusAfter;        // a row to focus once listed (the folder come up from, a file just made)
        // Select: A ticks rows (Select all / none), then Copy, Move or Delete takes them all; only in one folder at a time
        bool selecting = false;
        std::vector<std::string> selected;
        std::string selectIn;
    } files;
    bool fileSelected(const std::string& p) const;
    void fileSelect(const std::string& p, bool on);
    void fileDeleteMany(const std::vector<std::string>& paths);
    // One piece of work at a time on a worker (copying, deleting a folder): the progress stack shows it (slot 3).
    struct FileWork {
        std::thread thread;
        std::atomic<bool> done{ false };
        std::atomic<int> count{ 0 }, total{ 0 };
        std::string label, dest, result, failed;   // the card's label, the folder it ends in, the toast, what went wrong
        bool library = false;                      // it changed the music folder: a rescan follows
        std::function<void()> after;               // on the main thread once it is done (Storage's Clear)
        int cat = -1;                              // the Settings category its card opens (−1: Files, where it ends)
    } fileWork;
    void drawFiles(float x, float y, float w, float h);
    std::string filesStart() const;                // where Files opens (Settings → Storage and reset)
    void openFiles(const std::string& dir);
    void openFileMenu(const std::string& path);
    /** The rows and header of the Files pages of the sheet (FileMenu, FileInfo, FileLink). */
    void fileSheetRows(std::vector<SheetRow>& rows);
    void fileSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    void filePaste();
    void fileRename(const std::string& path);
    void fileDelete(const std::string& path);
    void fileNewFolder(const std::string& dir);
    bool startFileWork(const std::string& label, const std::string& dest, bool library, std::function<void(FileWork&)> work,
                       std::function<void()> after = nullptr);   // false: another is running (a toast says so)
    void updateFileWork();
    void linkLyrics(const std::string& file, int track);
    /** A rename or move in the music folder: Liked, playlists, your edits, covers and lyric timings follow the paths. */
    void remapPaths(const std::string& from, const std::string& to);
    bool inMusicFolder(const std::string& path) const;
    void reloadNowTrack();                          // tags, lyrics and cover read again (an .lrc just linked)
    void setCoverFor(const std::string& image, const std::vector<int>& tracks);

    // ================= view_settings.cpp
    void drawSettings(float x, float y, float w, float h);

    // ================= view_updates.cpp: Settings → Updates (updater.hpp): releases, changelogs, install, roll back, backups
public:
    std::string selfPath = "sdmc:/switch/Gatefold/Gatefold.nro";   // the running .nro (main's argv[0])
private:
    Updater updater;
    bool relaunch = false;              // Gatefold closes and starts again (the new .nro, swapped in at shutdown)
    bool updateQuiet = false;           // the check at launch: only a toast when a newer version is out
    std::vector<UpdBackup> backupsShown;
    double backupsAt = -100;
    std::string backupRoot() const;     // store.backupDir, else sdmc:/config/gatefold-nx-backups
    // ---- Settings → Storage and reset: where each browser starts (the music-folder picker chooses it), what Gatefold keeps
    int folderFor = -1;                 // 0 Files, 1 import, 2 export, 3 covers, 4 backups; −1 the picker is the music folder's
    std::string& folderPickField();
    const char* folderPickName() const;
    void chooseFolder(int which);
    struct StorageUse { int64_t bytes = 0; int files = 0; };
    struct StorageScan {
        std::thread thread;
        std::atomic<bool> done{ false };
        StorageUse use[6];              // the covers cache, overlay covers, loudness, tag cache, backups, downloads
        StorageUse found[6];            // the worker's, taken over when it is done
        bool seen = false;
        double at = -100;
    } storage;
    std::string storageValue(int which);   // "4.2 MB", "Empty", "…" while first measured
    void scanStorage();                 // on a worker; Settings shows the sizes when it is done
    void clearStorage(int which);       // asks first
    const std::vector<UpdBackup>& updateBackups();   // read again every few seconds while shown
    void allowUpdates();                // the card that asks before anything goes online
    void checkUpdates(bool quiet);
    void openRelease(int i);            // its changelog and Install / Go back (Sheet::Release)
    void installRelease(int i);         // asks first, saying all that will happen
    void restoreBackup(const UpdBackup& b);
    void resetSettings();    // Settings → Reset: asks first; every setting as it came, your music and lists stay
    void defaultSettings();  // ... without asking (Reset settings, and a factory reset's Settings part)
    // Settings → Reset → Factory reset: a sheet of what Gatefold keeps, each with a switch (factoryPick), then Erase what
    // is ticked, or Erase everything (asks twice). It is done when Gatefold closes (factoryMask_), which then starts again.
    enum FactoryPart { FacSettings, FacMusic, FacPlaylists, FacLiked, FacEdits, FacLyrics, FacEqPresets, FacStats, FacQueue, FacCaches, FacParts };
    void openFactory();
    void factoryRows(std::vector<SheetRow>& rows);
    void factoryErase(unsigned mask);   // asks (twice for everything), then closes Gatefold
    unsigned factoryPick = 0, factoryMask_ = 0;
    std::thread eraseOld_;   // the next launch: a factory reset's old folder deleted in the background
    void finishUpdate();                // Restart now: the background player, then Gatefold
    void updateTick();                  // every frame: what the worker finished
    void updateSheetRows(std::vector<SheetRow>& rows);
    void updateSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    const char* changelogIcon(const std::string& name) const;
    std::string updateStatus() const;   // the worker's progress in words ("Downloading 1.2.0 · 4.1 of 12.9 MB")

    // ================= view_sheet.cpp: the track menu
    Sheet sheet;
    int hoverTrack = -1, hoverTrackShown = -1;   // the track row under focus this frame / last frame
    int hoverQueue = -1, hoverQueueShown = -1;   // its position in the queue, on the Playlist view and Up next
    bool hoverUpNext = false, hoverUpNextShown = false;
    /** queuePos ≥ 0: opened on a row of the queue, which adds Remove, Move and Play next for that row. */
    void openTrackMenu(int track, int queuePos = -1, bool upNext = false);
    void closeSheet();
    void sheetBack();

    // ================= view_smart.cpp: smart playlists
    void openNewList();
    void openSleep();
    // Settings → Appearance → Custom: the colour picker (view_smart.cpp). The field changes live, so the
    // interface shows the colour as it is picked; Cancel (and B) put back what it was.
    struct ColourPick {
        int* field = nullptr;   // store.customBg / customAccent / customAccent2 (0xRRGGBB, −1 the cover's)
        int before = 0;
        float h = 0, s = 0, v = 1;   // hue 0..360, saturation and value 0..1
        const char* what = "";
    } pick;
    SDL_Texture* svTex = nullptr;    // the square for pick.h (made again when the hue moves)
    SDL_Texture* hueTex = nullptr;   // the hue bar, made once
    float svTexHue = -1;
    SDL_Texture* hueTexture();       // made on first use (the picker's bar)
    SDL_Texture* wheelTex = nullptr;
    SDL_Texture* wheelTexture();     // a colour wheel with soft edges: the picker's swatch in Settings
    void openColourPicker(int* field, const char* what, const Rgb& start);
    void drawColourSheet();
    void setPick(float h, float s, float v);
    void closeColourPicker(bool keep);   // Use, or Cancel: the field as it was
    void drawSmartSheet();
    /** The tracks of a "smart:…" spec, worked out again when the library or the statistics change. */
    const std::vector<int>& smartTracks(const std::string& spec) const;
    const char* smartIcon(const std::string& spec) const;
    /** A sidebar playlist's icon: the one chosen (plicons.inc), else its kind's (folder, the smart recipe's, a list); −1 Liked. */
    const char* playlistIcon(int ref) const;
    void openPlaylistMenu(int ref);   // + on the Playlists page: Move, Icon, Rename
    void openPlaylistIcons(int ref);  // the playlist page's icon button: the icon grid at once
    void renamePlaylist(int ref);
    std::string smartSummary(const std::string& spec) const;
    struct SmartCache { unsigned statsGen = ~0u, indexGen = ~0u; int64_t day = -1; std::vector<int> tracks; };
    mutable std::unordered_map<std::string, SmartCache> smartCache_;
    void drawSheet();
    void applyEdits(Track& t);
    void editTag(const char* field, const char* label);
    void saveTagEdits();
    void undoTagEdits(int track);
    void setTrackCover(const std::string& image);
    void deleteTrack(int track);
    void listCoverDir(const std::string& dir);
    bool keyboard(const char* header, const std::string& initial, std::string& out);
    void loadTrackCoverAgain();
    void openFindSetting();
    float segmented(Id id, float rx, float cy, const std::vector<std::string>& opts, int cur, int* picked, unsigned disabled = 0);
    int settingsCat = 0;
    bool settingsFocusFirst = false;   // a category was chosen: its first setting takes the focus next frame
    int settingsCatSeen_ = -1;   // the category last drawn: opening one may read the card once (drawSettings)
    /** Settings' categories that other pages open by number (the progress cards, the equalizer button). */
    enum SettingsCat { SetAppearance = 0, SetPlayback = 1, SetEqualizer = 2, SetLibrary = 3, SetStorage = 4, SetUpdates = 9, SetReset = 10 };
    std::string settingsQuery;
};
