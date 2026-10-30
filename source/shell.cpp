/*
 * Gatefold NX · shell.cpp
 * The frame around every view: #bg, #titlebar, #sidebar, #playerbar (app.css).
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

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
    plHoverShown = route.view == View::Playlists ? plHover : -2;
    ui.setModal(sheet.page != Sheet::Closed || ((coverNotice == 1 || appCoverNotice == 1) && !canvas) || sleepPromptAt > 0 || tourStep >= 0 || je.open || ap.open);
    if (canvas) {
        g.holdCache(automixFocus(store.mixAnimCanvas == 0 ? 0 : 2).ui < 1);   // its text stays cached while an Automix join hides it (as Now playing's)
        drawCanvas();
        profMark(2);
        drawSheet();
        drawTour();
        drawSleepPrompt();
        drawToast();
        if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);
        drawFocusRing();
        drawHint();
        drawPointer();
        drawFrameTimes();
        profMark(5);
        return;
    }
    drawBackground();
    profMark(1);
    // Automix joining on Now playing (automixFocus): how much of the interface shows. Fully faded, it is not drawn at all:
    // drawn at nothing it cost a whole frame's layout on the console, and the join's animation dropped frames (the user's
    // video, 2026-10-28: the light held for two or three frames, then jumped). Focus stays where it was meanwhile
    // (Full only: Immersive keeps the sidebar and the bars, fading the page's info alone; Simple fades nothing)
    const float shown = route.view == View::Now ? automixFocus(store.mixAnimNow == 0 ? 0 : 2).ui : 1.f;
    const bool hidden = shown <= 0.001f;
    g.holdCache(shown < 1);   // what it shows stays cached meanwhile, so its return is not a stutter

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
    case View::Files: drawFiles(mx, my, mw, mh); break;
    case View::Playlists: drawPlaylists(mx, my, mw, mh); break;
    case View::PickFolder:
        if (folderFor >= 0 && route.view != View::PickFolder) folderFor = -1;
        [[fallthrough]];
    case View::PickPlaylist:
    case View::PickExport: drawPicker(mx, my, mw, mh); break;
    default: drawTrackList(mx, my, mw, mh); break;
    }
    g.unclip();
    profMark(2);

    // ... the sidebar and the bars fade with the page's info, the cover glides over them all (drawNow's glideCover_; outside
    // the page's clip)
    if (shown < 1) g.setFade(shown);
    if (!hidden) {
        ui.region(Region::Side);
        drawSidebar();
        ui.region(Region::Title);
        drawTitlebar();
    }
    profMark(3);
    if (!hidden) {
        ui.region(Region::Bar);
        drawPlayerbar();
    }
    g.setFade(1);
    if (glideOn_) {
        drawNowCover(glideCover_.x, glideCover_.y, glideCover_.w);
        glideOn_ = false;
    }
    profMark(4);
    drawCoverNotice();
    drawJoinEditor();   // a playlist's join (view_join.cpp)
    drawAutomixPicker();   // a new Automix playlist's songs (view_automix.cpp)
    drawSheet();
    drawTour();   // over every page it shows (tour.cpp)
    drawSleepPrompt();
    drawToast();
    if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);   // Dim the screen, docked
    if (!hidden) {   // with the interface (the user: "if something is selected, it remains selected" while it faded)
        if (shown < 1) g.setFade(shown);
        drawFocusRing();
        g.setFade(1);
    }
    if (!hidden) drawHint();
    else hints_.clear();
#ifdef GF_WINDOWED
    drawWindowLights();
#endif
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
#ifdef GF_WINDOWED
namespace {
constexpr float kLightY = css::TbH / 2, kLightR = 6.5f;
float lightX(int i) { return 21 + i * 20.f; }
}  // namespace

/** Which of the window's lights is at that place: 0 close, 1 minimise, 2 full screen, or -1. */
int App::lightAt(float px, float py) const {
    if (canvas || lightsP_ < 0.9f || py > css::TbH) return -1;
    for (int i = 0; i < 3; i++) if (std::fabs(px - lightX(i)) <= 10) return i;
    return -1;
}

/** 0 away, 1 all there, eased. */
float App::lightsShown() const { return lightsP_ * lightsP_ * (3 - 2 * lightsP_); }

/** The lights stay out of the way until the mouse reaches the window's top left corner (the system's cursor is there: in the resizing
 *  edge the app gets no mouse events, so it is asked where it is). They wait a moment for it to travel to them, and for as long
 *  as it stays near them. */
void App::updateLights() {
    SDL_ShowCursor(canvas && canvasIdle > 3 ? SDL_DISABLE : SDL_ENABLE);   // the arrow leaves the Canvas with its controls
    int gx = 0, gy = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(window_, &wx, &wy);
    SDL_GetWindowSize(window_, &ww, &wh);
    const int px = gx - wx, py = gy - wy;
    const bool inside = py >= 0 && py < wh && px < ww;
    constexpr int kWall = 3;
    const bool atWall = inside && px >= 0 && px < kWall && py / scale_ < css::TbH;   // the top left corner: the bar's height of the wall
    if (atWall && cameFromInside_) lightsHold_ = 2.5f;
    else if (lightsHold_ > 0 && inside && px >= 0 && px / scale_ < kLightsW + 44 && py / scale_ < css::TbH) lightsHold_ = std::max(lightsHold_, 0.6f);
    lightsHold_ = std::max(0.f, lightsHold_ - dt);
    // the wall counts when reached from inside: the mouse only crossing it on its way in is not a request
    if (!atWall) cameFromInside_ = inside && px >= kWall;
    const float want = lightsHold_ > 0 && !canvas ? 1.f : 0.f;
    if (!store.motion) lightsP_ = want;
    else lightsP_ = want > lightsP_ ? std::min(want, lightsP_ + dt / 0.22f) : std::max(want, lightsP_ - dt / 0.22f);
}

void App::pressLight(int i) {
    if (i == 0) closeWindow();
    else if (i == 1) SDL_MinimizeWindow(window_);
    else fullApp_ = !fullApp_;
}

/** The window's controls, drawn into the app's own bar: red closes, yellow minimises, green goes full screen. Over everything, as
 *  they work whatever is open; their marks show while the mouse is near, grey when another window is in front. */
void App::drawWindowLights() {
    const float e = lightsShown();
    if (e <= 0.002f) return;
    static const Rgb lit[3] = { { 1.f, 0.373f, 0.341f }, { 0.996f, 0.737f, 0.180f }, { 0.157f, 0.784f, 0.251f } };
    static const Rgb ink[3] = { { 0.302f, 0, 0 }, { 0.6f, 0.345f, 0 }, { 0, 0.396f, 0.051f } };
    const bool front = SDL_GetKeyboardFocus() == window_;
    const bool near = pointerIsMouse_ && pointer.idle < 3 && pointer.y <= css::TbH && pointer.x <= kLightsW - 4;
    for (int i = 0; i < 3; i++) {
        const float cx = lightX(i) - (1 - e) * (22 - i * 4.f), cy = kLightY;   // in from the wall, the far one furthest
        const bool down = lightDown_ == i;
        if (front || near) {
            g.circle(cx, cy, kLightR, lit[i], e);
            if (down) g.circle(cx, cy, kLightR, { 0, 0, 0 }, 0.22f);
        } else g.circle(cx, cy, kLightR, pal.text, 0.2f * e);
        if (!near || e < 0.9f) continue;
        const float a = 3.2f;
        if (i == 0) {
            g.polyline({ { cx - a, cy - a }, { cx + a, cy + a } }, 1.6f, ink[i]);
            g.polyline({ { cx - a, cy + a }, { cx + a, cy - a } }, 1.6f, ink[i]);
        } else if (i == 1) {
            g.polyline({ { cx - a - 0.6f, cy }, { cx + a + 0.6f, cy } }, 1.8f, ink[i]);
        } else {   // two corners, pointing apart
            g.polyline({ { cx - a, cy + 0.6f }, { cx - a, cy + a }, { cx - 0.6f, cy + a } }, 1.6f, ink[i]);
            g.polyline({ { cx + a, cy - 0.6f }, { cx + a, cy - a }, { cx + 0.6f, cy - a } }, 1.6f, ink[i]);
        }
    }
}
#endif

void App::drawPointer() {
#ifdef GF_WINDOWED
    if (pointerIsMouse_) return;
#endif
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
#ifdef GF_WINDOWED
    float x = 10 + (kLightsW - 10) * lightsShown();   // it steps aside for the window's three lights (drawWindowLights)
#else
    float x = 10;
#endif
    hint(uid("tb.sidebar"), sidebarFolded() ? "Show the sidebar" : "Hide the sidebar");
    if (iconButton(uid("tb.sidebar"), ICON_SIDEBAR, x + 17, cy)) {
#ifdef GF_WINDOWED
        if (css::W < kNarrowW) sideOpenedNarrow_ = !sideOpenedNarrow_;   // for now: the setting is for windows with room
        else
#endif
        { store.sidebarCollapsed = !store.sidebarCollapsed; store.save(); }
    }
#ifdef GF_WINDOWED
    titleKeep_[0] = { x, 0, 34, css::TbH };
#endif
    x += 34 + 6;

    // .brand: the name alone, its own wordmark (the user, 2026-10-30: "better font, bold, magical", then the mark beside it
    // taken out: "just the name"): Fraunces Black in the theme's accents turning light, a glow that breathes, now and then
    // a shimmer across it (none of it moving with Settings › Appearance › Motion off). It comes and goes with the sidebar:
    // in from the left as it opens, the shimmer as it lands
    const float open = sideOpen();
    if (open > 0.02f) {
        const float in = std::clamp((open - 0.35f) / 0.65f, 0.f, 1.f), ina = in * in * (3 - 2 * in);
        auto mix = [](const Rgb& p, const Rgb& q, float t) { return Rgb{ p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t }; };
        const float wx = x + 2 - 10 * (1 - ina);
        constexpr double period = 9;   // a shimmer every 9 s, 1.3 s across
        const double since = time - sideOpenedAt_ - 0.22;   // (from the sidebar opening: the first as it lands)
        const float t = store.motion && since >= 0 ? float(std::fmod(since, period)) : -1;
        const float shine = t >= 0 && t < 1.3f ? -0.25f + 1.5f * t / 1.3f : -1;
        const float breathe = store.motion ? 0.5f + 0.5f * std::sin(float(time) * 2 * 3.14159265f / 4.5f) : 0.5f;
        // unfurled from the left as the sidebar opens (clipped to the open part)
        g.clip(wx - 12, 0, (g.wordmarkW() + 24) * std::min(1.f, in * 1.25f) + 12, css::TbH);
        g.wordmark(wx, cy, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow,
                   0.32f + 0.18f * breathe + (shine > 0 && shine < 1 ? 0.15f * std::sin(shine * 3.14159265f) : 0.f), shine, ina);
        g.unclip();
    }

    // .search: centred in what is left of the bar; opens the console keyboard
#ifdef GF_WINDOWED
    const float left = std::max(sideW + 12, 10 + 40 + 120.f), right = css::W - 150;   // where it is with the lights away: it does not move with them
#else
    const float left = std::max(sideW + 12, x + 120), right = css::W - 150;
#endif
    const float sw = std::min(520.f, right - left), sx = left + (right - left - sw) / 2, sy = cy - 16;
    const Id sid = uid("tb.search");
    if (ui.item(sid, { sx, sy, sw, 32 }, 9)) openSearch();
#ifdef GF_WINDOWED
    titleKeep_[1] = { sx, 0, sw, css::TbH };
#endif
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 9, hot ? pal.surface : pal.text, hot ? 1.f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 9, 1, hot ? pal.accent : pal.text, hot ? 0.55f : 0.09f);
    g.icon(ICON_SEARCH, sx + 12 + 8, cy, 16, pal.text3);
    const std::string q = route.view == View::Search ? route.query : std::string();
    g.textC(q.empty() ? "Artist, album or track" : q, { css::Sm }, sx + 12 + 16 + 8, cy, q.empty() ? pal.text3 : pal.text, 1, Align::Left,
            int(sw - 90));
    // <kbd>: the button that opens search
    const Font kf{ css::Xs };
#ifdef GF_WINDOWED
    const std::string searchKey = plat::capName("Y");
#else
    const char* const searchKey = "Y";
#endif
    const float kw = g.measure(searchKey, kf) + 12;
    g.roundStroke(sx + sw - 12 - kw, cy - 9, kw, 18, 5, 1, pal.text, 0.15f);
    g.textC(searchKey, kf, sx + sw - 12 - kw / 2, cy, pal.text3, 1, Align::Center);

    // Where Windows has its caption buttons: the clock and the battery.
    const std::time_t tt = std::time(nullptr);
    const std::tm* lt = std::localtime(&tt);
    char clock[8];
    std::strftime(clock, sizeof clock, "%H:%M", lt);
    unsigned pct = 0;
    const bool haveBattery = plat::batteryLevel(pct);
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
    // how far open (App::sideOpen): the labels come in over its second half, settling 8 px towards the icons
    const float open = sideOpen(), lab = std::clamp((open - 0.4f) / 0.6f, 0.f, 1.f), labA = lab * lab * (3 - 2 * lab);
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
        // the icon glides between the middle (closed) and its place before the label (open)
        const float ix = x0 + 12 + 9.5f + (w / 2 - 12 - 9.5f) * (1 - open);
        g.icon(icon, ix, y + h / 2, 19, active ? pal.accentInk : fg);
        if (labA > 0) {   // (kept clear of the icon: it settles in from the right)
            const float tx = ix + 9.5f + 12 + 8 * (1 - labA);
            float right = x0 + w - 12;
            // while it moves the label is cut by the sidebar's edge rather than shortened ("Now p…"), the count coming in last
            const bool moving = open < 0.999f;
            const float badgeA = moving ? std::pow(labA, 4.f) : 1.f;
            if (playingHere && labA > 0.6f) { eqGlyph(right - 13, y + h / 2, pal.accentInk); right -= 22; }
            else if (!badge.empty()) right -= g.textC(badge, { css::Xs }, right, y + h / 2, pal.text3, badgeA, Align::Right) + 8;
            // (inside the playlists' own clip there: the two met, and that one put back after)
            SDL_Renderer* rr = g.renderer();
            const bool hadClip = SDL_RenderIsClipEnabled(rr);
            SDL_Rect was{};
            if (moving) {
                SDL_RenderGetClipRect(rr, &was);
                const SDL_Rect row{ int(x0), int(std::floor(y)), int(w - 4), int(std::ceil(h)) };
                SDL_Rect cut = row;
                if (hadClip && !SDL_IntersectRect(&row, &was, &cut)) cut = { 0, 0, 0, 0 };
                SDL_RenderSetClipRect(rr, &cut);
            }
            g.textC(label, f, tx, y + h / 2, fg, labA, Align::Left, moving ? 0 : int(right - tx));
            if (moving) SDL_RenderSetClipRect(rr, hadClip ? &was : nullptr);
        }
        y += h + 2;
        return hit;
    };

    const View v = route.view;
    // The tabs in your order (Settings → Appearance → Sidebar, store.tabOrder); any can be hidden (store.hiddenTabs)
    // but Now playing. Settings always stays at the bottom.
    const unsigned hide = store.hiddenTabs;
    // A folder page lights the tab it belongs to: Folders, or Library when opened from its Folders tab
    // (or when the Folders tab is hidden, so something stays lit).
    const bool folderInLibrary = v == View::Folders && (folderFrom == View::Library || (hide & 16));
    // An artist's or a genre's page lights its own tab, or Library when that tab is hidden (App::tabFor)
    const bool artistInLibrary = v == View::Artist && (hide & Store::tabBit(Store::TabArtists));
    const bool genreInLibrary = v == View::Genre && (hide & Store::tabBit(Store::TabGenres));
    // Your lines: drawn only between two tabs that show (never first, last or twice in a row), in the rows' own gap
    std::vector<int> shownTabs;
    for (int tab : store.tabOrder) {
        if (Store::isLine(tab)) { if (!shownTabs.empty() && !Store::isLine(shownTabs.back())) shownTabs.push_back(tab); continue; }
        if (tab != Store::TabNow && (hide & Store::tabBit(tab))) continue;
        shownTabs.push_back(tab);
    }
    while (!shownTabs.empty() && Store::isLine(shownTabs.back())) shownTabs.pop_back();
    for (int tab : shownTabs) {
        if (Store::isLine(tab)) {
            // every line in the sidebar the same: 6 px clear to the highlight above (the row's own 2 px gap + 4) and 6 below
            y += 4;
            g.rect(x0 + 8, y, w - 16, 1, pal.text, 0.09f);
            y += 7;
            continue;
        }
        switch (tab) {
        case Store::TabNow:
            if (navItem("nav.now", ICON_DISC, "Now playing", v == View::Now, "", 38, navFont)) navigate(View::Now);
            break;
        case Store::TabQueue:
            if (navItem("nav.queue", ICON_LIST, "Playlist", v == View::Queue, q.tracks.empty() ? "" : std::to_string(q.tracks.size()), 38, navFont))
                navigate(View::Queue);
            break;
        case Store::TabAlbums:
            if (navItem("nav.albums", ICON_V_GRID, "Albums", v == View::Albums || (v == View::Album && libFrom == View::Albums),
                        std::to_string(lib.albums.size()), 38, navFont)) {
                libFrom = View::Albums;
                navigate(View::Albums);
            }
            break;
        case Store::TabArtists:
            if (navItem("nav.artists", ICON_MIC, "Artists", v == View::Artists || v == View::Artist, std::to_string(artists.size()), 38, navFont))
                navigate(View::Artists);
            break;
        case Store::TabGenres:
            if (navItem("nav.genres", ICON_TAG, "Genres", v == View::Genres || v == View::Genre, std::to_string(genres.size()), 38, navFont))
                navigate(View::Genres);
            break;
        case Store::TabFolders:
            if (navItem("nav.folders", ICON_FOLDER, "Folders", v == View::Folders && !folderInLibrary,
                        std::to_string(std::max(0, int(lib.folders.size()) - int(lib.rootFolders.size()))), 38, navFont)) {
                navigate(View::Folders, lib.rootFolders.empty() ? -1 : lib.rootFolders[0]);
                folderFrom = View::Folders;   // the tab itself, even when clicked from Library
            }
            break;
        case Store::TabLibrary:
            if (navItem("nav.library", ICON_GRID, "Library", v == View::Library || (v == View::Album && libFrom == View::Library) || folderInLibrary ||
                                                        artistInLibrary || genreInLibrary,
                        std::to_string(lib.tracks.size()), 38, navFont)) {
                libFrom = View::Library;
                navigate(View::Library);
            }
            break;
        case Store::TabFiles:
            if (navItem("nav.files", PLI_SDCARD, "Files", v == View::Files, "", 38, navFont)) openFiles(filesStart());
            break;
        }
    }

    // .nav-sep + .nav-label with the new-playlist button; the line as clear of the tab above as your lines are (6 px)
    y += 4;
    g.rect(x0 + 8, y, w - 16, 1, pal.text, 0.09f);
    y += 11;
    if (labA > 0) {   // (fading in with the labels; the rows under it make room as it does)
        const float fade0 = g.fade();
        g.setFade(fade0 * labA);
        // the header opens the Playlists page (all of them, and their order); lit while it shows
        const Id hid = uid("nav.playlists");
        const float hw = w - 36;
        if (ui.item(hid, { x0, y, hw, 24 }, 7)) navigate(View::Playlists);
        const bool here = v == View::Playlists, hot = ui.pressed(hid) || (ui.focused(hid) && ui.showFocusRing());
        if (here) g.roundRect(x0, y, hw, 24, 7, pal.accent, 0.09f);
        else if (hot) g.roundRect(x0, y, hw, 24, 7, pal.text, ui.pressed(hid) ? 0.11f : 0.065f);
        kicker("Playlists", x0 + 12, y + 12, here ? pal.accentInk : hot ? pal.text2 : pal.text3, 0.08f);
        hint(uid("nav.newpl"), "New playlist");
        if (iconButton(uid("nav.newpl"), ICON_PLUS, x0 + w - 12, y + 12, false, 24, 14)) openNewList();   // your own, or a smart one
        g.setFade(fade0);
        y += 28 * labA;
    }

    // #playlists: Liked, then the folders added as playlists
    // The progress card sits above Settings while something runs in the background; the playlists make room.
    const float jobH = jobsHeight(collapsed);
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
        if (navItem("nav.pl", playlistIcon(i), p.name, v == View::List && route.ref == i, std::to_string(count), 32, plFont, i, q.name == p.name && isPlaying()))
            navigate(View::List, i);
    }
    g.unclip();
    ui.popArea();

    // .side-foot
    y = css::H - css::BarH - 12 - 38 - 2;
    // One stack above Settings: the first job running in front, the others peeking out behind it (furthest drawn first).
    {
        const JobStack st = jobStack();
        const float bottom = y - 11 - 6, frontH = collapsed ? 22 : 44, peek = collapsed ? 4 : 7;
        // Only the strip above the front card is drawn: the front card is see-through, and the others must not show in it.
        // The cards behind are counted in fractions (a job leaving or arriving is part of one), so they slide, never jump.
        g.clip(x0, 0, w, bottom - frontH);
        for (int k = int(std::ceil(st.behind - 0.01f)); k >= 1; k--) {
            const float part = std::min(1.f, st.behind - (k - 1));
            const float inset = 8.f * k, top = bottom - frontH - peek * std::min(float(k), st.behind);
            g.roundRect(x0 + inset, top, w - inset * 2, frontH, 10, pal.text, (0.07f - 0.015f * k) * part);
            g.roundStroke(x0 + inset, top, w - inset * 2, frontH, 10, 1, pal.text, (0.12f - 0.03f * k) * part);
        }
        g.unclip();
        if (st.body > 0.01f) drawJobCard(st, x0, bottom, w, collapsed);
        // The last job stopped (or done) with the focus on the stack: Settings, just under where it was, takes it.
        else if (ui.focused(uid("side.job"))) ui.focus(uid("nav.settings", 0));
    }
    g.rect(x0 + 8, y - 11, w - 16, 1, pal.text, 0.09f);
    if (navItem("nav.settings", ICON_GEAR, "Settings", v == View::Settings, "", 38, navFont)) navigate(View::Settings);
}

// ------------------------------------------------------------------ #playerbar

void App::drawSeek(float x, float y, float w, float h, Id id) {
    const double dur = now.info.duration;
    ui.item(id, { x, y - 4, w, h + 8 }, 6, Slider | NoSideEntry);   // reached with Up/Down only (it spans the controls below)
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
    const bool waveNow = wave.ready() && wave.path() == (now.track >= 0 ? lib.tracks[now.track].path : std::string());
    // an Automix join, Immersive or Simple: the outgoing song's waveform becomes the next one's, each bar on its way from the
    // one height to the other, timed to the join (the old one alone until the next one's is measured)
    const bool morph = store.mixAnimNow != 0 && mixOld.on && !mixOld.wave.empty();
    if (waveNow || morph) {
        // .seek.wave: 2 px bars every 3 px; played part in the accent-2 → accent gradient, the rest dim
        const auto& pk = waveNow ? wave.peaks() : mixOld.wave;
        const float mp = morph && waveNow ? mixWipe() : morph ? 0.f : 1.f;
        // as every section's (App::wipeEdge), across the waveform and the song's length beside it (one swipe: wipeGroup_)
        float soft;
        const float edgeX = wipeGroup_.on ? wipeEdge(wipeGroup_.x0, wipeGroup_.x1, mp, soft) : wipeEdge(x, x + w, mp, soft);
        const int bars = int(w / 3);
        const float px = shown * w, cur = frac * w;
        auto barOf = [&](const std::vector<float>& q, int i) {
            const size_t a = size_t(i) * q.size() / bars, b = std::max(a + 1, size_t(i + 1) * q.size() / bars);
            float v = 0;
            for (size_t j = a; j < b && j < q.size(); j++) v = std::max(v, q[j]);
            return v;
        };
        for (int i = 0; i < bars; i++) {
            float v = barOf(pk, i);
            if (mp < 1) {   // left of the edge the next song's bar, right of it the outgoing one's, blended across it
                const float old = barOf(mixOld.wave, i), wf = waveNow ? std::clamp((edgeX - (x + i * 3)) / soft + 0.5f, 0.f, 1.f) : 0.f;
                v = old + (v - old) * wf;
            }
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
        if (morph && waveNow && mp > 0 && mp < 1)
            magicEdge(edgeX, wipeGroup_.on ? wipeGroup_.x0 : x, wipeGroup_.on ? wipeGroup_.x1 : x + w, y, y + h, mp);
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
    // an Automix join, Immersive or Simple: the bar swipes as the page does (App::pageWipe_), each section across its own
    // width on the join's clock: its cover and frost, names, time and waveform
    struct PageOff { PageWipe& p; ~PageOff() { p.on = false; } } pageOff{ pageWipe_ };
    if (store.mixAnimNow != 0 && hasTrack && time < mixUntil && mixOld.on) pageWipe_.on = true;
    if (pageWipe_.on) {
        wipedImage(artR.x, artR.x + 56, artR.y, artR.y + 56, false, !now.cover, [&](bool old) {
            if (SDL_Texture* c = mixCover(old, now.cover, now.prevCover)) g.image(c, artR.x, artR.y, 56, 56, 8);
            else {
                g.roundRect(artR.x, artR.y, 56, 56, 8, pal.surface2);
                g.icon(ICON_MUSIC, artR.x + 28, artR.y + 28, 22, pal.text3, 0.6f);
            }
        });
    } else if (now.cover) {
        if (now.prevCover && now.fade < 1) g.image(now.prevCover, artR.x, artR.y, 56, 56, 8);
        g.image(now.cover, artR.x, artR.y, 56, 56, 8, ease(now.fade));
    } else {
        g.roundRect(artR.x, artR.y, 56, 56, 8, pal.surface2);
        g.icon(ICON_MUSIC, artR.x + 28, artR.y + 28, 22, pal.text3, 0.6f);
    }
    // an Automix join, Immersive or Simple (Settings › Appearance › Automix: the user, 2026-10-29): the player bar's cover
    // frosts as the big one does, without the hint; its title and artist and the seek bar's waveform give way to the next
    // song's, timed to the join (Full: the bar is not seen on Now playing then, and stays as it is elsewhere)
    const bool barMix = store.mixAnimNow != 0 && hasTrack && time < mixUntil;
    if (barMix) drawAutomixCover(artR.x, artR.y, 56, artR.x + 28, artR.y + 28, 56, 8, false, 1.5f);
    const float metaX = lx + 56 + 12, heartX = lx + leftW - 17;
    const int metaW = int(heartX - 17 - 12 - metaX);
    // A title or artist too long for its room scrolls slowly, in a loop, instead of ending in "…": it rests 3 s
    // at its start (each new track starts there), then moves at 30 px/s; the edge it is cut at fades
    // (Gatefold's 36 px mask, a little shorter here). With Motion off it does not move, the end just fades.
    if (now.track != marqueeTrack) { marqueeTrack = now.track; marqueeAt = time; }
    auto line = [&](const std::string& s, Font f, float y, const Rgb& c) {
        const int tw = g.measure(s, f);
        if (tw <= metaW) { g.text(s, f, metaX, y, c); return; }
        constexpr float Rest = 3, Speed = 30, Gap = 48, Fade = 24;
        const float loop = float(tw) + Gap;
        float off = 0;
        if (store.motion) {
            const float t = std::fmod(float(time - marqueeAt), Rest + loop / Speed);
            off = std::max(0.f, t - Rest) * Speed;
        }
        // no fade at the left while it rests at its start, nor as the next round settles there
        const float fadeL = std::max(0.f, std::min({ Fade, off * 2, (loop - off) * 2 }));
        g.textScroll(s, f, metaX, y, float(metaW), off, fadeL, Fade, c);
        if (off > loop - metaW) g.textScroll(s, f, metaX, y, float(metaW), off - loop, fadeL, Fade, c);   // the next round coming in
    };
    auto names = [&](bool old) {
        const TrackInfo& in = old ? mixOld.info : now.info;
        line(hasTrack ? in.title : "—", { css::Base, Weight::SemiBold }, cy - 21, pal.text);
        line(hasTrack ? in.artist : "", { css::Sm }, cy + 1, pal.text3);
    };
    if (barMix && mixOld.on) wiped(metaX, metaX + metaW, cy - 24, cy + 20, names);
    else names(false);
    const bool liked = hasTrack && store.isLiked(lib.tracks[now.track].path);
    hint(uid("pb.like"), liked ? "Remove from Liked" : "Add to Liked");
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
    {
        // the order playing, as the button's toast names it (the user, 2026-10-29: no "Order:")
        static const char* orderHints[] = { "Default order", "Repeat playlist", "Repeat track", "Shuffle" };
        hint(uid("pb.order"), orderHints[int(store.order)]);
        hint(uid("pb.prev"), "Previous");
        hint(uid("pb.play"), isPlaying() ? "Pause" : "Play");
        hint(uid("pb.next"), "Next");
        hint(uid("pb.sac"), stopAfterCurrent ? "Stop after this track: on" : "Stop after this track");
        hint(uid("pb.canvas"), "Canvas");
        hint(uid("pb.mute"), volumeShown <= 0.001f ? "Unmute" : "Mute");
        hint(uid("pb.queue"), "Playlist");
        hint(uid("pb.eq"), "Equalizer (hold for its settings)");
        hint(uid("pb.sleep"), "Sleep timer");
    }
    if (iconButton(uid("pb.order"), orderIcon, mid - 21 - 10 - 34 - 10 - 17, ctrlY, store.order != Order::Default)) {
        Order next = Order((int(store.order) + 1) % 4);
        if (next == Order::Shuffle && automixQueue()) next = Order::Default;   // an Automix playlist: no Shuffle (holdOrderForAutomix)
        setOrder(next);
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
    auto durText = [&](bool old) { g.textC(hasTrack ? fmtTime(old ? mixOld.info.duration : now.info.duration) : "0:00", { css::Xs }, sx + seekW - 48, seekY, pal.text3); };
    // the song's length swipes with the waveform, as one (the user, 2026-10-29: "just the waveform's effect")
    if (pageWipe_.on) wipeGroup_ = { true, sx + 58, sx + seekW - 48 + float(g.measure("00:00", { css::Xs })) };
    if (pageWipe_.on) wiped(sx + seekW - 48, sx + seekW, seekY - 10, seekY + 10, durText);
    else durText(false);
    drawSeek(sx + 58, seekY - 15, seekW - 116, 30, uid("pb.seek"));
    wipeGroup_.on = false;

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
    // The equalizer: lit while it is on. A press turns it on or off; a long press opens Settings → Equalizer.
    x -= 34 + 8;
    if (iconButton(uid("pb.eq"), ICON_EQ, x - 17, cy, store.eq.on != 0, 34, 18, Hold)) toggleEq();
    if (ui.longPressed(uid("pb.eq"))) openEqualizer();
    // The sleep timer: lit while it runs (or stop-after is on, its "End of this track"), the time left beside it.
    x -= 34 + 8;
    const double sl = sleepLeft();
    if (iconButton(uid("pb.sleep"), ICON_MOON, x - 17, cy, sl > 0)) openSleep();
    if (sl > 0) g.textC(fmtTime(std::ceil(sl)), { css::Xs, Weight::SemiBold }, x - 34 - 4, cy, pal.accentInk, 1, Align::Right);
}

// ------------------------------------------------------------------ overlays

/** What runs in the background now, by slot: the library's own work (one step at a time), the overlay's covers, levelling. */
void App::backgroundJobs(Job (&jobs)[JobSlots], bool (&busy)[JobSlots]) {
    const int n = int(lib.tracks.size());
    const int pos = meta.sweepPos(), cached = meta.coversCached();
    if (libCheck_.joinable() && !libCheckDone_) { jobs[0] = { "Checking the music folder", 0, 0, SetLibrary, Stoppable::None, "Music folder" }; busy[0] = true; }
    else if (n > 0 && meta.taggedCount() < n) { jobs[0] = { "Reading tags", meta.taggedCount(), n, SetLibrary, Stoppable::None, "Tags read" }; busy[0] = true; }
    else if (!store.stopCovers && pos > 0 && pos < n && cached < n) {
        jobs[0] = { "Preparing covers", std::min(cached, n), n, SetLibrary, Stoppable::Covers, "Covers ready" };
        busy[0] = true;
    }
    if (thumbs.running() && !store.stopThumbs) {
        int made = 0, left = 0;
        thumbs.progress(made, left);
        if (left > 0) { jobs[1] = { "Overlay covers", made, made + left, SetPlayback, Stoppable::Thumbs, "Covers for the overlay" }; busy[1] = true; }
    }
    // Mood playlists: the library's moods, in the same worker (it measures loudness and Automix's analysis in the same decode);
    // first, or its busy worker would read as "Levelling" with every track measured
    int moodDone = 0, moodTotal = 0;
    moodProgress(moodDone, moodTotal);
    if (moodTotal > 0 && !store.stopLevels && moodDone < moodTotal && levels.loaded()) {
        jobs[2] = { "Moods", moodDone, moodTotal, SetPlayback, Stoppable::Levels, "Analysed" };
        busy[2] = true;
    } else if (store.levelMode && !store.stopLevels && levels.busy()) {   // short: the count shares the line
        jobs[2] = { "Levelling", std::min(int(levels.count()), n), n, SetPlayback, Stoppable::Levels, "Measured" };
        busy[2] = true;
    } else if (!store.levelMode) {   // levelling off: only Automix playlists' tracks are analysed, first in the worker's queue
        // (shown while any is waiting: the worker counted itself busy for the library only, so the card never came)
        int done = 0, total = 0;
        automixProgress(done, total);
        if (done < total) { jobs[2] = { "Automix", done, total, SetPlayback, Stoppable::Levels, "Analysed" }; busy[2] = true; }
    }
    // Files: copying, or deleting a folder (settingsCat −1: the card opens Files where it ends)
    if (fileWork.thread.joinable() && !fileWork.done) {
        jobs[3] = { fileWork.label, fileWork.count.load(), fileWork.total.load(), fileWork.cat, Stoppable::None, nullptr };
        busy[3] = true;
    }
    // An update downloading, checked and put in place (not the short check of the list): its card opens Settings → Updates
    if (updater.busy() && updater.phase() != Updater::Checking) {
        const Updater::Phase ph = updater.phase();
        const bool bytes = ph == Updater::Downloading || ph == Updater::Verifying;
        // Short: the count in megabytes takes room (the version is on Settings' row)
        const char* what = ph == Updater::Downloading ? "Downloading" : ph == Updater::Verifying ? "Checking it" : "Installing";
        jobs[4] = { what, int(bytes ? updater.done() / 1024 : updater.done()), int(bytes ? updater.total() / 1024 : updater.total()),
                    SetUpdates, Stoppable::None, "Check for updates", bytes };
        if (ph == Updater::BackingUp || ph == Updater::Installing) jobs[4].total = 0;   // quick: just busy
        busy[4] = true;
    }
#ifdef GF_PREVIEW
    // The PC preview has no background player, and its covers are made too fast to overlap: GF_JOBS=1 shows all
    // three at once (hostshim.cpp), so the stack and its menus can be looked at.
    if (getenv("GF_JOBS")) {
        const int t = int(time * 20);
        if (!busy[0] && !store.stopCovers) { jobs[0] = { "Preparing covers", std::min(1015 + t, 3074), 3074, SetLibrary, Stoppable::Covers, "Covers ready" }; busy[0] = true; }
        if (!busy[1] && !store.stopThumbs) { jobs[1] = { "Overlay covers", std::min(449 + t, 2547), 2547, SetPlayback, Stoppable::Thumbs, "Covers for the overlay" }; busy[1] = true; }
        if (!busy[2] && !store.stopLevels) { jobs[2] = { "Levelling", std::min(13 + t / 10, 3074), 3074, SetPlayback, Stoppable::Levels, "Measured" }; busy[2] = true; }
    }
#endif
}

/**
 * The stack as layers: each job's share of the front card (the first one running in slot order has it; one fading
 * out hands its share to the next, so the card stays and only its contents change), the front card's own opacity
 * (all of them together), and how many cards peek out behind it, in fractions.
 */
App::JobStack App::jobStack() const {
    JobStack st;
    float rest = 1, layers = 0;
    for (int i = 0; i < JobSlots; i++) {
        const float s = jobCards[i].shown;
        st.weight[i] = s * rest;
        rest *= 1 - s;
        layers += s;
        st.running += s > 0.5f;
        if (st.front < 0 || st.weight[i] > st.weight[st.front]) st.front = i;
    }
    st.body = 1 - rest;
    st.behind = std::max(0.f, layers - st.body);
    return st;
}

float App::jobsHeight(bool collapsed) const {
    // The front card's height as the stack fades in, and a peek for each card behind it.
    const JobStack st = jobStack();
    return (collapsed ? 30 : 52) * st.body + (collapsed ? 4 : 6) * st.behind;
}

std::string App::jobCount(const Job& j) const {
    if (j.total <= 0) return {};
    if (!j.kb) return std::to_string(j.done) + " of " + std::to_string(j.total);
    char b[48];
    snprintf(b, sizeof b, "%.1f of %.1f MB", j.done / 1024.0, j.total / 1024.0);
    return b;
}

void App::showSetting(const Job& j) {
    if (j.settingsCat < 0) { openFiles(fileWork.dest); return; }   // Files' work: the folder it ends in
    settingsCat = j.settingsCat;
    settingsQuery.clear();
    navigate(View::Settings);
    settingsJump = j.row ? j.row : "";
    settingsJumpAt = time;
}

void App::applyJobStops() {
    meta.pauseCovers(store.stopCovers || coversHeld);
    thumbs.pause(store.stopThumbs);
    levels.pause(store.stopLevels);
}

void App::setJobStopped(Stoppable which, bool stopped) {
    bool& flag = which == Stoppable::Covers ? store.stopCovers : which == Stoppable::Thumbs ? store.stopThumbs : store.stopLevels;
    if (which == Stoppable::None || flag == stopped) return;
    flag = stopped;
    if (which == Stoppable::Covers) coversHeld = false;   // your Stop or Resume answers the launch's hold too
    applyJobStops();
    persistSoon(0.3f);
    static const char* names[] = { "", "Covers", "Overlay covers", "Levelling" };
    toast(std::string(names[int(which)]) + (stopped ? " stopped. Resume it in Settings" : " resumed"));
}

/**
 * The front card of the stack above Settings: the job, a count and a thin bar (a sliding one when there is no
 * count). Shown once a job has run for a second, so the quick ones at launch do not flash; it fades out when
 * the work is done. With others running behind it, pressing it lists them all (openJobList); alone, a job
 * that can wait (the covers, levelling) gets its menu (stop it, or its Settings row), the others go straight
 * to their Settings row. Collapsed: just the bar.
 */
void App::drawJobCard(const JobStack& st, float x, float bottom, float w, bool collapsed) {
    const float h = collapsed ? 22 : 44, y = bottom - h;
    const int slot = st.front;
    const Id id = uid("side.job");   // one id for the stack, whichever job is in front: the focus stays on it when the front one goes
    if (ui.item(id, { x, y, w, h }, 10)) {
        const Job& j = jobCards[slot].job;
        if (st.running > 1) openJobList();                          // every job running, each with its own menu
        else if (j.stop != Stoppable::None) openJobMenu(slot, false);   // Stop it, or its Settings row
        else showSetting(j);
    }
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    g.roundRect(x, y, w, h, 10, pal.text, (hot ? 0.09f : 0.05f) * st.body);
    const float bx = x + 10, bw = w - 20, by = y + h - 10;
    g.roundRect(bx, by, bw, 3, 1.5f, pal.text, 0.12f * st.body);
    // The contents of each job with a share of the front: a job leaving fades out, then the next one fades in,
    // in the same card (no crossing lines: each shows only while it has most of the card).
    for (int i = 0; i < JobSlots; i++) {
        const float a = std::clamp((st.weight[i] - 0.5f) / 0.5f, 0.f, 1.f);
        if (a <= 0.01f) continue;
        const Job& j = jobCards[i].job;
        const float frac = j.total > 0 ? std::clamp(float(j.done) / float(j.total), 0.f, 1.f) : -1;
        if (frac >= 0) {
            g.roundRect(bx, by, std::max(3.f, bw * frac), 3, 1.5f, pal.accent, a);
        } else {   // busy without a count: a segment sliding along
            const float t = float(std::fmod(time, 1.6) / 1.6), seg = bw * 0.3f;
            const float sx = bx + (bw + seg) * t - seg, l = std::max(bx, sx), r = std::min(bx + bw, sx + seg);
            if (r > l) g.roundRect(l, by, r - l, 3, 1.5f, pal.accent, a);
        }
        if (collapsed) continue;
        const float right = x + w - 10;
        const Font lf{ css::Xs, Weight::SemiBold };
        const std::string count = jobCount(j);
        // The count in fixed-width digits, and the label's room taken from the widest the count can be: nothing moves as it rises.
        Job full = j;
        full.done = j.total;
        const std::string widest = jobCount(full);
        const int cw = widest.empty() ? 0 : g.measureNum(widest, { css::Xs });
        g.text(j.label, lf, x + 10, y + 8, pal.text2, a, Align::Left, int(right - x - 10 - cw - 8));
        if (!count.empty()) g.textNum(count, { css::Xs }, right, y + 8, pal.text3, a, Align::Right);
    }
}

/** The overlay is missing many covers: a small modal offering to make them now (they are made in the background). */
void App::drawCoverNotice() {
    const bool app = appCoverNotice == 1;   // the app's own covers come first, the overlay's after (never both at once)
    if (!app && coverNotice != 1) return;
    const float a = std::clamp(float(time - coverNoticeAt) / 0.25f, 0.f, 1.f);
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.45f * a);
    const Font tf{ css::Lg, Weight::Bold }, sf{ css::Base };
    const float w = 520, pad = 24, x = (css::W - w) / 2;
    char sub[240];
    if (app)
        snprintf(sub, sizeof sub, "%d of %d tracks have no small cover yet, so lists make each one as you scroll to it. Made now, "
                 "in the background, every list opens with its covers (a few minutes for a large library).", coverMissing, int(lib.tracks.size()));
    else
        snprintf(sub, sizeof sub, "%d of %d tracks have none yet, so the overlay shows no cover for them. They are made in the "
                 "background while Songate is open.", coverMissing, int(lib.tracks.size()));
    const std::vector<std::string> lines = g.wrap(sub, sf, int(w - pad * 2 - 52), 4);
    const float h = pad + 26 + 8 + lines.size() * 21.f + 20 + 34 + pad;
    const float y = (css::H - h) / 2 + (1 - a) * 10;
    g.roundRect(x, y + 10, w, h, 18, { 0, 0, 0 }, 0.3f * a);
    g.roundRect(x, y, w, h, 18, pal.surface2, a);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f * a);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f * a);
    g.icon(ICON_IMAGE, x + pad + 20, y + pad + 20, 20, pal.accentInk, a);
    const float tx = x + pad + 52;
    g.text(app ? "Covers for your library" : "Covers for the overlay are missing", tf, tx, y + pad, pal.text, a, Align::Left, int(w - pad * 2 - 52));
    float ly = y + pad + 26 + 8;
    for (const auto& l : lines) { g.text(l, sf, tx, ly, pal.text2, a); ly += 21; }
    bool make = false, later = false;
    const float by = ly + 20;
    const float bw = button(uid("notice.make"), ICON_REFRESH, "Make them now", tx, by, true, &make, false, false, DefaultFocus);
    button(uid("notice.later"), nullptr, "Not now", tx + bw + 8, by, false, &later);
    if (coverNoticeAt == time) ui.focus(uid("notice.make"));   // the first frame: focus moves onto it
    if (app) {
        if (make) {
            releaseCovers();
            toast("Making them in the background: the card above Settings shows how far it is");
        } else if (later) {
            toast("Settings › Library › Covers ready makes them when you want");
        }
        if (make || later) closeCoverNotice();
        return;
    }
    if (make) {
        setJobStopped(Stoppable::Thumbs, false);
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
        g.wrap("The sleep timer has paused the music. " GF_CAP_THE_CONSOLE " goes to sleep by itself unless you stay awake.", sf, int(w - pad * 2 - 52), 3);
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
    if (sleepNow) { closeSleepPrompt(); plat::requestSleep(); }
    if (stay) closeSleepPrompt();
}

/** The covers cache's sweep, held at launch until the covers were counted (or asked about), goes ahead. */
void App::releaseCovers() {
    if (!coversHeld) return;
    coversHeld = false;
    applyJobStops();
}

void App::closeCoverNotice() {
    // the app's question is answered first (B or Not now leaves its sweep held for this launch); then the overlay's
    if (appCoverNotice == 1) appCoverNotice = 2;
    else coverNotice = 2;
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
