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

/**
 * Automix's chip while it joins two tracks (the user, 2026-10-28: "a real nice animation, like some magic is happening"):
 * it opens from the left with a little overshoot, then lives: a gradient between the two accents drifting across it, a
 * light sweeping over it every 2.4 s, the sparkle twinkling, motes rising off its top and fading, a soft halo breathing
 * around it, and its outline lit lap-wise as the join goes (a full lap at its end); it fades away as the join ends. `k` scales it (the
 * Canvas draws it larger), `alpha` fades it with the text around it. Returns its width.
 */
float App::automixChip(const std::string& label, float x, float cy, float k, float alpha, bool lap, bool glass) {
    if (alpha <= 0.002f) return 0;
    auto mixc = [](const Rgb& a, const Rgb& b, float t) { return Rgb{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; };
    // (the palette itself blends from the outgoing track's to the incoming one's over the join: App::update)
    const Rgb acc = pal.accent, acc2 = pal.accent2, ink = pal.accentInk;
    const bool still = !store.motion;
    const float since = float(time - mixShownAt), left = float(mixUntil - time);
    const float in = still ? 1.f : std::clamp(since / 0.45f, 0.f, 1.f), out = std::clamp(left / 0.4f, 0.f, 1.f);
    const float e = 1 - std::pow(1 - in, 3);                                  // ease-out
    const float pop = 1 + (still ? 0 : 0.08f * std::sin(in * 3.14159f));        // a little overshoot as it opens
    const float a = alpha * e * out;
    const Font f{ int(std::round(css::Xs * k)), Weight::SemiBold };
    const float icon = 15 * k, pad = 8 * k, h = 22 * k * pop;
    const float w = pad + icon + 5 * k + float(g.measure(label, f)) + pad;   // (36 k + the label: the Canvas centres it by that)
    const float ph = still ? 0.f : float(time);
    // a soft halo around it, breathing
    const float br = 0.5f + 0.5f * std::sin(ph * 2.1f);
    // (one ring fading out over 7 px: three layers showed as rings, the user said; then ten layers, each so faint that its
    // alpha had only three or four levels, and the aura breathed in steps, all ten at once: the user, 2026-10-29)
    const float glowA = a * (0.105f + 0.078f * br);   // what the ten layers added up to at the pill's edge
    g.roundRect(x, cy - h / 2, w, h, h / 2, acc, glowA);   // (under the pill, as the layers were)
    g.glowAround(x, cy - h / 2, w, h, h / 2, 7 * k, 7 * k * e, acc, glowA);
    // revealed from the left as it opens
    g.clip(x - 2, cy - h / 2 - 16 * k, (w + 4) * e, h + 32 * k);
    const float drift = 0.5f + 0.5f * std::sin(ph * 1.3f);
    // glass: on the frosted cover, a dark backing first, so it reads on any cover (the user: "barely visible")
    if (glass) g.roundRect(x, cy - h / 2, w, h, h / 2, pal.dark ? Rgb{ 0, 0, 0 } : pal.bg, a * (pal.dark ? 0.55f : 0.8f));
    g.roundRectH(x, cy - h / 2, w, h, h / 2, mixc(acc, acc2, drift * 0.6f), mixc(acc2, acc, drift * 0.6f), a * 0.22f);
    g.roundStroke(x, cy - h / 2, w, h, h / 2, 1, mixc(acc, ink, 0.4f), a * (0.45f + 0.2f * br));
    if (!still) {   // the light sweeping across the whole pill, end to end, every 3.2 s
        // one soft band at fractional positions, shaped to the pill (Gfx::bandInPill): clipped to the pill's box it showed
        // as a pale square in the corners; clipped to its straight middle it began late and stopped early (the user,
        // 2026-10-29); it enters wholly beyond the left end and leaves wholly beyond the right one
        const float half = 16 * k;
        const float sweep = std::fmod(ph, 3.2f) / 3.2f, sx = x - half + sweep * (w + 2 * half);
        g.bandInPill(x + 1, cy - h / 2 + 1, w - 2, h - 2, sx - half, 2 * half, { 1, 1, 1 }, a * 0.24f);
    }
    // the sparkle, twinkling
    const float tw = still ? 1.f : 0.85f + 0.15f * std::sin(ph * 5.3f);
    g.icon(ICON_SPARKLE, x + pad + icon / 2, cy, icon, mixc(ink, { 1, 1, 1 }, 0.35f * tw), a, tw);   // one icon, scaled smoothly
    g.textC(label, f, x + pad + icon + 5 * k, cy, glass ? pal.text : ink, a);
    g.unclip();
    // the join's progress: a full lap of its own outline, from the middle of its left end, clockwise over the top, round the
    // right end and back along the foot, closing as the join ends (the user, 2026-10-28: "the whole edge, a full lap")
    if (lap && mixUntil > mixFrom && e >= 1) {
        const float p = float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
        const float r = h / 2, lx = x + r, rx = x + w - r, straight = rx - lx, arc = 3.14159265f * r;
        const float total = 2 * straight + 2 * arc, want = total * p;
        std::vector<SDL_FPoint>& pts = lapPts_;
        pts.clear();
        // the path in four pieces (left end's upper half, top, right end, foot, left end's lower half), cut at `want`
        auto arcTo = [&](float cxA, float a0, float a1, float from) {   // a half or quarter circle, from length `from`
            const float len = std::fabs(a1 - a0) * r;
            const int seg = std::max(4, int(len / 1.5f));
            for (int i = 0; i <= seg; i++) {
                const float s = from + len * i / seg;
                if (s > want) break;
                const float an = a0 + (a1 - a0) * i / seg;
                pts.push_back({ cxA + r * std::cos(an), cy + r * std::sin(an) });
            }
            return from + len;
        };
        auto lineTo = [&](float x0, float x1, float yy, float from) {
            const float len = std::fabs(x1 - x0);
            const float s = std::min(len, want - from);
            if (s > 0) pts.push_back({ x0 + (x1 > x0 ? s : -s), yy });
            return from + len;
        };
        constexpr float Pi = 3.14159265f;
        float at = arcTo(lx, Pi, 1.5f * Pi, 0);                 // left end, from its middle up to the top
        if (at < want) at = lineTo(lx, rx, cy - r, at);          // the top
        if (at < want) at = arcTo(rx, -0.5f * Pi, 0.5f * Pi, at);   // the right end
        if (at < want) at = lineTo(rx, lx, cy + r, at);          // the foot
        if (at < want) arcTo(lx, 0.5f * Pi, Pi, at);             // the left end's lower half, closing the lap
        if (pts.size() >= 2) g.polyline(pts, 1.6f * k, ink, a * 0.95f);
    }
    if (!still)   // motes rising off its top, each on its own 1.6 s cycle
        for (int i = 0; i < 6; i++) {
            const float cyc = std::fmod(ph / 1.6f + i * 0.37f, 1.f), mx = x + w * (0.12f + 0.76f * std::fmod(i * 0.618f, 1.f));
            const float my = cy - h / 2 - cyc * 14 * k, r = (1.1f + 0.6f * std::fmod(i * 0.41f, 1.f)) * k;
            g.circle(mx + std::sin(ph * 2 + i) * 2 * k, my, r, i % 2 ? ink : acc2, a * 0.8f * std::sin(cyc * 3.14159f));
        }
    return w;
}

void App::lapPath(float x, float y, float w, float h, float r, float p, std::vector<SDL_FPoint>& pts) {
    pts.clear();
    r = std::clamp(r, 0.f, std::min(w, h) / 2);
    constexpr float Pi = 3.14159265f;
    const float sw = w - 2 * r, sh = h - 2 * r, arc = Pi / 2 * r;
    const float want = p * (2 * sw + 2 * sh + 4 * arc);
    float at = 0;
    auto line = [&](float x0, float y0, float x1, float y1) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        if (at >= want || len <= 0) { at += len; return; }
        if (pts.empty()) pts.push_back({ x0, y0 });
        const float t = std::min(1.f, (want - at) / len);
        pts.push_back({ x0 + (x1 - x0) * t, y0 + (y1 - y0) * t });
        at += len;
    };
    auto corner = [&](float cx, float cy, float a0) {   // a quarter circle, clockwise from angle a0, cut at `want`
        const int seg = std::max(3, int(arc / 2));
        const float step = arc / seg;
        for (int i = 0; i < seg && at + i * step < want; i++) {
            const float f = std::min(1.f, (want - at - i * step) / step), an = a0 + Pi / 2 * (i + f) / seg;
            pts.push_back({ cx + r * std::cos(an), cy + r * std::sin(an) });
        }
        at += arc;
    };
    line(x + w / 2, y, x + w - r, y);
    corner(x + w - r, y + r, -Pi / 2);
    line(x + w, y + r, x + w, y + h - r);
    corner(x + w - r, y + h - r, 0);
    line(x + w - r, y + h, x + r, y + h);
    corner(x + r, y + h - r, Pi / 2);
    line(x, y + h - r, x, y + r);
    corner(x + r, y + r, Pi);
    line(x + r, y, x + w / 2, y);
}

/**
 * Automix joining (the user, 2026-10-28: "blur the cover, put the hint inside the blur and bigger"): Now playing's cover
 * frosts over: its own blurred copy (TrackLoader's frost) fades in on top with a light veil, the hint large in the middle
 * on a dark glass backing (and still: the cover breathes with the beat, the hint does not), and the join's progress as a lap round the whole cover, from the
 * middle of its top edge, clockwise ("around the whole cover"); all of it eases in and, as the join ends, back out to the
 * sharp cover.
 */
void App::drawAutomixCover(float x, float y, float s, float hx, float hy, float rest, float Rad) {
    const bool still = !store.motion;
    // in and out over 0.9 s, cubic in and out: the glide's timing (automixFocus), as the Canvas's morph
    auto inOut = [](float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; };
    const float in = still ? 1.f : std::clamp(float(time - mixShownAt) / 0.9f, 0.f, 1.f), out = std::clamp(float(mixUntil - time) / 0.9f, 0.f, 1.f);
    const float e = inOut(in) * inOut(out);
    if (e <= 0.002f) return;
    // the frost: the cover's blur (both, while the covers crossfade), else a tint
    const float k = now.fade;
    // the old frost and the new one as e·(1 − k) and e·k of the whole, the new drawn over the old: whole while the covers
    // change (both at half had thinned it), and nothing of the old left as the frost clears at the join's end (drawn at e
    // under e·k, a quarter of the old cover's colours showed through there: the user, 2026-10-28)
    const float wn = now.frost ? e * (now.prevFrost ? k : 1.f) : 0.f;
    if (now.prevFrost && k < 1) g.image(now.prevFrost, x, y, s, s, Rad, e * (1 - k) / std::max(1e-3f, 1 - wn));
    if (now.frost) g.image(now.frost, x, y, s, s, Rad, wn);
    else g.roundRect(x, y, s, s, Rad, pal.surface2, e * 0.9f);
    g.roundRect(x, y, s, s, Rad, pal.bg, e * (pal.dark ? 0.2f : 0.12f));
    // the hint, large, and what the join does under it
    // by the cover at rest (its size did breathe with the beat), and never wider than most of it (the Canvas's small
    // cover in its Lyrics layout, in a join too short to glide)
    const float k1 = 36 + float(g.measure(mixLabel, { css::Xs, Weight::SemiBold }));
    const float kk = std::min(std::clamp(rest / 210.f, 1.4f, 2.0f), std::max(0.5f, rest * 0.85f / k1));
    const float cw = 36 * kk + float(g.measure(mixLabel, { int(std::round(css::Xs * kk)), Weight::SemiBold }));
    automixChip(mixLabel, hx - cw / 2, hy, kk, e, false, true);   // alone, in the middle of the cover at rest (the user, 2026-10-28)
    // the join's progress: a lap round the whole cover
    if (mixUntil > mixFrom) {
        const float p = float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
        lapPath(x + 1, y + 1, s - 2, s - 2, Rad - 1, p, lapPts_);
        if (lapPts_.size() >= 2) g.polyline(lapPts_, 2.5f, pal.accentInk, e);   // (its colour blends with the palette)
    }
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
