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
#include "library.hpp"
#include "loader.hpp"
#include "metacache.hpp"
#include "metadata.hpp"
#include "palette.hpp"
#include "player.hpp"
#include "spectrum.hpp"
#include "store.hpp"
#include "thumbs.hpp"
#include "ui.hpp"
#include "waveform.hpp"

namespace css {
constexpr int W = 1280, H = 720;
constexpr float TbH = 46, BarH = 92, SideW = 236, SideWCollapsed = 68;
// --fs-* scale, one pixel up from the desktop values for a handheld screen
constexpr int Xs = 12, Sm = 13, Base = 15, Md = 16, Lg = 19, Xl = 25, Xxl = 34, Hero = 52;
}  // namespace css

enum class View { Now, Queue, Artists, Artist, Genres, Genre, Library, Album, Folders, List, Search, Settings, PickFolder, Albums };

/** Gatefold 1.4.2's four list views (.pl-modes): by album, with covers, as tiles, as a plain list. */
enum class ListMode { Albums = 0, Covers = 1, Grid = 2, List = 3 };

struct LyricLine { double t; std::string text; };

/** An artist or a genre: its name, the albums under it and every track, albums or not. */
struct Artist {
    std::string name;
    std::vector<int> albums;
    std::vector<int> tracks;
};

/** The track menu (+ on a track): its page and what it is working on. */
struct Sheet {
    enum Page { Closed, Menu, Tags, Cover, Delete, Info, Lists };
    Page page = Closed;
    int track = -1;
    Id returnFocus = 0;
    std::map<std::string, std::string> draft;   // Tags: the fields as they are being edited
    std::string dir;                            // Cover: the folder being browsed
    std::vector<std::string> entries;           // its sub-folders ("/" at the end) and images
    bool wholeAlbum = true;
    std::string previewPath;                    // Cover: the image shown in the header
    SDL_Texture* preview = nullptr;
    TrackInfo info;                             // Info
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
        bool loading = false;   // its tags, lyrics and cover are on their way (TrackLoader)
    } now;
    double lyricsHold = 0;      // while loading: where the previous lyrics stay
    double lyricsPos() const { return now.loading ? lyricsHold : player.position(); }

    // ---------------- play queue (Gatefold's "active playlist")
    struct Queue {
        std::string name;
        std::vector<int> tracks;
        std::vector<int> order;   // positions into tracks, in play order
        int pos = -1;             // index into order
        int failed = 0;
    } q;
    bool stopAfterCurrent = false;

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
    void playAt(int orderPos);
    void loadTrack(int trackIdx, bool paused = false, bool start = true);
    void bgConnect();
    void bgUpdate();
    void bgSyncQueue();
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
    void applyLoaded(TrackLoader::Result& r);
    void applyCover(TrackLoader::Result& r);
    TrackLoader::Result coverWait_;   // a loaded cover kept for the frame after its tags (one thing at a time)
    bool coverWaiting_ = false;
    void parseLyrics(const std::string& text);

    void navigate(View v, int ref = -1, const std::string& query = {}, bool push = true);
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
    // The sidebar's progress card: what runs in the background (the music folder check, tags, the cover cache, the overlay's covers)
    struct Job { std::string label; int done = 0, total = 0; int settingsCat = 2; };   // total 0: no count, just busy
    bool backgroundJob(Job& j);
    void drawJobCard(float x, float bottom, float w, bool collapsed);
    float jobShown = 0, jobSince = 0;
    Job jobLast;
    void drawSeek(float x, float y, float w, float h, Id id);

    // ================= widgets.cpp: Gatefold's components
    bool iconButton(Id id, const char* icon, float cx, float cy, bool on = false, float size = 34, float iconSize = 18);
    float button(Id id, const char* icon, const std::string& label, float x, float y, bool primary = false, bool* clicked = nullptr,
                 bool toggled = false, bool danger = false, uint8_t flags = 0);
    float chip(const std::string& label, float x, float cy, bool accent);
    void kicker(const std::string& s, float x, float cy, const Rgb& c, float spacing = 0.12f);
    void eqGlyph(float x, float cy, const Rgb& c);
    void lyricDots(float x, float cy, float d, const Rgb& c, float alpha, bool current);
    void coverBox(int track, float x, float y, float size, float rad, float alpha = 1);
    bool trackRow(Id id, int track, int number, float x, float y, float w, float h, bool showAlbum);
    void listHead(float x, float y, float w, bool showAlbum);
    float viewHead(const std::string& kick, const std::string& title, const std::string& sub, float x, float y, float w);
    void slider(Id id, float x, float cy, float w, float value, float* out);
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
    void drawPicker(float x, float y, float w, float h);
    void useMusicFolder(const std::string& path);
    int routeAlbum() const;
    int routeArtist() const;
    void openAlbum(int a);
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
              const std::vector<int>* quad = nullptr);

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
    float viewSwitch(const char* key, float rx, float cy, int& mode);
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

    // ================= view_settings.cpp
    void drawSettings(float x, float y, float w, float h);

    // ================= view_sheet.cpp: the track menu
    Sheet sheet;
    int hoverTrack = -1, hoverTrackShown = -1;   // the track row under focus this frame / last frame
    void openTrackMenu(int track);
    void closeSheet();
    void sheetBack();
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
    std::string settingsQuery;
};
