/*
 * Gatefold NX · store.hpp
 * Everything that survives a restart, as small text files in sdmc:/config/gatefold-nx/.
 */
#pragma once
#include "subsonic.hpp"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfeqcfg.h"

/** A playlist in the sidebar: a folder kept in sync with the library, your own list of tracks, or a smart one. */
struct FolderPlaylist {
    std::string name;
    std::string path;                  // a folder playlist: every track below this folder; empty for your own;
                                       // "smart:<recipe>:<setting>" for a smart one (view_smart.cpp)
    std::vector<std::string> tracks;   // your own: track paths, in your order
    std::string icon;                  // its icon's key (plicons.inc), empty for its kind's own (playlist-icons.tsv)
    // An Automix playlist (one of your own; automix-lists.tsv): Gatefold orders it for the best joins (automix::order),
    // again whenever tracks are added, until it is moved by hand (manual) and "Order again" is pressed; pin: the track
    // that stays first (its path; empty: Gatefold chooses); waiting: ordered before all of it was analysed
    bool automix = false, manual = false, waiting = false;
    std::string pin;
    bool own() const { return path.empty(); }
    bool smart() const { return path.rfind("smart:", 0) == 0; }
    bool folder() const { return !own() && !smart(); }
};

enum class Order { Default, RepeatAll, RepeatOne, Shuffle };

/** "Resume where I left off": the play queue by path, so it survives a rescan. */
struct Session {
    std::string name;
    std::vector<std::string> tracks;
    std::vector<int> order;
    int pos = -1;          // index into order
    double position = 0;   // seconds into the current track
};

struct Store {
    static constexpr const char* Dir = "sdmc:/config/gatefold-nx";

    SubsonicConfig subsonic;
    std::string musicRoot;   // the folder the library is read from; empty until chosen or found
    std::vector<FolderPlaylist> playlists;
    std::vector<std::string> liked;   // track paths
    float volume = 0.5f;   // first-run defaults: the user's choices (2026-10-28, NEXT_SESSION #18)
    Order order = Order::RepeatAll;
    bool sidebarCollapsed = false;
    bool motion = true;
    // Settings › Appearance › Automix (the user, 2026-10-29): how a join shows. Now playing: 0 Full (the interface steps
    // aside, the cover glides to the screen's middle), 1 Immersive (the cover to the page's middle, the bars stay), 2 Simple
    // (all in place: the cover frosts, the texts wipe from one song to the next). The Canvas: 0 Full, 1 Simple
    int mixAnimNow = 0, mixAnimCanvas = 0;
    int mixHint = 0;   // the Automix hint: 0 Automatic (each animation's own place), 1 on the cover, 2 above it
    bool showSpectrum = true;
    int canvasLayout = 0;     // 0 cover, 1 lyrics, 2 minimal
    bool canvasLyrics = true;
    bool lyricTiming = false;  // Settings → Lyrics: the timing control (Earlier · 0.0 s · Later) on Now playing and the Canvas
    int lyricTimingFor = 2;    // ... for lyrics from 0 the tags, 1 .lrc files, 2 both
    bool resume = true;
    bool background = true;   // play through the background player (sysmodule) when it is installed
    // The list views (ListMode) of the playlists, Folders and Library → Tracks, and the Library's tab
    int viewPlaylist = 1, viewFolders = 2, viewTracks = 1, viewGenre = 1;   // viewGenre: a genre's or an artist's Tracks tab
    int viewPlaylists = 2;     // the Playlists page: tiles
    int libTab = 1;            // 0 Albums, 1 Tracks, 2 Folders
    int tracksSort = 0;        // by artist, title, album, year, genre, length
    bool tracksDesc = false;
    int theme = 0;                  // 0 from the cover, 1 Dark, 3 Light (theme.js FIXED), 4 Custom; 2 was Grey, read as Dark
    int brightness = 0;             // From the cover: 0 dark, 1 light (palette-core.js opts.mode)
    float saturation = 1;           // opts.intensity, 0..1 (higher changes nothing)
    float colourTransition = 1.1f;  // seconds the colours take to follow a new cover (Gatefold's --tx)
    // Custom: your background and accents as 0xRRGGBB, −1 when left to the cover (theme.js customTheme)
    int customBg = 0x1a1720, customAccent = 0xff5a6a, customAccent2 = -1;
    int customMode = 2;   // Custom's brightness: 0 dark, 1 light (your colours as a cover's), 2 the background as picked
    bool canvasAutoMinimal = true;  // Minimal while a track has no lyrics
    bool canvasFloat = true;   // the cover's slow float
    bool canvasZoom = true;    // its Ken Burns zoom
    // Settings → Visualizer (defaults are Gatefold's, except a calmer cover pulse)
    float vizSensitivity = 0.5f;
    float vizSmoothing = 0.f;
    int vizBands = 56;
    bool vizPeaks = true;
    float coverPulse = 1.f;     // 1 = Gatefold's breathing
    bool canvasPulse = true;    // the pulse on the Canvas cover too
    int canvasBars = 112;           // the Canvas's idle spectrum (Now playing has vizBands)
    float canvasIdleAfter = 3.5f;   // seconds without input before the Canvas controls fade
    float canvasVizTime = 0.9f;     // seconds the progress line takes to grow into the spectrum
    // Settings → Controls: the left stick's pointer. Speed 0..1 (0.5 is 0.6's pace); acceleration: slow near the centre, fast at the edge
    float pointerSpeed = 0.2f;   // Settings shows it doubled: 40%
    bool pointerAccel = true;
    bool frameTimes = false;        // Settings → About: the frame-time graph
    int heldOrder = -1;             // Shuffle, set aside while an Automix playlist plays (App::holdOrderForAutomix); -1 none
    std::string automixModel;       // the Automix model by its name ("Automix V1.2.0"): Settings › Updates › Automix models (a
                                    // downloaded one, or the built-in); Settings › Debug's built-in ones in dev builds; empty: the newest built-in
    std::string modelToasted;       // the downloadable model announced at launch (once each)
    bool keepAwake = true;          // while Gatefold is open: no dimming and no auto-sleep (until a sleep timer ends)
    int dimAfter = 0;               // … but dim the screen after this many minutes without input; 0 off
    bool batteryPercent = true;     // the title bar: the battery's percentage
    bool batteryEstimate = true;    // ... and how long to full or to empty
    int sleepConsole = 30;
    // Background work stopped by hand (the progress stack's menu, Settings) until resumed: the cover cache, the overlay's covers, measuring loudness
    bool stopCovers = false, stopThumbs = false, stopLevels = false;
    int levelMode = 0;              // volume levelling: 0 off (the default: the user chooses), 1 track, 2 album, 3 auto (GfbgLevel)
    int levelTarget = -14;          // ... to this loudness, LUFS          // seconds of the "going to sleep" card after the sleep timer ends; 0: do not sleep the console
    // Settings → Playback → Crossfade: seconds (0 off), and whether consecutive tracks of one album keep their join
    float crossfade = 0;
    bool xfadeNotInAlbum = true;
    // Settings → Equalizer (gfeqcfg.h); with the background player connected, the player's own copy wins (the overlay edits it too)
    GfbgEq eq = gfEqDefault();
    std::vector<GfEqUser> eqUser;   // your saved presets (GFEQ_USER, eq-presets.txt): eq.preset GFEQ_PRESETS + k is eqUser[k]

    // Your changes, kept by Gatefold with the music files untouched (the console's FFmpeg cannot write files):
    std::unordered_map<std::string, std::map<std::string, std::string>> edits;   // track path → tag field → value
    std::unordered_map<std::string, std::string> covers;                         // track path → image in Dir/covers
    std::unordered_map<std::string, float> lyricNudge;                           // track path → seconds its synced lyrics move (+ later)
    // Automix's joins edited on a playlist page: "playlist<TAB>path A<TAB>path B" → automix::Join::pack() (mixes.tsv)
    std::map<std::string, std::string> joins;
    static std::string joinKey(const std::string& playlist, const std::string& a, const std::string& b) { return playlist + '\t' + a + '\t' + b; }
    // Sidebar tabs hidden in Settings → Appearance: bit 0 Playlist, 1 Artists, 2 Genres, 3 Library, 4 Folders, 5 Albums, 6 Files
    // (Files is hidden until shown: a settings.txt from before it existed gets bit 6 too, Store::load)
    unsigned hiddenTabs = 64;
    // The sidebar's tabs in your order (Settings → Appearance → Sidebar); Settings stays at the bottom
    enum Tab { TabNow, TabQueue, TabAlbums, TabArtists, TabGenres, TabFolders, TabLibrary, TabFiles, TabCount };
    // Lines between the tabs, placed as you like (Settings → Appearance → Sidebar: Add a line, Move, Remove): ids TabLine + k
    static constexpr int TabLine = 100, MaxLines = 4;
    static bool isLine(int tab) { return tab >= TabLine && tab < TabLine + MaxLines; }
    static unsigned tabBit(int tab) {   // its bit in hiddenTabs (0.4's numbering); Now playing and lines have none
        static const unsigned bits[TabCount] = { 0, 1, 32, 2, 4, 16, 8, 64 };
        return tab >= 0 && tab < TabCount ? bits[tab] : 0;
    }
    static const char* tabName(int tab) {
        static const char* names[TabCount] = { "Now playing", "Playlist", "Albums", "Artists", "Genres", "Folders", "Library", "Files" };
        return tab >= 0 && tab < TabCount ? names[tab] : "Line";
    }
    // (a line above Files by default: it had one of its own before lines could be placed)
    std::vector<int> tabOrder = { TabNow, TabQueue, TabAlbums, TabArtists, TabGenres, TabFolders, TabLibrary, TabLine, TabFiles };
    bool showHidden = false;   // Files: names starting with a dot
    // Settings → Storage and reset → Folders: where each browser starts (empty: its own default)
    std::string filesDir, importDir, exportDir, coverDir, backupDir;
    // Settings → Updates: 0 off (nothing goes online; the default until you allow it), 1 allowed; a check at launch;
    // the newer version already announced at launch (once per version)
    int updates = 0;
    bool updateAtStart = false;
    bool betas = false;   // Beta versions: listed (apart) and offered; asked first (App::allowBetas)
    bool reports = false; // Settings › Support › Reports: off, nothing can be sent; on, asked first (App::allowReports)
    std::string updateToasted;

    void load();
    /** Queued for a background thread (store.cpp); flush() waits until everything is on the card. */
    void save() const;
    static void flush();
    /** At exit, after everything is saved: the writer's thread ends (a later write starts it again). */
    static void stopWriter();
    /** Any file of ours, written on the same thread (and atomically: next to it, then renamed over it). */
    void writeFile(const char* file, std::string data) const;
    void saveEdits() const;
    void saveLyricNudges() const;
    void saveJoins() const;
    void saveEqPresets() const;
    bool loadSession(Session& s) const;
    void saveSession(const Session& s) const;
    std::string path(const char* file) const { return std::string(Dir) + "/" + file; }

    bool isLiked(const std::string& p) const;
    void toggleLiked(const std::string& p);
    int playlistByPath(const std::string& p) const;
    /** A new playlist of your own, its name made unique; returns its index. */
    std::string freePlaylistName(std::string name) const;
    int newPlaylist(std::string name);
};
