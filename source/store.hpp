/*
 * Gatefold NX · store.hpp
 * Everything that survives a restart, as small text files in sdmc:/config/gatefold-nx/.
 */
#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

/** A playlist in the sidebar: a folder kept in sync with the library, or your own list of tracks. */
struct FolderPlaylist {
    std::string name;
    std::string path;                  // a folder playlist: every track below this folder; empty for your own
    std::vector<std::string> tracks;   // your own: track paths, in your order
    bool own() const { return path.empty(); }
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

    std::string musicRoot;   // the folder the library is read from; empty until chosen or found
    std::vector<FolderPlaylist> playlists;
    std::vector<std::string> liked;   // track paths
    float volume = 1.f;
    Order order = Order::RepeatAll;
    bool sidebarCollapsed = false;
    bool motion = true;
    bool showSpectrum = true;
    int canvasLayout = 0;     // 0 cover, 1 lyrics, 2 minimal
    bool canvasLyrics = true;
    bool resume = true;
    bool background = true;   // play through the background player (sysmodule) when it is installed
    // The list views (ListMode) of the playlists, Folders and Library → Tracks, and the Library's tab
    int viewPlaylist = 1, viewFolders = 2, viewTracks = 1;
    int libTab = 1;            // 0 Albums, 1 Tracks, 2 Folders
    int tracksSort = 0;        // by artist, title, album, year, genre, length
    bool tracksDesc = false;
    int theme = 0;                  // 0 from the cover, 1 Dark, 3 Light (theme.js FIXED), 4 Custom; 2 was Grey, read as Dark
    int brightness = 0;             // From the cover: 0 dark, 1 light (palette-core.js opts.mode)
    float saturation = 1;           // opts.intensity, 0..1.5
    float colourTransition = 1.1f;  // seconds the colours take to follow a new cover (Gatefold's --tx)
    // Custom: your background and accents as 0xRRGGBB, −1 when left to the cover (theme.js customTheme)
    int customBg = 0x1a1720, customAccent = 0xff5a6a, customAccent2 = -1;
    bool canvasAutoMinimal = true;  // Minimal while a track has no lyrics
    bool canvasFloat = true;   // the cover's slow float
    bool canvasZoom = true;    // its Ken Burns zoom
    // Settings → Visualizer (defaults are Gatefold's, except a calmer cover pulse)
    float vizSensitivity = 0.5f;
    float vizSmoothing = 0.5f;
    int vizBands = 56;
    bool vizPeaks = true;
    float coverPulse = 0.35f;   // 1 = Gatefold's breathing
    bool canvasPulse = false;   // the pulse on the Canvas cover too
    int canvasBars = 112;           // the Canvas's idle spectrum (Now playing has vizBands)
    float canvasIdleAfter = 3.5f;   // seconds without input before the Canvas controls fade
    float canvasVizTime = 0.9f;     // seconds the progress line takes to grow into the spectrum
    // Settings → Controls: the left stick's pointer. Speed 0..1 (0.5 is 0.6's pace); acceleration: slow near the centre, fast at the edge
    float pointerSpeed = 0.5f;
    bool pointerAccel = true;
    bool frameTimes = false;        // Settings → About: the frame-time graph

    // Your changes, kept by Gatefold with the music files untouched (the console's FFmpeg cannot write files):
    std::unordered_map<std::string, std::map<std::string, std::string>> edits;   // track path → tag field → value
    std::unordered_map<std::string, std::string> covers;                         // track path → image in Dir/covers
    // Sidebar tabs hidden in Settings → Appearance: bit 0 Playlist, 1 Artists, 2 Genres, 3 Library, 4 Folders, 5 Albums
    unsigned hiddenTabs = 0;

    void load();
    /** Queued for a background thread (store.cpp); flush() waits until everything is on the card. */
    void save() const;
    static void flush();
    /** Any file of ours, written on the same thread (and atomically: next to it, then renamed over it). */
    void writeFile(const char* file, std::string data) const;
    void saveEdits() const;
    bool loadSession(Session& s) const;
    void saveSession(const Session& s) const;
    std::string path(const char* file) const { return std::string(Dir) + "/" + file; }

    bool isLiked(const std::string& p) const;
    void toggleLiked(const std::string& p);
    int playlistByPath(const std::string& p) const;
    /** A new playlist of your own, its name made unique; returns its index. */
    int newPlaylist(std::string name);
};
