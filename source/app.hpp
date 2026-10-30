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

#include "automix.hpp"
#include "bgclient.hpp"
#include "gfx.hpp"
#include "levels.hpp"
#include "mood.hpp"
#include "report.hpp"
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
    // Setting: its own options (Support's form) instead of a smart recipe's: the labels, the one chosen, and what a pick does
    std::vector<std::string> opts;
    int cur = 0;
    std::function<void(int)> pick;
};

/** The track menu (+ on a track): its page and what it is working on. */
struct Sheet {
    // NewList, Smart and SmartForm (view_smart.cpp) belong to no track: + in the sidebar opens them.
    enum Page { Closed, Menu, Tags, Cover, Delete, Info, Lists, NewList, Smart, SmartForm, Sleep, Job, Jobs, Colour, PlFile, Confirm, EqPreset, ActList, ListActions,
                PlMenu, PlIcon,     // PlMenu / PlIcon: a playlist page's ⋯ and its icons (recipe: the playlist, name: its name)
                FileMenu, FileInfo, FileLink,     // Files: a file's menu, its details, the track an .lrc goes with (dir: the file)
                Release,                          // Settings → Updates: a release's changelog and Install (recipe: its index)
                Model,                            // … an Automix model: Download, Use, Go back (name: the model's)
                Factory,                          // Settings → Reset → Factory reset: what to erase (App::factoryPick)
                Report, Crashes };                // Settings › Support: the report's form, Atmosphère's crash reports (view_support.cpp)
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
    // Report (Settings › Support): the form as it is filled
    int repKind = 0, repArea = -1, repOften = 0;   // a bug or an idea; the part of the app (App::kReportAreas); always, sometimes, once
    std::string repTitle, repWhat, repSteps;
    bool repCrashes = true, repLog = true;
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
        SDL_Texture* frost = nullptr;      // the cover blurred smooth (TrackLoader's frost): Automix's join
        SDL_Texture* prevFrost = nullptr;
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
    bool levelAllSent = false;   // the library was handed to the worker (levelling on), not only what Automix needs
    int levelModeSent = -1, levelTargetUsed = 0;
    // Crossfade (Gatefold's own player; the background player does its own): this track's was looked at
    bool xfadeChecked = false;
    // Automix (automix.hpp): while the join runs, the chip on Now playing says so, until mixUntil
    double mixUntil = 0;
    double mixShownAt = 0, mixFrom = 0;   // the chip's start (its animation), and where the join itself begins
    double mixShowLen = 0;                // the show's length as announced (endJoinShow shortens mixUntil, not this)
    bool mixAnnounced = false;            // shown, the next track not loaded yet (0.4 s: updateAutomix)
    /** A join's show cut short (a skip, a pause, a seek, Stop: the user, 2026-10-29, "they must end cleanly"): every
     *  animation carried on from where it is to its end over `over` seconds (the hint's own 0.9 s ease), the swipes too. */
    void endJoinShow(double over = 0.9);
    unsigned mixSeeksSeen = 0;            // Player::seeks() last frame (a seek through a join's show: endJoinShow)
    void holdOrderForAutomix(bool rebuild = false);        // Shuffle set aside while an Automix playlist is queued, back after (app.cpp)
    automix::Model mixModel() const;      // Settings › Debug's Automix model in dev builds; a release plays automix::Latest
    // Automix's models over the air (the user, 2026-10-30): the ones downloaded (config/gatefold-nx/models/*.model), read at
    // launch and after a download; the one chosen in Settings › Updates › Automix models plans the joins (planJoin)
    struct ModelEntry { std::string file; automix::ModelFile m; };
    std::vector<ModelEntry> modelFiles;
    void loadModelFiles();
    const ModelEntry* mixFile() const;    // the downloaded model chosen, or nullptr (a built-in one)
    std::string mixName() const;          // the model playing, by its name
    automix::Plan planJoin(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const automix::Join* want) const;
    struct ModelRow { std::string name; bool builtin = false, downloaded = false; int offer = -1; };
    std::vector<ModelRow> modelList() const;   // Settings › Updates › Automix models: built in, downloaded, listed
    void openModel(const std::string& name);   // its sheet (Sheet::Model)
    void useModel(const std::string& name);
    void downloadModel(int offer);
    void modelSheetRows(std::vector<SheetRow>& rows);
    void modelSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    /** Automix's hold on Now playing during a join: `move` the cover's glide to the middle (0..1), `ui` how much of the rest
     *  shows (1: all), `quiet` how far the cover's pulse rests (1: still). As the Canvas's layout morph. */
    struct Focus { float move = 0, ui = 1, quiet = 0; };
    // mode: Settings › Appearance › Automix: 0 Full, 1 Immersive (the same curve; what it moves and fades is the caller's),
    // 2 Simple (nothing moves or fades: the pulse's rest only)
    Focus automixFocus(int mode = 0) const;
    // The outgoing song through a join (Simple and Immersive: the user, 2026-10-29): what it showed, kept when the next one
    // loads, so its title, lyrics, Up next and waveform can give way to the next one's as the join goes (mixWipe)
    struct MixOld {
        bool on = false;
        TrackInfo info;
        std::vector<LyricLine> lyrics;
        bool synced = false;
        std::string lyricsSource;
        std::vector<int> upNext;     // its Up next (the next one first)
        std::vector<float> wave;     // its seek bar's waveform (Waveform::peaks), empty if it had none yet
        double lyricsAt = 0, at = 0; // where its lyrics were, and when (they go on with the clock)
        int track = -1;              // its track (its genre chip)
        double posAt = 0;            // where it was playing at `at` (its time on the Canvas)
        bool coverIn = false;        // the next one's cover is in (applyCover): the outgoing one's is now.prevCover, else now.cover
        Palette palette;             // its palette (the Palette tab's swatches and roles)
    } mixOld;
    /** A cover (or its blur, its frost) of either song through a join: the outgoing one's, or the incoming one's. */
    SDL_Texture* mixCover(bool old, SDL_Texture* cur, SDL_Texture* prev) const { return old && mixOld.coverIn ? prev : cur; }
    /** The Automix hint above the cover rather than on it (Settings › Appearance › Automix › The hint; Automatic: each
     *  animation's own place, `simpleAbove` for the ones that set it above) */
    // Settings › Appearance › Automix › The hint (store.mixHint) is Now playing's Simple's only (the user, 2026-10-29: "Simple
    // mode will only let the users change the hint position on Now playing"), but its Lyrics tab's (always the lyrics'
    // place: hintInLyrics): 0 Automatic (above the cover), 1 on the cover, 2 above it. Every other animation, the Canvas's included, has its
    // own place: on the cover in Full and Immersive; the Canvas's Simple in the lyrics' place, else above the cover.
    bool nowHintAbove() const { return store.mixAnimNow == 2 && store.mixHint != 1; }
    // A page swiped (Simple: the whole of Now playing, or the whole Canvas but its controls; the player bar in Immersive and
    // Simple; the user, 2026-10-29): its images too (wipedImage: the covers, their frost, the Canvas's background). Every
    // section swipes across its own width, all on the join's one clock (mixWipe): they start together and end together,
    // each at its own speed ("each section separate, but all start at the same time and end at the same time", the user)
    struct PageWipe { bool on = false; } pageWipe_;
    // … but sections drawn as one (Now playing's info column, from the title to Up next or the lyrics: "this one should be
    // the only one not separated", the user, 2026-10-29): one wipe across x0..x1 for all of them, its light once (groupMagic)
    struct WipeGroup { bool on = false; float x0 = 0, x1 = 0; } wipeGroup_;
    void groupMagic(float y0, float y1) {
        const float p = mixOld.on ? mixWipe() : 1.f;
        if (!wipeGroup_.on || p <= 0 || p >= 1) return;
        float soft;
        magicEdge(wipeEdge(wipeGroup_.x0, wipeGroup_.x1, p, soft), wipeGroup_.x0, wipeGroup_.x1, y0, y1, p);
    }
    /** A section's wipe across x0..x1, p of the way: its edge, and `soft`, how wide the change across it is (the letters
     *  fade over three quarters of it each side of a 10 px gap, blurring over all of it); before it starts and after it
     *  ends nothing of it is on the section */
    static float wipeEdge(float x0, float x1, float p, float& soft) {
        soft = std::max(48.f, (x1 - x0) * 0.3f);
        const float reach = soft * 0.75f + 5;
        return x0 - reach + p * (x1 - x0 + 2 * reach);
    }
    /** An image of either song through a wipe across x0..x1 (the cover, its frost, the Canvas's background), on the join's
     *  clock as every section: draw(true) the outgoing one's, draw(false) the incoming one's over it from the left. `under`:
     *  the outgoing one only right of the edge (where the incoming one does not cover it: translucent, or no cover); `magic`:
     *  its edge's light down y0..y1. Without a page swiped, the incoming one's. */
    template <class F> void wipedImage(float x0, float x1, float y0, float y1, bool magic, bool under, F&& draw) {
        const float p = mixOld.on && pageWipe_.on ? mixWipe() : 1.f;
        if (p >= 1) { draw(false); return; }
        if (p <= 0) { draw(true); return; }
        float soft;
        const float edge = wipeEdge(x0, x1, p, soft);   // as wiped's
        if (under) g.setWipe(edge, soft, false);
        draw(true);
        g.setWipe(edge, soft, true);
        draw(false);
        g.clearWipe();
        if (magic) magicEdge(edge, x0, x1, y0, y1, p);   // its light inside the image only (it bled past the cover: the user, 2026-10-29)
    }
    /** How far the join has gone over to the next song, eased: 0 the outgoing song's, 1 the incoming one's (at least 0.8 s). */
    float mixWipe() const;
    /** Draws a block twice through a wipe from left to right (Gfx::setWipe): draw(true) the outgoing song's, draw(false) the
     *  next one's; once (the next one's) when no join shows. x0..x1: the block's width, y0..y1 its height (the magic's). */
    template <class F> void wiped(float x0, float x1, float y0, float y1, F&& draw) {
        const float p = mixOld.on ? mixWipe() : 1.f;
        if (p >= 1) { draw(false); return; }
        if (p <= 0) { draw(true); return; }
        if (wipeGroup_.on) { x0 = wipeGroup_.x0; x1 = wipeGroup_.x1; }   // one of a group's sections: the group's wipe
        float soft;
        const float edge = wipeEdge(x0, x1, p, soft), gap = 10;
        // shapes, images and icons (a chip, a row's cover): the outgoing one's into the next one's across the edge, per pixel
        // (by their middles, each pair of chips changed at once and the two songs' overlapped: "looks strange", the user)
        g.setShapesOnly(true);
        g.setWipe(edge, soft, false);
        draw(true);
        g.setWipe(edge, soft, true);
        draw(false);
        g.setShapesOnly(false);
        // the two songs' letters never on top of each other (they blurred into each other, the user said, 2026-10-29): the
        // outgoing one's gone just right of the edge, the next one's from just left of it, each over 3/4 of the soft width
        // (smoother, the user, 2026-10-29: over half, the letters turned to blur too abruptly, a title most);
        // between them a blur: both songs' letters again, faint, spread sideways, round the edge only, under its light
        ui.setQuiet(true);
        g.setTextNudge(0, 0, true);
        const float half = soft * 0.75f;
        g.setWipe(edge + gap / 2 + half / 2, half, false);
        draw(true);
        g.setWipe(edge - gap / 2 - half / 2, half, true);
        draw(false);
        g.setTextNudge(0, 0, false);
        // the blur: each song's letters from their blurred copies (Gfx::blurTexture, made once), round the edge only (copies
        // of the sharp letters, nudged round, showed as copies however many: "make the blur cleaner", the user, 2026-10-29)
        const float f0 = g.fade();
        g.setFade(f0 * 0.65f);
        g.setWipeBump(edge, soft);
        g.setTextBlur(true);
        draw(true);
        draw(false);
        g.setTextBlur(false);
        g.setFade(f0);
        ui.setQuiet(false);
        g.clearWipe();
        if (!wipeGroup_.on) magicEdge(edge, x0, x1, y0, y1, p);   // (a group's: once, groupMagic)
    }
    /** In a Simple join, lyrics on screen give the hint their place (the user, 2026-10-29: swiped, the two songs' lines
     *  never read right, however they met): they fade out as the hint comes in, the hint shows large where they were, and
     *  the next song's lyrics fade back in as it leaves (automixCoverEase: the hint's 0.9 s). The Canvas always; Now
     *  playing's Lyrics tab whatever its hint's setting (the user, 2026-10-29: set Above, it stayed on top of the cover). */
    bool hintInLyrics(bool canvas) const { return canvas ? store.mixAnimCanvas == 1 : store.mixAnimNow == 2; }
    /** … the hint there: centred in the lyrics' box, k its size (1: the chip's own). */
    void lyricsHint(float x, float y, float w, float h, float k) {
        const float e = automixCoverEase();
        if (e <= 0.002f) return;
        const float cw = 36 * k + float(g.measure(mixLabel, { int(std::round(css::Xs * k)), Weight::SemiBold }));
        automixChip(mixLabel, std::round(x + (w - cw) / 2), std::round(y + h * 0.42f), k, e, false, true);
    }
    /** The wipe's edge as something magical happening (the user, 2026-10-29): a soft column of light in the accent's ink
     *  travelling with it and sparkles rising round it, swelling mid-way and gone at both ends (p: how far the wipe has gone).
     *  Only inside its section, x0..x1 by y0..y1: it fades as the edge comes in and goes out, and is clipped there (the edge
     *  starts and ends outside the section: the info column's light ran over the cover, the user, 2026-10-29). */
    void magicEdge(float edge, float x0, float x1, float y0, float y1, float p);
    void drawMagic(float edge, float y0, float y1, float s);   // (its light and sparkles, s their strength)
    void drawNowCover(float x, float y, float art);
    SDL_FRect glideCover_{};   // where drawNow wants the cover while it glides: App::draw draws it last
    bool glideOn_ = false;
    std::vector<SDL_FPoint> lapPts_;      // the chip's progress lap (kept: nothing allocated per frame)
    bool mixPrepared = false;   // the next track opened ahead on the player's worker (Player::prepare)
    bool stretchBegun = false;   // this track started towards the next one's tempo (Automix v3: Player::beginStretch)
    // the dev build's audio trace (Player::takeTrace): audio-trace.txt in the config folder, every 5 s something went wrong
    double traceAt = 0;
    // ... and each Automix join's frames (the user's console videos, 2026-10-28: the animation dropped frames there): its
    // frames, average and worst frame time and work, frames over 17 ms; a line when the join ends
    int joinFrames_ = 0, joinLate_ = 0;
    double joinSum_ = 0, joinWorst_ = 0, joinWorkSum_ = 0, joinWorkWorst_ = 0;
    void traceJoinFrame(float frameMs, float workMs);
    // ... and what the worst one did (the console's first trace, 2026-10-29: one frame of 228 ms of work in a join): refreshes
    // really missed (a frame of 25 ms or more), when in the join the worst work came, its sections, update's slowest step
    int joinDrops_ = 0;
    std::string joinWhy_;
    double profFrame_[8] = {};   // this frame's sections (profMark)
    const char* updSlow_ = "";
    double updSlowMs_ = 0;
    Uint64 updAt_ = 0;
    void updMark(const char* step) {
        const Uint64 t = SDL_GetPerformanceCounter();
        const double ms = double(t - updAt_) / double(SDL_GetPerformanceFrequency()) * 1000;
        if (ms > updSlowMs_) { updSlowMs_ = ms; updSlow_ = step; }
        updAt_ = t;
    }
    unsigned traceStarved = 0;
    std::string audioTrace;
    std::string mixLabel;
    // The join editor (view_join.cpp): a playlist's join between two of its tracks, as Spotify's transition editor
    struct JoinEdit {
        bool open = false;
        std::string playlist;              // its name: the joins' key in mixes.tsv (Store::joinKey)
        int a = -1, b = -1;                // the two tracks (library indices)
        automix::Join join, kept;          // as edited; as kept (Auto when none)
        mixan::Info infoA, infoB;          // their analyses (empty: not analysed yet)
        double durA = 0;
        // Decoded on a worker when it opens: the first track's last 45 s and the second's first 40 s, 16-bit stereo at
        // the player's rate, and their loudness every 20 ms for the drawing; then, for Listen, the join rendered
        std::thread worker;                // one job at a time; joined before the next and when it closes
        std::atomic<bool> cancel{ false };
        std::atomic<int> loaded{ 0 };      // 0 reading, 1 ready, 2 could not
        std::atomic<int> rendered{ 0 };    // 0 none, 1 rendering, 2 ready (pcm), then handed to the player
        std::vector<int16_t> pcmA, pcmB, pcm;
        double aFrom = 0;                  // the first track's time at pcmA's start (s)
        std::vector<float> envA, envB;     // 0..1 every 20 ms
        double listenFrom = 0;             // the first track's time at the preview's start (s)
        bool listening = false;
        int rate = 48000;
        // a touch drag on the picture: the second track (when it comes in) or the join's end (its length); the time
        // across the picture is held still while it lasts, and the join as it was when it began
        double viewT0 = 0, viewT1 = 0;     // the first track's time across the picture: kept while it is open (widened when
                                           // the join moves out of it), so a dragged track stays where it was let go
        int drag = 0;                      // 0 none, 1 the second track, 2 the join's end
        float dragX = 0;
        double dragT0 = 0, dragT1 = 0;
        automix::Join dragFrom;
    } je;
    // Automix playlists (view_automix.cpp): the picker, and their order
    struct AutomixPick {
        bool open = false;
        int tab = 0;               // Tracks, Albums, Artists, Genres, Folders
        std::vector<char> sel;     // per library track: ticked
    } ap;
    double automixTickAt = 0, automixSavedAt = 0;
    bool automixReady(const std::string& path) const;   // analysed for Automix (its mix.tsv row)
    void openAutomixPicker();
    void closeAutomixPicker();
    void drawAutomixPicker();
    void orderAutomixList(int ref, bool announce);
    void automixListsTick();
    std::vector<std::string> automixWaitingTracks(size_t max) const;
    // Automix is only Automix playlists' (the user, 2026-10-28): the queue came from one of them
    bool automixQueue() const;
    int queuePlaylist() const;   // the playlist the queue came from (by its name), or -1
    void automixProgress(int& done, int& total) const;   // their tracks analysed, of all of them
    void openJoinEditor(const std::string& playlist, int a, int b);
    void closeJoinEditor();
    void drawJoinEditor();
    automix::Plan joinPlan() const;   // the editor's join as it would play
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
        int modelSent = -1;        // Automix's model, as told to the player (protocol 6)
        std::string modelFileSent = "\x01";   // … a downloaded one's file ("": the built-in modelSent)
        bool automix = false;      // the queue it has is an Automix playlist
        unsigned joinGen = 0;      // its last Automix join, shown
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
    bool mouseHeld_ = false;               // a mouse's left button held: a finger at the pointer (App::input)
    float rehover_ = 0;                    // a list scrolled under the still pointer: what it is over, again, while it glides

    // ---------------- misc
    std::string toastText;
    float toastT = 0;
    double time = 0;
    float dt = 0;
    bool running = true;
    float sideW = css::SideW;
    // The sidebar opening and closing (the user, 2026-10-30: "a nice animation"): sideP_ runs 0 (icons only) to 1 (open) in
    // 0.38 s, eased in and out; sideW follows it, and the labels, counts and the wordmark fade and slide in with it
    float sideP_ = 1;
    double sideOpenedAt_ = -10;   // when it last began to open: the wordmark's shimmer plays as it lands
    /** How open the sidebar is, 0..1, as drawn (from sideW) */
    float sideOpen() const { return std::clamp((sideW - css::SideWCollapsed) / (css::SideW - css::SideWCollapsed), 0.f, 1.f); }
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
        const double ms = double(t - profAt_) / double(SDL_GetPerformanceFrequency()) * 1000;
        profAcc_[section] += ms;
        profFrame_[section] += ms;
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
    std::vector<SDL_FRect> settingsRows;   // each row of Settings' pane as last drawn (the tour lights whole rows)
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
    void playAt(int orderPos, double fade = 0, const automix::Plan* mix = nullptr);
    void loadTrack(int trackIdx, bool paused = false, bool start = true, double fade = 0, const automix::Plan* mix = nullptr);
    /** Every frame: Automix's join at the end of this track, planned from both analyses and started on time. */
    void updateAutomix();
    void keepMixOld();                          // the outgoing song kept through a join (mixOld)
    void announceBgJoin(const BgState& s);      // the background player's join announced (protocol 6)
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
    // List: a track of one of your own playlists (route.ref), by its row on the page
    struct Grab { enum Kind { Queue, Playlists, Tabs, List } kind = Queue; int pos = -1; bool upNext = false; int cols = 1; int ref = -1; } grab;
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
    float automixChip(const std::string& label, float x, float cy, float k, float alpha, bool lap = true, bool glass = false);   // animated, while Automix joins
    /** Now playing's cover frosted over during a join (its beat-scaled box), the hint inside, centred on (hx, hy) and sized
     *  by `rest`: the cover at rest, so the hint holds still while the cover breathes. */
    void drawAutomixCover(float x, float y, float s, float hx, float hy, float rest, float rad = 16, bool hint = true, float lapW = 2.5f);
    float automixCoverEase() const;   // the frost's strength now (0.9 s in and out, as the glide): for a hint drawn beside it
    /** A part `p` (0..1) of a rounded box's outline, from the middle of its top edge, clockwise (Automix's progress lap). */
    void lapPath(float x, float y, float w, float h, float r, float p, std::vector<SDL_FPoint>& pts);
    void kicker(const std::string& s, float x, float cy, const Rgb& c, float spacing = 0.12f);
    void automixKicker(const std::string& s, float x, float cy);
    // Hints (the user, 2026-10-29): an icon-only button's name, in a small pill by it once it has been focused (the D-pad,
    // or the pointer over it) for half a second. Registered as the button is drawn; drawn last (widgets.cpp)
    void hint(Id id, const std::string& text) { hints_.push_back({ id, text }); }
    void drawHint();
    std::vector<std::pair<Id, std::string>> hints_;
    Id hintId_ = 0;
    double hintSince_ = 0;
    float hintA_ = 0;
    std::string hintText_;
    SDL_FRect hintAt_{};   // "Automix playlist", with a little magic (widgets.cpp)
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
    // Now playing's lyrics from a source: the song playing (live: the timing control, taps, the scroll by hand), or the
    // outgoing one in a Simple join (at: where its lines are)
    void drawLyricsOf(float x, float y, float w, float h, const std::vector<LyricLine>& lyrics, bool synced, const std::string& source,
                      double at, ScrollArea& sa, bool live);
    void drawPaletteTab(float x, float y, float w, float h);
    void drawPaletteOf(float x, float y, float w, float h, const Palette& target, bool live);   // (a join's outgoing colours: not live)
    void drawCanvas();
    void warmCanvas();
    void drawCanvasLyrics(float x, float y, float w, float h, int size, int layout, float alpha);
    void drawCanvasLyricsOf(float x, float y, float w, float h, int size, int layout, float alpha, const std::vector<LyricLine>& lyrics,
                            bool synced, double at, ScrollArea& sa, bool live);   // (a source: the song playing, or the outgoing one)
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
    const std::vector<TourStep>& tourSteps() const { return tourStepsAt(tourLevel); }   // the chosen tour's
    static const std::vector<TourStep>& tourStepsAt(int level);   // 0 Basic, 1 Advanced, 2 Complete
    int tourStep = -1;                  // the step shown; −1: no tour
    int tourLevel = 0;                  // the tour chosen on its first card
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
    // joinGap: room after each row but the last (a playlist page's Mix: the joins between its tracks)
    std::vector<ListEntry> listLayout(const std::vector<int>& tracks, float w, ListMode mode, float joinGap = 0) const;
    float listRowY(const std::vector<ListEntry>& layout, int index) const;
    float drawTrackBody(const char* key, const std::vector<int>& tracks, float x, float top, float w, float clipY, float clipH, ListMode mode,
                        bool extended, const std::function<void(int)>& play, float joinGap = 0);
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
    std::string selfPath = "sdmc:/switch/Songate/Songate.nro";   // the running .nro (main's argv[0])
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
    void allowBetas();                  // Settings › Updates › Beta versions: asked first, with the warning
    // Settings › Support (view_support.cpp, report.hpp): the report's form, sent on a worker; Atmosphère's crash reports and the
    // console's versions, read on a worker when Support first opens; the app's own log (every toast, with its time)
    void openReport(int kind);          // 0 a bug, 1 an idea
    void allowReports();                // Support's switch: asked first, saying what a report sends and where (as Updates)
    void reportRows(std::vector<SheetRow>& rows);
    void reportHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    void reportFooter(float x, float w, float fy);
    void crashRows(std::vector<SheetRow>& rows);
    void supportScan();                 // (starts the worker once; again after a report)
    void supportTick();                 // every frame: takes the workers' results in
    std::string reportBody() const;     // the form and the app's part, Markdown (the worker adds the console's and the crashes)
    std::thread supportThread_, reportThread_;
    std::atomic<bool> supportReady_{ false }, reportDone_{ false };
    bool supportAsked_ = false;
    report::System supportSys_;
    std::vector<report::Crash> supportCrashes_;
    bool reportOk_ = false;
    int reportNumber_ = 0;
    std::string reportErr_;
    double reportSentAt_ = -1e9;        // one report each five minutes
    std::vector<std::string> logLines_; // the app's log: at most 200 lines, saved to log.txt now and then (supportTick)
    bool logDirty_ = false;
    double logSavedAt_ = 0;
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
    const char* playlistKind(int ref) const;
    static bool isMoodPlaylist(const FolderPlaylist& p) { return p.path.rfind("smart:mood:", 0) == 0; }   // (a smart recipe of its own kind)   // "Automix playlist", "Smart playlist"…: the page's and the menu's kicker
    void openPlaylistMenu(int ref);   // + on the Playlists page: Move, Icon, Rename
    void openPlaylistIcons(int ref);  // the playlist page's icon button: the icon grid at once
    void renamePlaylist(int ref);
    void renameJoins(const std::string& from, const std::string& to);
    std::string smartSummary(const std::string& spec) const;
    struct SmartCache { unsigned statsGen = ~0u, indexGen = ~0u, moodGen = ~0u; int64_t day = -1; std::vector<int> tracks; };
    // Mood playlists (mood.hpp): the library's moods, built from the analysis when it changes (moodTick); their progress
    mood::Index moodIdx_;
    // … sorted on a worker (cores::Files: 2.5 ms on the PC for 3,074 tracks, several times that on the console: never in a
    // frame), the features copied to it first; taken in when it is done (moodTick), joined at shutdown
    std::thread moodThread_;
    std::atomic<bool> moodReady_{ false };
    mood::Index moodNext_;
    std::vector<mixan::Mood> moodIn_;
    unsigned moodGen_ = 0, moodLevelsGen_ = ~0u, moodLibEpoch_ = ~0u;
    double moodBuiltAt_ = -100;
    int moodDone_ = 0, moodTotal_ = 0;
    void moodTick();
    void moodProgress(int& done, int& total) const { done = moodDone_; total = moodTotal_; }   // (total 0: no Mood playlist)
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
    enum SettingsCat { SetAppearance = 0, SetPlayback = 1, SetEqualizer = 2, SetLibrary = 3, SetStorage = 4, SetReset = 9, SetUpdates = 10, SetSupport = 11 };   // (Reset above Updates: the user, 2026-10-30)
    std::string settingsQuery;
};
