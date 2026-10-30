/*
 * Gatefold NX · tour.cpp
 * The guided tour (the user, 2026-10-28): offered by the first launch's welcome card, again from Settings › About.
 * Each step opens the real page it talks about, lights up the part (the rest dimmed) and explains it in a card with an
 * example. Next / Back / Chapters / ×, by D-pad, A and B, or touch. The pages under it are drawn as usual but take no
 * input (the tour is a modal); Ui::rectAny still knows where their buttons are.
 */
#include "words.hpp"
#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

#include <algorithm>
#include <cmath>

namespace {
constexpr uint64_t Golden = 0x9e3779b97f4a7c15ull;   // a segmented control's options: its id + golden × (k + 1)

}  // namespace

/** What a step lights up: an item (key, index), a segmented control's options (seg > 0), or a part of the screen. */
struct TourTarget {
    const char* key;
    int64_t i = 0;
    int seg = 0;
    float insetY = 0;   // lit this much less at the top and bottom (the seek bar: its touch area is taller than the bar)
};

struct TourStep {
    int level;   // the shortest tour it is in: 0 Basic, 1 Advanced, 2 Complete
    int chapter;
    const char* icon;
    const char* title;
    const char* text;
    std::function<void(App&)> enter;   // the page it shows (nullptr: stays where it is)
    std::vector<TourTarget> targets;   // nothing: the card alone, in the middle
    bool canvas = false;               // shown over the Canvas
};

namespace {
// A Joy-Con direction button's arrow: a triangle with soft corners (tinted by Gfx::icon), one per direction. Each is
// 13 wide and 8 deep with its centroid on the grid's centre (12, 12): centred by the bounding box, a triangle looks off
#define GF_ARROW(d) R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24"><path d=")SVG" d R"SVG(" fill="#ffffff" stroke="#ffffff" stroke-width="2.4" stroke-linejoin="round"/></svg>)SVG"
const char kArrowDown[] = GF_ARROW("M5.5 9.33h13L12 17.33z");
const char kArrowUp[] = GF_ARROW("M5.5 14.67h13L12 6.67z");
const char kArrowLeft[] = GF_ARROW("M14.67 5.5v13L6.67 12z");
const char kArrowRight[] = GF_ARROW("M9.33 5.5v13L17.33 12z");
#undef GF_ARROW
const char* const kChapters[] = { "Welcome", "Playing", "Library", "Playlists", "Files", "Sound", "Canvas", "Background player", "Settings" };
constexpr int kChapterCount = int(std::size(kChapters));
// The three tours the first card offers (the user, 2026-10-30)
struct TourLevel { const char* icon; const char* name; const char* what; };
const TourLevel kLevels[] = {
    { ICON_PLAY, "Basic", "The essentials, in a few minutes" },
    { ICON_GRID, "Advanced", "Every screen, and what its buttons do" },
    { ICON_SPARKLE, "Complete", "Everything in depth: Automix, tags, looks and more" },
};
}  // namespace

/** The steps, built here (an App member) so their pages can be opened with App's own calls; each tour's, in order. */
const std::vector<TourStep>& App::tourStepsAt(int level) {
    auto page = [](View v, int ref = -1) -> std::function<void(App&)> {
        return [v, ref](App& a) { a.navigate(v, ref, {}, false); };
    };
    auto settings = [](int cat) -> std::function<void(App&)> {
        return [cat](App& a) {
            a.settingsCat = cat;
            a.settingsQuery.clear();
            a.navigate(View::Settings, -1, {}, false);
            a.area("settings.pane").target = a.area("settings.pane").y = 0;
        };
    };
    // Written for someone who has never seen Songate: what the part is, what it does, and one thing to try.
    // Buttons are key caps ({A} … drawTour). Kept to about six lines each, so the card stays beside what it shows.
    // Each step's level (the user, 2026-10-30: "the app is quite complex: a basic, an advanced and a complete tour"):
    // 0 in every tour, 1 Advanced and Complete, 2 Complete only.
    static const std::vector<TourStep> all = {
        // ---- Welcome (the first card asks which tour: drawTour)
        { 0, 0, ICON_SPARKLE, "Welcome to the tour",
          "Songate shows you around its own screens, using your music. {A} or Next moves on, {B} or Back goes back, "
          "Chapters jumps to a topic and × ends the tour. Choose how deep to go:",
          page(View::Now), {} },
        // ---- Playing
        { 0, 1, ICON_DISC, "Now playing",
          "This is Now playing: the cover, the title, artist, album and year, and what kind of file it is. The genre is "
          "a link: " GF_TAP " it to see every track of that genre.",
          page(View::Now), { { "@main" } } },
        { 1, 1, ICON_HEART, "Track buttons",
          "Add to Liked saves the track to your Liked list. Album, Artist and Folder take you to their pages. The last "
          "button opens the track menu, with everything else you can do to the track.",
          page(View::Now), { { "np.like" }, { "np.album" }, { "np.artist" }, { "np.folder" }, { "np.more" } } },
        { 1, 1, ICON_LIST, "Up next, Lyrics and Palette",
          "Up next shows what plays after this track. Lyrics shows the words: synced lyrics follow the music, and tapping "
          "a line jumps to it. If they run early or late, Earlier and Later fix them for this track. Palette shows the "
          "colours taken from the cover.",
          page(View::Now), { { "np.tab" }, { "np.tab", 1 }, { "np.tab", 2 } } },
        { 0, 1, ICON_PLAY, "The player bar",
          "The controls are always here. The first button sets the order: Default, Repeat list, Repeat track or Shuffle. "
          "The last one stops the music after this track. " GF_ON_THE_PADS "{X} plays and pauses, {L} and {R} skip, and "
          "{ZL} and {ZR} jump ten seconds.",
          nullptr, { { "pb.order" }, { "pb.prev" }, { "pb.play" }, { "pb.next" }, { "pb.sac" } } },
        { 1, 1, ICON_WAVE, "The seek bar",
          "The waveform of the track. Drag it to jump anywhere. With the buttons, move onto it with {DPAD}, press {A} "
          "to grab it, steer with left and right, then press {A} or {B} to let go.",
          nullptr, { { "pb.seek", 0, 0, 10 } } },
        { 1, 1, ICON_MOON, "More controls",
          "The moon starts a sleep timer: the music fades out and pauses. The equalizer button switches it on and off; "
          "hold it to open the equalizer's settings. Then come the queue, the volume and the Canvas, which {-} opens "
          "too.",
          nullptr, { { "pb.sleep" }, { "pb.eq" }, { "pb.queue" }, { "pb.mute" }, { "pb.vol" }, { "pb.canvas" } } },
        { 2, 1, ICON_MOON, "The sleep timer",
          "15 to 90 minutes, or the end of the track. The music fades out over its last ten seconds and pauses where you "
          GF_SLEEP_WORDS "Then a card counts down and puts " GF_THE_CONSOLE " to sleep, unless you stay awake.",
          settings(App::SetPlayback), { { "set.sleepcon", 0, 4 } } },
        { 0, 1, ICON_PLUS, "The track menu",
          "Every track has a menu: " GF_TOUR_MENU ". For example, Play next slots "
          "it in right after the current track, and Add to a playlist… files it away.",
          page(View::Queue), { { "@main" } } },
        { 2, 1, ICON_TAG, "Tags, covers and details",
          "The track menu also edits its tags (title, artist, album, genre, year…) and changes its cover with any image on "
          GF_THE_CARD ", for one track or the whole album. Songate keeps your changes apart: your files are never "
          "touched, and Undo puts them back. Details shows its plays and its loudness.",
          page(View::Queue), { { "@main" } } },
        // ---- Library
        { 0, 2, ICON_GRID, "The sidebar",
          "Songate sorts your music by its tags, never by how your folders are arranged. Albums, Artists, Genres, "
          "Folders and Library each show it a different way. Settings › Appearance › Sidebar can reorder these tabs, "
          "hide some, or add dividing lines.",
          nullptr, { { "nav.now" }, { "nav.queue" }, { "nav.albums" }, { "nav.artists" }, { "nav.genres" }, { "nav.folders" }, { "nav.library" } } },
        { 1, 2, ICON_V_GRID, "Albums",
          "Every album, with its cover. The button at the top right sorts them by artist, title, year or folder. Press "
          "{A} on an album to open it: play it, shuffle it or add it to the queue.",
          page(View::Albums), { { "@main" } } },
        { 1, 2, ICON_MIC, "Artists and genres",
          "Every artist and every genre has a page of its own, with its albums and its tracks on separate tabs.",
          page(View::Artists), { { "@main" } } },
        { 1, 2, ICON_GRID, "Library",
          "Three tabs: Albums, Tracks and Folders. Tracks can be sorted by artist, title, album, year, genre or length, "
          "either way round. The filter narrows the list: sort by genre and type \"jazz\" to see only jazz. The letters "
          "on the right jump through it.",
          [](App& a) { a.store.libTab = 1; a.navigate(View::Library, -1, {}, false); },
          { { "lib.tab", 0, 3 }, { "trk.filter" }, { "trk.sort" }, { "trk.dir" } } },
        { 2, 2, ICON_V_LIST, "Four ways to list",
          "Every list of tracks can show them by album, with covers, as tiles, or as a plain list, and remembers which. "
          "Try them on Liked or any playlist: the view you pick there stays with that page.",
          page(View::List, -1), { { "pl.view", 0 }, { "pl.view", 1 }, { "pl.view", 2 }, { "pl.view", 3 } } },
        { 1, 2, ICON_FOLDER, "Folders",
          "Your music folder exactly as it sits " GF_ON_CARD ". Add as playlist puts a folder in the sidebar; Add all "
          "as playlists does it for every folder inside it at once.",
          [](App& a) { a.navigate(View::Folders, a.lib.rootFolders.empty() ? -1 : a.lib.rootFolders[0], {}, false); },
          { { "fo.play" }, { "fo.shuffle" }, { "fo.addpl" }, { "fo.addall" } } },
        { 1, 2, ICON_SEARCH, "Search",
          "Press {Y} or " GF_TAP " here to search your artists, albums and tracks. For example, \"live\" finds every live "
          "recording.",
          nullptr, { { "tb.search" } } },
        { 2, 2, ICON_HISTORY, "Play statistics",
          "A track counts as played after a minute, or nearly all of a shorter one, " GF_PLAYED_WHERE ". Plays, the last play and the date added show in Details, and smart playlists like Most played and "
          "Forgotten favourites are built from them.",
          page(View::Queue), { { "@main" } } },
        // ---- Playlists
        { 1, 3, ICON_LIST, "The queue",
          "Playlist is everything lined up to play. Press {A} on a row to play it, or {+} for more: Play next, Remove, "
          "or Move, which lets you carry it with {DPAD} and drop it with {A}. Current track scrolls back to what's "
          "playing; Clear empties the list.",
          page(View::Queue), { { "@main" } } },
        { 0, 3, ICON_PLUS, "Your playlists",
          "The + next to PLAYLISTS creates a playlist: your own, a smart one that keeps itself up to date (most played, "
          "recently added, one genre…), an Automix or a Mood playlist, or one imported from an .m3u file. " GF_CAP_TAP " the "
          "PLAYLISTS heading to see them all and put them in order.",
          nullptr, { { "nav.playlists" }, { "nav.newpl" } } },
        { 2, 3, PLI_SMART, "Smart playlists",
          "A smart playlist fills itself: Recently added, Most played, Recently played, Forgotten favourites, Never "
          "played, one genre, one decade, or long tracks. Each has one setting, shows how many tracks match before you "
          "create it, and stays up to date as you play.",
          nullptr, { { "nav.newpl" } } },
        { 0, 3, ICON_SPARKLE, "Automix playlists",
          "An Automix playlist plays like a DJ set. Songate listens to each song once, puts them in the order where they "
          "join best, and blends one into the next on the beat, on the drop, or through its fade. For example: + › "
          "Automix playlist…, then tick a few albums.",
          nullptr, { { "nav.newpl" } } },
        { 2, 3, ICON_SPARKLE, "How Automix joins",
          "A join plans itself from both songs' tempo, bars, key and bass: on the drop, a bridge between tempos, by the "
          "songs' structure, or a smart crossfade. While it plays, the cover frosts over with Automix's hint. Settings › "
          "Appearance › Automix chooses how a join shows, on Now playing and on the Canvas.",
          settings(App::SetAppearance), { { "set.mixnow", 0, 3 }, { "set.mixcanvas", 0, 2 } } },
        { 2, 3, ICON_SPARKLE, "Automix's order",
          "An Automix playlist keeps the order it found, and orders itself again as songs are added. Move a song by hand "
          "and your order stays until Order again; Play this first pins the opener. Automix playlists never shuffle: "
          "their order is the point.",
          page(View::Queue), { { "nav.playlists" } } },
        { 1, 3, PLI_MOOD, "Mood playlists",
          "A Mood playlist sorts your library by how it sounds: Happy, Party, Relaxed, Sad or Aggressive. Songate "
          "measures each song on " GF_THE_CONSOLE ", so the list fills in as it goes. For example: + › Mood playlist…, then "
          "Relaxed for a quiet evening.",
          page(View::Queue), { { "nav.newpl" } } },
        { 1, 3, ICON_HEART, "Playlist pages",
          "Liked and every playlist have a page like this. Play or shuffle it, export it as an .m3u8 file that other "
          "players can read" GF_GAMES_WORDS ". You can also give it an icon, rename it "
          "or delete it.",
          page(View::List, -1), { { "@main" } } },
        { 2, 3, PLI_STAR, "Icons and order",
          "Every playlist can wear one of 78 icons, shown in the sidebar even when it is folded. " GF_CAP_TAP " the PLAYLISTS "
          "heading for the Playlists page: Move puts them in your order, as in the queue.",
          page(View::Playlists), { { "@main" } } },
#ifndef GF_WINDOWED
        { 2, 3, ICON_GAME, "A playlist for a game",
          "Games on a playlist's page makes it start by itself when a game opens, on the HOME menu, back in the game or "
          "when it closes: where it was left, or shuffled afresh. The background player plays it, with Songate closed.",
          page(View::List, -1), { { "pl.games" } } },
#endif
        // ---- Files
        { 1, 4, PLI_SDCARD, "Files",
          "Files browses the whole " GF_CARD_NAME ". It is hidden at first: turn it on in Settings › Appearance › Sidebar. "
          GF_FILE_MENU ": an image can become a cover, an .lrc file a track's lyrics, an .m3u8 a "
          "playlist. You can also rename, copy, move and delete, one file or several.",
          nullptr, { { "nav.files" } } },
        // ---- Sound
        { 1, 5, ICON_EQ, "Equalizer",
          "Simple mode gives you a preset plus bass and treble; Advanced adds ten bands and a preamp. Save your own "
          "sound under a name. For example: choose Rock, add a little bass, then Save as a new preset.",
          settings(App::SetEqualizer), { { "set.eqon" }, { "set.eqmode", 0, 2 }, { "set.eqpreset" } } },
        { 1, 5, ICON_PLAY, "Playback",
          "Volume levelling makes every track about as loud as the next. Crossfade blends the end of one track into the "
          "start of the next, but not between tracks of one album, so live albums stay seamless.",
          settings(App::SetPlayback), { { "set.xfade", 0, 6 }, { "set.level", 0, 4 } } },
        { 2, 5, ICON_WAVE, "Volume levelling",
          "Each track's loudness is measured on " GF_THE_CONSOLE ", as broadcasters measure it, with its true peak. Track "
          "levels every song alike; Album keeps an album's quiet and loud songs as they were; Auto does Album, but Track "
          "while shuffling. Your files are never changed.",
          settings(App::SetPlayback), { { "set.level", 0, 4 } } },
        // ---- Canvas
        { 0, 6, ICON_CANVAS, "The Canvas",
          "The Canvas gives the whole screen to the music, in three layouts: Cover, Lyrics and Minimal. Leave it alone "
          "and the controls fade away while the progress line grows into a spectrum. {-} or × closes it.",
          [](App& a) { a.navigate(View::Now, -1, {}, false); a.openCanvas(true); },
          { { "cv.layout", 0 }, { "cv.layout", 1 }, { "cv.layout", 2 }, { "cv.mute" }, { "cv.vol" }, { "cv.sleep" }, { "cv.lyrics" }, { "cv.eq" }, { "cv.close" } },
          true },
        { 2, 6, ICON_CANVAS, "The Canvas, your way",
          "Settings › Canvas picks its layout, a slow float and zoom on the cover, how soon the controls fade, and the "
          "spectrum. A track without lyrics switches to Minimal by itself and back with the next one that has some.",
          settings(6), { { "set.cvlayout", 0, 3 }, { "set.cvfloat" }, { "set.cvzoom" }, { "set.cvidle" } } },
        // ---- Looks (Complete)
        { 2, 8, ICON_SUN, "Colours",
          "From the cover takes the whole interface's colours from the album playing, on a dark or a light background. "
          "Dark and Light are fixed; Custom is yours, with a colour picker. Saturation and the colour transition fine-tune "
          "it.",
          settings(App::SetAppearance), { { "set.theme", 0 }, { "set.theme", 1 }, { "set.theme", 3 }, { "set.theme", 4 } } },
        { 2, 8, ICON_SIDEBAR, "The sidebar, your way",
          "Fold the sidebar to icons, reorder its tabs, hide the ones you never use (Now playing stays), add dividing "
          "lines, and show Files. " GF_CAP_TAP " the button at the top left to fold it any time.",
          settings(App::SetAppearance), { { "set.side" } } },
        { 2, 8, ICON_SLIDERS, "The visualizer",
          "The spectrum's sensitivity, smoothing, number of bars and peak caps, for Now playing and the Canvas apart, and "
          "how much the cover pulses with the bass.",
          settings(7), { { "set.vsens" }, { "set.vbands" }, { "set.vpeaks" } } },
        { 2, 8, ICON_GAME, "Controls",
          GF_TOUR_POINTER,
          settings(8), { { "set.ptrspeed" }, { "set.ptraccel" } } },
        // ---- Background player and overlay
#ifndef GF_WINDOWED
        { 0, 7, ICON_GAME, "The background player",
          "The background player keeps the music going on the HOME menu, in games, and after Songate is closed. Game "
          "actions decide what it does when a game opens, when you go to HOME, when you return and when the game "
          "closes: nothing, play, resume, pause, or start one of your playlists.",
          settings(App::SetPlayback), { { "set.bg" } } },   // drawTour scrolls the page to it
        { 2, 7, ICON_GAME, "Fades and game volumes",
          "Each game action fades the music in or out over the time you set. The game in front keeps its own music "
          "volume, and every other game shares one, set from the overlay. Automix playlists join there as here.",
          settings(App::SetPlayback), { { "set.bgfout" }, { "set.bgfin" } } },
        { 0, 7, ICON_GAME, "The overlay",
          "While a game is running, open Ultrahand ({L} + {BTN-DOWN} + a click of the right stick {RS}, unless you changed it) and choose "
          "Songate. From there you control the music, the queue, game actions, the equalizer, the sleep timer and the "
          "volume without leaving the game.",
          nullptr, {} },
        { 2, 7, ICON_FOLDER, "Browsing in the overlay",
          "The overlay's Browse opens your folders, or your playlists by kind: Normal, Smart, Mood and Automix, each with "
          "Songate's own icons. Play from any track, add one or all to the queue, all without leaving the game.",
          nullptr, {} },
#endif
        // ---- Settings
        { 0, 8, ICON_GEAR, "Settings",
          "Every option lives here, grouped by category. Find a setting searches them by name: \"sleep\" finds the sleep "
          "timer, " GF_THE_CONSOLE "'s sleep and the screen dimming.",
          settings(0), { { "@settingsCats" } } },
        { 2, 8, ICON_FOLDER, "Storage",
          "Where each browser opens, where backups go, and what Songate keeps " GF_KEPT_WHERE " (covers, measurements, "
          "tags), each with its size and a Clear button.",
          settings(App::SetStorage), { { "@main" } } },
        { 1, 8, ICON_HISTORY, "Backups and resets",
          "Back up the settings now keeps a copy you can restore later, and Songate backs up by itself before every "
          "update. Reset settings puts every setting back as it came. Factory reset erases what you choose, or "
          "everything.",
          settings(App::SetReset), { { "set.backupnow" }, { "set.reset" }, { "set.factory" } } },
        { 1, 8, ICON_DOWNLOAD, "Updates",
          "Updates stays off until you turn it on. Then Songate can check GitHub for new versions, show what changed, "
          "install one, or go back to an earlier one, and fetch new Automix models. With it off, Songate never goes "
          "online.",
          settings(App::SetUpdates), { { "set.updon" } } },
        { 2, 8, ICON_BOLT, "Betas and Automix models",
          "Beta versions lets you try new features before they are released: they can have bugs, so please report what "
          "you find. Automix models brings new ways of joining songs, downloaded and checked, and you can always go back "
          "to the one Songate came with.",
          settings(App::SetUpdates), { { "set.updbeta" }, { "set.model" } } },
        { 1, 8, ICON_HELP, "Support",
          "Found a bug, or have an idea? Support sends it straight to Songate's developer, with what helps to fix it: "
          GF_VERSIONS_WORDS ". It stays off until you "
          "turn it on.",
          settings(App::SetSupport), { { "set.repon" } } },
        { 0, 8, ICON_SPARKLE, "You're all set",
          GF_TOUR_END,
          nullptr, {} },
    };
    static const std::vector<TourStep> levels[3] = {
        [] { std::vector<TourStep> v; for (const auto& t : all) if (t.level <= 0) v.push_back(t); return v; }(),
        [] { std::vector<TourStep> v; for (const auto& t : all) if (t.level <= 1) v.push_back(t); return v; }(),
        all,
    };
    return levels[std::clamp(level, 0, 2)];
}

int App::tourCount() const { return int(tourSteps().size()); }

void App::startTour() {
    if (sheet.page != Sheet::Closed) closeSheet();
    // Nothing playing (a first launch): an album is put in the queue, paused, so Now playing, Up next and the
    // player bar show real music while the tour talks about them. Nothing is heard until you press play.
    if (now.track < 0 && q.tracks.empty() && !lib.tracks.empty()) {
        // the first album; on a first start, before its tags are read, the first folder that holds tracks
        std::vector<int> tracks;
        std::string name;
        if (!lib.albums.empty()) {
            const Album& al = lib.albums[albumOrder.empty() ? 0 : size_t(albumOrder[0])];
            tracks = al.tracks;
            name = al.title;
        } else {
            for (const auto& f : lib.folders)
                if (!f.tracks.empty()) { tracks = f.tracks; name = f.name; break; }
        }
        if (tracks.empty()) tracks = { 0 };
        grabDrop();
        q.tracks = tracks;
        q.name = name;
        q.failed = 0;
        q.order.resize(q.tracks.size());
        for (int i = 0; i < int(q.tracks.size()); i++) q.order[size_t(i)] = i;
        q.pos = 0;
        loadTrack(q.tracks[0], true);
    }
    tourFrom = route;
    tourStep = -1;
    tourChapters = false;
    tourLit = false;       // the light closes in from the whole screen
    tourCardSet = false;   // the card appears where its first step puts it
    tourMiss = 0;
    tourGo(0);
}

void App::tourGo(int k) {
    const auto& S = tourSteps();
    k = std::clamp(k, 0, int(S.size()) - 1);
    const bool wasCanvas = canvas;
    if (wasCanvas && !S[size_t(k)].canvas) openCanvas(false);
    tourStep = k;
    tourChapters = false;
    tourFocus = true;
    tourAt = time;   // its words fade in
    if (S[size_t(k)].enter) S[size_t(k)].enter(*this);
}

void App::endTour() {
    if (tourStep < 0) return;
    tourStep = -1;
    tourChapters = false;
    if (canvas) openCanvas(false);
    navigate(tourFrom.view, tourFrom.ref, tourFrom.query, false);
    ui.focusDefault();
    toast("The tour is in Settings › About whenever you want it again");
}

void App::tourBack() {
    if (tourChapters) { tourChapters = false; tourFocus = true; tourAt = time; return; }
    if (tourStep > 0) tourGo(tourStep - 1);
}

void App::drawTour() {
    if (tourStep < 0) return;
    const auto& S = tourSteps();
    const TourStep& st = S[size_t(tourStep)];
    ui.region(Region::Modal);

    // ---- what it lights up: the union of its targets as they are drawn (Ui::rectAny), scrolled into view
    SDL_FRect t{};
    bool have = false;
    auto add = [&](SDL_FRect r) {
        if (r.w <= 0 || r.h <= 0) return;
        if (!have) { t = r; have = true; return; }
        const float x1 = std::min(t.x, r.x), y1 = std::min(t.y, r.y);
        const float x2 = std::max(t.x + t.w, r.x + r.w), y2 = std::max(t.y + t.h, r.y + r.h);
        t = { x1, y1, x2 - x1, y2 - y1 };
    };
    ScrollArea* scroll = nullptr;
    // A setting is lit as its whole row, its name and words with its control (the user, 2026-10-30: "the user wants to see
    // settings and toggles, not just toggles"): across Settings' cards, from the pane's left to its right
    // (each row's own bounds as Settings drew it, the same as showSetting lights: every row alike, whatever its height)
    auto widen = [&](const std::string& key, SDL_FRect r) {
        if (route.view != View::Settings || key.rfind("set.", 0) != 0 || key == "set.cat" || r.w <= 0) return r;
        const float cy = r.y + r.h / 2;
        for (const SDL_FRect& row : settingsRows)
            if (cy >= row.y && cy < row.y + row.h) return row;
        return r;
    };
    for (const TourTarget& tg : st.targets) {
        const std::string key = tg.key;
        if (key == "@main") add({ sideW, float(css::TbH), css::W - sideW, float(css::H - css::TbH - css::BarH) });
        else if (key == "@settingsCats") {
            for (int i = 0; i < 16; i++) add(ui.rectAny(uid("set.cat", i)));
        } else if (tg.seg > 0) {
            for (int k = 0; k < tg.seg; k++) {
                ScrollArea* a = nullptr;
                add(widen(key, ui.rectAny(uid(tg.key) + Golden * uint64_t(k + 1), &a)));
                if (a) scroll = a;
            }
        } else {
            ScrollArea* a = nullptr;
            SDL_FRect r = ui.rectAny(uid(tg.key, tg.i), &a);
            if (r.h > tg.insetY * 2) { r.y += tg.insetY; r.h -= tg.insetY * 2; }
            add(widen(key, r));
            if (a) scroll = a;
        }
    }
    if (have && scroll) {   // a row out of its list's view: the list scrolls to it
        const SDL_FRect v = scroll->view;
        // from where the list is now (not added again each frame while it glides: that overshot)
        if (t.y < v.y + 8) { scroll->target = scroll->y - (v.y + 8 - t.y); scroll->clamp(); }
        else if (t.y + t.h > v.y + v.h - 8) { scroll->target = scroll->y + (t.y + t.h - (v.y + v.h - 8)); scroll->clamp(); }
        // the light and the card go where the part will be once the list has glided there, not after it as it moves
        t.y += scroll->y - scroll->target;
    }
    // ---- where the light goes, and how (the user, 2026-10-28: some moves were smooth, others snapped)
    // A page just opened may not have drawn the part yet: the light waits where it was (up to 0.4 s) instead of going
    // out for a frame. A step with nothing to light closes it to a point in the middle, smoothly, rather than at once.
    if (have) tourMiss = 0;
    else if (!st.targets.empty() && (tourMiss += dt) < 0.4f) { t = tourWant; have = true; }
    const bool open = have && t.w > 0;
    if (!open) t = { css::W / 2.f, css::H / 2.f, 0, 0 };
    tourWant = t;
    if (!tourLit) {   // the tour's first frame: the light closes in from the whole screen as the dimming comes up
        tourRect = { 0, 0, float(css::W), float(css::H) };
        tourDim = 0;
        tourLit = true;
    }
    const float k = 1 - std::exp(-dt / 0.11f);   // one ease for the light, the dimming and the card
    tourRect.x += (t.x - tourRect.x) * k;
    tourRect.y += (t.y - tourRect.y) * k;
    tourRect.w += (t.w - tourRect.w) * k;
    tourRect.h += (t.h - tourRect.h) * k;
    tourDim += (0.6f - tourDim) * k;

    // ---- the dimming, with a hole round the part; the margin and the outline shrink with the hole as it closes
    const Rgb black{ 0, 0, 0 };
    {
        const float grown = std::clamp(std::min(tourRect.w, tourRect.h) / 14.f, 0.f, 1.f);
        const float pad = 6 * grown;
        SDL_FRect h{ tourRect.x - pad, tourRect.y - pad, tourRect.w + pad * 2, tourRect.h + pad * 2 };
        // kept inside the screen, outline and all (the seek bar's touch area reaches the bottom edge)
        const float x1 = std::max(h.x, 2.f), y1 = std::max(h.y, 2.f);
        const float x2 = std::min(h.x + h.w, css::W - 2.f), y2 = std::min(h.y + h.h, css::H - 2.f);
        h = { x1, y1, std::max(0.f, x2 - x1), std::max(0.f, y2 - y1) };
        g.rect(0, 0, css::W, std::max(0.f, h.y), black, tourDim);
        g.rect(0, h.y + h.h, css::W, css::H - (h.y + h.h), black, tourDim);
        g.rect(0, h.y, std::max(0.f, h.x), h.h, black, tourDim);
        g.rect(h.x + h.w, h.y, css::W - (h.x + h.w), h.h, black, tourDim);
        if (grown > 0.01f) g.roundStroke(h.x, h.y, h.w, h.h, std::min(12.f, std::min(h.w, h.h) / 2), 2, pal.accent, 0.95f * grown);
    }

    // ---- the card
    const Font tf{ css::Lg, Weight::Bold }, bf{ css::Base };
    float w = 520, iw = w - 24 * 2 - 52;   // narrower beside a part too wide to have the card above or below it
    const float pad = 24;
    // The text, with the Joy-Con's buttons as key caps in it (the user, 2026-10-28): {A} {B} {X} {Y} {L} {R} {ZL} {ZR}
    // {+} {-} {DPAD} ({DPAD-DOWN}: one button marked) {BTN-DOWN} (one direction button, as the Joy-Con's) {LS} {RS}, laid out word by word (Gfx::wrap knows nothing of them)
    struct Run { std::string text, cap; };   // one or the other
    const Font capF{ css::Xs, Weight::Bold };
    auto round = [](const std::string& c) { return c == "A" || c == "B" || c == "X" || c == "Y" || c == "LS" || c == "RS"; };
    auto capW = [&](const std::string& c) { return c.rfind("DPAD", 0) == 0 || c.rfind("BTN-", 0) == 0 || c == "+" || c == "-" || round(c) ? 20.f : float(g.measure(c, capF)) + 14; };
    auto runW = [&](const std::vector<Run>& rs) {
        float rw = 0;
        for (const Run& r : rs) rw += r.cap.empty() ? float(g.measure(r.text, bf)) : capW(r.cap) + 2;
        return rw;
    };
    auto parse = [](const std::string& word) {
        std::vector<Run> rs;
        for (size_t i = 0; i < word.size();) {
            const size_t o = word.find('{', i), c = o == std::string::npos ? o : word.find('}', o);
            if (c == std::string::npos) { rs.push_back({ word.substr(i), "" }); break; }
            if (o > i) rs.push_back({ word.substr(i, o - i), "" });
#ifdef GF_WINDOWED
            rs.push_back({ "", plat::capName(word.substr(o + 1, c - o - 1)) });
#else
            rs.push_back({ "", word.substr(o + 1, c - o - 1) });
#endif
            i = c + 1;
        }
        return rs;
    };
    std::vector<std::vector<Run>> lines;
    auto wrap = [&] {
        lines.clear();
        const float spaceW = std::max(3.f, float(g.measure("a b", bf) - g.measure("ab", bf)));
        std::vector<Run> cur;
        float curW = 0;
        size_t i = 0;
        const std::string text = st.text;
        while (i < text.size()) {
            size_t e = text.find(' ', i);
            if (e == std::string::npos) e = text.size();
            const std::vector<Run> word = parse(text.substr(i, e - i));
            const float ww = runW(word);
            if (!cur.empty() && curW + spaceW + ww > iw) { lines.push_back(cur); cur.clear(); curW = 0; }
            if (!cur.empty()) { cur.push_back({ " ", "" }); curW += spaceW; }
            cur.insert(cur.end(), word.begin(), word.end());
            curW += ww;
            i = e + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
    };
    wrap();
    auto drawCap = [&](const std::string& c, float cx0, float cyIn, float a) {
        const float cw = capW(c);
        const float cx = std::round(cx0 + cw / 2), cy = std::round(cyIn);   // whole pixels: the circle and its arrow share a centre
        if (c.rfind("BTN-", 0) == 0) {   // one direction button as on the left Joy-Con: round, with its arrow (the user, 2026-10-28)
            const std::string dir = c.substr(4);
            g.circle(cx, cy, 10, pal.text, 0.16f * a);
            g.icon(dir == "UP" ? kArrowUp : dir == "LEFT" ? kArrowLeft : dir == "RIGHT" ? kArrowRight : kArrowDown, cx, cy, 12, pal.text, a);
            return;
        }
        if (c.rfind("DPAD", 0) == 0) {   // the left Joy-Con's four direction buttons, in a diamond
            // {DPAD-UP} {DPAD-DOWN} {DPAD-LEFT} {DPAD-RIGHT}: that button marked with a dark spot (the user, 2026-10-28)
            const std::string dir = c.size() > 5 ? c.substr(5) : std::string();
            const std::pair<const char*, std::pair<float, float>> buttons[] = {
                { "UP", { 0.f, -6.f } }, { "DOWN", { 0.f, 6.f } }, { "LEFT", { -6.f, 0.f } }, { "RIGHT", { 6.f, 0.f } } };
            for (const auto& [name, d] : buttons) {
                const bool marked = dir == name;
                g.circle(cx + d.first, cy + d.second, marked ? 3.8f : 3.4f, pal.text, (marked ? 0.95f : 0.75f) * a);
                if (marked) g.circle(cx + d.first, cy + d.second, 1.7f, pal.surface2, a);
            }
            return;
        }
        if (c == "+" || c == "-") {   // as the buttons themselves: a solid plus, a solid bar (the user, 2026-10-28)
            g.roundRect(cx - 7, cy - 2.25f, 14, 4.5f, 1.5f, pal.text, 0.85f * a);
            if (c == "+") g.roundRect(cx - 2.25f, cy - 7, 4.5f, 14, 1.5f, pal.text, 0.85f * a);
            return;
        }
        if (round(c)) {
            g.circle(cx, cy, 10, pal.text, 0.16f * a);
            if (c == "LS" || c == "RS") g.roundStroke(cx - 10, cy - 10, 20, 20, 10, 1.2f, pal.text, 0.5f * a);   // a stick: ringed
            g.textC(c == "LS" ? "L" : c == "RS" ? "R" : c, capF, cx, cy, pal.text, a, Align::Center);
            return;
        }
        g.roundRect(cx0, cy - 9, cw, 18, 9, pal.text, 0.16f * a);
        g.textC(c, capF, cx0 + cw / 2, cy, pal.text, a, Align::Center);
    };
    constexpr float kChapRow = 44;   // a 34 px pill and 10 px clear between rows
    constexpr float kLevelRow = 58, kLevelGap = 8;   // the first card's tours
    const bool choosing = tourStep == 0 && !tourChapters;   // the first card: which tour (its rows replace Next)
    auto heightNow = [&] {
        const float bodyH = tourChapters ? kChapRow * ((kChapterCount + 1) / 2) - 10
                            : lines.size() * 22.f + (choosing ? 12 + 3 * kLevelRow + 2 * kLevelGap : 0);
        return pad + 20 + 28 + 10 + bodyH + 18 + 34 + pad;
    };
    float hWant = heightNow();
    // Where it goes: beside the part (where the light is heading, not where it is now), never over it: below, above,
    // right, left; with nothing lit, the middle. Then it glides there, and grows or shrinks to its new text.
    float wx = (css::W - w) / 2, wy = (css::H - hWant) / 2;
    if (open) {
        const float below = t.y + t.h + 18, above = t.y - 18 - hWant;
        if (below + hWant < css::H - 8) wy = below;
        else if (above > 8) wy = above;
        else if (t.x + t.w + 18 + w < css::W - 8) { wx = t.x + t.w + 18; wy = std::clamp(t.y, 8.f, css::H - 8 - hWant); }
        else if (t.x - 18 - w > 8) { wx = t.x - 18 - w; wy = std::clamp(t.y, 8.f, css::H - 8 - hWant); }
        else {   // no room for the whole card anywhere: a narrower one on the wider side (Settings' rows: over the categories)
            const float left = t.x - 18 - 8, right = css::W - 8 - (t.x + t.w + 18);
            if (std::max(left, right) >= 400) {
                w = std::min(520.f, std::max(left, right));
                iw = w - pad * 2 - 52;
                wrap();
                hWant = heightNow();
                wx = left >= right ? t.x - 18 - w : t.x + t.w + 18;
                wy = std::clamp(t.y + t.h / 2 - hWant / 2, 8.f, css::H - 8 - hWant);
            }
        }
        if (wy == below || wy == above) wx = std::clamp(t.x + t.w / 2 - w / 2, 8.f, css::W - 8 - w);
    }
    wx = std::clamp(wx, 8.f, css::W - 8 - w);   // never off the screen
    wy = std::clamp(wy, 8.f, css::H - 8 - hWant);
    if (!tourCardSet) { tourCard = { wx, wy, w, hWant }; tourCardSet = true; }
    tourCard.x += (wx - tourCard.x) * k;
    tourCard.y += (wy - tourCard.y) * k;
    tourCard.h += (hWant - tourCard.h) * k;
    tourCard.w += (w - tourCard.w) * k;
    w = std::round(tourCard.w);
    const float x = std::round(tourCard.x), y = std::round(tourCard.y), h = std::round(tourCard.h);
    // the step's words fade in and rise a little (the buttons stay put)
    const float fade = std::clamp(float(time - tourAt) / 0.22f, 0.f, 1.f), ta = fade * fade * (3 - 2 * fade), rise = 6 * (1 - ta);
    g.roundRect(x, y + 10, w, h, 18, black, 0.3f);
    g.roundRect(x, y, w, h, 18, pal.surface2);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f);
    g.icon(tourChapters ? ICON_LIST : st.icon, x + pad + 20, y + pad + 20, 20, pal.accentInk, ta);
    const float tx = x + pad + 52;
    // kicker: the chapter and where in the tour
    char where[96];
    if (tourStep == 0) snprintf(where, sizeof where, "%s · 3 tours", kChapters[st.chapter]);
    else snprintf(where, sizeof where, "%s · %d of %d", kChapters[st.chapter], tourStep + 1, int(S.size()));
    kicker(tourChapters ? std::string("Chapters") : std::string(where), tx, y + pad + 6 + rise, Rgb{ pal.surface2.r + (pal.accentInk.r - pal.surface2.r) * ta, pal.surface2.g + (pal.accentInk.g - pal.surface2.g) * ta,
                  pal.surface2.b + (pal.accentInk.b - pal.surface2.b) * ta });
    g.text(tourChapters ? "Jump to a chapter" : st.title, tf, tx, y + pad + 16 + rise, pal.text, ta, Align::Left, int(iw));
    float ly = y + pad + 20 + 28 + 10 + rise;
    bool c = false;
    if (tourChapters) {
        const float cw = (w - pad * 2) / 2;
        for (int ch = 0; ch < kChapterCount; ch++) {
            int first = 0;
            while (first < int(S.size()) && S[size_t(first)].chapter != ch) first++;
            const float cx = x + pad + (ch % 2) * cw, cy = ly + (ch / 2) * kChapRow;
            button(uid("tour.chap", ch), nullptr, kChapters[ch], cx, cy, st.chapter == ch, &c, false, false, st.chapter == ch ? DefaultFocus : 0);
            if (c) { tourGo(first); return; }
        }
    } else {
        for (const auto& l : lines) {
            float lx = tx;
            for (const Run& r : l) {
                if (r.cap.empty()) {
                    g.text(r.text, bf, lx, ly, pal.text2, ta);
                    lx += float(g.measure(r.text, bf));
                } else {
                    drawCap(r.cap, lx + 1, ly + 10.5f, ta);
                    lx += capW(r.cap) + 2;
                }
            }
            ly += 22;
        }
        if (choosing) {   // Basic, Advanced, Complete: each with its length; A on one starts it
            ly += 12;
            const float rx = x + pad, rw = w - pad * 2;
            const Font nameF{ css::Base, Weight::SemiBold }, whatF{ css::Sm };
            for (int i = 0; i < 3; i++) {
                const Id id = uid("tour.level", i);
                const float ry = ly + i * (kLevelRow + kLevelGap);
                if (ui.item(id, { rx, ry, rw, kLevelRow }, 12, i == tourLevel ? DefaultFocus : 0)) { tourLevel = i; tourGo(1); return; }
                const bool hot = ui.pressed(id) || ui.focused(id);
                g.roundRect(rx, ry, rw, kLevelRow, 12, pal.text, (hot ? 0.09f : 0.04f) * ta);
                g.roundRect(rx + 10, ry + 11, 36, 36, 10, pal.accent, 0.16f * ta);
                g.icon(kLevels[i].icon, rx + 28, ry + 29, 18, pal.accentInk, ta);
                char n[24];
                snprintf(n, sizeof n, "%d steps", int(tourStepsAt(i).size()));
                const float nw = float(g.measure(n, whatF));
                g.text(kLevels[i].name, nameF, rx + 58, ry + 9, pal.text, ta);
                g.text(kLevels[i].what, whatF, rx + 58, ry + 32, pal.text2, ta, Align::Left, int(rw - 58 - nw - 24));
                g.text(n, whatF, rx + rw - 14 - nw, ry + 20, pal.accentInk, ta);
            }
        }
    }
    // footer: Chapters on the left; Back and Next (Finish at the end) on the right; × at the top
    const float by = y + h - pad - 34;
    button(uid("tour.chapters"), ICON_LIST, tourChapters ? "Back to the step" : "Chapters", x + pad, by, false, &c);
    if (c) { tourChapters = !tourChapters; tourFocus = true; tourAt = time; return; }
    const bool last = tourStep == int(S.size()) - 1;
    const char* nextLabel = last ? "Finish" : "Next";
    const Font nf{ css::Sm, Weight::SemiBold };
    const float nw = g.measure(nextLabel, nf) + 32 + 24;
    const float nx = x + w - pad - nw;
    if (!tourChapters && !choosing) {
        // DefaultFocus too: a step that opens a page asks for the page's default focus a frame later, which took it off Next
        button(uid("tour.next"), last ? ICON_CHECK : ICON_CHEV, nextLabel, nx, by, true, &c, false, false, DefaultFocus);
        if (c) { last ? endTour() : tourGo(tourStep + 1); return; }
        if (tourStep > 0) {
            const float bw = g.measure("Back", nf) + 32 + 24;
            button(uid("tour.back"), ICON_BACK, "Back", nx - 8 - bw, by, false, &c);
            if (c) { tourBack(); return; }
        }
    }
    hint(uid("tour.x"), "Close");
    if (iconButton(uid("tour.x"), ICON_X, x + w - pad - 14, y + pad + 14)) { endTour(); return; }
    if (tourFocus) {   // each step opens on Next (on the chapter list, the chapter shown)
        tourFocus = false;
        if (tourChapters) ui.focusDefault();
        else ui.focus(choosing ? uid("tour.level", tourLevel) : uid("tour.next"));
    }
}

/** A first launch whose folder was chosen in the picker (not on the welcome card): the tour, offered once. */
void App::offerTour() {
    confirm(ICON_SPARKLE, "Welcome to Songate", "A tour of Songate", "Basic, Advanced or Complete: you choose", "Take the tour?",
            "Songate shows you around its own screens, with your music: playing, the library, playlists, files, sound, the "
            "Canvas, " GF_TOUR_BG_WORDS "and Settings. It starts once your music is read. It is also in "
            "Settings › About.",
            "Take the tour", [this] { tourPending = true; });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_SPARKLE;
}
