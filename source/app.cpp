/*
 * Gatefold NX · app.cpp
 * Lifecycle, input mapping, play queue and track loading.
 */
#include "app.hpp"
#include "cores.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    metadataInit();
    startMark("sys");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    startMark("init");
    window_ = SDL_CreateWindow("Gatefold", 0, 0, css::W, css::H, 0);
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window_ || !renderer_ || !g.init(renderer_)) return false;
    // Text in a scroll area follows its scroll sub-pixel, so a glide does not end in one-pixel steps.
    ui.onArea = [this](const ScrollArea* a) { g.setShift(0, textShift - (a ? a->y : 0)); };
    startMark("sdl");

    store.load();
    sideW = store.sidebarCollapsed ? css::SideWCollapsed : css::SideW;
    applyTheme();
    pal = target;
    loader.start();
    player.init();   // the UI still runs without audio, so a failure is not fatal
    player.setVolume(store.volume);
    volumeShown = store.volume;
    startMark("audio");
    if (store.background) bgConnect();
    startMark("player");

    // Closing from the HOME menu ends the process without running shutdown(): hold the exit until the
    // settings and the session are written, and save whenever the app loses focus anyway.
    appletLockExit();
    appletHook(&appletCookie_, &App::appletEvent, this);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);

    // First start: use a music folder at the top of the SD card if there is one, else ask.
    if (store.musicRoot.empty())
        for (const auto& d : listDirs("sdmc:/"))
            if (strcasecmp(d.c_str(), "music") == 0) { store.musicRoot = "sdmc:/" + d; break; }   // keep the card's own spelling
    rescan();
    pcm_.resize(Spectrum::N);
    if (store.musicRoot.empty()) navigate(View::PickFolder, -1, "sdmc:/", false);
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
    history.clear();
    areas.clear();
}

void App::shutdown() {
    persistNow();
    Store::flush();
    appletUnhook(&appletCookie_);
    appletSetMediaPlaybackState(false);
    wave.cancel();
    loader.stop();
    if (libCheck_.joinable()) libCheck_.join();
    if (tracksView.thread.joinable()) tracksView.thread.join();
    meta.stop();
    player.shutdown();
    thumbs.stop();
    bg.disconnect();   // the background player carries on by itself
    for (SDL_Texture* t : { now.cover, now.prevCover, now.blur, now.prevBlur }) if (t) SDL_DestroyTexture(t);
    g.shutdown();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    IMG_Quit();
    SDL_Quit();
    psmExit();
    plExit();
    romfsExit();
    appletUnlockExit();
}

void App::appletEvent(AppletHookType type, void* self) {
    auto* app = static_cast<App*>(self);
    if (type == AppletHookType_OnExitRequest || (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)) {
        app->persistNow();
        if (type == AppletHookType_OnExitRequest) Store::flush();
    }
}

void App::persistSoon(float delay) {
    if (persistAt == 0 || time + delay < persistAt) persistAt = time + delay;
}

/** Writes the settings and the session (sessionOnly: just the session, the 20 s save while playing). The files are written on a thread. */
void App::persistNow(bool sessionOnly) {
    if (!sessionOnly) persistAt = 0;
    if (!ready_) return;
    sessionAt = time;
    store.volume = volumeShown;
    if (!sessionOnly) store.save();
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
        ui.beginFrame(dt);
        input();
        update();
        profMark(0);
        draw();
        ui.endFrame();
        frameMs_[frameAt_] = float(interval * 1000);
        workMs_[frameAt_] = float(double(SDL_GetPerformanceCounter() - nowTicks) / freq * 1000);
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

void App::input() {
    SDL_Event e;
    bool touched = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running = false;
        if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION) touched = true;
        const float x = e.tfinger.x * css::W, y = e.tfinger.y * css::H;
        if (e.type == SDL_FINGERDOWN) ui.touchDown(x, y);
        else if (e.type == SDL_FINGERMOTION) ui.touchMove(x, y);
        else if (e.type == SDL_FINGERUP) ui.touchUp(x, y);
    }

    padUpdate(&pad_);
    const u64 down = padGetButtonsDown(&pad_);
    const u64 held = padGetButtons(&pad_);
    // Any input brings the Canvas controls back.
    canvasIdle = (down || touched) ? 0 : canvasIdle + dt;

    // + opens the track menu for the focused track (or the one playing); HOME closes the app, so + no longer exits.
    const bool notice = coverNotice == 1;   // the overlay covers' modal (drawCoverNotice)
    if ((down & HidNpadButton_Plus) && !notice) {
        if (sheet.page != Sheet::Closed) closeSheet();
        else {
            const int t = hoverTrackShown >= 0 ? hoverTrackShown : (route.view == View::Now || canvas) ? now.track : -1;
            if (t >= 0) openTrackMenu(t);
            else toast("Select a track, then press +");
        }
    }
    const bool modal = sheet.page != Sheet::Closed || notice;
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
    } else {
        pointer.idle += dt;
    }
    if (touched || (down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right)))
        pointer.idle = Pointer::Hide + 1;   // the D-pad or a finger took over
    const bool pointing = pointer.idle <= Pointer::Hide || pointer.down;
    if (pointing) canvasIdle = 0;
    if (pointerMoved) {
        if (pointer.down) ui.touchMove(pointer.x, pointer.y);
        else ui.point(ui.itemAt(pointer.x, pointer.y));
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
    // B puts a picked-up slider down before it goes back.
    if (down & HidNpadButton_B) ui.anyEngaged() ? ui.disengage() : notice ? closeCoverNotice() : modal ? sheetBack() : back();
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
    if (rUp.step(up, dt)) ui.navigate(0, -1);
    if (rDown.step(dn, dt)) ui.navigate(0, 1);
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
    if (rLeft.step(lf, dt)) slider ? ui.slide(-1) : ui.navigate(-1, 0);
    if (rRight.step(rt, dt)) { if (slider) ui.slide(1); else if (!intoSettingsPane()) ui.navigate(1, 0); }

    // Right stick scrolls whatever list has focus.
    const HidAnalogStickState rs = padGetStickPos(&pad_, 1);
    if (std::abs(rs.y) > 6000) {
        ui.scrollFocusedArea(-rs.y / 32767.f * 1400 * dt);
        ui.setPadMode(true);
        canvasIdle = 0;   // reading the Canvas lyrics is not idle
    }
}

// ============================================================ update

void App::update() {
    if (coverWaiting_) { coverWaiting_ = false; applyCover(coverWait_); coverWait_ = TrackLoader::Result{}; }
    else if (TrackLoader::Result r; loader.take(r)) applyLoaded(r);
    bgUpdate();
    if (player.takeFinished()) next(true);

    // Gatefold crossfades the palette over --tx (1.1 s) with an ease-out curve.
    pal = store.colourTransition < 0.05f ? target : paletteMix(pal, target, 1 - std::exp(-dt / (store.colourTransition / 3.6f)));
    now.fade = std::min(1.f, now.fade + dt / 0.5f);
    sideW += ((store.sidebarCollapsed ? css::SideWCollapsed : css::SideW) - sideW) * (1 - std::exp(-dt / 0.06f));
    toastT = std::max(0.f, toastT - dt);

    // Only a burst that actually arrived is analysed; between bursts the bars glide (spectrum.hpp).
    spec.params = { canvas ? store.canvasBars : store.vizBands, store.vizSensitivity, store.vizSmoothing };
    if (isPlaying() && player.latestPcm(pcm_.data(), Spectrum::N)) spec.feed(pcm_.data(), player.sampleRate());
    spec.step(dt, isPlaying());
    // The cover's bass pulse (Now playing, and the Canvas when asked for), eased so a kick swells rather than pops.
    const float wantBeat = store.coverPulse > 0 ? std::min(0.03f, spec.beat * 0.07f) * store.coverPulse : 0;
    coverBeat += (wantBeat - coverBeat) * (1 - std::exp(-dt / (wantBeat > coverBeat ? 0.05f : 0.14f)));

    // Saved as things change rather than on exit, which the HOME menu can skip: the volume once the
    // slider settles, the session on every track change, play/pause, and every 20 s while playing.
    if (std::abs(volumeShown - store.volume) > 0.0005f) { store.volume = volumeShown; persistSoon(0.8f); }
    if (now.track != savedTrack || isPlaying() != savedPlaying) {
        savedTrack = now.track;
        savedPlaying = isPlaying();
        persistSoon(0.5f);
    }
    if (isPlaying() && time - sessionAt > 20) persistNow(true);
    if (persistAt > 0 && time >= persistAt) persistNow();

    // The overlay's covers: when many are missing, say so once (drawCoverNotice). Looked at once the
    // library has settled a little after launch, and only with the background player connected.
    // It is a small modal: focus goes to it, B or Not now closes it. Not over the Canvas or the track menu.
    if (coverNotice == 0 && player.remote() && thumbs.running() && time > 6 && !lib.tracks.empty() && !canvas && sheet.page == Sheet::Closed) {
        coverMissing = 0;
        for (const auto& t : lib.tracks) coverMissing += !thumbs.has(t.path);
        coverNotice = coverMissing * 10 > int(lib.tracks.size()) ? 1 : 2;
        coverNoticeAt = time;
        if (coverNotice == 1) noticeFocus_ = ui.focusedId();
    }
    if (coverNotice == 1 && !player.remote()) closeCoverNotice();

    // The sidebar's progress card: a job that has run for a second shows, and fades out once nothing runs.
    {
        Job j;
        const bool busy = backgroundJob(j);
        jobSince = busy ? jobSince + dt : 0;
        if (busy) jobLast = j;
        const float want = busy && jobSince > 1 ? 1.f : 0.f;
        jobShown += (want - jobShown) * (1 - std::exp(-dt / 0.15f));
        if (jobShown < 0.005f && want == 0) jobShown = 0;
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

    if (meta.pump(renderer_)) indexDirty = true;
    if (indexDirty && time - indexAt > 1.5) rebuildIndex();
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

void App::openGenre(const std::string& name) {
    if (!name.empty()) navigate(View::Genre, -1, name);
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

void App::openArtist(const std::string& name) { navigate(View::Artist, -1, name.empty() ? "Unknown artist" : name); }

// ============================================================ play queue

bool App::isPlaying() const { return player.loaded() && !player.paused(); }

int App::currentQueuePos() const { return q.pos >= 0 && q.pos < int(q.order.size()) ? q.order[q.pos] : -1; }

void App::playTracks(const std::vector<int>& tracks, int startPos, const std::string& name, bool shuffle) {
    if (tracks.empty()) return;
    q.tracks = tracks;
    q.name = name;
    q.failed = 0;
    startPos = std::clamp(startPos, 0, int(tracks.size()) - 1);
    if (shuffle) store.order = Order::Shuffle;
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

void App::playAt(int orderPos) {
    if (q.order.empty()) return;
    q.pos = std::clamp(orderPos, 0, int(q.order.size()) - 1);
    loadTrack(q.tracks[q.order[q.pos]]);
    if (!player.loaded()) {
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
        appletSetMediaPlaybackState(false);
        return;
    }
    int p = q.pos + 1;
    if (p >= int(q.order.size())) {
        if (store.order == Order::Default && natural) {
            player.stop();
            appletSetMediaPlaybackState(false);
            return;
        }
        if (store.order == Order::Shuffle) std::shuffle(q.order.begin(), q.order.end(), rng);
        p = 0;
    }
    playAt(p);
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
    appletSetMediaPlaybackState(!player.paused());
}

void App::seekBy(double d) {
    if (!player.loaded()) return;
    double p = player.position() + d;
    if (now.info.duration > 0) p = std::min(p, now.info.duration - 1);
    player.seek(std::max(0.0, p));
}

void App::setOrder(Order o) {
    const int cur = currentQueuePos();
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

// ============================================================ track loading

/**
 * Shows trackIdx as the one playing; start: also start it (false when the background player already did).
 * The file's tags, format, lyrics and cover are read by the loader (applyLoaded), so the frame of a track
 * change is not spent on the SD card. Until they arrive, a moment later, the previous track's text and
 * lyrics stay as they were (frozen where they were), and then everything changes at once: showing the
 * library's tags first made the lyrics read "No lyrics" and the Canvas text jump for a frame or two.
 */
void App::loadTrack(int trackIdx, bool paused, bool start) {
    const Track& t = lib.tracks[trackIdx];
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
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));

    wave.request(t.path);
    meta.wantTags(trackIdx);
    if (!start) return;
    if (player.remote()) {
        bgSyncQueue();
        bg.playAt(q.pos, paused, 0);
        bgs.awaitUntil = time + 1.5;
        if (!paused) appletSetMediaPlaybackState(true);
        return;
    }
    if (player.load(t.path, paused) && !paused) appletSetMediaPlaybackState(true);
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
    bgs.cfgRead = bg.config(bgs.cfg);
}

/** Takes the background player's queue as Gatefold's own (at launch, and after the overlay reshuffled it). */
bool App::bgAdoptQueue() {
    BgQueue bq;
    if (!bg.getQueue(bq) || bq.paths.empty()) return false;
    std::vector<int> keep, tracks;   // its index → ours, −1 for files this library does not have
    for (const auto& p : bq.paths) {
        auto it = pathIndex.find(p);
        keep.push_back(it == pathIndex.end() ? -1 : int(tracks.size()));
        if (it != pathIndex.end()) tracks.push_back(it->second);
    }
    if (tracks.empty()) return false;
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
    store.order = Order(std::clamp(bq.mode, 0, 3));
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

/** Sends Gatefold's queue when it changed here: a new list, a reshuffle, an order change, a track queued. */
void App::bgSyncQueue() {
    // An empty queue here (no music folder yet, a library that lacks its files) never clears the player's.
    if (!player.remote() || q.tracks.empty()) return;
    if (q.tracks == bgs.tracks && q.order == bgs.order && int(store.order) == bgs.mode && q.name == bgs.name) return;
    BgQueue bq;
    bq.name = q.name;
    bq.paths.reserve(q.tracks.size());
    for (int t : q.tracks) bq.paths.push_back(lib.tracks[t].path);
    bq.order = q.order;
    bq.pos = q.pos;
    bq.mode = int(store.order);
    if (!bg.setQueue(bq)) return;
    for (int t : q.tracks) thumbFor(t, false);
    bgs.tracks = q.tracks;
    bgs.order = q.order;
    bgs.mode = bq.mode;
    bgs.name = q.name;
}

/** Every frame while connected: what the player did on its own (next track, the overlay's buttons), and ours to it. */
void App::bgUpdate() {
    if (!player.remote()) return;
    if (!bg.poll(time)) {
        player.setRemote(nullptr);
        toast("The background player stopped, so Gatefold plays on its own");
        return;
    }
    bgSyncQueue();
    const BgState& s = bg.state();
    if (s.queueGen != bgs.queueGen) {
        bgs.queueGen = s.queueGen;
        if (s.queueBy != BgClient::ByGatefold) bgAdoptQueue();
    }
    if (s.trackGen != bgs.trackGen) { bgs.trackGen = s.trackGen; bgs.awaitUntil = 0; }
    if (time >= bgs.awaitUntil && s.pos >= 0 && s.pos < int(q.order.size()) && s.pos != q.pos) {
        q.pos = s.pos;
        loadTrack(q.tracks[q.order[q.pos]], false, false);
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
    toast("Added to “" + p.name + "”");
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
            bgSyncQueue();
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
        toast("Playing in Gatefold only: music stops when you leave it");
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
        parseLyrics(r.lyrics);
        // The new texts render this frame; the cover is uploaded the next one (update), not both at once.
        coverWait_ = std::move(r);
        coverWaiting_ = true;
        return;
    }
    applyCover(r);
}

/** The loaded cover: uploaded, crossfaded in, and the palette from it. */
void App::applyCover(TrackLoader::Result& r) {
    if (r.gen != loadGen || now.track < 0) { r.release(); return; }
    // Keep the outgoing cover for the 500 ms crossfade.
    if (now.prevCover) SDL_DestroyTexture(now.prevCover);
    if (now.prevBlur) SDL_DestroyTexture(now.prevBlur);
    now.prevCover = now.cover;
    now.prevBlur = now.blur;
    now.cover = now.blur = nullptr;
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
        std::stable_sort(now.lyrics.begin(), now.lyrics.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    }
    while (!now.lyrics.empty() && now.lyrics.back().text.empty() && !now.synced) now.lyrics.pop_back();
}

// ============================================================ navigation

ScrollArea& App::area(const char* key, int ref) { return areas[uid(key, ref)]; }

void App::navigate(View v, int ref, const std::string& query, bool push) {
    canvas = false;
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
}

// ============================================================ helpers

std::vector<int> App::listTracks(View v, int ref, const std::string&) const {
    std::vector<int> out;
    switch (v) {
    case View::Queue: return q.tracks;
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
                for (const auto& path : p.tracks)
                    if (auto it = pathIndex.find(path); it != pathIndex.end()) out.push_back(it->second);
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


int main(int, char**) {
    App app;
    if (app.init()) app.run();
    app.shutdown();
    return 0;
}
