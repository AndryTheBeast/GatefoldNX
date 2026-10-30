/*
 * Gatefold NX · app.cpp
 * Lifecycle, input mapping, play queue and track loading.
 */
#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"
#include "m3u.hpp"
#include "plicons.inc"
#include "net.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <random>
#include <strings.h>
#include <sys/stat.h>

namespace {


bool iless(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

std::string lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

/** Fires on press, then repeats while held (D-pad and stick). */
struct Repeat {
    double held = 0, nextAt = 0;
    bool step(bool down, double dt) {
        if (!down) { held = 0; return false; }
        if (held == 0) { held = dt; nextAt = 0.38; return true; }
        held += dt;
        if (held >= nextAt) { nextAt += 0.075; return true; }
        return false;
    }
};
Repeat rUp, rDown, rLeft, rRight;

std::mt19937 rng{ 0x6a7e };

}  // namespace

// ============================================================ lifecycle

bool App::init() {
    launchAt_ = profAt_ = SDL_GetPerformanceCounter();
    romfsInit();
    plInitialize(PlServiceType_User);
    psmInitialize();
    lblInitialize();   // Dim the screen: the handheld backlight (failing is fine: a veil instead)
    metadataInit();
    startMark("sys");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    startMark("init");
    window_ = SDL_CreateWindow("Songate", 0, 0, css::W, css::H, 0);
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window_ || !renderer_ || !g.init(renderer_)) return false;
    // Text in a scroll area follows its scroll sub-pixel, so a glide does not end in one-pixel steps.
    ui.onArea = [this](const ScrollArea* a) { g.setShift(0, textShift - (a ? a->y : 0)); };
    startMark("sdl");

    store.load();
    // After a factory reset: its old settings folder, moved aside at exit, is deleted in the background
    if (struct stat st; stat((std::string(Store::Dir) + ".erased").c_str(), &st) == 0)
        eraseOld_ = std::thread([] {
            cores::moveHere(cores::FileOps);
            Updater::eraseLeftovers(Store::Dir);
        });
    coversHeld = !store.stopCovers;   // until the covers are counted (and asked about, if many are missing)
    applyJobStops();   // before the workers start: a stopped job never begins
    stats.start(Store::Dir);
    levels.start(Store::Dir);
    sideW = store.sidebarCollapsed ? css::SideWCollapsed : css::SideW;
    sideP_ = store.sidebarCollapsed ? 0.f : 1.f;
    applyTheme();
    pal = target;
    loader.start();
    player.init();   // the UI still runs without audio, so a failure is not fatal
    player.setVolume(store.volume);
    player.setEq(store.eq);
    volumeShown = store.volume;
    startMark("audio");
    if (store.background) bgConnect();
    startMark("player");
    // Settings → Updates: the last check's list, read from the card (nothing goes online here unless allowed and asked for)
    updater.start(Store::Dir, selfPath);
    loadModelFiles();   // Automix's downloaded models (a few small files)
    if (store.updates && store.updateAtStart) checkUpdates(true);

    // Closing from the HOME menu ends the process without running shutdown(): hold the exit until the
    // settings and the session are written, and save whenever the app loses focus anyway.
    appletLockExit();
    appletHook(&appletCookie_, &App::appletEvent, this);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);

    // First start: always ask (the user, 2026-10-27). The folder picker at the card's top, and over it, when the card has
    // a Music folder at its top, a welcome card offering it; Choose another, B or × leave the picker to choose with.
    std::string found;
    if (store.musicRoot.empty())
        for (const auto& d : listDirs("sdmc:/"))
            if (strcasecmp(d.c_str(), "music") == 0) { found = "sdmc:/" + d; break; }   // keep the card's own spelling
    rescan();
    pcm_.resize(Spectrum::N);
    if (store.musicRoot.empty()) {
        tourAsk = true;   // a first launch: the tour is offered (the welcome card, or once a folder is chosen)
        navigate(View::PickFolder, -1, "sdmc:/", false);
        if (!found.empty()) welcomeCard(found);
    }
    else if (player.remote() && bgAdoptQueue()) {}   // the background player's queue, playing or not, wins
    else if (store.resume) restoreSession();
    startMark("queue");
    ui.focusDefault();
    ready_ = true;
    return true;
}

void App::useMusicFolder(const std::string& path) {
    store.musicRoot = path;
    store.save();
    rescan();
    navigate(View::Library, -1, {}, false);
    toast(std::to_string(lib.tracks.size()) + " tracks found in " + path);
    if (tourAsk) {   // a first launch whose folder was chosen in the picker: the tour, offered now
        tourAsk = false;
        offerTour();
    }
}

void App::rescan(const std::vector<std::string>* given) {
    // Track indexes change with the scan: remember the queue by path.
    const std::string current = now.track >= 0 ? lib.tracks[now.track].path : std::string();
    std::vector<std::string> queued;
    for (int t : q.tracks) queued.push_back(lib.tracks[t].path);

    meta.stop();
    libEpoch++;
    if (!ready_) startMark("start");
    const std::vector<std::string> roots = Library::validRoots(store.musicRoot.empty() ? std::vector<std::string>{} : std::vector<std::string>{ store.musicRoot });
    std::vector<std::string> files;
    bool remembered = false;
    if (given) files = *given;
    else if (!ready_ && !roots.empty()) {
        // Launch: the list from last time, if it is for the same folder ("root=" first, then one path a line).
        if (FILE* f = fopen(store.path("library.tsv").c_str(), "rb")) {
            char line[2048];
            std::string root;
            if (fgets(line, sizeof line, f)) { root = line; while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) root.pop_back(); }
            if (root == "root=" + roots[0])
                while (fgets(line, sizeof line, f)) {
                    std::string p = line;
                    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
                    if (!p.empty()) files.push_back(std::move(p));
                }
            fclose(f);
            remembered = !files.empty();
        }
    }
    if (!given && !remembered) files = Library::listFiles(roots);
    std::sort(files.begin(), files.end());
    lib.build(roots, files);
    if (!remembered && !roots.empty() && files != libFiles_) {
        std::string out = "root=" + roots[0] + "\n";
        for (const auto& p : files) out += p + "\n";
        store.writeFile("library.tsv", std::move(out));
    }
    libFiles_ = files;
    if (remembered) {
        // The card may have changed since: walked again on a worker core, compared in update().
        if (libCheck_.joinable()) libCheck_.join();
        libCheckDone_ = false;
        libCheck_ = std::thread([this, roots] {
            cores::moveHere(cores::Files);
            libChecked_ = Library::listFiles(roots);
            std::sort(libChecked_.begin(), libChecked_.end());
            libCheckDone_ = true;
        });
    }
    if (!ready_) startMark(remembered ? "list" : "scan");
    store.save();   // creates the config folder for the tag cache
    meta.overlay = [this](Track& t) { applyEdits(t); };
    meta.start(&lib, store.path("tags.tsv"));   // tags.tsv: meta.tsv from 0.2 cached failed M4A reads
    if (!ready_) startMark("tags");
    for (const auto& [path, file] : store.covers)
        if (auto it = std::find_if(lib.tracks.begin(), lib.tracks.end(), [&](const Track& t) { return t.path == path; }); it != lib.tracks.end())
            meta.setCoverFile(int(it - lib.tracks.begin()), file);
    indexDirty = true;
    rebuildIndex();
    if (!ready_) startMark("index");

    auto find = [&](const std::string& p) { auto it = pathIndex.find(p); return it == pathIndex.end() ? -1 : it->second; };
    now.track = find(current);
    q.tracks.clear();
    for (const auto& p : queued) if (const int t = find(p); t >= 0) q.tracks.push_back(t);
    q.order.resize(q.tracks.size());
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    q.pos = -1;
    for (int i = 0; i < int(q.tracks.size()); i++) if (q.tracks[i] == now.track) q.pos = i;
    // What the background player was sent is remembered by track index, and indexes mean other files now (a rename in
    // Files kept the same indexes for new paths, so the player kept the old ones): it is sent again, by path
    bgs.tracks.clear();
    history.clear();
    // Every list starts at its top again. Reset in place, never freed: a rescan can come in the middle of a frame (the
    // welcome card's button, the folder picker's "Use this folder"), and Ui still holds this frame's lists; freeing them
    // here let the next frame write into freed memory, which crashed the first launch's tour (2026-10-28, a data abort in
    // malloc on the loader thread)
    for (auto& [key, a] : areas) a = ScrollArea{};
}

void App::shutdown() {
    closeJoinEditor();   // its worker ends first
    updater.stop();   // a download stops; the files are only swapped when it finished
    setDimmed(false);
    persistNow();
    Store::flush();
    stats.stop();
    levels.stop();
    appletUnhook(&appletCookie_);
    appletSetMediaPlaybackState(false);
    if (awakeHeld) {   // the console's own idle times again
        appletSetIdleTimeDetectionExtension(AppletIdleTimeDetectionExtension_None);
        appletSetAutoSleepDisabled(false);
    }
    wave.cancel();
    loader.stop();
    if (libCheck_.joinable()) libCheck_.join();
    if (tracksView.thread.joinable()) tracksView.thread.join();
    if (fileWork.thread.joinable()) fileWork.thread.join();   // a copy finishes before Gatefold goes
    if (storage.thread.joinable()) storage.thread.join();     // Storage and reset's sizes
    if (eraseOld_.joinable()) eraseOld_.join();               // a factory reset's old folder (it carries on next launch)
    if (moodThread_.joinable()) moodThread_.join();           // Mood playlists' sorting (a few ms)
    if (supportThread_.joinable()) supportThread_.join();     // Settings › Support's reads (the crash reports)
    if (reportThread_.joinable()) reportThread_.join();       // … a report being sent (curl gives up within its timeouts)
    if (logDirty_) {
        std::string t;
        for (const auto& l : logLines_) { t += l; t += '\n'; }
        store.writeFile("log.txt", std::move(t));
    }
    meta.stop();
    player.shutdown();
    thumbs.stop();
    bg.disconnect();   // the background player carries on by itself
    Store::stopWriter();   // the last thread of ours: none may outlive Gatefold (hbloader cannot unload it then)
    for (SDL_Texture* t : { now.cover, now.prevCover, now.blur, now.prevBlur, now.frost, now.prevFrost, svTex, hueTex, wheelTex }) if (t) SDL_DestroyTexture(t);
    g.shutdown();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    IMG_Quit();
    SDL_Quit();
    lblExit();
    psmExit();
    plExit();
    romfsExit();
    net::end();
    // An update's files that wait (the app's own .nro above all: romfs no longer reads it), a fresh start or a restore;
    // then, after Restart now, the (new) Gatefold starts again.
    const std::string next = Updater::finishPending(Store::Dir, selfPath);
    const std::string again = next.empty() ? selfPath : next;   // (going back to 1.x: its own .nro, where it is)
    if (factoryMask_) Updater::factoryReset(Store::Dir, factoryMask_);   // what was ticked (or everything), moved aside; it starts again
    if (relaunch) {
#ifdef __SWITCH__
        if (envHasNextLoad()) envSetNextLoad(again.c_str(), again.c_str());
#else
        printf("[updater] Songate starts again: %s\n", again.c_str());
#endif
    }
    appletUnlockExit();
}

void App::appletEvent(AppletHookType type, void* self) {
    auto* app = static_cast<App*>(self);
    if (type == AppletHookType_OnExitRequest || (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)) {
        app->setDimmed(false);   // the backlight is the system's again outside Gatefold
        app->persistNow();
        if (type == AppletHookType_OnExitRequest) Store::flush();
    }
}

void App::persistSoon(float delay) {
    if (persistAt == 0 || time + delay < persistAt) persistAt = time + delay;
}

void App::saveStats() {
    statsSavedAt = time;
    if (stats.loaded() && stats.dirty()) store.writeFile("stats.tsv", stats.serialise());
    if (levels.loaded() && levels.dirty()) {
        store.writeFile("loudness.tsv", levels.serialise());
        store.writeFile("mix.tsv", levels.serialiseMix());
        store.writeFile("mood.tsv", levels.serialiseMood());
    }
}

/** Writes the settings and the session (sessionOnly: just the session, the 20 s save while playing). The files are written on a thread. */
void App::persistNow(bool sessionOnly) {
    if (!sessionOnly) persistAt = 0;
    if (!ready_) return;
    sessionAt = time;
    store.volume = volumeShown;
    if (!sessionOnly) {
        // A colour being tried in the picker is not chosen yet: what it was is saved until Use
        const int live = pick.field ? *pick.field : 0;
        if (pick.field) *pick.field = pick.before;
        store.save();
        if (pick.field) *pick.field = live;
    }
    saveStats();
    if (!store.resume) return;
    Session s;
    s.name = q.name;
    for (int t : q.tracks) s.tracks.push_back(lib.tracks[t].path);
    s.order = q.order;
    s.pos = q.pos;
    s.position = player.loaded() ? player.position() : 0;
    store.saveSession(s);
}

/** Puts back the queue of the last run, paused where it was. Paths that are gone are dropped. */
void App::restoreSession() {
    Session s;
    if (!store.loadSession(s)) return;
    std::vector<int> keep;   // old index → new index, −1 when the file is gone
    for (const auto& p : s.tracks) {
        auto it = pathIndex.find(p);
        keep.push_back(it == pathIndex.end() ? -1 : int(q.tracks.size()));
        if (it != pathIndex.end()) q.tracks.push_back(it->second);
    }
    if (q.tracks.empty()) return;
    q.name = s.name;
    const int cur = s.pos >= 0 && s.pos < int(s.order.size()) && s.order[s.pos] >= 0 && s.order[s.pos] < int(keep.size()) ? keep[s.order[s.pos]] : -1;
    q.order.clear();
    for (int o : s.order) if (o >= 0 && o < int(keep.size()) && keep[o] >= 0) q.order.push_back(keep[o]);
    if (q.order.size() != q.tracks.size()) {   // damaged or partial: the saved order is not a permutation any more
        q.order.resize(q.tracks.size());
        for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    }
    q.pos = 0;
    for (int i = 0; i < int(q.order.size()); i++) if (q.order[i] == cur) q.pos = i;
    queueNormalize();   // an older queue may hold a "Play next" outside the list's order
    holdOrderForAutomix(true);   // an Automix playlist restored with Shuffle on: its own order
    if (cur < 0) return;   // the track that was playing is gone: keep the queue, start nothing
    loadTrack(q.tracks[q.order[q.pos]], true);
    if (player.loaded() && s.position > 1) player.seek(s.position);
    savedTrack = now.track;
}

void App::run() {
    const double freq = double(SDL_GetPerformanceFrequency());
    Uint64 last = SDL_GetPerformanceCounter();
    while (running && appletMainLoop()) {
        const Uint64 nowTicks = SDL_GetPerformanceCounter();
        const double interval = double(nowTicks - last) / freq;
        last = nowTicks;
        // Frames are presented on vsync, so each one is on screen for a whole number of refreshes. The measured
        // interval jitters a millisecond or two around that; stepping animations by the jitter made their
        // motion uneven, most visibly at the slow start and end of an eased one. So dt is whole refreshes (at most 6).
        dt = std::clamp(int(std::lround(interval * 60)), 1, 6) / 60.f;
        time += dt;

        profAt_ = nowTicks;
        std::fill(std::begin(profFrame_), std::end(profFrame_), 0.0);
        ui.beginFrame(dt);
        input();
        update();
        profMark(0);
        draw();
        ui.endFrame();
#ifndef __SWITCH__
        // The preview's D-pad audit: hostshim sets GF_AUDIT from the script's "audit <label>"
        if (const char* a = getenv("GF_AUDIT"); a && *a) {
            static FILE* out = fopen("dpad-audit.txt", "w");
            if (out) ui.audit(a, out);
            _putenv("GF_AUDIT=");
        }
#endif
        frameMs_[frameAt_] = float(interval * 1000);
        workMs_[frameAt_] = float(double(SDL_GetPerformanceCounter() - nowTicks) / freq * 1000);
#ifdef GF_DEV
        traceJoinFrame(frameMs_[frameAt_], workMs_[frameAt_]);
#endif
        frameAt_ = (frameAt_ + 1) % 120;
        profAt_ = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer_);
        profMark(7);
        if (firstFrame_) { firstFrame_ = false; launchAt("frame1"); }
        profFrames_++;
        g.endFrame();
    }
}

// ============================================================ input

/** + (or a long touch, held): the menu of the row under focus. A long touch on something without one (a button)
 *  does nothing, without the toasts or Now playing's own track. */
void App::plusMenu(bool held) {
    if (route.view == View::Playlists && !canvas) {   // the Playlists page: the playlist's menu (Move, Icon, Rename)
        if (plHoverShown >= 0) openPlaylistMenu(plHoverShown);
        else if (!held) toast(plHoverShown == -1 ? "Liked stays first" : "Select a playlist, then press +");
    } else if (route.view == View::Files && !canvas) {   // Files: the menu of the row under focus
        if (!files.hover.empty()) openFileMenu(files.hover);
        else if (!held) toast("Select a file or folder, then press +");
    } else {
        const int t = hoverTrackShown >= 0 ? hoverTrackShown : !held && (route.view == View::Now || canvas) ? now.track : -1;
        if (t >= 0) openTrackMenu(t, hoverTrackShown >= 0 ? hoverQueueShown : -1, hoverUpNextShown);
        else if (!held) toast("Select a track, then press +");
    }
}

void App::input() {
    SDL_Event e;
    bool touched = false;
    bool mouseMoved = false;
    float wheel = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running = false;
        // A mouse (a USB one on the console; the PC's in the preview and the live page): it moves the pointer, its wheel
        // scrolls the list under it, its left button presses there (the user, 2026-10-30: "the UI should scroll where the
        // mouse is, app wide"). The mouse events SDL makes up from touches are left to the finger events.
        if ((e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) ||
            ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.which != SDL_TOUCH_MOUSEID)) {
            int ww = css::W, wh = css::H;
            SDL_GetWindowSize(window_, &ww, &wh);
            const float mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) * float(css::W) / float(std::max(1, ww));
            const float my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) * float(css::H) / float(std::max(1, wh));
            pointer.x = std::clamp(mx, 0.f, float(css::W - 1));
            pointer.y = std::clamp(my, 0.f, float(css::H - 1));
            pointer.idle = 0;
            mouseMoved = true;
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { ui.touchDown(pointer.x, pointer.y); mouseHeld_ = true; }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && mouseHeld_) { ui.touchUp(pointer.x, pointer.y); mouseHeld_ = false; }
        }
        if (e.type == SDL_MOUSEWHEEL && e.wheel.which != SDL_TOUCH_MOUSEID) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
            wheel += e.wheel.preciseY;
#else
            wheel += float(e.wheel.y);
#endif
            pointer.idle = 0;
        }
        if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION) touched = true;
        const float x = e.tfinger.x * css::W, y = e.tfinger.y * css::H;
        if (e.type == SDL_FINGERDOWN) ui.touchDown(x, y);
        else if (e.type == SDL_FINGERMOTION) ui.touchMove(x, y);
        else if (e.type == SDL_FINGERUP) ui.touchUp(x, y);
    }

    padUpdate(&pad_);
    u64 down = padGetButtonsDown(&pad_);
    const u64 held = padGetButtons(&pad_);
    // Any input brings the Canvas controls back.
    canvasIdle = (down || touched) ? 0 : canvasIdle + dt;
    // ... and the screen out of its dimming. That first press only wakes it: it does nothing else.
    {
        const HidAnalogStickState l0 = padGetStickPos(&pad_, 0), r0 = padGetStickPos(&pad_, 1);
        const bool any = down || held || touched || std::abs(l0.x) + std::abs(l0.y) > 9000 || std::abs(r0.x) + std::abs(r0.y) > 9000;
        idleFor = any ? 0 : idleFor + dt;
        if (any && dimmed) { setDimmed(false); down = 0; }
    }

    // + opens the track menu for the focused track (or the one playing); HOME closes the app, so + no longer exits.
    const bool notice = coverNotice == 1 || appCoverNotice == 1 || sleepPromptAt > 0 || tourStep >= 0;   // … and the tour   // the overlay covers' modal (drawCoverNotice), the sleep card
    if ((down & HidNpadButton_Plus) && !notice) {
        if (sheet.page != Sheet::Closed) closeSheet();
        else plusMenu(false);
    }
    // A finger held on a row (the user, 2026-10-27): what + opens. Ui focuses the row; the rows say what they hold
    // while they are drawn, and the frame after that publishes it (the …Shown values), so it opens two frames on.
    if (ui.touchHeld()) plusAfterHold = 2;
    else if (plusAfterHold > 0 && --plusAfterHold == 0 && sheet.page == Sheet::Closed && !notice) plusMenu(true);
    const bool modal = sheet.page != Sheet::Closed || notice || je.open || ap.open;
    if (!modal && (down & HidNpadButton_Minus)) openCanvas(!canvas);
    // Left stick: a pointer for what the D-pad reaches badly (the player bar's right end, small
    // buttons). Slow near the centre, fast at the edge; A taps and drags under it like a finger.
    const HidAnalogStickState ls = padGetStickPos(&pad_, 0);
    const float mag = std::hypot(float(ls.x), float(ls.y)) / 32767.f;
    bool pointerMoved = false;
    if (mag > 0.15f) {
        if (pointer.idle > Pointer::Hide) {
            // It appears where the focus is, so the eye does not have to look for it.
            const SDL_FRect r = ui.rectOf(ui.focusedId());
            pointer.x = r.w > 0 ? r.x + r.w / 2 : css::W / 2.f;
            pointer.y = r.w > 0 ? r.y + r.h / 2 : css::H / 2.f;
        }
        // Settings → Controls → Pointer speed scales the whole curve (0.5 is 0.6's 90 + 1250·k² px/s);
        // without acceleration the speed simply follows the stick.
        const float k = std::min(1.f, (mag - 0.15f) / 0.8f);
        const float scale = 0.35f + 1.3f * store.pointerSpeed;
        const float speed = (store.pointerAccel ? 90 + 1250 * k * k : 120 + 700 * k) * scale;   // px/s
        pointer.x = std::clamp(pointer.x + ls.x / 32767.f / mag * speed * dt, 0.f, float(css::W - 1));
        pointer.y = std::clamp(pointer.y - ls.y / 32767.f / mag * speed * dt, 0.f, float(css::H - 1));
        pointer.idle = 0;
        pointerMoved = true;
    } else if (mouseMoved) {
        pointerMoved = true;   // (the mouse moved it: as the stick does)
    } else {
        pointer.idle += dt;
    }
    // the wheel: the list under the pointer, a notch about two rows (the focused list when nothing scrolls there)
    if (wheel != 0 && !ui.scrollAreaAt(pointer.x, pointer.y, -wheel * 110)) ui.scrollFocusedArea(-wheel * 110);
    if (wheel != 0) rehover_ = 0.5f;
    if (touched || (down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right)))
        pointer.idle = Pointer::Hide + 1;   // the D-pad or a finger took over
    const bool pointing = pointer.idle <= Pointer::Hide || pointer.down;
    if (pointing) canvasIdle = 0;
    if (pointerMoved) {
        if (pointer.down || mouseHeld_) ui.touchMove(pointer.x, pointer.y);
        else ui.point(ui.itemAt(pointer.x, pointer.y));
    } else if (rehover_ > 0) {
        // the list moved under it (the wheel, the right stick): the row now under the pointer lights, not the one it left
        rehover_ -= dt;
        if (pointer.idle <= Pointer::Hide && !pointer.down && !mouseHeld_) ui.point(ui.itemAt(pointer.x, pointer.y));
    }
    // A picked-up queue row (the track menu's Move): A or B puts it down, as with a slider; a
    // finger, the pointer or leaving the page put it down too.
    if (grab.pos >= 0) {
        const View gv = grab.kind == Grab::Playlists ? View::Playlists : grab.kind == Grab::Tabs ? View::Settings
                      : grab.kind == Grab::List ? View::List : grab.upNext ? View::Now : View::Queue;
        if ((down & (HidNpadButton_A | HidNpadButton_B)) || touched || pointerMoved || modal || canvas || route.view != gv ||
            (grab.kind == Grab::List && route.ref != grab.ref)) {
            grabDrop();
            down &= ~u64(HidNpadButton_A | HidNpadButton_B);
        }
    }
    if (pointing && (down & HidNpadButton_A)) {
        ui.touchDown(pointer.x, pointer.y);
        ui.setPadMode(true);
        pointer.down = true;
    } else if (pointer.down && !(held & HidNpadButton_A)) {
        ui.touchUp(pointer.x, pointer.y);
        pointer.down = false;
    } else if (down & HidNpadButton_A) {
        ui.activate();
    }
    ui.padHeld(held & HidNpadButton_A);   // a Hold item (the equalizer buttons) acts when A is let go, or on a long press
    // B puts a picked-up slider down before it goes back.
    if (down & HidNpadButton_B)
        ui.anyEngaged() ? ui.disengage() : sleepPromptAt > 0 ? closeSleepPrompt() : tourStep >= 0 ? tourBack() : je.open ? closeJoinEditor() : ap.open ? closeAutomixPicker() : notice ? closeCoverNotice() : modal ? sheetBack() : back();
    if (down & HidNpadButton_X) togglePlay();
    if (!modal && (down & HidNpadButton_Y)) openSearch();
    if (down & HidNpadButton_L) prev();
    if (down & HidNpadButton_R) next(false);
    if (down & HidNpadButton_ZL) seekBy(-10);
    if (down & HidNpadButton_ZR) seekBy(10);

    const bool up = held & HidNpadButton_Up;
    const bool dn = held & HidNpadButton_Down;
    const bool lf = held & HidNpadButton_Left;
    const bool rt = held & HidNpadButton_Right;
    if (grab.pos >= 0) {
        if (rUp.step(up, dt)) grabStep(-grab.cols);
        if (rDown.step(dn, dt)) grabStep(grab.cols);
        if (rLeft.step(lf, dt) && grab.cols > 1) grabStep(-1);
        if (rRight.step(rt, dt) && grab.cols > 1) grabStep(1);
    } else {
        const bool square = ui.focusIsSlider2D();   // a colour square picked up with A: ↑/↓ move it too
        if (rUp.step(up, dt)) square ? ui.slideY(-1) : ui.navigate(0, -1);
        if (rDown.step(dn, dt)) square ? ui.slideY(1) : ui.navigate(0, 1);
    }
    const bool slider = ui.focusIsSlider();
    // Settings: → from the category list goes to the top of its options (spatially the title bar's search
    // field was nearer than the controls at the far right of the rows); B comes back (App::back).
    auto intoSettingsPane = [&] {
        if (route.view != View::Settings || modal) return false;
        const SDL_FRect f = ui.rectOf(ui.focusedId());
        if (f.w <= 0 || f.x < sideW || f.x >= settingsPaneX || f.y < css::TbH || f.y + f.h > css::H - css::BarH) return false;
        const Id first = ui.firstIn(&area("settings.pane"));
        if (!first) return false;
        ui.focus(first);
        ui.setPadMode(true);
        return true;
    };
    if (grab.pos < 0) {
        if (rLeft.step(lf, dt)) slider ? ui.slide(-1) : ui.navigate(-1, 0);
        if (rRight.step(rt, dt)) { if (slider) ui.slide(1); else if (!intoSettingsPane()) ui.navigate(1, 0); }
    }

    // Right stick: the list under the pointer while it shows (the user, 2026-10-30), else whatever list has focus.
    const HidAnalogStickState rs = padGetStickPos(&pad_, 1);
    if (std::abs(rs.y) > 6000) {
        const float dy = -rs.y / 32767.f * 1400 * dt;
        if (pointer.idle <= Pointer::Hide && ui.scrollAreaAt(pointer.x, pointer.y, dy)) rehover_ = 0.5f;
        else ui.scrollFocusedArea(dy);
        ui.setPadMode(true);
        canvasIdle = 0;   // reading the Canvas lyrics is not idle
    }
}

// ============================================================ update

// The dev build's join trace: update's slowest step this frame (updMark)
#ifdef GF_DEV
#define UPD_MARK(step) updMark(step)
#else
#define UPD_MARK(step)
#endif

void App::update() {
#ifdef GF_DEV
    updAt_ = SDL_GetPerformanceCounter();
    updSlowMs_ = 0;
    updSlow_ = "";
#endif
    if (coverWaiting_) { coverWaiting_ = false; applyCover(coverWait_); coverWait_ = TrackLoader::Result{}; UPD_MARK("applyCover"); }
    else if (TrackLoader::Result r; loader.take(r)) { applyLoaded(r); UPD_MARK("applyLoaded"); }
    bgUpdate();
    updateTick();   // Settings → Updates: what the updater's worker finished
    UPD_MARK("bg+updates");
    player.update();
    UPD_MARK("player");
#ifdef GF_DEV
    if (time - traceAt >= 5 && !player.remote()) {   // stutters with Automix (the user, 2026-10-28): what the audio thread saw
        traceAt = time;
        const Player::Trace t = player.takeTrace();
        const unsigned st = t.starved - traceStarved;
        traceStarved = t.starved;
        if (t.late || t.lockMiss || st || t.worstMs > 30) {
            char line[512];
            snprintf(line, sizeof line, "%.0f s\tworst %.1f ms\tlate %u\tlock %u\tdry %u\t%s\t%s\n", time, double(t.worstMs), t.late, t.lockMiss, st,
                     time < mixUntil ? "join" : "-", now.track >= 0 ? lib.tracks[now.track].path.c_str() : "");
            audioTrace += line;
            if (audioTrace.size() > 64 * 1024) audioTrace.erase(0, audioTrace.find('\n', audioTrace.size() - 48 * 1024) + 1);
            store.writeFile("audio-trace.txt", audioTrace);
        }
    }
#endif
    if (player.takeFinished()) next(true);
    UPD_MARK("next");
    if (player.remote() && time - bgListsAt > 5) { bgListsAt = time; writeBgPlaylists(); }
    // Crossfade, in Gatefold's own player: over the last seconds of the track, into the next one.
    if (!player.remote() && store.crossfade > 0 && !automixQueue() && !xfadeChecked && isPlaying() && now.track >= 0) {
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
        const double left = dur - player.position();
        if (dur > 0 && left <= store.crossfade && left > 0.25) {
            xfadeChecked = true;
            // A track shorter than two fades plays whole: the fade takes at most half of it.
            const int p = dur > store.crossfade * 2 ? crossfadeNext(left) : -1;
            if (p >= 0) playAt(p, left);
        }
    }
    UPD_MARK("crossfade");
    // a join's show while the music stops or jumps: paused (the app's clock would run it on in silence), or sought (the
    // player drops the outgoing song): ended cleanly, on every screen (endJoinShow)
    if (time < mixUntil && (!isPlaying() || player.seeks() != mixSeeksSeen)) endJoinShow();
    mixSeeksSeen = player.seeks();
    // the outgoing song let go once the join has given way to the next one (its wipe lasts at least 0.8 s: mixWipe)
    if (mixOld.on && time >= mixFrom + std::max(0.8, mixUntil - mixFrom) + 0.05) mixOld = MixOld{};
    updateAutomix();
    UPD_MARK("updateAutomix");
    automixListsTick();
    moodTick();
    supportTick();
    UPD_MARK("automixLists");

    // Play statistics. Gatefold counts what it plays itself; while the background player plays, it
    // counts (bg-plays.txt) and Gatefold only takes its counts in.
    stats.poll();
    stats.setBackground(player.remote());
    if (stats.loaded() && statsEpoch != libEpoch && !lib.tracks.empty()) {
        statsEpoch = libEpoch;
        std::vector<std::string> paths;
        paths.reserve(lib.tracks.size());
        for (const Track& t : lib.tracks) paths.push_back(t.path);
        stats.wantDates(paths);
    }
    if (!player.remote() && isPlaying() && now.track >= 0 && !statCounted) {
        statHeard += dt;
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
        if (statHeard >= Stats::threshold(dur)) {
            statCounted = true;
            stats.countPlay(lib.tracks[now.track].path, int64_t(std::time(nullptr)));
        }
    }
    UPD_MARK("stats");
    if (stats.dirty() && time - statsSavedAt > 15) saveStats();
    UPD_MARK("saveStats");
    updateSleep();
    sampleBattery();
    updateLevels();
    updateFileWork();
    UPD_MARK("sleep+levels+files");

    // Gatefold crossfades the palette over --tx (1.1 s) with an ease-out curve.
    // During an Automix join, towards the incoming track's palette from wherever it is, arriving as the join ends (the user,
    // 2026-10-28: "the UI background colour should change gradually too"); from where it is, so a cover (and its palette)
    // that arrives a moment late never makes it jump (a blend from the outgoing palette did: "sometimes not gradual")
    if (time >= mixShownAt && time < mixUntil && mixUntil > mixFrom && store.colourTransition >= 0.05f) {
        pal = paletteMix(pal, target, float(std::clamp(dt / std::max(double(dt), mixUntil - time), 0.0, 1.0)));
    } else {
        pal = store.colourTransition < 0.05f ? target : paletteMix(pal, target, 1 - std::exp(-dt / (store.colourTransition / 3.6f)));
    }
    // the covers crossfade over 0.5 s; during an Automix join, over the join itself, eased at both ends (the user, 2026-10-28:
    // "from one to another smoothly with the automix timing"), so a long blend changes the cover slowly and a cut at once
    // (from the moment the join is decided: the next track, and its cover, load 0.6 s before it begins)
    if (time >= mixShownAt && time < mixUntil && mixUntil > mixFrom) {
        const float u = float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
        now.fade = std::max(now.fade, u * u * (3 - 2 * u));
    } else {
        now.fade = std::min(1.f, now.fade + dt / 0.5f);
    }
    {
        const float to = store.sidebarCollapsed ? 0.f : 1.f;
        if (to == 1 && sideP_ == 0) sideOpenedAt_ = time;
        sideP_ = !store.motion ? to : to > sideP_ ? std::min(to, sideP_ + dt / 0.38f) : std::max(to, sideP_ - dt / 0.38f);
        const float e = sideP_ < 0.5f ? 4 * sideP_ * sideP_ * sideP_ : 1 - std::pow(-2 * sideP_ + 2, 3.f) / 2;   // cubic, in and out
        sideW = css::SideWCollapsed + (css::SideW - css::SideWCollapsed) * e;
    }
    toastT = std::max(0.f, toastT - dt);

    // Only a burst that actually arrived is analysed; between bursts the bars glide (spectrum.hpp).
    spec.params = { canvas ? store.canvasBars : store.vizBands, store.vizSensitivity, store.vizSmoothing };
    if (isPlaying() && player.latestPcm(pcm_.data(), Spectrum::N)) spec.feed(pcm_.data(), player.sampleRate());
    spec.step(dt, isPlaying());
    // The cover's bass pulse (Now playing, and the Canvas when asked for), eased so a kick swells rather than pops.
    // (still while Automix joins: the user, 2026-10-28)
    const float wantBeat = store.coverPulse > 0 ? std::min(0.03f, spec.beat * 0.07f) * store.coverPulse * (1 - automixFocus().quiet) : 0;
    coverBeat += (wantBeat - coverBeat) * (1 - std::exp(-dt / (wantBeat > coverBeat ? 0.05f : 0.14f)));

    // Saved as things change rather than on exit, which the HOME menu can skip: the volume once the
    // slider settles, the session on every track change, play/pause, and every 20 s while playing.
    if (std::abs(volumeShown - store.volume) > 0.0005f) { store.volume = volumeShown; persistSoon(0.8f); }
    if (now.track != savedTrack || isPlaying() != savedPlaying) {
        savedTrack = now.track;
        savedPlaying = isPlaying();
        persistSoon(0.5f);
    }
    UPD_MARK("palette+spectrum");
    if (isPlaying() && time - sessionAt > 20) persistNow(true);
    if (persistAt > 0 && time >= persistAt) persistNow();
    UPD_MARK("persist");

    // The overlay's covers: when many are missing, say so once (drawCoverNotice). Looked at once the
    // library has settled a little after launch, and only with the background player connected.
    // It is a small modal: focus goes to it, B or Not now closes it. Not over the Canvas or the track menu.
    // First the app's own covers (the user, 2026-10-27): once every track's tags are in, counted; more than a tenth
    // missing (a first launch, a cleared cache) asks before the sweep runs, else it just goes on. The overlay's after.
    if (appCoverNotice == 0 && !lib.tracks.empty() && meta.taggedCount() >= int(lib.tracks.size()) && time > 3 && !canvas &&
        sheet.page == Sheet::Closed && sleepPromptAt <= 0 && !tourPending && tourStep < 0) {   // never over the tour: after it
        if (!coversHeld) appCoverNotice = 2;   // stopped by you (Settings): nothing to ask
        else if (std::min(meta.coversCached(), int(lib.tracks.size())) * 10 < int(lib.tracks.size()) * 9) {
            appCoverNotice = 1;
            coverMissing = int(lib.tracks.size()) - std::min(meta.coversCached(), int(lib.tracks.size()));
            coverNoticeAt = time;
            noticeFocus_ = ui.focusedId();
        } else {
            appCoverNotice = 2;
            releaseCovers();
        }
    }
    // The tour the welcome card asked for: at once, as soon as the folder's files are listed (seconds); the tags are read
    // during it (the user, 2026-10-28: reading a big library took minutes, and the app was in use by then), and the
    // covers question waits for it to end
    if (tourPending && tourStep < 0 && coverNotice != 1 && appCoverNotice != 1 && sheet.page == Sheet::Closed && sleepPromptAt <= 0 &&
        !lib.tracks.empty()) {
        tourPending = false;
        startTour();
    }
    if (coverNotice == 0 && appCoverNotice == 2 && !tourPending && tourStep < 0 && player.remote() && thumbs.running() && !store.stopThumbs && time > 6 && !lib.tracks.empty() && !canvas && sheet.page == Sheet::Closed) {
        coverMissing = 0;
        for (const auto& t : lib.tracks) coverMissing += !thumbs.has(t.path);
        coverNotice = coverMissing * 10 > int(lib.tracks.size()) ? 1 : 2;
        coverNoticeAt = time;
        if (coverNotice == 1) noticeFocus_ = ui.focusedId();
    }
    if (coverNotice == 1 && !player.remote()) closeCoverNotice();

    // The sidebar's progress cards: each job that has run for a second shows, and fades out once it is done (or stopped).
    {
        Job jobs[JobSlots];
        bool busy[JobSlots] = {};
        backgroundJobs(jobs, busy);
        for (int i = 0; i < JobSlots; i++) {
            JobCard& c = jobCards[i];
            c.since = busy[i] ? c.since + dt : 0;
            if (busy[i]) c.job = jobs[i];
            const float want = busy[i] && c.since > 1 ? 1.f : 0.f;
            c.shown += (want - c.shown) * (1 - std::exp(-dt / 0.15f));
            if (c.shown < 0.005f && want == 0) c.shown = 0;
        }
    }

    // The launch check of the music folder: a rescan only if the card's files are not the remembered ones.
    if (libCheckDone_) {
        libCheckDone_ = false;
        libCheck_.join();
        if (libChecked_ != libFiles_) {
            const std::vector<std::string> files = std::move(libChecked_);
            rescan(&files);
            toast("Library updated: " + std::to_string(lib.tracks.size()) + " tracks");
        }
        libChecked_.clear();
    }

    UPD_MARK("notices+jobs+libcheck");
    if (meta.pump(renderer_)) indexDirty = true;
    UPD_MARK("meta.pump");
    if (indexDirty && time - indexAt > 1.5) rebuildIndex();
    UPD_MARK("rebuildIndex");
}

void App::rebuildIndex() {
    indexDirty = false;
    indexGen++;
    indexAt = time;
    pathIndex.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) pathIndex[lib.tracks[i].path] = i;

    // Albums come from tags alone (album + album artist, else the track's artist), whatever the folders look like.
    // Tracks without an album tag are not in any album; they stay reachable through Artists, Folders and search.
    lib.albums.clear();
    albumByKey.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        Track& t = lib.tracks[i];
        t.albumIdx = -1;
        if (t.album.empty()) continue;
        const std::string credit = t.albumArtist.empty() ? t.artist : t.albumArtist;
        const std::string key = lower(t.album) + "|" + lower(credit);
        auto it = albumByKey.find(key);
        if (it == albumByKey.end()) {
            it = albumByKey.emplace(key, int(lib.albums.size())).first;
            lib.albums.push_back({ key, t.album, credit, {}, {} });
        }
        t.albumIdx = it->second;
        Album& a = lib.albums[it->second];
        a.tracks.push_back(i);
        if (a.year.empty() && t.date.size() >= 4) a.year = t.date.substr(0, 4);
    }
    for (Album& a : lib.albums)
        std::stable_sort(a.tracks.begin(), a.tracks.end(), [&](int x, int y) {
            const Track &p = lib.tracks[x], &q2 = lib.tracks[y];
            if (p.trackNo != q2.trackNo) return (p.trackNo ? p.trackNo : 9999) < (q2.trackNo ? q2.trackNo : 9999);
            return iless(p.path, q2.path);
        });

    albumOrder.resize(lib.albums.size());
    for (int i = 0; i < int(albumOrder.size()); i++) albumOrder[i] = i;
    auto& A = lib.albums;
    std::stable_sort(albumOrder.begin(), albumOrder.end(), [&](int a, int b) {
        switch (librarySort) {
        case 1: return iless(A[a].title, A[b].title);
        case 2: return A[a].year != A[b].year ? A[a].year > A[b].year : iless(A[a].title, A[b].title);
        case 3: return iless(lib.tracks[A[a].tracks[0]].path, lib.tracks[A[b].tracks[0]].path);
        default:
            if (strcasecmp(A[a].artist.c_str(), A[b].artist.c_str()) != 0) return iless(A[a].artist, A[b].artist);
            return A[a].year != A[b].year ? A[a].year < A[b].year : iless(A[a].title, A[b].title);
        }
    });

    // Artists: everyone credited on a track, with their tracks and the albums credited to them.
    artists.clear();
    artistByKey.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const std::string name = lib.tracks[i].artist.empty() ? "Unknown artist" : lib.tracks[i].artist;
        auto it = artistByKey.find(lower(name));
        if (it == artistByKey.end()) {
            it = artistByKey.emplace(lower(name), int(artists.size())).first;
            artists.push_back({ name, {}, {} });
        }
        artists[it->second].tracks.push_back(i);
    }
    for (int i = 0; i < int(A.size()); i++) {
        auto it = artistByKey.find(lower(A[i].artist));
        if (it != artistByKey.end()) artists[it->second].albums.push_back(i);
    }
    std::sort(artists.begin(), artists.end(), [](const Artist& a, const Artist& b) { return iless(a.name, b.name); });
    artistByKey.clear();
    for (int i = 0; i < int(artists.size()); i++) {
        artistByKey[lower(artists[i].name)] = i;
        auto& al = artists[i].albums;
        std::sort(al.begin(), al.end(), [&](int a, int b) { return A[a].year != A[b].year ? A[a].year < A[b].year : iless(A[a].title, A[b].title); });
    }

    // Genres: from the GENRE tag, several per track when separated by ";". An album is under every genre its tracks have.
    genres.clear();
    genreByKey.clear();
    std::vector<std::vector<int>> albumGenres(A.size());
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const std::string& gs = lib.tracks[i].genre;
        size_t a = 0;
        while (a < gs.size()) {
            size_t b = gs.find(';', a);
            if (b == std::string::npos) b = gs.size();
            std::string name = gs.substr(a, b - a);
            a = b + 1;
            while (!name.empty() && name.front() == ' ') name.erase(0, 1);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) continue;
            auto it = genreByKey.find(lower(name));
            if (it == genreByKey.end()) {
                it = genreByKey.emplace(lower(name), int(genres.size())).first;
                genres.push_back({ name, {}, {} });
            }
            auto& gt = genres[it->second].tracks;
            if (gt.empty() || gt.back() != i) gt.push_back(i);
            const int al = lib.tracks[i].albumIdx;
            if (al >= 0 && std::find(albumGenres[al].begin(), albumGenres[al].end(), it->second) == albumGenres[al].end())
                albumGenres[al].push_back(it->second);
        }
    }
    for (int al = 0; al < int(A.size()); al++)
        for (int gi : albumGenres[al]) genres[gi].albums.push_back(al);
    std::sort(genres.begin(), genres.end(), [](const Artist& a, const Artist& b) { return iless(a.name, b.name); });
    genreByKey.clear();
    for (int i = 0; i < int(genres.size()); i++) {
        genreByKey[lower(genres[i].name)] = i;
        auto& al = genres[i].albums;
        std::sort(al.begin(), al.end(), [&](int a, int b) { return iless(A[a].artist, A[b].artist) || (!iless(A[b].artist, A[a].artist) && A[a].year < A[b].year); });
    }
}

int App::routeGenre() const {
    auto it = genreByKey.find(lower(route.query));
    return it == genreByKey.end() ? -1 : it->second;
}

/** Before a page that belongs to a sidebar tab (an album, an artist, a genre): its own tab when shown, else Library
 *  (which lights for it), else nowhere to show it from: a toast says which tab to show. */
bool App::tabFor(int tab, const char* page) {
    if (!(store.hiddenTabs & Store::tabBit(tab))) {
        if (tab == Store::TabAlbums) libFrom = View::Albums;
        if (tab == Store::TabFolders) folderFrom = View::Folders;
        return true;
    }
    if (!(store.hiddenTabs & Store::tabBit(Store::TabLibrary))) {
        if (tab == Store::TabAlbums) libFrom = View::Library;
        if (tab == Store::TabFolders) folderFrom = View::Library;
        return true;
    }
    toast(std::string("To see the ") + page + ", show " + Store::tabName(tab) + " or Library in Settings › Appearance › Sidebar");
    return false;
}

void App::openGenre(const std::string& name) {
    if (!name.empty() && tabFor(Store::TabGenres, "genre")) navigate(View::Genre, -1, name);
}

int App::routeAlbum() const {
    auto it = albumByKey.find(route.query);
    return it == albumByKey.end() ? -1 : it->second;
}

int App::routeArtist() const {
    auto it = artistByKey.find(lower(route.query));
    return it == artistByKey.end() ? -1 : it->second;
}

void App::openAlbum(int a) {
    if (a >= 0 && a < int(lib.albums.size())) navigate(View::Album, -1, lib.albums[a].key);
}

/** An album from outside its grids (Now playing, the track menu, Files): the tab it lights follows tabFor. */
void App::goToAlbum(int a) {
    if (a >= 0 && tabFor(Store::TabAlbums, "album")) openAlbum(a);
}

void App::openArtist(const std::string& name) {
    if (tabFor(Store::TabArtists, "artist")) navigate(View::Artist, -1, name.empty() ? "Unknown artist" : name);
}

// ============================================================ play queue

bool App::isPlaying() const { return player.loaded() && !player.paused(); }

int App::currentQueuePos() const { return q.pos >= 0 && q.pos < int(q.order.size()) ? q.order[q.pos] : -1; }

void App::playTracks(const std::vector<int>& tracks, int startPos, const std::string& name, bool shuffle) {
    if (tracks.empty()) return;
    grabDrop();   // a new queue: the row held belonged to the old one
    q.tracks = tracks;
    q.name = name;
    q.failed = 0;
    startPos = std::clamp(startPos, 0, int(tracks.size()) - 1);
    if (shuffle && !automixQueue()) store.order = Order::Shuffle;
    holdOrderForAutomix();
    q.order.resize(tracks.size());
    for (int i = 0; i < int(tracks.size()); i++) q.order[i] = i;
    if (store.order == Order::Shuffle) {
        std::shuffle(q.order.begin(), q.order.end(), rng);
        if (!shuffle) std::iter_swap(q.order.begin(), std::find(q.order.begin(), q.order.end(), startPos));
        playAt(0);
    } else {
        playAt(startPos);
    }
}

void App::playAt(int orderPos, double fade, const automix::Plan* mix) {
    if (q.order.empty()) return;
    q.pos = std::clamp(orderPos, 0, int(q.order.size()) - 1);
    loadTrack(q.tracks[q.order[q.pos]], false, true, fade, mix);
    // The background player skips an unplayable file itself and says so (errorGen, bgUpdate); its polled
    // state is a frame old here, and on a fresh install it still said "no queue", which skipped every track.
    if (player.remote()) {
        q.failed = 0;
    } else if (!player.loaded()) {
        // Unplayable file: say so, skip it, but never loop forever over a list of broken files.
        toast("Can't play “" + baseName(lib.tracks[q.tracks[q.order[q.pos]]].path) + "”");
        if (++q.failed < int(q.order.size())) next(false);
        else toast("None of these tracks could be played");
    } else {
        q.failed = 0;
    }
}

void App::next(bool natural) {
    if (q.order.empty()) return;
    if (natural && store.order == Order::RepeatOne) return playAt(q.pos);
    if (natural && stopAfterCurrent) {
        stopAfterCurrent = false;
        player.stop();
        return;
    }
    int p = q.pos + 1;
    if (p >= int(q.order.size())) {
        if (store.order == Order::Default && natural) {
            player.stop();
            return;
        }
        if (store.order == Order::Shuffle) { grabDrop(); std::shuffle(q.order.begin(), q.order.end(), rng); }
        p = 0;
    }
    playAt(p);
}

/**
 * The rules of next(true), without doing anything: none at Repeat track, Stop after this track, the end of
 * the list, a reshuffle at the end, a sleep timer ending first, and (by default) between consecutive tracks
 * of one album, which keep their gapless join.
 */
#ifdef GF_DEV
/** The dev build's join trace: each Automix join's frame times, a line in audio-trace.txt when it ends. */
void App::traceJoinFrame(float frameMs, float workMs) {
    const bool joining = !player.remote() && now.track >= 0 && mixUntil > mixShownAt && time >= mixShownAt && time < mixUntil;
    if (joining) {
        joinFrames_++;
        joinSum_ += frameMs;
        joinWorst_ = std::max(joinWorst_, double(frameMs));
        joinWorkSum_ += workMs;
        joinWorkWorst_ = std::max(joinWorkWorst_, double(workMs));
        if (frameMs > 17.5f) joinLate_++;
        if (frameMs >= 25) joinDrops_++;
        if (workMs >= joinWorkWorst_) {   // what the worst frame did: when, its sections, update's slowest step
            char why[256];
            snprintf(why, sizeof why, "at %.1f s: upd %.1f (%s %.1f) bg %.1f view %.1f side %.1f bar %.1f rest %.1f warm %.1f", time - mixShownAt,
                     profFrame_[0], updSlow_, updSlowMs_, profFrame_[1], profFrame_[2], profFrame_[3], profFrame_[4], profFrame_[5], profFrame_[6]);
            joinWhy_ = why;
        }
        return;
    }
    if (joinFrames_ == 0) return;
    char line[512];
    snprintf(line, sizeof line, "%.0f s\tjoin %.1f s\t%d frames\tframe avg %.1f worst %.1f ms\twork avg %.1f worst %.1f ms\tover 17 ms: %d\tdropped (25 ms+): %d\tworst work %s\t%s\n", time,
             mixUntil - mixShownAt, joinFrames_, joinSum_ / joinFrames_, joinWorst_, joinWorkSum_ / joinFrames_, joinWorkWorst_, joinLate_,
             joinDrops_, joinWhy_.c_str(), now.track >= 0 ? lib.tracks[now.track].path.c_str() : "");
    audioTrace += line;
    store.writeFile("audio-trace.txt", audioTrace);
    joinFrames_ = joinLate_ = joinDrops_ = 0;
    joinSum_ = joinWorst_ = joinWorkSum_ = joinWorkWorst_ = 0;
}
#endif

void App::endJoinShow(double over) {
    mixAnnounced = false;   // (announced, not joined yet: no join now; a pause gets a new show when it plays on)
    if (time >= mixUntil || mixUntil - time <= over) return;   // over, or ending as soon on its own
    // the swipes and the lap go on from where they are, (time - mixFrom) / (mixUntil - mixFrom) unchanged now, and reach
    // the end with the hint, the frost and the glide (their 0.9 s eases out from mixUntil)
    const double p = mixUntil > mixFrom && time > mixFrom ? std::min((time - mixFrom) / (mixUntil - mixFrom), 0.999) : 0.0;
    mixUntil = time + over;
    mixFrom = mixUntil - over / (1 - p);
}

/**
 * Automix (Gatefold's own player): from ten seconds into a track, each frame, the join to the next one is
 * planned from both analyses (automix.hpp; the rules of crossfadeNext still decide whether there is one),
 * and started 0.6 s before its moment: the file is opened while this one plays, and the player starts it on
 * its exact sample. A track not analysed yet gets a plain fade.
 */
void App::updateAutomix() {
    if (player.remote() || !automixQueue() || xfadeChecked || !isPlaying() || now.track < 0) return;
    const Level* la = levels.find(lib.tracks[now.track].path);
    double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
    if (dur <= 0 && la && la->mix.seconds > 0) dur = la->mix.seconds;   // no length in its tags: the analysis measured it
    const double pos = player.position(), left = dur - pos;
    if (dur <= 0 || pos < 10 || left <= 0.25) return;
    const int p = crossfadeNext(left);
    if (p < 0) return;   // looked at again next frame: the queue, the order or Stop after may change
    const Level* lb = levels.find(lib.tracks[q.tracks[q.order[p]]].path);
    // Auto, always (the user, 2026-10-29: "all in on Auto" for 2.0; the join editor is out of reach). Joins edited before
    // stay in mixes.tsv, unused, for a later version that edits them again
    const bool edited = false;
    const automix::Plan pl = planJoin(la ? &la->mix : nullptr, dur, lb ? &lb->mix : nullptr, store.crossfade, nullptr);
    if (pl.off || dur <= pl.length * 2) { xfadeChecked = true; return; }   // Off, or a track shorter than two fades: it plays whole
    if (pl.speed != 1 && !stretchBegun && pos >= pl.stretchFrom) {   // towards the next track's tempo, a few beats before
        stretchBegun = true;
        player.beginStretch(pl);
    }
    if (!mixPrepared && pos >= pl.startOut - 4) {   // opened and moved to its offset on a worker, ahead of the join
        mixPrepared = true;
        player.prepare(lib.tracks[q.tracks[q.order[p]]].path, pl.offset);
    }
    // Shown 0.4 s before the next track is loaded (the stage's fade-out: automixFocus): this track's title and lyrics leave
    // before the next one's take their place (they changed as they began to fade: the user, 2026-10-29, the Canvas)
    if (pos < pl.startOut - 1.0) {
        if (mixAnnounced) endJoinShow();   // sought back: not yet
        return;
    }
    if (!mixAnnounced || time >= mixUntil) {   // (again after a long pause in between: it had run out)
        mixAnnounced = true;
        mixLabel = "Automix";
        if (edited && pl.join.preset != automix::Preset::Auto) mixLabel += std::string(" · ") + automix::name(pl.join.preset);
        if (pl.beatmatched) mixLabel += " · " + std::to_string(int(std::lround(pl.bpm))) + " BPM";
        mixUntil = time + std::max(0.0, pl.startOut - pos) + pl.length;
        mixShownAt = time;
        mixFrom = time + std::max(0.0, pl.startOut - pos);
        mixShowLen = mixUntil - mixShownAt;
    }
    if (pos < pl.startOut - 0.6) return;
    // the next track still being opened and moved to its offset (the position landed in the join's last seconds at once, a
    // seek: prepare, the stage and the join came in one frame): the join waits for the worker, a frame at a time, the
    // outgoing song playing on, rather than the frame waiting for it in load() (228-640 ms on the console's trace, the
    // join's first frame, 2026-10-29); it starts as soon as it can (late: at once)
    if (player.preparing()) return;
    xfadeChecked = true;
    mixAnnounced = false;   // (loadTrack: a load other than this join's ends the stage)
    keepMixOld();
#ifndef __SWITCH__
    SDL_Log("automix: at %.2f s, next from %.2f s, %.2f s%s, %s: %s, %s, %s, %s, loop %d", pl.startOut, pl.offset, pl.length,
            pl.beatmatched ? " on the beat" : "", automix::name(pl.join.preset), automix::name(pl.join.volume), automix::name(pl.join.eq),
            automix::name(pl.join.filter), automix::name(pl.join.effect), pl.join.loopBars);   // the PC preview checks it with this
    if (pl.speed != 1) SDL_Log("automix: this track at %+.2f %% from %.1f s, to its end", (pl.speed - 1) * 100, pl.stretchFrom);
#endif
    playAt(p, pl.length, &pl);
}

/** The outgoing song, kept through a join (MixOld: Simple and Immersive give its title, lyrics and the rest way to the next). */
void App::keepMixOld() {
    mixOld.on = true;
    mixOld.info = now.info;
    mixOld.lyrics = now.lyrics;
    mixOld.synced = now.synced;
    mixOld.lyricsSource = now.lyricsSource;
    mixOld.upNext = upNext(15);
    mixOld.wave = wave.ready() && wave.path() == lib.tracks[now.track].path ? wave.peaks() : std::vector<float>{};
    mixOld.lyricsAt = lyricsPos();
    mixOld.at = time;
    mixOld.track = now.track;
    mixOld.posAt = player.position();
    mixOld.coverIn = false;
    mixOld.palette = target;
}

/** A join's show announced, its times from the background player's (protocol 6): as updateAutomix's, 1 s before it. */
void App::announceBgJoin(const BgState& s) {
    mixAnnounced = true;
    mixLabel = "Automix";
    if (s.joinBpm > 0) mixLabel += " · " + std::to_string(int(std::lround(s.joinBpm))) + " BPM";
    mixShownAt = time;
    mixFrom = time - s.joinAt;
    mixUntil = mixFrom + s.joinLength;
    mixShowLen = mixUntil - mixShownAt;
}

int App::crossfadeNext(double left) const {
    if (q.order.empty() || store.order == Order::RepeatOne || stopAfterCurrent) return -1;
    const double sl = sleepLeft();
    if (sl > 0 && sl <= left + store.crossfade) return -1;
    const int p = q.pos + 1;
    if (p >= int(q.order.size())) return -1;   // the end, or a new round (and a reshuffle): as before
    // an album's own joins kept; not in an Automix playlist, made to be mixed (the user, 2026-10-28: one album's songs in one
    // were never joined)
    if (store.xfadeNotInAlbum && !automixQueue()) {
        const int a = lib.tracks[now.track].albumIdx, b = lib.tracks[q.tracks[q.order[p]]].albumIdx;
        if (a >= 0 && a == b) return -1;
    }
    return p;
}

bool App::bgShown() const {
#ifndef __SWITCH__
    static const bool fake = getenv("GF_BGCFG") != nullptr;   // the preview has no background player: its settings, to look at
    if (fake) return true;
#endif
    return player.remote() && bgs.cfgRead;
}

uint64_t App::listHash(const std::string& name) {
    uint64_t h = 0xcbf29ce484222325ULL;   // gfbgHash (FNV-1a 64), as the player hashes the "#name" lines
    for (unsigned char c : name) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

std::string App::listName(uint64_t hash) const {
    if (!hash) return {};
    if (hash == listHash("Liked")) return "Liked";
    for (const auto& p : store.playlists) if (listHash(p.name) == hash) return p.name;
    return {};
}

/**
 * GFBG_PLAYLISTS: Liked, then every playlist in the sidebar (yours, folder ones, smart ones as they stand now) as
 * "#name" and its tracks' paths. The background player reads the one a game event asks for; the overlay the names.
 * Protocol 6, where the readers before it look past them: first "!kinds" and a letter per playlist (GFBG_KIND_*: Liked,
 * yours, a folder's, smart, mood, Automix), "!icons" and each one's icon key (plicons.inc's, as the sidebar shows it):
 * the overlay's Playlists pages sort and mark them by these, and the player plays an Automix one in its order, joined;
 * after the last playlist a lone "#" and a "!t path<TAB>title<TAB>artist" line per track listed, so the overlay names a
 * playlist's tracks as its folders' (title by artist) without opening a file.
 */
void App::writeBgPlaylists() {
    std::string out;
    out.reserve(64 * 1024);
    out += "!kinds L";
    for (const auto& pl : store.playlists)
        out += pl.own() ? (pl.automix ? 'A' : 'N') : pl.smart() ? (isMoodPlaylist(pl) ? 'M' : 'S') : 'F';
    out += "\n!icons ";
    for (int i = -1; i < int(store.playlists.size()); i++) {   // (-1: Liked)
        const char* svg = playlistIcon(i);
        const char* key = "list";
        for (const PlaylistIcon& k : kPlaylistIcons) if (k.svg == svg) { key = k.key; break; }
        if (i >= 0) out += ',';
        out += key;
    }
    out += '\n';
    std::vector<int> listed;   // the tracks named in the "!t" lines
    std::vector<char> seen(lib.tracks.size(), 0);
    auto add = [&](const std::string& path, int idx) {
        out += path;
        out += '\n';
        if (idx < 0) {
            const auto it = pathIndex.find(path);
            idx = it == pathIndex.end() ? -1 : it->second;
        }
        if (idx >= 0 && idx < int(seen.size()) && !seen[size_t(idx)]) { seen[size_t(idx)] = 1; listed.push_back(idx); }
    };
    out += "#Liked\n";
    for (const auto& p : store.liked) add(p, -1);
    for (const auto& pl : store.playlists) {
        out += '#';
        out += pl.name;
        out += '\n';
        if (pl.own()) {
            for (const auto& p : pl.tracks) add(p, -1);
        } else {
            const std::vector<int> t = pl.smart() ? smartTracks(pl.path) : lib.tracksUnder(lib.folderByPath(pl.path));
            for (int i : t) add(lib.tracks[size_t(i)].path, i);
        }
    }
    out += "#\n";
    for (int i : listed) {
        const Track& t = lib.tracks[size_t(i)];
        out += "!t ";
        out += t.path;
        out += '\t';
        out += t.title;
        out += '\t';
        out += t.artist;
        out += '\n';
    }
    const uint64_t sig = listHash(out);
    if (sig == bgListsSig) return;
    bgListsSig = sig;
    store.writeFile("bg-playlists.txt", std::move(out));
}

void App::setEq(const GfbgEq& e) {
    store.eq = e;
    gfEqClamp(&store.eq);
    player.setEq(store.eq);
    eqDirty = true;
    persistSoon(1.0f);
}

void App::prev() {
    if (q.order.empty()) return;
    if (player.position() > 3) { player.seek(0); return; }   // first restart, then go back
    playAt(q.pos > 0 ? q.pos - 1 : int(q.order.size()) - 1);
}

void App::togglePlay() {
    if (!player.loaded()) {
        if (q.pos >= 0) playAt(q.pos);
        else if (!lib.tracks.empty()) playTracks(listTracks(View::Library, -1, {}), 0, "Library");
        return;
    }
    player.togglePause();
}

void App::seekBy(double d) {
    if (!player.loaded()) return;
    double p = player.position() + d;
    if (now.info.duration > 0) p = std::min(p, now.info.duration - 1);
    player.seek(std::max(0.0, p));
}

// ------------------------------------------------------------ volume levelling

namespace {

/** gfbgHash (FNV-1a 64) of a path: gains.bin's key, as the background player computes it. */
uint64_t pathHash(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

/** The gain to the target, limited so the true peak stays under −1 dBTP, in dB. */
double gainTo(double lufs, double peak, int target) {
    double g = target - lufs;
    if (peak > 0) g = std::min(g, -1.0 - 20 * std::log10(peak));
    return std::clamp(g, -24.0, 12.0);
}

}  // namespace

double App::levelDb(int track, int mode) const {
    if (mode < 0) mode = store.levelMode;
    if (mode == 0 || track < 0 || track >= int(lib.tracks.size())) return 0;
    const Level* l = levels.find(lib.tracks[track].path);
    if (!l || std::isnan(l->lufs)) return 0;
    // Auto: the album's gain, but the track's while shuffling (foobar2000's "by playback order").
    const bool album = mode == 2 || (mode == 3 && store.order != Order::Shuffle);
    const int a = lib.tracks[track].albumIdx;
    if (!album || a < 0) return gainTo(l->lufs, l->peak, store.levelTarget);
    // The album: the power mean of its measured tracks, weighted by their gated blocks; its loudest peak.
    double p = 0, n = 0, peak = 0;
    for (int t : lib.albums[a].tracks) {
        const Level* m = levels.find(lib.tracks[t].path);
        if (!m || std::isnan(m->lufs) || m->blocks <= 0) continue;
        p += m->blocks * std::pow(10, (m->lufs + 0.691) / 10);
        n += m->blocks;
        peak = std::max(peak, double(m->peak));
    }
    if (n <= 0) return gainTo(l->lufs, l->peak, store.levelTarget);
    return gainTo(-0.691 + 10 * std::log10(p / n), peak, store.levelTarget);
}

/** gains.bin for the background player (gfbg.h, GFBG_GAINS): every measured track's track and album gain. */
void App::writeGains() {
    gainsDirty = false;
    gainsWrittenAt = time;
    gainsHave.clear();
    struct Entry { uint64_t hash; float track, album; };   // GfbgGain
    std::vector<Entry> out;
    out.reserve(lib.tracks.size());
    for (int t = 0; t < int(lib.tracks.size()); t++) {
        const Level* l = levels.find(lib.tracks[t].path);
        if (!l || std::isnan(l->lufs)) continue;
        out.push_back({ pathHash(lib.tracks[t].path), float(levelDb(t, 1)), float(levelDb(t, 2)) });
        gainsHave.insert(lib.tracks[t].path);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
    // A new generation each time, also across restarts: the player reloads when it differs.
    gainsGen = std::max(gainsGen + 1, unsigned(std::time(nullptr)));
    const uint32_t header[4] = { 0x564C4647u, uint32_t(out.size()), gainsGen, 0 };   // GfbgGainsHeader, "GFLV"
    std::string data(reinterpret_cast<const char*>(header), sizeof header);
    data.append(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(Entry));
    store.writeFile("gains.bin", std::move(data));
}

/** Every frame: what to measure next, the gains file, and the gain here. */
void App::updateLevels() {
    const bool changed = levels.poll();
    const std::vector<std::string> amWaiting = automixWaitingTracks(32);   // an Automix playlist's tracks not analysed yet
    // Mood playlists: every track's mood, so the whole library is analysed (in the same decode as the rest: levels.hpp)
    bool moods = false;
    for (const auto& p : store.playlists) if (isMoodPlaylist(p)) { moods = true; break; }
    levels.wantMood(moods);
    if (store.levelMode == 0 && !automixQueue() && amWaiting.empty() && !moods) {   // the worker measures for levelling and analyses Automix and Mood playlists
        if (!levelFirst.empty() || !levelAll.empty()) {
            levelFirst.clear();
            levelAll.clear();
            levelEpoch = ~0u;
            levels.want({}, {});
        }
    } else {
        // First the track playing and the next three, then the whole library in its order.
        std::vector<std::string> first;
        if (now.track >= 0) first.push_back(lib.tracks[now.track].path);
        for (int t : upNext(3)) first.push_back(lib.tracks[t].path);
        first.insert(first.end(), amWaiting.begin(), amWaiting.end());
        const bool libChanged = levelEpoch != libEpoch && !lib.tracks.empty();
        if (libChanged) {
            levelEpoch = libEpoch;
            levelAll.clear();
            levelAll.reserve(lib.tracks.size());
            for (const Track& t : lib.tracks) levelAll.push_back(t.path);
        }
        // levelling off: only what Automix needs (the queue's next tracks, Automix playlists' tracks), not the whole library
        // (it was decoded track after track while an Automix playlist played: the SD card and core 1 busy under the audio)
        const bool library = store.levelMode != 0 || moods;
        if (libChanged || first != levelFirst || library != levelAllSent) {
            levelFirst = first;
            levelAllSent = library;
            levels.want(first, library ? levelAll : std::vector<std::string>{});
        }
    }
    // The gains: new measurements or a new target. Soon when a track asked for first has its value now
    // (it is about to play, or plays), else at most every 20 s while the library is measured.
    if (changed || store.levelTarget != levelTargetUsed) { levelTargetUsed = store.levelTarget; gainsDirty = true; }
    if (gainsDirty && levels.loaded()) {
        bool soon = false;
        for (const auto& p : levelFirst)
            if (levels.find(p) && !gainsHave.count(p)) soon = true;
        if ((soon && time - gainsWrittenAt > 1) || time - gainsWrittenAt > 20) writeGains();
    }
    if (levels.dirty() && time - levelsSavedAt > 30) {
        levelsSavedAt = time;
        store.writeFile("loudness.tsv", levels.serialise());
        store.writeFile("mix.tsv", levels.serialiseMix());
        store.writeFile("mood.tsv", levels.serialiseMood());
    }
    if (player.remote()) {
        if (store.levelMode != levelModeSent) { bg.setLevelMode(store.levelMode); levelModeSent = store.levelMode; }
    } else {
        player.setLevel(float(std::pow(10, levelDb(now.track) / 20)));   // eased: a value measured mid-track comes in gently
    }
}

// ------------------------------------------------------------ the sleep timer

double App::sleepLeft() const {
    if (player.remote()) return bg.state().sleepLeft;
    return sleepUntil > 0 ? std::max(0.0, sleepUntil - time) : 0;
}

/** seconds 0: off. choice: what Settings' control shows (0 off, 1… the minutes in order). */
void App::setSleep(double seconds, int choice) {
    sleepChoice = seconds > 0 ? choice : 0;
    if (sleepFading) player.setVolume(volumeShown);   // turned off or moved during the fade: back up
    sleepFading = false;
    sleepUntil = seconds > 0 ? time + seconds : 0;
    if (player.remote()) {
        bg.setSleep(float(seconds));
        sleepUntil = 0;   // the background player keeps it: it carries on with Gatefold closed
    }
}

/** Every frame. Gatefold's own timer fades the volume over the last 10 s and pauses; the background player's is only watched. */
void App::updateSleep() {
    if (player.remote()) {
        const BgState& s = bg.state();
        // Seen as it happened when the timer was seen running a moment ago (the player reports 0 left for up
        // to a quarter second before it bumps sleepGen, so not "last frame"): an end that came while Gatefold
        // was in the background is not followed by the sleep card. A steady clock: App::time stops while suspended.
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s.sleepLeft > 0) sleepSeenAt = wall;
        if (s.sleepGen != sleepGenSeen) { sleepGenSeen = s.sleepGen; sleepEnded(sleepSeenAt > 0 && wall - sleepSeenAt < 5); }
    } else if (sleepUntil > 0) {
        const double left = sleepUntil - time;
        if (left <= 0) {
            sleepUntil = 0;
            if (isPlaying()) player.togglePause();
            if (sleepFading) player.setVolume(volumeShown);
            sleepFading = false;
            sleepEnded(true);
        } else if (left <= 10 && isPlaying()) {
            sleepFading = true;
            player.setVolume(volumeShown * float(left / 10));
        }
    }
    // Settings → Playback → Keep the console awake: no dimming or auto-sleep while Gatefold is open, until
    // a sleep timer ends; then the console may sleep by itself again, until music plays once more.
    if (isPlaying()) sleepReleased = false;
    const bool want = store.keepAwake && !sleepReleased;
    if (want != awakeHeld) {
        // (1.1 called appletSetAutoSleepTimeAndDimmingTimeEnabled, which only an overlay applet may: from Gatefold it did
        // nothing, so the console dimmed by itself whenever the music was paused.) An application's own ways: the idle
        // time stretched as far as it goes, and auto-sleep off.
        awakeHeld = want;
        appletSetIdleTimeDetectionExtension(want ? AppletIdleTimeDetectionExtension_ExtendedUnsafe : AppletIdleTimeDetectionExtension_None);
        appletSetAutoSleepDisabled(want);
    }
    // "Media playing" keeps the screen from dimming and the console from sleeping: while music plays, and all the time
    // while Keep awake holds (Gatefold's own dimming, below, then decides). Set from here alone, once per change.
    if (const int media = (isPlaying() || want) ? 1 : 0; media != mediaState) {
        mediaState = media;
        appletSetMediaPlaybackState(media != 0);
    }
    // ... and Dim the screen, after store.dimAfter minutes without input (only then: else the system dims).
    const bool dim = want && store.dimAfter > 0 && idleFor >= store.dimAfter * 60.0 && sleepPromptAt == 0;
    if (dim != (dimmed != 0)) setDimmed(dim);
    veilA += ((dimmed == 2 ? 0.78f : 0.f) - veilA) * (1 - std::exp(-dt / 0.35f));

    // The sleep card's count: at 0 the console goes to sleep.
    if (sleepPromptAt > 0 && time - sleepPromptAt >= store.sleepConsole) {
        closeSleepPrompt();
        appletRequestToSleep();
    }
}

void App::sleepEnded(bool seen) {
    sleepChoice = 0;
    sleepReleased = true;
    // Then the console sleeps, after a card that lets you stay awake; only Gatefold in front can ask for that.
    if (seen && store.sleepConsole > 0 && appletGetFocusState() == AppletFocusState_InFocus) {
        if (sheet.page != Sheet::Closed) closeSheet();
        sleepPromptAt = time;
        idleFor = 0;
        if (dimmed) setDimmed(false);
        ui.setPadMode(true);
        return;
    }
    toast("Sleep timer: the music is paused");
}

void App::closeSleepPrompt() {
    sleepPromptAt = 0;
    ui.focusDefault();
}

/** Handheld: the backlight goes down, and back up to what it was; docked: a veil (veilA), as the TV is not ours. */
void App::setDimmed(bool on) {
    if (on && !dimmed) {
        if (appletGetOperationMode() == AppletOperationMode_Handheld && R_SUCCEEDED(lblGetCurrentBrightnessSetting(&dimSaved)) &&
            R_SUCCEEDED(lblSetCurrentBrightnessSetting(std::min(dimSaved, 0.04f)))) {
            lblApplyCurrentBrightnessSettingToBacklight();
            dimmed = 1;
        } else {
            dimmed = 2;
        }
    } else if (!on && dimmed) {
        if (dimmed == 1) {
            lblSetCurrentBrightnessSetting(dimSaved);
            lblApplyCurrentBrightnessSettingToBacklight();
        }
        dimmed = 0;
    }
}

/** Every 10 s: the charge, and from how fast it moved over the last 10 minutes, how long to full or to empty.
 *  The raw charge has decimals, so a minute of samples already gives a rate; it steadies as more come. */
void App::sampleBattery() {
    if (time - battery.lastAt < 10) return;
    battery.lastAt = time;
    double pct = 0;
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetRawBatteryChargePercentage(&pct)) || R_FAILED(psmGetChargerType(&ct))) return;
    const int charging = ct != PsmChargerType_Unconnected;
    const long long wall = (long long)std::time(nullptr);
    // A charger in or out, or a gap (the app was suspended, its clock stopped): the old samples no longer tell the rate.
    if (charging != battery.charging || (battery.lastWall && wall - battery.lastWall > 120)) battery.samples.clear();
    battery.charging = charging;
    battery.lastWall = wall;
    auto& sm = battery.samples;
    sm.push_back({ time, pct });
    while (sm.size() > 2 && time - sm.front().first > 10 * 60) sm.erase(sm.begin());
    battery.est.clear();
    if (charging && pct >= 99.5) { battery.est = "charged"; return; }
    if (sm.size() < 6 || time - sm.front().first < 60) return;   // too soon to tell
    // The rate: a line fitted through the samples (per second).
    double mt = 0, mp = 0;
    for (const auto& [t, p] : sm) { mt += t; mp += p; }
    mt /= double(sm.size());
    mp /= double(sm.size());
    double num = 0, den = 0;
    for (const auto& [t, p] : sm) { num += (t - mt) * (p - mp); den += (t - mt) * (t - mt); }
    if (den <= 0) return;
    const double rate = num / den;
    if (charging ? rate <= 1e-5 : rate >= -1e-5) return;   // not moving the way it should: no guess
    const double secs = charging ? (100 - pct) / rate : pct / -rate;
    if (secs > 24 * 3600) return;
    const int mins = std::max(5, int(std::lround(secs / 60 / 5)) * 5);
    char b[32];
    if (mins >= 60) snprintf(b, sizeof b, "%d h %02d", mins / 60, mins % 60);
    else snprintf(b, sizeof b, "%d min", mins);
    battery.est = charging ? std::string("full in ") + b : std::string(b);
}

/**
 * Automix playlists play in the order Gatefold worked out for their joins (the user, 2026-10-29: "shuffle should not exist
 * on Automix playlists"): queued with Shuffle on, Shuffle is set aside (store.heldOrder, kept over a restart) and the order
 * is the playlist's; the next queue that is not one brings Shuffle back. While one plays, Shuffle is not offered (the
 * player bar's button passes it by, Settings greys it, the page has no Shuffle button).
 */
void App::holdOrderForAutomix(bool rebuild) {
    // rebuild: a queue already in place (restored at launch, or the background player's) has its order made again (setOrder);
    // playTracks only sets the mode, before it makes the new queue's order
    if (automixQueue()) {
        if (store.order == Order::Shuffle) {
            store.heldOrder = int(Order::Shuffle);
            if (rebuild) setOrder(Order::Default);
            else store.order = Order::Default;
            persistSoon(0.5f);
        }
    } else if (store.heldOrder >= 0) {
        const Order back = Order(store.heldOrder);
        store.heldOrder = -1;
        if (rebuild) setOrder(back);
        else store.order = back;
        persistSoon(0.5f);
    }
}

void App::setOrder(Order o) {
    const int cur = currentQueuePos();
    grabDrop();
    store.order = o;
    persistSoon(0.5f);
    if (q.order.empty()) return;
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    if (o == Order::Shuffle) {
        std::shuffle(q.order.begin(), q.order.end(), rng);
        if (cur >= 0) std::iter_swap(q.order.begin(), std::find(q.order.begin(), q.order.end(), cur));
        q.pos = 0;
    } else {
        q.pos = std::max(0, cur);
    }
}

std::vector<int> App::upNext(int n) const {
    std::vector<int> out;
    if (q.order.empty() || q.pos < 0) return out;
    const bool wrap = store.order == Order::RepeatAll || store.order == Order::Shuffle;
    for (int i = 1; i <= n && i < int(q.order.size()); i++) {
        int p = q.pos + i;
        if (p >= int(q.order.size())) {
            if (!wrap) break;
            p %= int(q.order.size());
        }
        out.push_back(q.tracks[q.order[p]]);
    }
    return out;
}

// ------------------------------------------------------------ editing the queue

/** The queue as it will play: what the Playlist view shows, so it agrees with Up next. */
std::vector<int> App::queueInPlayOrder() const {
    std::vector<int> out;
    out.reserve(q.order.size());
    for (int o : q.order) out.push_back(q.tracks[o]);
    return out;
}

/** Without Shuffle the list is rewritten in play order and the order becomes 0, 1, 2…: setOrder
 *  resets the order to the list's, which would otherwise undo a move or a "Play next". */
void App::queueNormalize() {
    if (store.order == Order::Shuffle) return;
    q.tracks = queueInPlayOrder();
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
}

void App::queueEdited() {
    queueNormalize();
    persistSoon(0.5f);
    // bgSyncQueue sends it next frame with the new pos; until the player's state shows that, its
    // old pos must not be taken for a track change (bgUpdate).
    if (player.remote()) bgs.awaitUntil = std::max(bgs.awaitUntil, time + 1.0);
}

void App::queueAdd(int track, bool next) {
    if (q.order.empty()) { playTracks({ track }, 0, "Queue"); return; }
    q.tracks.push_back(track);
    const int at = next ? std::min(q.pos + 1, int(q.order.size())) : int(q.order.size());
    q.order.insert(q.order.begin() + at, int(q.tracks.size()) - 1);
    queueEdited();
}

void App::queueRemove(int pos) {
    if (pos < 0 || pos >= int(q.order.size()) || pos == q.pos) return;   // the track playing stays
    const int idx = q.order[pos];
    q.order.erase(q.order.begin() + pos);
    q.tracks.erase(q.tracks.begin() + idx);
    for (int& o : q.order) if (o > idx) o--;
    if (pos < q.pos) q.pos--;
    queueEdited();
}

/** Moves the row at play position from so that it ends up at to; the playing track keeps playing. */
void App::queueMove(int from, int to) {
    const int n = int(q.order.size());
    if (from < 0 || from >= n) return;
    to = std::clamp(to, 0, n - 1);
    if (from == to) return;
    const int v = q.order[from];
    q.order.erase(q.order.begin() + from);
    q.order.insert(q.order.begin() + to, v);
    if (q.pos == from) q.pos = to;
    else if (from < q.pos && to >= q.pos) q.pos--;
    else if (from > q.pos && to <= q.pos) q.pos++;
    queueEdited();
}

/** "Play next" on a row already in the queue moves it rather than adding a copy. */
void App::queuePlayNext(int pos) {
    if (pos == q.pos) return;
    queueMove(pos, pos < q.pos ? q.pos : q.pos + 1);
}

/** Clear empties the queue, the track playing too: the music stops, as with the overlay's Remove All (the user, 2026-10-27;
 *  1.1.0 kept the track playing). */
void App::queueClear() {
    grabDrop();
    q.tracks.clear();
    q.order.clear();
    q.pos = -1;
    q.name.clear();
    q.failed = 0;
    stopAfterCurrent = false;
    if (player.remote()) {
        // An empty queue stops the background player; sent here, as bgSyncQueue never sends an empty one
        BgQueue none;
        none.mode = int(store.order);
        bg.setQueue(none);
        bgs.tracks.clear();
        bgs.order.clear();
        bgs.name.clear();
        bgs.mode = none.mode;
    } else {
        player.stop();
    }
    forgetNowPlaying();
    persistSoon(0.5f);
}

void App::grabStep(int d) {
    if (grab.kind == Grab::List) {   // a track of one of your own playlists: it swaps places with its neighbour
        if (grab.ref < 0 || grab.ref >= int(store.playlists.size()) || !store.playlists[size_t(grab.ref)].own()) { grabDrop(); return; }
        FolderPlaylist& p = store.playlists[size_t(grab.ref)];
        const std::vector<int> rows = playlistTracks(grab.ref);
        const int to = std::clamp(grab.pos + d, 0, int(rows.size()) - 1);
        if (to == grab.pos || grab.pos >= int(rows.size())) return;
        auto at = [&](int row) { return std::find(p.tracks.begin(), p.tracks.end(), lib.tracks[size_t(rows[size_t(row)])].path); };
        const auto a = at(grab.pos), b = at(to);
        if (a != p.tracks.end() && b != p.tracks.end()) std::iter_swap(a, b);
        if (p.automix) p.manual = true;   // moved by hand: it keeps this order until Order again
        store.save();
        grab.pos = to;
        ui.focus(uid("pl.r", to));
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::Playlists) {   // the Playlists page: the playlist itself moves (its rows keep their ids)
        const int to = std::clamp(grab.pos + d, 0, int(store.playlists.size()) - 1);
        if (to != grab.pos) { movePlaylist(grab.pos, to); grab.pos = to; }
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::Tabs) {        // Settings → Appearance → Sidebar: a tab
        const int to = std::clamp(grab.pos + d, 0, int(store.tabOrder.size()) - 1);
        if (to != grab.pos) { std::swap(store.tabOrder[size_t(grab.pos)], store.tabOrder[size_t(to)]); grab.pos = to; persistSoon(0.3f); }
        ui.setPadMode(true);
        return;
    }
    const int n = int(q.order.size());
    if (grab.pos < 0 || grab.pos >= n) { grabDrop(); return; }
    // In Up next the row stays after the track playing (what lies before it has been played),
    // and within the 15 rows Up next shows.
    const int lo = grab.upNext ? q.pos + 1 : 0, hi = grab.upNext ? std::min(n - 1, q.pos + 15) : n - 1;
    const int to = std::clamp(grab.pos + d, lo, hi);
    if (to == grab.pos) return;
    queueMove(grab.pos, to);
    grab.pos = to;
    ui.focus(grab.upNext ? uid("np.next", to - q.pos - 1) : uid("pl.r", to));
    ui.setPadMode(true);
}

void App::grabDrop() { grab = Grab{}; }

// ============================================================ track loading

/**
 * Shows trackIdx as the one playing; start: also start it (false when the background player already did).
 * The file's tags, format, lyrics and cover are read by the loader (applyLoaded), so the frame of a track
 * change is not spent on the SD card. Until they arrive, a moment later, the previous track's text and
 * lyrics stay as they were (frozen where they were), and then everything changes at once: showing the
 * library's tags first made the lyrics read "No lyrics" and the Canvas text jump for a frame or two.
 */
void App::loadTrack(int trackIdx, bool paused, bool start, double fade, const automix::Plan* mix) {
    const Track& t = lib.tracks[trackIdx];
    // A crossfade is set: a track changed by hand fades quickly instead of cutting (Player: only while one plays).
    if (fade <= 0 && start && !paused && (store.crossfade > 0 || automixQueue()) && !player.remote() && isPlaying()) fade = 0.3;
    xfadeChecked = false;
    stretchBegun = false;
    mixPrepared = false;
    // a load that is not the join (a skip, before or through one): its show ends cleanly (it stayed on screen: the user,
    // 2026-10-29), and nothing is left to give way from
    if (!mix) endJoinShow();
    if (!mix && mixOld.on) mixOld = MixOld{};
    lyricsHold = player.loaded() ? player.position() : 0;
    const bool shown = now.track >= 0 && !now.info.title.empty();
    now.track = trackIdx;
    if (!shown) {   // nothing to keep: what the library knows, until the file's own
        now.info = TrackInfo{};
        now.info.title = t.title;
        now.info.artist = t.artist;
        now.info.album = t.album;
        now.info.albumArtist = t.albumArtist;
        now.info.genre = t.genre;
        now.info.date = t.date;
        now.info.trackNo = t.trackNo;
        now.info.duration = t.duration;
        now.lyrics.clear();
        now.synced = false;
    }
    now.loading = true;
    statHeard = 0;   // a new play of it, also when Repeat track plays it again
    statCounted = false;
    const float gain = float(std::pow(10, levelDb(trackIdx) / 20));
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));

    wave.request(t.path);
    meta.wantTags(trackIdx);
    // levelling from its first sample; set after load(), which keeps the outgoing track's gain for its fade
    if (!start) { player.setLevel(gain, true); return; }
    if (player.remote() && bgSyncQueue(false)) {
        player.setLevel(gain, true);
        bg.playAt(q.pos, paused, 0);
        bgs.awaitUntil = time + 1.5;
        return;
    }
    UPD_MARK("loadTrack");   // (the dev trace: the join's frame split, the player's load apart)
    player.load(t.path, paused, fade, mix);
    player.setLevel(gain, true);
    UPD_MARK("player.load");
}

// ============================================================ the background player

void App::bgConnect() {
    if (!bg.connect()) return;
    player.setRemote(&bg);
    thumbs.start(store.path("thumbs"));   // GFBG_THUMBS
    bg.poll(time);
    const BgState& s = bg.state();
    bgs = BgSync{};
    bgs.trackGen = s.trackGen;
    bgs.queueGen = s.queueGen;
    bgs.errorGen = s.errorGen;
    bgs.seenStopAfter = bgs.stopAfter = stopAfterCurrent = s.stopAfter;
    // The overlay may have changed the volume since Gatefold last ran: the player's is the one heard.
    bgs.volume = s.volume;
    volumeShown = store.volume = s.volume;
    sleepGenSeen = s.sleepGen;   // a timer that ran out before Gatefold opened is not news
    levelModeSent = -1;          // tell it the levelling mode again
    bgs.cfgRead = bg.config(bgs.cfg);
    // The equalizer: the player's copy is the one heard (the overlay may have changed it meanwhile).
    bgs.eqGen = s.eqGen;
    bgs.joinGen = s.joinGen;   // a join already under way is not shown from its middle
    if (GfbgEq e; bg.eq(e)) { store.eq = e; player.setEq(e); }
    eqDirty = false;
}

/** Takes the background player's queue as Gatefold's own (at launch, and after the overlay reshuffled it). */
bool App::bgAdoptQueue() {
    BgQueue bq;
    if (!bg.getQueue(bq)) return false;
    if (bq.paths.empty()) {
        // Emptied in the overlay (Remove All): Gatefold's queue goes too. An empty queue from anyone else (a player just
        // started with nothing) is not news: then Gatefold keeps, or at launch restores, its own. Before, Gatefold kept
        // its queue either way, and at its next launch restored it and sent it back to the player.
        if (bg.state().queueBy != BgClient::ByOverlay) return false;
        grabDrop();
        q.tracks.clear();
        q.order.clear();
        q.pos = -1;
        q.name.clear();
        q.failed = 0;
        bgs.tracks.clear();
        bgs.order.clear();
        bgs.name.clear();
        bgs.mode = int(store.order);
        forgetNowPlaying();
        persistSoon(0.5f);   // the session too: an empty one restores nothing
        return true;
    }
    std::vector<int> keep, tracks;   // its index → ours, −1 for files this library does not have
    for (const auto& p : bq.paths) {
        auto it = pathIndex.find(p);
        keep.push_back(it == pathIndex.end() ? -1 : int(tracks.size()));
        if (it != pathIndex.end()) tracks.push_back(it->second);
    }
    if (tracks.empty()) return false;
    grabDrop();
    q.tracks = tracks;
    q.name = bq.name;
    q.failed = 0;
    q.order.clear();
    for (int o : bq.order) if (o >= 0 && o < int(keep.size()) && keep[o] >= 0) q.order.push_back(keep[o]);
    if (q.order.size() != q.tracks.size()) {
        q.order.resize(q.tracks.size());
        for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    }
    const int cur = bq.pos >= 0 && bq.pos < int(bq.order.size()) && bq.order[bq.pos] >= 0 && bq.order[bq.pos] < int(keep.size())
                        ? keep[bq.order[bq.pos]] : -1;
    q.pos = 0;
    for (int i = 0; i < int(q.order.size()); i++) if (q.order[i] == cur) q.pos = i;
    queueNormalize();   // an older queue may hold a "Play next" outside the list's order
    store.order = Order(std::clamp(bq.mode, 0, 3));
    holdOrderForAutomix(true);   // (the background player's queue: Automix playlists not shuffled)
    // Remembered as sent, so it is not sent straight back; unless files were dropped, which it should hear about.
    bgs.tracks = q.tracks.size() == bq.paths.size() ? q.tracks : std::vector<int>{};
    bgs.order = q.order;
    bgs.mode = bq.mode;
    bgs.name = q.name;
    for (int t : q.tracks) thumbFor(t, false);
    const int t = cur >= 0 ? q.tracks[q.order[q.pos]] : -1;
    if (t >= 0 && t != now.track) loadTrack(t, true, false);
    savedTrack = now.track;
    return true;
}

/** Sends Gatefold's queue when it changed here: a new list, a reshuffle, an order change, a track queued. False when the
 *  background player could not take it (too big for its memory): Gatefold plays by itself from then on (reload: and
 *  carries on with the track playing, where it was). */
bool App::bgSyncQueue(bool reload) {
    // An empty queue here (no music folder yet, a library that lacks its files) never clears the player's.
    if (!player.remote() || q.tracks.empty()) return player.remote();
    if (q.tracks == bgs.tracks && q.order == bgs.order && int(store.order) == bgs.mode && q.name == bgs.name && automixQueue() == bgs.automix)
        return true;
    BgQueue bq;
    bq.name = q.name;
    bq.automix = automixQueue();   // joined by Automix there too (protocol 6), in its own order
    bq.paths.reserve(q.tracks.size());
    for (int t : q.tracks) bq.paths.push_back(lib.tracks[t].path);
    bq.order = q.order;
    bq.pos = q.pos;
    bq.mode = int(store.order);
    if (!bg.setQueue(bq)) {
        if (!bg.queueRefused()) return false;
        const bool had = now.track >= 0 && q.pos >= 0, playing = isPlaying();
        const double at = player.loaded() ? player.position() : 0;
        bg.stop();
        bg.disconnect();
        player.setRemote(nullptr);
        toast("This queue is too big for the background player: Songate plays it itself");
        if (reload && had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seek(at);
        }
        return false;
    }
    for (int t : q.tracks) thumbFor(t, false);
    bgs.tracks = q.tracks;
    bgs.order = q.order;
    bgs.mode = bq.mode;
    bgs.name = q.name;
    bgs.automix = bq.automix;
    return true;
}

/** Every frame while connected: what the player did on its own (next track, the overlay's buttons), and ours to it. */
void App::bgUpdate() {
    if (!player.remote()) return;
    if (!bg.poll(time)) {
        player.setRemote(nullptr);
        toast("The background player stopped, so Songate plays on its own");
        return;
    }
    if (!bgSyncQueue()) return;   // too big for it: Gatefold plays by itself now
    const BgState& s = bg.state();
    if (s.queueGen != bgs.queueGen) {
        bgs.queueGen = s.queueGen;
        if (s.queueBy != BgClient::ByGatefold) bgAdoptQueue();
    }
    if (s.trackGen != bgs.trackGen) { bgs.trackGen = s.trackGen; bgs.awaitUntil = 0; }
    // Automix's joins in the player (protocol 6), shown as Gatefold's own (updateAutomix): announced a second before, the
    // outgoing song kept as the next one loads, on the player's clock
    automix::Plan joined;
    bool join = false;
    if (automixQueue() && now.track >= 0) {
        if (s.joining == 2 && s.joinGen != bgs.joinGen) {
            if (!mixAnnounced || time >= mixUntil) announceBgJoin(s);
            mixAnnounced = false;
            keepMixOld();
            join = true;
            joined.length = s.joinLength;
            joined.bpm = s.joinBpm;
            joined.beatmatched = (s.joinFlags & 1) != 0;
            joined.drop = (s.joinFlags & 2) != 0;
        } else if (s.joining == 1 && s.joinAt >= -1.0 && isPlaying()) {
            if (!mixAnnounced || time >= mixUntil) announceBgJoin(s);
        } else if (mixAnnounced && s.joining != 2) {
            endJoinShow();   // sought back before it, or it is off now
        }
    }
    bgs.joinGen = s.joinGen;
    if (time >= bgs.awaitUntil && s.pos >= 0 && s.pos < int(q.order.size()) && s.pos != q.pos) {
        q.pos = s.pos;
        loadTrack(q.tracks[q.order[q.pos]], false, false, 0, join ? &joined : nullptr);
    }
    if (s.errorGen != bgs.errorGen) {
        bgs.errorGen = s.errorGen;
        toast("A track could not be played, so it was skipped");
    }
    // Stop after current: whichever side changed it last wins (the player clears it when it stops).
    if (s.stopAfter != bgs.seenStopAfter) { bgs.seenStopAfter = bgs.stopAfter = stopAfterCurrent = s.stopAfter; }
    else if (stopAfterCurrent != bgs.stopAfter) { bg.setStopAfter(stopAfterCurrent); bgs.stopAfter = stopAfterCurrent; }
    if (std::fabs(s.volume - bgs.volume) > 0.0005f) {
        bgs.volume = s.volume;
        if (std::fabs(s.volume - volumeShown) > 0.0005f) volumeShown = s.volume;   // the overlay moved it
    }
    // Automix's model, for an Automix playlist's joins in the player (protocol 6)
    if (const std::string file = mixFile() ? mixFile()->file : std::string(); int(mixModel()) != bgs.modelSent || file != bgs.modelFileSent) {
        bg.setAutomixModel(int(mixModel()), file);   // a downloaded model: the player reads the same file
        bgs.modelSent = int(mixModel());
        bgs.modelFileSent = file;
    }
    // Crossfade's settings, whenever they differ from what it was told.
    if (store.crossfade != bgs.xfadeSent || int(store.xfadeNotInAlbum) != bgs.xfadeFlagSent) {
        bg.setCrossfade(store.crossfade, store.xfadeNotInAlbum);
        bgs.xfadeSent = store.crossfade;
        bgs.xfadeFlagSent = store.xfadeNotInAlbum;
    }
    // The equalizer: ours when it changed here (a slider: at most every 0.1 s), else the player's when the overlay changed it.
    if (eqDirty && time - eqSentAt >= 0.1) {
        bg.setEq(store.eq);
        eqDirty = false;
        eqSentAt = time;
    } else if (!eqDirty && s.eqGen != bgs.eqGen) {
        bgs.eqGen = s.eqGen;
        if (GfbgEq e; bg.eq(e) && memcmp(&e, &store.eq, sizeof e) != 0) { store.eq = e; player.setEq(e); persistSoon(1.0f); }
    }
}

/** Queues the overlay's copy of a track's cover; now: ahead of the rest, and made again even if it exists. */
void App::thumbFor(int track, bool now) {
    const Track& t = lib.tracks[track];
    auto mine = store.covers.find(t.path);
    thumbs.want({ t.path, t.folder, mine == store.covers.end() ? std::string() : mine->second, now }, now);
}

void App::newPlaylist(int track) {
    std::string name = "Playlist " + std::to_string(store.playlists.size() + 1);
    if (!keyboard("New playlist", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    const int p = store.newPlaylist(name);
    if (track >= 0) addToPlaylist(p, track);
    store.save();
    if (track < 0) navigate(View::List, p);
    toast("Made the playlist “" + store.playlists[p].name + "”");
}

void App::addToPlaylist(int playlist, int track) {
    FolderPlaylist& p = store.playlists[playlist];
    const std::string& path = lib.tracks[track].path;
    if (std::find(p.tracks.begin(), p.tracks.end(), path) != p.tracks.end()) { toast("Already in “" + p.name + "”"); return; }
    p.tracks.push_back(path);
    store.save();
    if (p.automix && !p.manual) {   // an Automix playlist: ordered again with it
        orderAutomixList(playlist, false);
        toast("Added to “" + p.name + "”, ordered for Automix");
        return;
    }
    toast("Added to “" + p.name + "”");
}

// ============================================================ playlist files (m3u.hpp)

/**
 * A playlist file's lines as library tracks, in its order. Each line is tried as written (sdmc:/…, "/…" from the
 * card's root, or relative to the file), then the same path in any case (FAT ignores it), then, for a path from
 * elsewhere (a PC's "C:/Music/…"), the library file of that name that shares the most folders from the end.
 */
std::vector<int> App::resolvePlaylistFile(const std::string& file, int& missing) {
    missing = 0;
    std::vector<M3uLine> lines;
    if (!readM3u(file, lines)) return {};
    const std::string dir = file.substr(0, file.find_last_of('/'));
    std::unordered_map<std::string, int> byLower;
    std::unordered_multimap<std::string, int> byName;
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        byLower.emplace(lower(lib.tracks[i].path), i);
        byName.emplace(lower(baseName(lib.tracks[i].path)), i);
    }
    auto parts = [](const std::string& p) {
        std::vector<std::string> v;
        size_t at = 0;
        while (at <= p.size()) {
            size_t s = p.find('/', at);
            if (s == std::string::npos) s = p.size();
            if (s > at) v.push_back(lower(p.substr(at, s - at)));
            at = s + 1;
        }
        return v;
    };
    std::vector<int> out;
    for (const M3uLine& l : lines) {
        const std::string& p = l.path;
        std::string cand;
        if (p.compare(0, 6, "sdmc:/") == 0) cand = p;
        else if (!p.empty() && p[0] == '/') cand = "sdmc:" + p;
        else if (!(p.size() > 1 && p[1] == ':')) cand = dir + "/" + p;   // a drive letter is someone else's card
        int t = -1;
        if (!cand.empty()) {
            cand = normalisePath(cand);
            if (auto it = pathIndex.find(cand); it != pathIndex.end()) t = it->second;
            else if (auto lt = byLower.find(lower(cand)); lt != byLower.end()) t = lt->second;
        }
        if (t < 0) {
            const std::vector<std::string> want = parts(p);
            const auto range = byName.equal_range(lower(baseName(p)));
            int best = 0;
            for (auto it = range.first; it != range.second; ++it) {
                const std::vector<std::string> have = parts(lib.tracks[it->second].path);
                int n = 0;
                while (n < int(want.size()) && n < int(have.size()) && want[want.size() - 1 - n] == have[have.size() - 1 - n]) n++;
                if (n > best) { best = n; t = it->second; }
            }
        }
        if (t >= 0) out.push_back(t);
        else missing++;
    }
    return out;
}

void App::openPlaylistFile(const std::string& file) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::PlFile;
    sheet.dir = file;
    sheet.returnFocus = back;
    sheet.fileTracks = resolvePlaylistFile(file, sheet.fileMissing);
    playlistPickerAt = file.substr(0, file.find_last_of('/'));
    ui.focusDefault();
}

/** Starts where the last file was picked this session, else at the card's root. */
void App::openPlaylistPicker() {
    closeSheet();
    std::string at = playlistPickerAt;
    struct stat st;
    if (at.empty() || stat(at.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) at = store.importDir;   // Settings → Storage and reset
    if (at.empty() || stat(at.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) at = "sdmc:/";
    navigate(View::PickPlaylist, -1, at);
}

void App::leavePlaylistPicker() {
    if (route.view == View::PickPlaylist || route.view == View::PickExport) route.query = "sdmc:/";   // as if at the root: back() leaves
    back();
}

/** Starts in the folder last exported to this session, else <music folder>/Playlists if there is one, else the music folder. */
void App::openExportPicker(const std::string& name, const std::vector<int>& tracks) {
    if (tracks.empty()) { toast("Nothing to export: the playlist is empty"); return; }
    exportName = name.empty() ? "Playlist" : name;
    exportTracks = tracks;
    auto isDir = [](const std::string& p) {
        struct stat st;
        return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };
    std::string at = exportPickerAt;
    if (!isDir(at)) at = store.exportDir;   // Settings → Storage and reset
    if (!isDir(at) && !lib.roots.empty()) at = isDir(lib.roots[0] + "/Playlists") ? lib.roots[0] + "/Playlists" : lib.roots[0];
    if (!isDir(at)) at = "sdmc:/";
    navigate(View::PickExport, -1, at);
}

/** <dir>/<name>.m3u8, the paths relative to it (writeM3u), so it works on a PC copy of the card too. */
void App::exportPlaylist(const std::string& dir) {
    std::string safe = exportName;
    for (char& c : safe) if (strchr("/\\:*?\"<>|", c)) c = ' ';
    const std::string file = (dir.back() == '/' ? dir : dir + "/") + safe + ".m3u8";
    auto write = [this, dir, file, safe] {
        std::vector<M3uTrack> out;
        out.reserve(exportTracks.size());
        for (int t : exportTracks) {
            if (t < 0 || t >= int(lib.tracks.size())) continue;   // the library was rebuilt meanwhile
            const Track& tr = lib.tracks[size_t(t)];
            out.push_back({ tr.path, tr.artist, tr.title, tr.duration });
        }
        if (!writeM3u(file, exportName, out)) { toast("Could not write " + file); return; }
        exportPickerAt = dir;
        pickerGen++;   // the next export opens here: its listing shows this file
        const size_t n = out.size();
        leavePlaylistPicker();
        toast("Exported " + std::to_string(n) + (n == 1 ? " track to " : " tracks to ") + safe + ".m3u8 in " +
              (dir == "sdmc:/" ? std::string("the SD card") : baseName(dir)));
    };
    struct stat st;
    if (stat(file.c_str(), &st) != 0) { write(); return; }
    confirm(nullptr, "Export", safe + ".m3u8", "Already in " + dir, "Replace the playlist file that is there?",
            "The file there is overwritten with this playlist's tracks.", "Replace", write);
}

void App::confirm(const char* icon, const std::string& kicker, const std::string& title, const std::string& sub, const std::string& question,
                  const std::string& text, const std::string& button, std::function<void()> yes) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Confirm;
    sheet.returnFocus = back;
    sheet.askIcon = icon;
    sheet.askKicker = kicker;
    sheet.askTitle = title;
    sheet.askSub = sub;
    sheet.askQuestion = question;
    sheet.askText = text;
    sheet.askButton = button;
    sheet.askYes = std::move(yes);
    ui.focusDefault();
}

/** Settings → Playback → Background player: hands the track over, at the same place, either way. */
void App::setBackground(bool on) {
    store.background = on;
    persistSoon(0.3f);
    const bool had = now.track >= 0 && q.pos >= 0;
    const double at = player.loaded() ? player.position() : 0;
    const bool playing = isPlaying();
    if (on && !player.remote()) {
        player.stop();
        bgConnect();
        if (!player.remote()) { toast("The background player is not installed"); return; }
        bgs.tracks.clear();   // send ours
        if (had) {
            if (!bgSyncQueue(false)) {   // too big for it (it said so): Gatefold carries on by itself
                loadTrack(q.tracks[q.order[q.pos]], !playing);
                if (at > 1) player.seek(at);
                return;
            }
            bg.playAt(q.pos, !playing, at);
            bgs.awaitUntil = time + 1.5;
        }
        toast("Playing through the background player");
    } else if (!on && player.remote()) {
        bg.pause(time);
        bg.disconnect();
        player.setRemote(nullptr);
        if (had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seek(at);
        }
        toast("Playing in Songate only: music stops when you leave it");
    }
}

/** After your cover for the playing track is set or forgotten: its own cover (or the folder's) again. */
void App::loadTrackCoverAgain() {
    if (now.track < 0) return;
    const Track& t = lib.tracks[now.track];
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    r.readInfo = now.loading;   // the track's own load would be dropped for this one: do its part too
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));
}

/** What the loader read for the track playing: its tags and lyrics, and the cover, uploaded here (textures belong to this thread). */
void App::applyLoaded(TrackLoader::Result& r) {
    if (r.gen != loadGen || now.track < 0) { r.release(); return; }   // a track that is no longer the one playing
    now.loading = false;
    const Track& t = lib.tracks[now.track];
    if (r.readInfo) {
        TrackInfo& in = r.info;
        if (in.title.empty()) in.title = t.title;
        if (in.artist.empty()) in.artist = t.artist;
        if (in.album.empty()) in.album = t.album;
        if (in.duration <= 0) in.duration = t.duration;
        if (store.edits.count(t.path)) {
            // Your edits win over the file's tags here too (the library copy already has them).
            in.title = t.title; in.artist = t.artist; in.album = t.album;
            in.albumArtist = t.albumArtist; in.genre = t.genre; in.date = t.date; in.trackNo = t.trackNo;
        }
        now.info = std::move(in);
        now.lyricsSource = r.sidecar ? ".lrc file" : "Tags";
        now.lyricsFromFile = r.sidecar;
        parseLyrics(r.lyrics);
        // The new texts render this frame; the cover is uploaded the next one (update), not both at once.
        coverWait_ = std::move(r);
        coverWaiting_ = true;
        return;
    }
    applyCover(r);
}

void App::forgetNowPlaying() {
    loadGen++;   // what the loader still reads for the old track is dropped
    now.track = -1;
    now.info = TrackInfo{};
    now.lyrics.clear();
    now.synced = false;
    now.loading = false;
    // The cover fades out, as it fades between tracks
    if (now.prevCover) SDL_DestroyTexture(now.prevCover);
    if (now.prevBlur) SDL_DestroyTexture(now.prevBlur);
    if (now.prevFrost) SDL_DestroyTexture(now.prevFrost);
    now.prevCover = now.cover;
    now.prevBlur = now.blur;
    now.prevFrost = now.frost;
    now.cover = now.blur = now.frost = nullptr;
    now.fade = 0;
    savedTrack = -1;
}

/** The loaded cover: uploaded, crossfaded in, and the palette from it. */
void App::applyCover(TrackLoader::Result& r) {
    if (r.gen != loadGen || now.track < 0) { r.release(); return; }
    if (mixOld.on) mixOld.coverIn = true;   // (a join's outgoing cover is now.prevCover from here: mixCover)
    // Keep the outgoing cover for the 500 ms crossfade.
    if (now.prevCover) SDL_DestroyTexture(now.prevCover);
    if (now.prevBlur) SDL_DestroyTexture(now.prevBlur);
    if (now.prevFrost) SDL_DestroyTexture(now.prevFrost);
    now.prevCover = now.cover;
    now.prevBlur = now.blur;
    now.prevFrost = now.frost;
    now.cover = now.blur = now.frost = nullptr;
    now.fade = 0;
    coverPx.clear();
    coverPal = store.brightness ? paletteFixed(FixedTheme::Light) : paletteDefault();
    if (firstCover_) { firstCover_ = false; launchAt("cover"); }
    if (r.cover) {
        now.cover = SDL_CreateTextureFromSurface(renderer_, r.cover);
        if (now.cover) SDL_SetTextureScaleMode(now.cover, SDL_ScaleModeLinear);
        // The overlay's copy, made again for the track shown: a cover set in the track menu changes it.
        if (player.remote()) thumbFor(now.track, true);
    }
    if (r.blur) {
        now.blur = SDL_CreateTextureFromSurface(renderer_, r.blur);
        if (now.blur) SDL_SetTextureScaleMode(now.blur, SDL_ScaleModeLinear);
    }
    if (r.frost) {
        now.frost = SDL_CreateTextureFromSurface(renderer_, r.frost);
        if (now.frost) SDL_SetTextureScaleMode(now.frost, SDL_ScaleModeLinear);
    }
    if (!r.px.empty()) {
        coverPal = r.pal;
        coverPx = std::move(r.px);
    }
    r.release();
    applyTheme();
}

/** [mm:ss.xx] LRC lines become timed lines; anything else is plain text. */
void App::parseLyrics(const std::string& text) {
    now.lyrics.clear();
    now.synced = false;
    now.lrcOffset = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<double> stamps;
        size_t p = 0;
        while (p < line.size() && line[p] == '[') {
            const size_t close = line.find(']', p);
            if (close == std::string::npos) break;
            int mm = 0;
            double ss = 0;
            if (sscanf(line.c_str() + p + 1, "%d:%lf", &mm, &ss) == 2) stamps.push_back(mm * 60 + ss);
            else if (!stamps.empty()) break;
            else if (!line.compare(p + 1, 7, "offset:")) now.lrcOffset = atof(line.c_str() + p + 8) / 1000;   // ms; + is sooner
            p = close + 1;
        }
        std::string body = line.substr(p);
        while (!body.empty() && body.front() == ' ') body.erase(0, 1);
        if (!stamps.empty()) {
            now.synced = true;
            for (double t : stamps) now.lyrics.push_back({ t, body });
        } else if (p == 0) {
            now.lyrics.push_back({ -1, body });
        }
        if (end == text.size()) break;
    }
    if (now.synced) {
        std::erase_if(now.lyrics, [](const LyricLine& l) { return l.t < 0; });
        // [offset:] as LRC defines it: a positive offset shows every line that much sooner
        if (now.lrcOffset != 0) for (LyricLine& l : now.lyrics) l.t = std::max(0.0, l.t - now.lrcOffset);
        std::stable_sort(now.lyrics.begin(), now.lyrics.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    }
    while (!now.lyrics.empty() && now.lyrics.back().text.empty() && !now.synced) now.lyrics.pop_back();
}

float App::lyricNudge() const {
    if (now.track < 0) return 0;
    const auto it = store.lyricNudge.find(lib.tracks[now.track].path);
    return it == store.lyricNudge.end() ? 0 : it->second;
}

void App::nudgeLyrics(float by) {
    if (now.track < 0) return;
    const std::string& path = lib.tracks[now.track].path;
    const float v = by == 0 ? 0 : std::clamp(std::round((lyricNudge() + by) * 10) / 10, -10.f, 10.f);
    if (std::fabs(v) < 0.05f) store.lyricNudge.erase(path);
    else store.lyricNudge[path] = v;
    store.saveLyricNudges();
}

void App::seekLyric(int line) {
    if (line >= 0 && line < int(now.lyrics.size()) && now.lyrics[line].t >= 0) player.seek(std::max(0.0, now.lyrics[line].t + lyricNudge()));
}

// ============================================================ navigation

ScrollArea& App::area(const char* key, int ref) { return areas[uid(key, ref)]; }

void App::navigate(View v, int ref, const std::string& query, bool push) {
    canvas = false;
    // A picker opened from elsewhere lists afresh: the card may have changed meanwhile (an export, Files)
    auto picker = [](View x) { return x == View::PickFolder || x == View::PickPlaylist || x == View::PickExport; };
    if (picker(v) && !picker(route.view)) pickerGen++;
    // Into the folder pages: from Library → Folders they stay Library's in the sidebar; moving between
    // folders keeps whichever it was.
    if (v == View::Folders && route.view != View::Folders) folderFrom = route.view == View::Library ? View::Library : View::Folders;
    if (push && !(route.view == v && route.ref == ref && route.query == query)) {
        route.focus = ui.focusedId();
        history.push_back(route);
        if (history.size() > 40) history.erase(history.begin());
    }
    route = { v, ref, query, 0 };
    if (v == View::Search) {
        searchResults.clear();
        std::vector<std::string> words;
        std::string w;
        for (char c : lower(query) + " ") {
            if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
            else w += c;
        }
        for (int i = 0; i < int(lib.tracks.size()); i++) {
            const Track& t = lib.tracks[i];
            const std::string hay = lower(t.title + " " + t.artist + " " + t.album + " " + t.genre + " " + t.path);
            if (std::all_of(words.begin(), words.end(), [&](const std::string& x) { return hay.find(x) != std::string::npos; }))
                searchResults.push_back(i);
        }
        area("search").target = area("search").y = 0;
    }
    ui.focusDefault();
}

PaletteOpts App::paletteOpts() const {
    PaletteOpts o;
    o.mode = store.brightness ? PaletteOpts::Light : PaletteOpts::Dark;
    o.intensity = store.saturation;
    return o;
}

/** Custom: your background and accents (theme.js customPalette); a colour left to "A" comes from the cover playing. */
Palette App::customPalette() const {
    PaletteOpts c = paletteOpts();
    auto rgb = [](int v) { return Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f }; };
    c.hasBg = store.customBg >= 0;
    c.hasAccent = store.customAccent >= 0;
    c.hasAccent2 = store.customAccent2 >= 0;
    if (c.hasBg) c.bg = rgb(store.customBg);
    c.accent = c.hasAccent ? rgb(store.customAccent) : coverPal.accent;
    c.hasAccent = true;   // theme.js: a custom theme always has its accent
    if (c.hasAccent2) c.accent2 = rgb(store.customAccent2);
    // Brightness: on a dark or a light background, your colours read as a cover's; As picked keeps your background
    // (with "A" for the background there is none to keep: the Brightness of From the cover applies)
    if (store.customMode == 0) c.mode = PaletteOpts::Dark;
    else if (store.customMode == 1) c.mode = PaletteOpts::Light;
    else if (c.hasBg) c.mode = PaletteOpts::Auto;
    return paletteCustom(c);
}

/** Settings → Appearance: From the cover (dark or light, any saturation), Dark, Light, or Custom. */
void App::applyTheme() {
    const PaletteOpts o = paletteOpts();
    paletteSetDefaults(o);   // the album pages' palettes, made on the metadata thread
    if (!coverPx.empty()) coverPal = paletteFromPixels(coverPx.data(), 64, 64, o);
    else coverPal = store.brightness ? paletteFixed(FixedTheme::Light) : paletteDefault();
    switch (store.theme) {
    case 1: target = paletteFixed(FixedTheme::Dark); break;
    case 2: target = paletteFixed(FixedTheme::Gray); break;
    case 3: target = paletteFixed(FixedTheme::Light); break;
    case 4: target = customPalette(); break;
    default: target = coverPal; break;
    }
}

int App::canvasLayoutShown() const {
    // A track's lyrics are still on their way: keep the layout, rather than going Minimal for a moment and back.
    if (now.loading && now.track >= 0) return cvTo;
    // Nothing to read: Minimal. So is the lyrics-first layout with lyrics switched off.
    if (now.lyrics.empty() && store.canvasAutoMinimal) return 2;
    if (store.canvasLayout == 1 && !store.canvasLyrics) return 2;
    return store.canvasLayout;
}

unsigned App::canvasLayoutsOff() const {
    if (now.lyrics.empty() && store.canvasAutoMinimal && now.track >= 0) return 0b011;   // Cover and Lyrics: Minimal until lyrics come back
    if (!store.canvasLyrics) return 0b010;                                                // Lyrics with lyrics switched off
    return 0;
}

void App::openCanvas(bool on) {
    canvas = on;
    // Opening shows the right layout at once; only changes while it is open are animated.
    cvTo = canvasLayoutShown();
    for (int i = 0; i < 3; i++) cvFrom[i] = i == cvTo ? 1.f : 0.f;
    cvStart = -10;
    canvasIdle = 0;
    canvasCtl = 1;
    canvasViz = 0;
    if (on) ui.focus(uid("cv.play"));
    else ui.focusDefault();
}

void App::back() {
    if (canvas) { openCanvas(false); return; }
    // The playlist browsers (import, export): B goes to the folder above; at the SD card's root it leaves them.
    if (route.view == View::PickPlaylist || route.view == View::PickExport) {
        const std::string& p = route.query;
        const size_t s = p.find_last_of('/');
        if (!(p.empty() || p == "sdmc:/" || p == "sdmc:")) {
            const std::string child = p;
            navigate(route.view, -1, s == std::string::npos || s <= 5 ? std::string("sdmc:/") : p.substr(0, s), false);
            ui.focus(uid("pk.dir", int(uid(child) & 0x7fffffff)));
            return;
        }
        while (!history.empty() && (history.back().view == View::PickPlaylist || history.back().view == View::PickExport)) history.pop_back();
    }
    // Files: B leaves Select first
    if (route.view == View::Files && files.selecting) {
        files.selecting = false;
        files.selected.clear();
        return;
    }
    // Files: B goes to the folder above (the one come from is focused); at the SD card's root it leaves as any page.
    if (route.view == View::Files && !(route.query.empty() || route.query == "sdmc:/" || route.query == "sdmc:")) {
        const std::string& p = route.query;
        const size_t s = p.find_last_of('/');
        files.focusAfter = p;
        navigate(View::Files, -1, s == std::string::npos || s <= 5 ? std::string("sdmc:/") : p.substr(0, s), false);
        return;
    }
    // Settings: from a category's options, B goes back to that category in the list; only from the list does it leave.
    if (const SDL_FRect f = ui.rectOf(ui.focusedId());
        route.view == View::Settings && f.x >= settingsPaneX && f.y >= css::TbH && f.y + f.h <= css::H - css::BarH) {
        ui.focus(settingsQuery.empty() ? uid("set.cat", settingsCat) : uid("set.find"));
        ui.setPadMode(true);
        return;
    }
    if (history.empty()) {
        if (route.view != View::Now) navigate(View::Now, -1, {}, false);
        return;
    }
    route = history.back();
    history.pop_back();
    if (route.focus) ui.focus(route.focus);
    else ui.focusDefault();
}

void App::openSearch() {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        toast("The keyboard is not available in applet mode");
        return;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, "Artist, album or track");
    swkbdConfigSetHeaderText(&kbd, "Search");
    if (route.view == View::Search) swkbdConfigSetInitialText(&kbd, route.query.c_str());
    char out[256] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
    if (R_SUCCEEDED(rc) && out[0]) navigate(View::Search, -1, out, route.view != View::Search);
}

void App::toast(const std::string& s) {
    toastText = s;
    toastT = 3.2f;
    // the app's log (Settings › Support sends its last lines with a report): what it told you, and when
    const time_t t = std::time(nullptr);
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
    logLines_.push_back(std::string(b) + "  " + s);
    if (logLines_.size() > 200) logLines_.erase(logLines_.begin(), logLines_.begin() + 50);
    logDirty_ = true;
}

// ============================================================ helpers

std::vector<int> App::listTracks(View v, int ref, const std::string&) const {
    std::vector<int> out;
    switch (v) {
    case View::Queue: return queueInPlayOrder();   // as it will play, like Up next
    case View::Search: return searchResults;
    case View::Album:
        if (ref >= 0 && ref < int(lib.albums.size())) out = lib.albums[ref].tracks;
        return out;
    case View::Artist:
        if (ref >= 0 && ref < int(artists.size())) out = artists[ref].tracks;
        return out;
    case View::Genre:
        if (ref >= 0 && ref < int(genres.size())) out = genres[ref].tracks;
        return out;
    case View::Folders: return lib.tracksUnder(ref);
    case View::List:
        if (ref < 0) {
            for (const auto& p : store.liked) {
                auto it = pathIndex.find(p);
                if (it != pathIndex.end()) out.push_back(it->second);
            }
        } else if (ref < int(store.playlists.size())) {
            const FolderPlaylist& p = store.playlists[ref];
            if (p.own()) {
                // an Automix playlist being analysed: only its songs analysed so far, in their order (the rest join it one by one)
                const bool hide = p.automix && p.waiting && !p.manual;
                for (const auto& path : p.tracks)
                    if (auto it = pathIndex.find(path); it != pathIndex.end() && (!hide || automixReady(path))) out.push_back(it->second);
            } else if (p.smart()) {
                out = smartTracks(p.path);
            } else {
                out = lib.tracksUnder(lib.folderByPath(p.path));
            }
        }
        return out;
    default:
        // The whole music folder, in folder order.
        for (int i = 0; i < int(lib.tracks.size()); i++) out.push_back(i);
        return out;
    }
}

std::string App::fmtTime(double s) const {
    if (s <= 0 || !std::isfinite(s)) return "–:––";
    const int t = int(s);
    char buf[16];
    if (t >= 3600) snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

std::string App::fmtTotal(const std::vector<int>& tracks) const {
    double s = 0;
    for (int t : tracks) s += lib.tracks[t].duration;
    const int m = int(s / 60 + 0.5);
    std::string n = std::to_string(tracks.size()) + (tracks.size() == 1 ? " track" : " tracks");
    if (m <= 0) return n;
    return n + " · " + (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
}

std::string App::trackSub(int t) const {
    const Track& tr = lib.tracks[t];
    if (tr.artist.empty()) return tr.album;
    if (tr.album.empty()) return tr.artist;
    return tr.artist + " · " + tr.album;
}


int main(int argc, char** argv) {
    // The running .nro (hbloader gives its path), where an update's new one is swapped in
    std::string self = "sdmc:/switch/Songate/Songate.nro";
#ifdef __SWITCH__
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0 && strstr(argv[0], ".nro")) self = argv[0];
    // An update installed while Gatefold was closed some other way (the console restarted): its .nro is swapped in
    // before anything reads this one, and the new one starts instead.
    if (const std::string next = Updater::finishPending(Store::Dir, self); !next.empty() && envHasNextLoad()) {
        envSetNextLoad(next.c_str(), next.c_str());
        return 0;
    }
    Updater::cleanLegacy(Store::Dir, self);   // after the rebranding: 1.x's .nro and overlay, once (the first launch)
#else
    (void)argc;
    (void)argv;
#endif
    App app;
    app.selfPath = self;
    if (app.init()) app.run();
    app.shutdown();
    return 0;
}
