/*
 * Gatefold NX · shell.cpp
 * The frame around every view: #bg, #titlebar, #sidebar, #playerbar (app.css).
 */
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app.hpp"
#include "icons.inc"

namespace {

/** var(--ease) = cubic-bezier(.22, .7, .18, 1), as ambient.js evaluates it. */
float ease(float t) {
    const float x1 = 0.22f, y1 = 0.7f, x2 = 0.18f, y2 = 1;
    auto bx = [&](float u) { return 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u; };
    auto by = [&](float u) { return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u; };
    float lo = 0, hi = 1;
    for (int i = 0; i < 18; i++) { const float m = (lo + hi) / 2; if (bx(m) < t) lo = m; else hi = m; }
    return by((lo + hi) / 2);
}

}  // namespace

void App::draw() {
    hoverTrackShown = hoverTrack;
    hoverTrack = -1;
    hoverQueueShown = hoverQueue;
    hoverQueue = -1;
    hoverUpNextShown = hoverUpNext;
    hoverUpNext = false;
    ui.setModal(sheet.page != Sheet::Closed || (coverNotice == 1 && !canvas) || sleepPromptAt > 0);
    if (canvas) {
        drawCanvas();
        profMark(2);
        drawSheet();
        drawSleepPrompt();
        drawToast();
        if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);
        drawFocusRing();
        drawPointer();
        drawFrameTimes();
        profMark(5);
        return;
    }
    drawBackground();
    profMark(1);

    const float mx = std::round(sideW), my = css::TbH, mw = css::W - mx, mh = css::H - css::TbH - css::BarH;
    ui.region(Region::Main);
    g.clip(mx, my, mw, mh);
    switch (route.view) {
    case View::Now: drawNow(mx, my, mw, mh); break;
    case View::Library: drawLibrary(mx, my, mw, mh); break;
    case View::Albums: drawAlbums(mx, my, mw, mh); break;
    case View::Artists: drawArtists(mx, my, mw, mh); break;
    case View::Artist: drawArtist(mx, my, mw, mh); break;
    case View::Genres: drawArtists(mx, my, mw, mh, true); break;
    case View::Genre: drawArtist(mx, my, mw, mh, true); break;
    case View::Album: drawAlbum(mx, my, mw, mh); break;
    case View::Folders: drawFolders(mx, my, mw, mh); break;
    case View::Settings: drawSettings(mx, my, mw, mh); break;
    case View::PickFolder: drawPicker(mx, my, mw, mh); break;
    default: drawTrackList(mx, my, mw, mh); break;
    }
    g.unclip();
    profMark(2);

    ui.region(Region::Side);
    drawSidebar();
    ui.region(Region::Title);
    drawTitlebar();
    profMark(3);
    ui.region(Region::Bar);
    drawPlayerbar();
    profMark(4);
    drawCoverNotice();
    drawSheet();
    drawSleepPrompt();
    drawToast();
    if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);   // Dim the screen, docked
    drawFocusRing();
    drawPointer();
    drawFrameTimes();
    profMark(5);
    warmCanvas();
    profMark(6);
}

/** Settings → About → Frame times: the last 120 frames as bars (interval, and the work before the present inside), 16.7 and 33.3 ms marked. */
void App::drawFrameTimes() {
#ifndef GF_DEV
    return;   // dev builds only: a release has no Diagnostics card, so a setting saved by a dev build stays hidden
#endif
    if (!store.frameTimes) return;
    const float w = 120 * 3, h = 70, x = css::W - w - 16, y = 56;
    const std::vector<std::string> launch = g.wrap(startStats_, { 11 }, int(w), 3);   // the launch line wraps: it was cut off
    g.roundRect(x - 8, y - 8, w + 16, h + 58 + 15 * launch.size() + 4, 10, { 0, 0, 0 }, 0.6f);
    const float scale = h / 50;   // 50 ms at the top
    float sum = 0, worst = 0, work = 0;
    int slow = 0;
    for (int i = 0; i < 120; i++) {
        const int k = (frameAt_ + i) % 120;
        const float f = frameMs_[k], wk = workMs_[k];
        sum += f;
        work = std::max(work, wk);
        worst = std::max(worst, f);
        slow += f > 20;
        const float bh = std::min(h, f * scale), wh = std::min(h, wk * scale);
        g.rect(x + i * 3, y + h - bh, 2, bh, f > 20 ? Rgb{ 1, 0.35f, 0.3f } : Rgb{ 0.4f, 0.85f, 0.5f }, 0.85f);
        g.rect(x + i * 3, y + h - wh, 2, wh, { 1, 1, 1 }, 0.55f);
    }
    for (float ms : { 16.7f, 33.3f }) g.rect(x, y + h - ms * scale, w, 1, { 1, 1, 1 }, 0.35f);
    // The figures change twice a second: a new text texture every frame would cost what it measures.
    if (time - frameStatsAt_ > 0.5 || frameStats_.empty()) {
        frameStatsAt_ = time;
        char buf[96];
        snprintf(buf, sizeof buf, "avg %.1f  max %.1f  work max %.1f ms  slow %d", sum / 120, worst, work, slow);
        frameStats_ = buf;
        // Average ms per frame since the last update: upd(ate) bg view side bar rest warm pres(ent)
        const double n = std::max(1, profFrames_);
        char pb[128];
        snprintf(pb, sizeof pb, "upd %.1f bg %.1f view %.1f side %.1f bar %.1f rest %.1f warm %.1f pres %.1f", profAcc_[0] / n,
                 profAcc_[1] / n, profAcc_[2] / n, profAcc_[3] / n, profAcc_[4] / n, profAcc_[5] / n, profAcc_[6] / n, profAcc_[7] / n);
        profStats_ = pb;
        std::fill(std::begin(profAcc_), std::end(profAcc_), 0.0);
        profFrames_ = 0;
    }
    g.text(frameStats_, { css::Xs, Weight::SemiBold }, x, y + h + 8, { 1, 1, 1 }, 0.9f);
    g.text(profStats_, { 11 }, x, y + h + 26, { 1, 1, 1 }, 0.8f);
    for (size_t i = 0; i < launch.size(); i++) g.text(launch[i], { 11 }, x, y + h + 42 + 15 * i, { 1, 1, 1 }, 0.8f);
}

/** The left stick's pointer: a translucent white dot in a soft dark ring, readable on any cover without glaring. */
void App::drawPointer() {
    const float a = pointer.down ? 1.f : std::clamp(1 - (pointer.idle - Pointer::Hide + 0.4f) / 0.4f, 0.f, 1.f);
    if (a <= 0) return;
    const float r = pointer.down ? 7 : 9;
    g.glow(pointer.x, pointer.y + 1, r + 9, { 0, 0, 0 }, 0.30f * a);
    g.circle(pointer.x, pointer.y, r + 1.5f, { 0, 0, 0 }, 0.30f * a);
    g.circle(pointer.x, pointer.y, r, { 1, 1, 1 }, (pointer.down ? 0.75f : 0.55f) * a);
}

// ------------------------------------------------------------------ #bg

void App::drawBackground() {
    g.fill(pal.bg);

    // .bg-art: the cover, blurred, inset −6%, opacity .62, crossfading with the previous one
    const float ix = css::W * 0.06f, iy = css::H * 0.06f;
    const SDL_Rect full{ int(-ix), int(-iy), int(css::W + ix * 2), int(css::H + iy * 2) };
    auto art = [&](SDL_Texture* t, float a) {
        if (!t || a <= 0) return;
        SDL_SetTextureAlphaMod(t, Uint8(std::clamp(a, 0.f, 1.f) * 255));
        SDL_RenderCopy(renderer_, t, nullptr, &full);
    };
    const float k = ease(now.fade);
    art(now.prevBlur, 0.62f * (1 - k));
    art(now.blur, 0.62f * k);

    // .aurora: ambient.js — four 78vmax radial gradients drifting there and back
    struct Blob { float x, y, dx, dy, s, T, a; };
    const float m = std::max(css::W, css::H) / 100.f;
    const Blob blobs[4] = {
        { -24 * m, -30 * m, 14, 10, 1.15f, 46, 0.62f },
        { css::W + 30 * m - 78 * m, -18 * m, -12, 14, 0.9f, 58, 0.62f },
        { 8 * m, css::H + 44 * m - 78 * m, 16, -12, 1.1f, 64, 0.62f },
        { css::W + 18 * m - 78 * m, css::H + 36 * m - 78 * m, -14, -8, 1.2f, 52, 0.5f },
    };
    const float t = store.motion ? float(time) : 0;
    for (int i = 0; i < 4; i++) {
        const Blob& b = blobs[i];
        const float ph = std::fmod(t / b.T, 2.f), e = ease(ph < 1 ? ph : 2 - ph);
        const float r = 39 * m * (1 + (b.s - 1) * e);
        g.glow(b.x + 39 * m + b.dx * m * e, b.y + 39 * m + b.dy * m * e, r, pal.blobs[i], b.a);
    }

    // .veil: radial-gradient(120% 90% at 30% 20%, transparent, bg 55% at 70%) + linear bg 20% → 70%
    g.veil(css::W * 0.3f, css::H * 0.2f, css::W * 1.2f, css::H * 0.9f, pal.bg, 0.55f);
    g.rectV(0, 0, css::W, css::H, pal.bg, 0.2f, pal.bg, 0.7f);
}

// ------------------------------------------------------------------ #titlebar

void App::drawTitlebar() {
    const float cy = css::TbH / 2;
    float x = 10;
    if (iconButton(uid("tb.sidebar"), ICON_SIDEBAR, x + 17, cy)) { store.sidebarCollapsed = !store.sidebarCollapsed; store.save(); }
    x += 34 + 6;

    // .brand: the Gatefold mark (accent, fold in accent 62% + black) and the name
    if (sideW > 150) {
        const Rgb fold{ pal.accent.r * 0.62f, pal.accent.g * 0.62f, pal.accent.b * 0.62f };
        g.glow(x + 9.5f, cy, 16, pal.glow, 0.5f);
        g.icon(ICON_BRAND_G, x + 9.5f, cy, 19, pal.accent);
        g.icon(ICON_BRAND_FOLD, x + 9.5f, cy, 19, fold);
        g.textC("Gatefold", { css::Md, Weight::SemiBold }, x + 19 + 9, cy, pal.text);
    }

    // .search: centred in what is left of the bar; opens the console keyboard
    const float left = std::max(sideW + 12, x + 120), right = css::W - 150;
    const float sw = std::min(520.f, right - left), sx = left + (right - left - sw) / 2, sy = cy - 16;
    const Id sid = uid("tb.search");
    if (ui.item(sid, { sx, sy, sw, 32 }, 9)) openSearch();
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 9, hot ? pal.surface : pal.text, hot ? 1.f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 9, 1, hot ? pal.accent : pal.text, hot ? 0.55f : 0.09f);
    g.icon(ICON_SEARCH, sx + 12 + 8, cy, 16, pal.text3);
    const std::string q = route.view == View::Search ? route.query : std::string();
    g.textC(q.empty() ? "Artist, album or track" : q, { css::Sm }, sx + 12 + 16 + 8, cy, q.empty() ? pal.text3 : pal.text, 1, Align::Left,
            int(sw - 90));
    // <kbd>: the button that opens search
    const Font kf{ css::Xs };
    const float kw = g.measure("Y", kf) + 12;
    g.roundStroke(sx + sw - 12 - kw, cy - 9, kw, 18, 5, 1, pal.text, 0.15f);
    g.textC("Y", kf, sx + sw - 12 - kw / 2, cy, pal.text3, 1, Align::Center);

    // Where Windows has its caption buttons: the clock and the battery.
    const std::time_t tt = std::time(nullptr);
    const std::tm* lt = std::localtime(&tt);
    char clock[8];
    std::strftime(clock, sizeof clock, "%H:%M", lt);
    u32 pct = 0;
    const bool haveBattery = R_SUCCEEDED(psmGetBatteryChargePercentage(&pct));
    float rx = css::W - 18;
    if (haveBattery) {
        const float bw = 24, bh = 12, bx = rx - bw - 3, by = cy - bh / 2;
        g.roundStroke(bx, by, bw, bh, 3, 1.5f, pal.text2);
        g.roundRect(bx + bw + 1, cy - 3, 2, 6, 1, pal.text2);
        const bool charging = battery.charging == 1;
        g.roundRect(bx + 2.5f, by + 2.5f, (bw - 5) * pct / 100.f, bh - 5, 1.5f, pct <= 15 && !charging ? Rgb{ 0.9f, 0.38f, 0.31f } : pal.text2);
        // Charging: the bolt over the fill, with a dark edge so it reads on the fill and off it.
        if (charging) {
            g.icon(ICON_BOLT, bx + bw / 2 + 0.5f, cy + 0.5f, 13, { 0, 0, 0 }, 0.55f);
            g.icon(ICON_BOLT, bx + bw / 2, cy, 12, pal.text);
        }
        rx = bx - 10;
        // How long to full or to empty, once the charge has moved long enough to tell (App::sampleBattery).
        // Settings → Appearance → Title bar can hide either.
        std::string label = store.batteryPercent ? std::to_string(pct) + "%" : "";
        if (store.batteryEstimate && !battery.est.empty()) label += (label.empty() ? "" : "  ·  ") + battery.est;
        if (!label.empty()) rx -= g.textC(label, { css::Sm }, rx, cy, pal.text3, 1, Align::Right) + 14;
        else rx -= 4;
    }
    g.textC(clock, { css::Sm, Weight::SemiBold }, rx, cy, pal.text2, 1, Align::Right);
}

// ------------------------------------------------------------------ #sidebar

void App::drawSidebar() {
    const float x0 = 10, w = sideW - 20;
    const bool collapsed = sideW < 120;
    float y = css::TbH + 6;
    const Font navFont{ css::Base, Weight::SemiBold };

    auto navItem = [&](const char* key, const char* icon, const std::string& label, bool active, const std::string& badge, float h, Font f,
                       int ref = 0, bool playingHere = false) {
        const Id id = uid(key, ref);
        const bool hit = ui.item(id, { x0, y, w, h }, 9);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (active) {
            g.roundRect(x0, y, w, h, 9, pal.accent, 0.09f);
            g.roundRect(x0, y + 10, 3, h - 20, 1.5f, pal.accent);
        } else if (hot) {
            g.roundRect(x0, y, w, h, 9, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
        }
        const Rgb fg = active || hot ? pal.text : pal.text2;
        if (collapsed) {
            g.icon(icon, x0 + w / 2, y + h / 2, 19, active ? pal.accentInk : fg);
        } else {
            g.icon(icon, x0 + 12 + 9.5f, y + h / 2, 19, active ? pal.accentInk : fg);
            const float tx = x0 + 12 + 19 + 12;
            float right = x0 + w - 12;
            if (playingHere) { eqGlyph(right - 13, y + h / 2, pal.accentInk); right -= 22; }
            else if (!badge.empty()) right -= g.textC(badge, { css::Xs }, right, y + h / 2, pal.text3, 1, Align::Right) + 8;
            g.textC(label, f, tx, y + h / 2, fg, 1, Align::Left, int(right - tx));
        }
        y += h + 2;
        return hit;
    };

    const View v = route.view;
    if (navItem("nav.now", ICON_DISC, "Now playing", v == View::Now, "", 38, navFont)) navigate(View::Now);
    // Settings → Appearance can hide any of these (store.hiddenTabs); Now playing and Settings always stay.
    const unsigned hide = store.hiddenTabs;
    if (!(hide & 1) &&
        navItem("nav.queue", ICON_LIST, "Playlist", v == View::Queue, q.tracks.empty() ? "" : std::to_string(q.tracks.size()), 38, navFont))
        navigate(View::Queue);
    // Gatefold 1.4.2's order, with Albums and Folders as tabs of their own (each can be hidden).
    if (!(hide & 32) && navItem("nav.albums", ICON_V_GRID, "Albums", v == View::Albums || (v == View::Album && libFrom == View::Albums),
                                std::to_string(lib.albums.size()), 38, navFont)) {
        libFrom = View::Albums;
        navigate(View::Albums);
    }
    if (!(hide & 2) &&
        navItem("nav.artists", ICON_MIC, "Artists", v == View::Artists || v == View::Artist, std::to_string(artists.size()), 38, navFont))
        navigate(View::Artists);
    if (!(hide & 4) && navItem("nav.genres", ICON_TAG, "Genres", v == View::Genres || v == View::Genre, std::to_string(genres.size()), 38, navFont))
        navigate(View::Genres);
    // A folder page lights the tab it belongs to: Folders, or Library when opened from its Folders tab
    // (or when the Folders tab is hidden, so something stays lit).
    const bool folderInLibrary = v == View::Folders && (folderFrom == View::Library || (hide & 16));
    if (!(hide & 16) && navItem("nav.folders", ICON_FOLDER, "Folders", v == View::Folders && !folderInLibrary,
                                std::to_string(std::max(0, int(lib.folders.size()) - int(lib.rootFolders.size()))), 38, navFont)) {
        navigate(View::Folders, lib.rootFolders.empty() ? -1 : lib.rootFolders[0]);
        folderFrom = View::Folders;   // the tab itself, even when clicked from Library
    }
    if (!(hide & 8) && navItem("nav.library", ICON_GRID, "Library", v == View::Library || (v == View::Album && libFrom == View::Library) || folderInLibrary,
                               std::to_string(lib.tracks.size()), 38, navFont)) {
        libFrom = View::Library;
        navigate(View::Library);
    }

    // .nav-sep + .nav-label with the new-playlist button
    y += 8;
    g.rect(x0 + 8, y, w - 16, 1, pal.text, 0.09f);
    y += 11;
    if (!collapsed) {
        kicker("Playlists", x0 + 12, y + 12, pal.text3, 0.08f);
        if (iconButton(uid("nav.newpl"), ICON_PLUS, x0 + w - 12, y + 12, false, 24, 14)) openNewList();   // your own, or a smart one
        y += 28;
    }

    // #playlists: Liked, then the folders added as playlists
    // The progress card sits above Settings while something runs in the background; the playlists make room.
    const float jobH = (collapsed ? 30 : 52) * jobShown;
    const float footH = 2 + 8 + 1 + 11 + 38 + jobH;
    const float listTop = y, listH = css::H - css::BarH - 12 - footH - listTop;
    ScrollArea& sa = area("side.playlists");
    const int n = 1 + int(store.playlists.size());
    sa.view = { 0, listTop, sideW, listH };
    sa.content = n * 34.f;
    ui.pushArea(&sa);
    g.clip(0, listTop, sideW, listH);
    y = listTop - sa.y;
    const Font plFont{ css::Sm };
    const bool likedPlaying = q.name == "Liked" && isPlaying();
    if (navItem("nav.pl", ICON_HEART, "Liked", v == View::List && route.ref == -1, std::to_string(store.liked.size()), 32, plFont, -1, likedPlaying))
        navigate(View::List, -1);
    for (int i = 0; i < int(store.playlists.size()); i++) {
        const FolderPlaylist& p = store.playlists[i];
        const int f = p.folder() ? lib.folderByPath(p.path) : -1;
        const int count = p.own() ? int(p.tracks.size()) : p.smart() ? int(smartTracks(p.path).size()) : f >= 0 ? lib.folders[f].last - lib.folders[f].first : 0;
        if (navItem("nav.pl", p.smart() ? smartIcon(p.path) : ICON_LIST, p.name, v == View::List && route.ref == i, std::to_string(count), 32, plFont, i, q.name == p.name && isPlaying()))
            navigate(View::List, i);
    }
    g.unclip();
    ui.popArea();

    // .side-foot
    y = css::H - css::BarH - 12 - 38 - 2;
    if (jobShown > 0.01f) drawJobCard(x0, y - 11 - 6, w, collapsed);
    g.rect(x0 + 8, y - 11, w - 16, 1, pal.text, 0.09f);
    if (navItem("nav.settings", ICON_GEAR, "Settings", v == View::Settings, "", 38, navFont)) navigate(View::Settings);
}

// ------------------------------------------------------------------ #playerbar

void App::drawSeek(float x, float y, float w, float h, Id id) {
    const double dur = now.info.duration;
    ui.item(id, { x, y - 4, w, h + 8 }, 6, Slider);
    float frac = dur > 0 ? float(std::clamp(player.position() / dur, 0.0, 1.0)) : 0;
    const float drag = ui.sliderDrag(id);
    float shown = drag >= 0 ? std::clamp((drag - x) / w, 0.f, 1.f) : frac;
    if (dur > 0) {
        if (const int st = ui.sliderStep(id)) seekBy(st * 5);
        if (ui.sliderReleased(id)) {
            const float f = std::clamp((ui.lastDragX() - x) / w, 0.f, 1.f);
            player.seek(f * dur);
            shown = f;
        }
    }
    const float cy = y + h / 2;
    if (wave.ready() && wave.path() == (now.track >= 0 ? lib.tracks[now.track].path : std::string())) {
        // .seek.wave: 2 px bars every 3 px; played part in the accent-2 → accent gradient, the rest dim
        const auto& pk = wave.peaks();
        const int bars = int(w / 3);
        const float px = shown * w, cur = frac * w;
        for (int i = 0; i < bars; i++) {
            const size_t a = size_t(i) * pk.size() / bars, b = std::max(a + 1, size_t(i + 1) * pk.size() / bars);
            float v = 0;
            for (size_t j = a; j < b && j < pk.size(); j++) v = std::max(v, pk[j]);
            const float bh = std::max(1.5f, v * h * 0.9f), bx = x + i * 3;
            const float t = float(i) / bars;
            if (i * 3 < px) {
                const Rgb c{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                             pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
                g.rect(bx, cy - bh / 2, 2, bh, c);
            } else if (drag >= 0 && i * 3 < cur) {
                g.rect(bx, cy - bh / 2, 2, bh, pal.text3);
            } else {
                g.rect(bx, cy - bh / 2, 2, bh, pal.text, 0.13f);
            }
        }
        if (ui.engaged(id)) {   // picked up with A: a knob where ←/→ move it
            g.circle(x + shown * w, cy, 9, pal.accent);
            g.circle(x + shown * w, cy, 6, pal.text);
        }
    } else {
        // .seek .track / .fill / .knob
        const bool hot = drag >= 0 || (ui.focused(id) && ui.showFocusRing());
        const float th = hot ? 6 : 4;
        g.roundRect(x, cy - th / 2, w, th, th / 2, pal.text, 0.16f);
        g.roundRectH(x, cy - th / 2, w * shown, th, th / 2, pal.accent2, pal.accent);
        if (ui.engaged(id)) g.circle(x + w * shown, cy, 9, pal.accent);
        if (hot) g.circle(x + w * shown, cy, 6, pal.text);
    }
    if (drag >= 0 && dur > 0) {
        // .seek .tip
        const std::string tip = fmtTime(shown * dur);
        const float tw = g.measure(tip, { css::Xs }) + 16;
        const float tx = std::clamp(x + w * shown - tw / 2, x - 20, x + w - tw + 20);
        g.roundRect(tx, y - 30, tw, 24, 6, pal.surface2);
        g.textC(tip, { css::Xs }, tx + tw / 2, y - 18, pal.text, 1, Align::Center);
    }
}

void App::drawPlayerbar() {
    const float y0 = css::H - css::BarH, cy = y0 + css::BarH / 2;
    g.rect(0, y0, css::W, css::BarH, pal.bg, 0.8f);   // --panel-strong
    g.rect(0, y0, css::W, 1, pal.text, 0.09f);

    const float pad = 18, gap = 20, total = css::W - pad * 2 - gap * 2;
    const float leftW = total / 4, centerW = total / 2;
    const float lx = pad, cx0 = pad + leftW + gap, rx = cx0 + centerW + gap;
    const bool hasTrack = now.track >= 0;

    // .pb-track
    const SDL_FRect artR{ lx, cy - 28, 56, 56 };
    const Id artId = uid("pb.art");
    if (ui.item(artId, artR, 8)) navigate(View::Now);
    // A soft halo all around the cover, like the play button's (three faint layers that ease into the
    // background), a little wider: it reaches ~16 px past the sides and just past the corners.
    for (float r : { 36.f, 40.f, 44.f }) g.glow(artR.x + 28, artR.y + 28, r, pal.glow, 0.26f);
    if (now.cover) {
        if (now.prevCover && now.fade < 1) g.image(now.prevCover, artR.x, artR.y, 56, 56, 8);
        g.image(now.cover, artR.x, artR.y, 56, 56, 8, ease(now.fade));
    } else {
        g.roundRect(artR.x, artR.y, 56, 56, 8, pal.surface2);
        g.icon(ICON_MUSIC, artR.x + 28, artR.y + 28, 22, pal.text3, 0.6f);
    }
    const float metaX = lx + 56 + 12, heartX = lx + leftW - 17;
    const int metaW = int(heartX - 17 - 12 - metaX);
    g.text(hasTrack ? now.info.title : "—", { css::Base, Weight::SemiBold }, metaX, cy - 21, pal.text, 1, Align::Left, metaW);
    g.text(hasTrack ? now.info.artist : "", { css::Sm }, metaX, cy + 1, pal.text3, 1, Align::Left, metaW);
    const bool liked = hasTrack && store.isLiked(lib.tracks[now.track].path);
    if (iconButton(uid("pb.like"), liked ? ICON_HEART_F : ICON_HEART, heartX, cy, liked) && hasTrack) {
        store.toggleLiked(lib.tracks[now.track].path);
        store.save();
        toast(liked ? "Removed from Liked" : "Added to Liked");
    }

    // .pb-center: controls (42 px) + 6 px + seek (30 px), centred in the bar
    // controls 42 px, gap 6 px, seek 30 px (seekY is the seek row's centre)
    const float ctrlY = y0 + (css::BarH - 78) / 2 + 21, seekY = ctrlY + 21 + 6 + 15;
    const float mid = cx0 + centerW / 2;
    const char* orderIcon = store.order == Order::Shuffle ? ICON_SHUFFLE : store.order == Order::RepeatOne ? ICON_REPEAT1
                          : store.order == Order::RepeatAll ? ICON_REPEAT : ICON_ORDER;
    if (iconButton(uid("pb.order"), orderIcon, mid - 21 - 10 - 34 - 10 - 17, ctrlY, store.order != Order::Default)) {
        setOrder(Order((int(store.order) + 1) % 4));
        static const char* names[] = { "Default order", "Repeat playlist", "Repeat track", "Shuffle" };
        toast(names[int(store.order)]);
        store.save();
    }
    if (iconButton(uid("pb.prev"), ICON_PREV, mid - 21 - 10 - 17, ctrlY)) prev();
    {
        // .play-btn: 42 px circle in --text, icon in --bg
        const Id pid = uid("pb.play");
        if (ui.item(pid, { mid - 21, ctrlY - 21, 42, 42 }, 21)) togglePlay();
        const float s = ui.pressed(pid) ? 0.95f : (ui.focused(pid) && ui.showFocusRing()) ? 1.06f : 1.f;
        // box-shadow: 0 6px 22px -8px glow 90%, drawn as a soft halo all around the button. Three faint layers
        // instead of one strong one, so it eases into the background rather than ringing the edge; it is gone
        // 9 px out, before the seek wave (27 px below the centre).
        for (float r : { 24.f, 27.f, 30.f }) g.glow(mid, ctrlY, r, pal.glow, 0.3f);
        g.circle(mid, ctrlY, 21 * s, pal.text);
        if (isPlaying()) g.icon(ICON_PAUSE, mid, ctrlY, 20 * s, pal.bg);
        else g.icon(ICON_PLAY, mid, ctrlY, 20 * s, pal.bg);
    }
    if (iconButton(uid("pb.next"), ICON_NEXT, mid + 21 + 10 + 17, ctrlY)) next(false);
    if (iconButton(uid("pb.sac"), ICON_PAUSE_C, mid + 21 + 10 + 34 + 10 + 17, ctrlY, stopAfterCurrent)) {
        stopAfterCurrent = !stopAfterCurrent;
        toast(stopAfterCurrent ? "Stop after the current track" : "Keep playing after this track");
    }

    // .pb-seek: 48 | 1fr | 48, gap 10
    const float seekW = std::min(760.f, centerW), sx = mid - seekW / 2;
    g.textC(hasTrack ? fmtTime(player.position()) : "0:00", { css::Xs }, sx + 48, seekY, pal.text3, 1, Align::Right);
    g.textC(hasTrack ? fmtTime(now.info.duration) : "0:00", { css::Xs }, sx + seekW - 48, seekY, pal.text3);
    drawSeek(sx + 58, seekY - 15, seekW - 116, 30, uid("pb.seek"));

    // .pb-right: queue, volume, canvas
    float x = rx + leftW;
    if (iconButton(uid("pb.canvas"), ICON_CANVAS, x - 17, cy)) openCanvas(true);
    x -= 34 + 14;   // Gatefold has the mini-player button here; keep the slider clear of the Canvas button
    float vol = volumeShown;
    slider(uid("pb.vol"), x - 110, cy, 110, volumeShown, &vol);
    if (vol != volumeShown) { volumeShown = vol; player.setVolume(vol); }
    x -= 110 + 4 + 7;
    static float beforeMute = 1;
    if (iconButton(uid("pb.mute"), volumeShown <= 0.001f ? ICON_VOL_X : ICON_VOL, x - 17, cy)) {
        if (volumeShown > 0.001f) { beforeMute = volumeShown; volumeShown = 0; }
        else volumeShown = beforeMute > 0.01f ? beforeMute : 1;
        player.setVolume(volumeShown);
    }
    x -= 34 + 8;
    if (iconButton(uid("pb.queue"), ICON_QUEUE, x - 17, cy, route.view == View::Queue)) navigate(View::Queue);
    // The sleep timer: lit while it runs (or stop-after is on, its "End of this track"), the time left beside it.
    x -= 34 + 8;
    const double sl = sleepLeft();
    if (iconButton(uid("pb.sleep"), ICON_MOON, x - 17, cy, sl > 0)) openSleep();
    if (sl > 0) g.textC(fmtTime(std::ceil(sl)), { css::Xs, Weight::SemiBold }, x - 34 - 4, cy, pal.accentInk, 1, Align::Right);
}

// ------------------------------------------------------------------ overlays

/** What runs in the background now, the most telling first. False when nothing does. */
bool App::backgroundJob(Job& j) {
    const int n = int(lib.tracks.size());
    if (libCheck_.joinable() && !libCheckDone_) { j = { "Checking the music folder", 0, 0, 2 }; return true; }
    if (n > 0 && meta.taggedCount() < n) { j = { "Reading tags", meta.taggedCount(), n, 2 }; return true; }
    const int pos = meta.sweepPos(), cached = meta.coversCached();
    if (pos > 0 && pos < n && cached < n) { j = { "Preparing covers", std::min(cached, n), n, 2 }; return true; }
    if (thumbs.running()) {
        int made = 0, left = 0;
        thumbs.progress(made, left);
        if (left > 0) { j = { "Overlay covers", made, made + left, 1 }; return true; }
    }
    if (store.levelMode && levels.busy()) { j = { "Levelling", std::min(int(levels.count()), n), n, 1 }; return true; }   // short: the count shares the line
    return false;
}

/**
 * The progress card above Settings: the job, a count and a thin bar (a sliding one when there is no
 * count). Shown once a job has run for a second, so the quick ones at launch do not flash; it fades out
 * when the work is done. Pressing it opens the Settings page about it. Collapsed: just the bar.
 */
void App::drawJobCard(float x, float bottom, float w, bool collapsed) {
    const float a = jobShown;
    const Job& j = jobLast;
    const float h = collapsed ? 22 : 44, y = bottom - h;
    const Id id = uid("side.job");
    if (ui.item(id, { x, y, w, h }, 10)) { settingsCat = j.settingsCat; settingsQuery.clear(); navigate(View::Settings); }
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    g.roundRect(x, y, w, h, 10, pal.text, (hot ? 0.09f : 0.05f) * a);
    const float frac = j.total > 0 ? std::clamp(float(j.done) / float(j.total), 0.f, 1.f) : -1;
    const float bx = x + 10, bw = w - 20, by = y + h - 10;
    g.roundRect(bx, by, bw, 3, 1.5f, pal.text, 0.12f * a);
    if (frac >= 0) {
        g.roundRect(bx, by, std::max(3.f, bw * frac), 3, 1.5f, pal.accent, a);
    } else {   // busy without a count: a segment sliding along
        const float t = float(std::fmod(time, 1.6) / 1.6), seg = bw * 0.3f;
        const float sx = bx + (bw + seg) * t - seg, l = std::max(bx, sx), r = std::min(bx + bw, sx + seg);
        if (r > l) g.roundRect(l, by, r - l, 3, 1.5f, pal.accent, a);
    }
    if (collapsed) return;
    const Font lf{ css::Xs, Weight::SemiBold };
    std::string count;
    if (j.total > 0) count = std::to_string(j.done) + " of " + std::to_string(j.total);
    // The count in fixed-width digits, and the label's room taken from the widest the count can be: nothing moves as it rises.
    const std::string widest = j.total > 0 ? std::to_string(j.total) + " of " + std::to_string(j.total) : std::string();
    const int cw = widest.empty() ? 0 : g.measureNum(widest, { css::Xs });
    g.text(j.label, lf, x + 10, y + 8, pal.text2, a, Align::Left, int(w - 20 - cw - 8));
    if (!count.empty()) g.textNum(count, { css::Xs }, x + w - 10, y + 8, pal.text3, a, Align::Right);
}

/** The overlay is missing many covers: a small modal offering to make them now (they are made in the background). */
void App::drawCoverNotice() {
    if (coverNotice != 1) return;
    const float a = std::clamp(float(time - coverNoticeAt) / 0.25f, 0.f, 1.f);
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.45f * a);
    const Font tf{ css::Lg, Weight::Bold }, sf{ css::Base };
    const float w = 520, pad = 24, x = (css::W - w) / 2;
    char sub[200];
    snprintf(sub, sizeof sub, "%d of %d tracks have none yet, so the overlay shows no cover for them. They are made in the "
             "background while Gatefold is open.", coverMissing, int(lib.tracks.size()));
    const std::vector<std::string> lines = g.wrap(sub, sf, int(w - pad * 2 - 52), 4);
    const float h = pad + 26 + 8 + lines.size() * 21.f + 20 + 34 + pad;
    const float y = (css::H - h) / 2 + (1 - a) * 10;
    g.roundRect(x, y + 10, w, h, 18, { 0, 0, 0 }, 0.3f * a);
    g.roundRect(x, y, w, h, 18, pal.surface2, a);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f * a);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f * a);
    g.icon(ICON_IMAGE, x + pad + 20, y + pad + 20, 20, pal.accentInk, a);
    const float tx = x + pad + 52;
    g.text("Covers for the overlay are missing", tf, tx, y + pad, pal.text, a, Align::Left, int(w - pad * 2 - 52));
    float ly = y + pad + 26 + 8;
    for (const auto& l : lines) { g.text(l, sf, tx, ly, pal.text2, a); ly += 21; }
    bool make = false, later = false;
    const float by = ly + 20;
    const float bw = button(uid("notice.make"), ICON_REFRESH, "Make them now", tx, by, true, &make, false, false, DefaultFocus);
    button(uid("notice.later"), nullptr, "Not now", tx + bw + 8, by, false, &later);
    if (coverNoticeAt == time) ui.focus(uid("notice.make"));   // the first frame: focus moves onto it
    if (make) {
        for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
        toast("Making them in the background: the card above Settings shows how far it is");
        closeCoverNotice();
    }
    if (later) closeCoverNotice();
}

/** After the sleep timer: the console goes to sleep when the count reaches 0, unless you stay awake. */
void App::drawSleepPrompt() {
    if (sleepPromptAt <= 0) return;
    const float a = std::clamp(float(time - sleepPromptAt) / 0.25f, 0.f, 1.f);
    const int left = std::max(0, int(std::ceil(store.sleepConsole - (time - sleepPromptAt))));
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.45f * a);
    const Font tf{ css::Lg, Weight::Bold }, sf{ css::Base };
    const float w = 520, pad = 24, x = (css::W - w) / 2;
    const std::vector<std::string> lines =
        g.wrap("The sleep timer has paused the music. The console goes to sleep by itself unless you stay awake.", sf, int(w - pad * 2 - 52), 3);
    const float h = pad + 26 + 8 + lines.size() * 21.f + 20 + 34 + pad;
    const float y = (css::H - h) / 2 + (1 - a) * 10;
    g.roundRect(x, y + 10, w, h, 18, { 0, 0, 0 }, 0.3f * a);
    g.roundRect(x, y, w, h, 18, pal.surface2, a);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f * a);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f * a);
    g.icon(ICON_MOON, x + pad + 20, y + pad + 20, 20, pal.accentInk, a);
    const float tx = x + pad + 52;
    g.text("Going to sleep in " + std::to_string(left), tf, tx, y + pad, pal.text, a, Align::Left, int(w - pad * 2 - 52));
    float ly = y + pad + 26 + 8;
    for (const auto& l : lines) { g.text(l, sf, tx, ly, pal.text2, a); ly += 21; }
    bool sleepNow = false, stay = false;
    const float by = ly + 20;
    const float bw = button(uid("sleep.now"), ICON_MOON, "Sleep now", tx, by, true, &sleepNow, false, false, DefaultFocus);
    button(uid("sleep.stay"), nullptr, "Stay awake", tx + bw + 8, by, false, &stay);
    if (sleepPromptAt == time) ui.focus(uid("sleep.now"));
    if (sleepNow) { closeSleepPrompt(); appletRequestToSleep(); }
    if (stay) closeSleepPrompt();
}

void App::closeCoverNotice() {
    coverNotice = 2;
    if (noticeFocus_) ui.focus(noticeFocus_);
    else ui.focusDefault();
}

void App::drawToast() {
    if (toastT <= 0) return;
    const float a = std::min(1.f, toastT / 0.25f) * std::min(1.f, (3.2f - toastT) / 0.2f + 0.01f);
    const Font f{ css::Sm, Weight::SemiBold };
    const float w = g.measure(toastText, f) + 36, h = 40;
    const float x = (css::W - w) / 2, y = css::H - css::BarH - 20 - h + (1 - a) * 8;
    g.roundRect(x, y + 6, w, h, 12, { 0, 0, 0 }, 0.25f * a);
    g.roundRect(x, y, w, h, 12, pal.surface2, a);
    g.roundStroke(x, y, w, h, 12, 1, pal.text, 0.09f * a);
    g.textC(toastText, f, x + 18, y + h / 2, pal.text, a);
}

void App::drawFocusRing() {
    if (!ui.showFocusRing()) return;
    const Id id = ui.focusedId();
    if (!id || !ui.hasItem(id)) return;
    const SDL_FRect r = ui.rectOf(id);
    if (r.w <= 0) return;
    // :focus-visible { outline: 2px solid var(--accent-ink); outline-offset: 2px }
    const float rad = ui.radiusOf(id) + 3;
    g.roundStroke(r.x - 4, r.y - 4, r.w + 8, r.h + 8, rad, 2, pal.accentInk);
}
