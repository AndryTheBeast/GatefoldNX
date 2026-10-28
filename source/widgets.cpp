/*
 * Gatefold NX · widgets.cpp
 * Gatefold's components (.icon-btn, .btn, .chip, .kicker, .vrow, .tile, .range,
 * .toggle) drawn with the current palette. Colour mixes follow app.css:
 *   --hover = text 6.5%, --press = text 11%, --line = text 9%, --line-2 = text 15%,
 *   --accent-faint = accent 9%, --accent-soft = accent 17%.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {
const char* upperAscii(std::string& s) {
    for (char& c : s) c = char(toupper((unsigned char)c));
    return s.c_str();
}
}  // namespace

bool App::iconButton(Id id, const char* icon, float cx, float cy, bool on, float size, float iconSize, uint8_t flags) {
    const SDL_FRect r{ cx - size / 2, cy - size / 2, size, size };
    const bool hit = ui.item(id, r, 9, flags);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(r.x, r.y, r.w, r.h, 9, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float s = ui.pressed(id) ? iconSize * 0.94f : iconSize;
    g.icon(icon, cx, cy, s, on ? pal.accentInk : hot ? pal.text : pal.text2);
    return hit;
}

float App::button(Id id, const char* icon, const std::string& label, float x, float y, bool primary, bool* clicked, bool toggled, bool danger,
                  uint8_t flags, float minW) {
    const Font f{ css::Sm, Weight::SemiBold };
    const float iconW = icon ? 16 + 8 : 0;
    const float natural = 16 + iconW + g.measure(label, f) + 16;
    const float w = std::max(natural, minW);   // a column of buttons one width (Settings › Reset)
    const float h = 34;
    const SDL_FRect r{ x, y, w, h };
    const bool hit = ui.item(id, r, h / 2, flags);
    if (clicked) *clicked = hit;
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    Rgb fg = pal.text;
    if (primary) {
        // .btn.primary, hover: accent 88% + text
        const Rgb bg = hot ? Rgb{ pal.accent.r * 0.88f + pal.text.r * 0.12f, pal.accent.g * 0.88f + pal.text.g * 0.12f,
                                  pal.accent.b * 0.88f + pal.text.b * 0.12f }
                           : pal.accent;
        g.roundRect(x, y, w, h, h / 2, bg);
        fg = pal.onAccent;
    } else if (toggled) {
        g.roundRect(x, y, w, h, h / 2, pal.text, 0.065f);
        g.roundRect(x, y, w, h, h / 2, pal.accent, 0.2f);
        g.roundStroke(x, y, w, h, h / 2, 1, pal.accent, 0.5f);
        fg = pal.accentInk;
    } else {
        g.roundRect(x, y, w, h, h / 2, pal.text, hot ? 0.11f : 0.065f);
        g.roundStroke(x, y, w, h, h / 2, 1, pal.text, 0.15f);
        if (danger) fg = dangerInk();
    }
    float tx = x + 16 + (w - natural) / 2;
    if (icon) { g.icon(icon, tx + 8, y + h / 2, 16, fg); tx += iconW; }
    g.textC(label, f, tx, y + h / 2, fg);
    return w;
}

float App::chip(const std::string& label, float x, float cy, bool accent) {
    const Font f{ css::Xs, Weight::SemiBold };
    const float w = g.measureSpaced(label, f, 0.02f) + 16, h = 22;
    if (accent) {
        g.roundRect(x, cy - h / 2, w, h, 6, pal.accent, 0.09f);
        g.roundStroke(x, cy - h / 2, w, h, 6, 1, pal.accent, 0.35f);
    } else {
        g.roundRect(x, cy - h / 2, w, h, 6, pal.text, 0.065f);
        g.roundStroke(x, cy - h / 2, w, h, 6, 1, pal.text, 0.09f);
    }
    g.textSpaced(label, f, x + 8, cy, 0.02f, accent ? pal.accentInk : pal.text2);
    return w;
}

void App::kicker(const std::string& s, float x, float cy, const Rgb& c, float spacing) {
    std::string u = s;
    g.textSpaced(upperAscii(u), { css::Xs, Weight::Bold }, x, cy, spacing, c);
}

/**
 * .lyr .ln.gap: an empty synced line (an instrumental break) as three dots, 7 px apart. On the line
 * playing they pulse one after another: lyrDot, 1.4 s, opacity .25 → 1 and scale .8 → 1.15 and back,
 * each half on --ease (cubic-bezier(.22, .7, .18, 1)), the second and third .2 s and .4 s behind.
 */
void App::lyricDots(float x, float cy, float d, const Rgb& c, float alpha, bool current) {
    auto ease = [](float t) {   // cubic-bezier(.22, .7, .18, 1): x(u) solved by Newton steps, then y(u)
        auto bez = [](float u, float p1, float p2) { const float v = 1 - u; return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u; };
        float u = t;
        for (int i = 0; i < 6; i++) {
            const float v = 1 - u, dx = 3 * v * v * 0.22f + 6 * v * u * (0.18f - 0.22f) + 3 * u * u * (1 - 0.18f);
            if (std::fabs(dx) < 1e-5f) break;
            u = std::clamp(u - (bez(u, 0.22f, 0.18f) - t) / dx, 0.f, 1.f);
        }
        return bez(u, 0.7f, 1.f);
    };
    for (int i = 0; i < 3; i++) {
        float a = 0.5f, s = 1;
        if (current) {
            const float t = std::fmod(std::max(0.0, time - 0.2 * i), 1.4) / 1.4f;
            const float k = t < 0.5f ? ease(t * 2) : 1 - ease((t - 0.5f) * 2);
            a = 0.25f + 0.75f * k;
            s = 0.8f + 0.35f * k;
        }
        g.circle(x + d / 2 + i * d * 2, cy, d / 2 * s, c, a * alpha);
    }
}

/** .eq: three bars bouncing while something plays. */
void App::eqGlyph(float x, float cy, const Rgb& c) {
    const float base[3] = { 0.6f, 1.f, 0.4f }, delay[3] = { -0.2f, -0.5f, -0.8f };
    for (int i = 0; i < 3; i++) {
        float s = base[i];
        if (isPlaying()) {
            float ph = std::fmod(float(time - delay[i]) / 0.9f, 2.f);
            ph = ph < 1 ? ph : 2 - ph;
            s = 0.25f + 0.75f * (0.5f - 0.5f * std::cos(ph * 3.14159f));
        }
        const float h = 12 * s;
        g.roundRect(x + i * 5, cy + 6 - h, 3, h, 1, c);
    }
}

void App::coverBox(int track, float x, float y, float size, float rad, float alpha) {
    AlbumArt& a = meta.art(track);
    if (a.state == AlbumArt::Ready) {
        g.image(a.tex, x, y, size, size, rad, alpha);
    } else {
        g.roundRect(x, y, size, size, rad, pal.surface2, alpha);
        if (size >= 36) g.icon(ICON_MUSIC, x + size / 2, y + size / 2, std::round(size * 0.34f), pal.text3, 0.6f * alpha);
    }
}

void App::listHead(float x, float y, float w, bool showAlbum) {
    const Font f{ css::Xs, Weight::SemiBold };
    const float pad = 30, gap = 14;
    const float inner = w - pad * 2;
    const float fixed = 44 + 64 + gap * (showAlbum ? 4 : 3);
    const float fr = (inner - fixed) / (showAlbum ? 3.6f : 2.6f);
    float cx = x + pad;
    const float cy = y + 15;
    g.textSpaced("#", f, cx + 44, cy, 0.08f, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    g.textSpaced("TITLE", f, cx, cy, 0.08f, pal.text3);
    cx += fr * 1.6f + gap;
    g.textSpaced("ARTIST", f, cx, cy, 0.08f, pal.text3);
    cx += fr + gap;
    if (showAlbum) { g.textSpaced("ALBUM", f, cx, cy, 0.08f, pal.text3); cx += fr + gap; }
    g.textSpaced("TIME", f, cx + 64, cy, 0.08f, pal.text3, 1, Align::Right);
    g.rect(x, y + 29, w, 1, pal.text, 0.09f);
}

bool App::trackRow(Id id, int track, int number, float x, float y, float w, float h, bool showAlbum) {
    const Track& t = lib.tracks[track];
    meta.wantTags(track);
    const bool hit = ui.item(id, { x, y, w, h }, 8);
    if (ui.focused(id)) hoverTrack = track;   // what + opens the track menu for
    const bool playing = now.track == track && player.loaded();
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (playing) g.roundRect(x, y, w, h, 8, pal.accent, 0.09f);
    if (hot) g.roundRect(x, y, w, h, 8, pal.text, ui.pressed(id) ? 0.11f : 0.065f);

    const float pad = 16, gap = 14;
    const float inner = w - pad * 2;
    const float fixed = 44 + 64 + gap * (showAlbum ? 4 : 3);
    const float fr = (inner - fixed) / (showAlbum ? 3.6f : 2.6f);
    const float cy = y + h / 2;
    float cx = x + pad;
    if (playing) eqGlyph(cx + 44 - 13, cy, pal.accentInk);
    else g.textC(std::to_string(number), { css::Sm }, cx + 44, cy, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    g.textC(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, cx, cy, playing ? pal.accentInk : pal.text, 1, Align::Left,
            int(fr * 1.6f));
    cx += fr * 1.6f + gap;
    g.textC(t.artist, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    cx += fr + gap;
    if (showAlbum) { g.textC(t.album, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr)); cx += fr + gap; }
    g.textC(t.duration > 0 ? fmtTime(t.duration) : "", { css::Sm }, cx + 64, cy, pal.text3, 1, Align::Right);
    return hit;
}

float App::viewHead(const std::string& kick, const std::string& title, const std::string& sub, float x, float y, float w) {
    // .view-head: padding 18px 28px 14px; kicker, h2 (34px/750), .sub
    float cy = y + 18 + 8;
    kicker(kick, x + 28, cy, pal.text3);
    cy += 10;
    const Font h2{ css::Xxl, Weight::Bold };
    g.text(title, h2, x + 28, cy, pal.text, 1, Align::Left, int(w - 56 - 260));
    cy += 38;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56 - 260));
    return cy + 4 + 18 + 14;
}

void App::slider(Id id, float x, float cy, float w, float value, float* out) {
    const SDL_FRect r{ x, cy - 11, w, 22 };
    ui.item(id, r, 11, Slider);
    // Like a native <input type=range>, the 14 px thumb travels inside the track, not past its ends.
    const float travel = w - 14, x0 = x + 7;
    float v = value;
    if (const int step = ui.sliderStep(id)) v = std::clamp(v + step * 0.05f, 0.f, 1.f);
    const float dx = ui.sliderDrag(id);
    if (dx >= 0) v = std::clamp((dx - x0) / travel, 0.f, 1.f);
    if (out) *out = v;
    // .range: 4px track, accent up to the value, 14px thumb in --text
    const float tx = x0 + travel * v;
    g.roundRect(x, cy - 2, w, 4, 2, pal.text, 0.16f);
    g.roundRect(x, cy - 2, tx - x, 4, 2, pal.accent);
    g.circle(tx, cy + 1, 8, { 0, 0, 0 }, 0.25f);
    if (ui.engaged(id)) g.circle(tx, cy, 11, pal.accent);   // picked up with A: ←/→ move it
    g.circle(tx, cy, 7, pal.text);
}

void App::vslider(Id id, float cx, float top, float h, float value, float* out, Id left, Id right) {
    ui.item(id, { cx - 16, top - 8, 32, h + 16 }, 10, Slider | Slider2D);
    // The 14 px thumb travels inside the track, as the flat slider's does.
    const float travel = h - 14, y0 = top + 7;
    float v = value;
    if (const int st = ui.sliderStepY(id)) v = std::clamp(v - st / 24.f, 0.f, 1.f);   // 1 dB a press on a ±12 dB band
    if (const int st = ui.sliderStep(id)) {
        // ←/→ hand it on: the next band is picked up straight away
        if (const Id n = st < 0 ? left : right) { ui.focus(n); ui.engage(n); }
        ui.useSlide();
    }
    if (const float dy = ui.sliderDragY(id); dy >= 0) v = std::clamp(1 - (dy - y0) / travel, 0.f, 1.f);
    if (out) *out = v;
    const float mid = y0 + travel * 0.5f, ky = y0 + travel * (1 - v);
    g.roundRect(cx - 2, top, 4, h, 2, pal.text, 0.16f);
    g.rect(cx - 7, mid - 0.5f, 14, 1, pal.text, 0.28f);   // 0 dB
    g.roundRect(cx - 2, std::min(mid, ky), 4, std::fabs(ky - mid), 2, pal.accent);
    g.circle(cx, ky + 1, 8, { 0, 0, 0 }, 0.25f);
    if (ui.engaged(id)) g.circle(cx, ky, 11, pal.accent);
    g.circle(cx, ky, 7, pal.text);
}

bool App::toggle(Id id, float x, float cy, bool on) {
    const bool hit = ui.item(id, { x, cy - 11, 40, 22 }, 11);
    g.roundRect(x, cy - 11, 40, 22, 11, on ? pal.accent : pal.text, on ? 1.f : 0.16f);
    g.circle(on ? x + 29 : x + 11, cy, 8, on ? pal.onAccent : pal.text);
    return hit;
}
