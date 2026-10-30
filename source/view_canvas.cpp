/*
 * Gatefold NX · view_canvas.cpp
 * The Canvas: Gatefold's full-screen Now playing (canvas.css, canvas.js), with
 * its three layouts (cover, lyrics, minimal), drifting backdrop, synced lyrics,
 * and controls that fade away when nothing is touched for a while.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {

float vw() { return css::W / 100.f; }
float vh() { return css::H / 100.f; }
float vmax() { return std::max(vw(), vh()); }
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

/** CSS `ease-in-out infinite alternate` over a period: 0 → 1 → 0 … */
float alternate(double t, float period) {
    const float ph = std::fmod(float(t) / period, 2.f), k = ph < 1 ? ph : 2 - ph;
    return 0.5f - 0.5f * std::cos(k * 3.14159265f);
}

}  // namespace

void App::drawCanvasLyricsOf(float x, float y, float w, float h, int size, int layout, float alpha, const std::vector<LyricLine>& lyrics,
                             bool synced, double at, ScrollArea& sa, bool live) {
    sa.view = { x, y, w, h };
    if (!synced) {
        // .ln.plain: UI font, regular weight; scrolls by touch and the right stick
        // Faded at the bottom as the synced lines are (their mask, below), with room below the last line so every line can
        // be read clear of it; faded at the top too once scrolled, by as much as it is (at the top, the first line sits
        // right under the title as before): cut off at the box, they ran under the progress line and half a line showed at
        // each end (the user, 2026-10-29)
        const Font f{ int(clampf(1.25f * vw(), 16, 22)) };
        const float below = std::round(h * 0.28f);
        float total = 0;
        for (const auto& l : lyrics) total += 26 * g.wrap(l.text.empty() ? " " : l.text, f, int(w), 3).size();
        sa.content = total + below;
        if (live && !g.ghost()) ui.pushArea(&sa);   // a hidden layout's lyrics take no touch or stick
        g.clip(x - 10, y, w + 20, h);
        float ly = y - sa.y;
        const float topK = clampf(sa.y / (h * 0.18f), 0, 1);
        for (const auto& l : lyrics)
            for (const auto& part : g.wrap(l.text.empty() ? " " : l.text, f, int(w), 3)) {
                if (ly + 26 > y && ly < y + h) {
                    const float rel = (ly + 13 - y) / h;
                    const float m = rel < 0.18f ? 1 - topK * (1 - clampf(rel / 0.18f, 0, 1)) : rel > 0.72f ? clampf((1 - rel) / 0.28f, 0, 1) : 1.f;
                    if (m * alpha > 0.01f) g.text(part, f, x, ly, pal.text2, alpha * m);
                }
                ly += 26;
            }
        g.unclip();
        if (live && !g.ghost()) ui.popArea();
        return;
    }
    // .lyr-canvas .ln: 760 weight, line-height 1.24, padding .32em; the current line sits a little above the middle
    const Font f{ size, Weight::ExtraBold };
    const float lineH = std::round(size * 1.24f), pad = std::round(size * 0.32f);
    const double pos = at + 0.15;
    int cur = -1;
    for (int i = 0; i < int(lyrics.size()); i++) if (lyrics[i].t <= pos) cur = i;

    std::vector<std::vector<std::string>> parts(lyrics.size());
    std::vector<float> top(lyrics.size());
    float yy = h * 0.45f;   // room above the first line, so it can sit at the focus point too
    for (size_t i = 0; i < lyrics.size(); i++) {
        parts[i] = lyrics[i].text.empty() ? std::vector<std::string>{ "♪" } : g.wrap(lyrics[i].text, f, int(w - 20), 3);
        top[i] = yy;
        yy += pad * 2 + lineH * parts[i].size();
    }
    sa.content = yy + h;
    const int focusLine = std::max(cur, 0);
    const float blockH = pad * 2 + lineH * (parts.empty() ? 1 : parts[focusLine].size());
    // Scrolled by hand: left there for 4 s, then back to the line playing.
    if (!live || ui.now() - sa.manual > 4) sa.target = top.empty() ? 0 : std::max(0.f, top[focusLine] + blockH / 2 - h * 0.45f);

    if (!live) sa.y = sa.target;   // (the outgoing song's: it stays on its line)
    if (live && !g.ghost()) ui.pushArea(&sa);   // a hidden layout's lyrics take no touch or stick
    // text-shadow: 0 0 40px glow — drawn before the clip so it fades out instead of ending at the box edge
    if (cur >= 0) {
        const float by = y + top[cur] - sa.y, bh = pad * 2 + lineH * parts[cur].size();
        const float rel = (by + bh / 2 - y) / h;
        const float m = rel < 0.18f ? clampf(rel / 0.18f, 0, 1) : rel > 0.72f ? clampf((1 - rel) / 0.28f, 0, 1) : 1.f;
        const float tw = std::min(w, float(g.measure(parts[cur][0], f)));
        g.glow(x + tw / 2, by + bh / 2, std::max(tw * 0.42f, bh), pal.glow, 0.16f * m * alpha);
    }
    g.clip(x - 10, y, w + 20, h);
    for (size_t i = 0; i < lyrics.size(); i++) {
        const float by = y + top[i] - sa.y;
        const float bh = pad * 2 + lineH * parts[i].size();
        if (by + bh < y || by > y + h) continue;
        // opacity: .cur 1, .near .5, .past .22, the rest .34
        const int d = int(i) - cur;
        float a = d == 0 ? 1.f : (d == 1 || d == -1) ? 0.5f : d < 0 ? 0.22f : 0.34f;
        // mask-image: linear-gradient(transparent, #000 18%, #000 72%, transparent)
        const float rel = (by + bh / 2 - y) / h;
        a *= rel < 0.18f ? clampf(rel / 0.18f, 0, 1) : rel > 0.72f ? clampf((1 - rel) / 0.28f, 0, 1) : 1.f;
        a *= alpha;
        if (a <= 0.01f) continue;
        if (live && alpha > 0.5f && ui.item(uid("cv.ln", int(i)), { x, by, w, bh }, 8)) seekLyric(int(i));
        float ly = by + pad;
        if (lyrics[i].text.empty()) {   // a break: Gatefold's three dots, sized with the lines (7 px at the panel's size)
            lyricDots(x, ly + lineH / 2, std::clamp(std::round(size * 0.24f), 7.f, 14.f), pal.text, a, d == 0);
            continue;
        }
        for (const auto& line : parts[i]) {
            g.text(line, f, x, ly, pal.text, a, Align::Left, int(w));
            ly += lineH;
        }
    }
    g.unclip();
    if (live && !g.ghost()) ui.popArea();
}

void App::drawCanvasLyrics(float x, float y, float w, float h, int size, int layout, float alpha) {
    // a Simple join (Settings › Appearance › Automix › On the Canvas): the lyrics fade out, the hint takes their place, the
    // next song's fade back in (App::hintInLyrics; the user, 2026-10-29); the outgoing song's until the join's middle, on
    // their own clock. Drawn when hidden too, so the next song's glide to their line meanwhile.
    if (store.mixAnimCanvas == 1) {
        const float e = automixCoverEase();
        if (e > 0.002f) {
            if (mixOld.on && mixWipe() < 0.5f)
                drawCanvasLyricsOf(x, y, w, h, size, layout, alpha * (1 - e), mixOld.lyrics, mixOld.synced, mixOld.lyricsAt + (time - mixOld.at),
                                   area("cv.lyrics.old", layout), false);
            else drawCanvasLyricsOf(x, y, w, h, size, layout, alpha * (1 - e), now.lyrics, now.synced, lyricsPos(), area("cv.lyrics", layout), true);
            if (hintInLyrics(true) && !g.ghost()) {
                const float f0 = g.fade();
                g.setFade(f0 * alpha);
                lyricsHint(x, y, w, h, layout == 1 ? 2.f : 1.7f);
                g.setFade(f0);
            }
            return;
        }
    }
    drawCanvasLyricsOf(x, y, w, h, size, layout, alpha, now.lyrics, now.synced, lyricsPos(), area("cv.lyrics", layout), true);
}

void App::drawCanvas() {
    ui.region(Region::Overlay);
    const bool has = now.track >= 0;
    const double t = store.motion ? time : 0;

    // The layout shown: the user's, except that a track without lyrics gets Minimal (the user's choice is kept and
    // comes back with the next track that has lyrics). Every layout has a weight that eases towards being shown,
    // so a change moves the cover and crossfades the text instead of cutting.
    // The switch runs on the clock, not per frame, so a slow frame (a track loading) cannot make it jump:
    // the cover travels on an ease-in-out curve over 0.9 s; the old text fades out first, the new eases in once the cover has nearly arrived.
    // Automix joining (automixFocus, as on Now playing; the user, 2026-10-29): the cover glides from the layout to a stage in
    // the middle and frosts over with the hint while the layout's text fades out; as the join ends it glides to the layout
    // the incoming track needs (Minimal without lyrics). A layout change while the stage hides the layout (one track with
    // lyrics, the other without) is taken at once: the return is a single move, never a switch in the middle of the join
    const bool simple = store.mixAnimCanvas == 1;   // Settings › Appearance › Automix › On the Canvas: Simple
    const Focus fc = automixFocus(simple ? 2 : 0);
    const float m = fc.move;
    const bool mixing = has && time < mixUntil;
    // Simple: the layout stays as it is through the join (the texts give way in place), and changes after it if it must
    const int layout = simple && mixing ? cvTo : canvasLayoutShown();
    auto progress = [&] { return clampf(float(time - cvStart) / 0.9f, 0, 1); };
    auto inOut = [](float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; };
    auto weights = [&](float e, float* w) { for (int i = 0; i < 3; i++) w[i] = cvFrom[i] + ((i == cvTo ? 1.f : 0.f) - cvFrom[i]) * e; };
    if (layout != cvTo && m > 0.99f) {
        for (int i = 0; i < 3; i++) cvFrom[i] = i == layout ? 1.f : 0.f;
        cvTo = layout;
        cvStart = -10;
    } else if (layout != cvTo) {
        float now3[3];
        weights(inOut(progress()), now3);   // start from wherever the cover is, even mid-way through another switch
        std::copy(now3, now3 + 3, cvFrom);
        cvTo = layout;
        cvStart = time;
    }
    const float p = progress();
    float wts[3];
    weights(inOut(p), wts);
    float textA[3];
    for (int i = 0; i < 3; i++) {
        const float in = clampf((p - 0.6f) / 0.4f, 0, 1), out = clampf(p / 0.4f, 0, 1);
        textA[i] = i == cvTo ? std::max(cvFrom[i], 1 - (1 - in) * (1 - in) * (1 - in)) : cvFrom[i] * (1 - out) * (1 - out);
        textA[i] *= fc.ui;   // and out while the stage is up (Automix)
    }
    // (in a Simple join, while either song has lyrics: the outgoing song's give way to the next one's)
    const bool lyricsOn = store.canvasLyrics && (!now.lyrics.empty() || (simple && mixOld.on && !mixOld.lyrics.empty()));
    // Simple: the whole Canvas but its controls (the progress line, the buttons) changes from the outgoing song to the next
    // from left to right, the background and the cover with it (the user, 2026-10-29): each section across its own width,
    // all on the join's clock (App::pageWipe_)
    struct PageOff { PageWipe& p; ~PageOff() { p.on = false; } } pageOff{ pageWipe_ };
    if (simple && mixing && mixOld.on) pageWipe_.on = true;

    // ---- .cv-bg: blurred cover (inset −12%, .62, slow drift), two glows, vignette
    g.fill(pal.bg);
    const float drift = 1.06f + 0.12f * alternate(t, 70);
    auto blur = [&](SDL_Texture* tex, float a) {
        if (!tex || a <= 0 || g.ghost()) return;
        const float bw = css::W * 1.24f * drift, bh = css::H * 1.24f * drift;
        const SDL_FRect r{ (css::W - bw) / 2, (css::H - bh) / 2, bw, bh };
        SDL_SetTextureAlphaMod(tex, Uint8(clampf(a, 0, 1) * 255));
        SDL_RenderCopyF(renderer_, tex, nullptr, &r);
    };
    // the new over the old, as the share of each a crossfade gives: both at half thinned the blur mid-way (as Now playing's frost)
    if (pageWipe_.on) {   // each song's on its side of the page's edge
        const float bw = css::W * 1.24f * drift, bh = css::H * 1.24f * drift;
        wipedImage(0, float(css::W), 0, float(css::H), false, true, [&](bool old) {   // (no light down the whole screen)
            if (SDL_Texture* tex = mixCover(old, now.blur, now.prevBlur)) g.imageStretched(tex, (css::W - bw) / 2, (css::H - bh) / 2, bw, bh, 0, 0.62f);
        });
    } else {
        const float bk = now.fade, bn = now.blur ? 0.62f * (now.prevBlur ? bk : 1.f) : 0.f;
        if (bk < 1) blur(now.prevBlur, 0.62f * (1 - bk) / std::max(1e-3f, 1 - bn));
        blur(now.blur, bn);
    }
    const float g1 = alternate(t, 60), g3 = alternate(t, 76);
    g.glow(-20 * vmax() + 35 * vmax() + 14 * vmax() * g1, -24 * vmax() + 35 * vmax() + 10 * vmax() * g1, 35 * vmax() * (1 + 0.15f * g1), pal.blobs[0], 0.5f);
    g.glow(css::W + 22 * vmax() - 35 * vmax() + 16 * vmax() * g3, css::H + 30 * vmax() - 35 * vmax() - 12 * vmax() * g3, 35 * vmax() * (1 + 0.1f * g3),
           pal.blobs[2], 0.5f);
    g.vignette(css::W * 0.5f, css::H * 0.45f, css::W * 1.2f, css::H * 0.9f, pal.bg, 0.7f);
    g.rectV(0, css::H * 0.7f, css::W, css::H * 0.3f, pal.bg, 0, pal.bg, 0.55f);

    // ---- .cv-stage, per layout
    struct Stage { float art = 0, ax = 0, ay = 0, sx = 0, sy = 0, sw = 0, sh = 0; };
    auto stage = [&](int l) {
        Stage s;
        if (l == 1) {
            s.art = std::min({ 16 * vh(), 12 * vw(), 200.f });
            s.ax = 8 * vw();
            s.ay = 7 * vh();
        } else if (l == 2) {
            s.art = std::min({ 54 * vh(), 62 * vw(), 900.f });
            const float top = 4 * vh(), bottom = css::H - 25 * vh();   // the text ends well above the seek bar
            const float textH = 3.5f * vh() + 34 + 6 + 24 + 6 + 20;
            s.ax = (css::W - s.art) / 2;
            s.ay = top + (bottom - top - s.art - textH) / 2;
        } else {
            s.art = std::min({ 62 * vh(), 44 * vw(), 860.f, css::H - 190.f });
            const float colW = std::min(46 * vw(), 760.f), gap = clampf(6 * vw(), 32, 110);
            const float padT = 5 * vh(), padB = std::max(19 * vh(), 170.f), padX = 6 * vw();
            const float total = s.art + gap + colW;
            s.ax = padX + (css::W - padX * 2 - total) / 2;
            s.ay = padT + (css::H - padT - padB - s.art) / 2;
            s.sx = s.ax + s.art + gap;
            s.sw = colW;
            s.sh = std::min(78 * vh(), s.art * 1.15f);
            s.sy = padT + (css::H - padT - padB - s.sh) / 2;
        }
        return s;
    };
    const Stage st[3] = { stage(0), stage(1), stage(2) };
    float art = 0, ax = 0, ay = 0;
    for (int i = 0; i < 3; i++) { art += st[i].art * wts[i]; ax += st[i].ax * wts[i]; ay += st[i].ay * wts[i]; }
    if (m > 0) {   // the stage: the Cover layout's cover (the largest), centred, at its height (clear of the controls)
        const float sa = st[0].art;
        art += (sa - art) * m;
        ax += ((css::W - sa) / 2 - ax) * m;
        ay += (st[0].ay - ay) * m;
    }
    const float restArt = art, restX = ax, restY = ay;   // the hint holds still while the cover pulses

    // .cv-art: a slow float, the cover with a Ken Burns zoom, a deep shadow. Both motions can be turned off in Settings → Canvas.
    // (it rests through an Automix join, easing out with the glide and back in after it, as the pulse does: the user, 2026-10-29)
    const float floatY = store.motion && store.canvasFloat ? (-5 + 11 * alternate(t, 14)) * (1 - std::max(m, fc.quiet)) : 0;
    const float rad = clampf(1.2f * vw(), 10, 18);
    // Optional bass pulse (Settings → Visualizer), scaled from 50% 60% like the Now playing cover
    if (store.canvasPulse && coverBeat > 0) {
        const float grown = art * (1 + coverBeat);
        ax -= (grown - art) / 2;
        ay -= (grown - art) * 0.6f;
        art = grown;
    }
    const float fy = ay + floatY;
    // (Gatefold's drop shadow, 0 50px 120px -30px, is left out as on Now playing: on the console's dark backgrounds it read
    // as a dark aura under the cover, the user said, 2026-10-29)
    g.roundRect(ax, fy, art, art, rad, pal.surface2);
    const float ken = store.motion && store.canvasZoom ? alternate(t, 42) : 0;
    // the new over the old, drawn whole (as Now playing's): a frost over them never thins mid-way
    if (pageWipe_.on) {   // the outgoing cover gives way to the next one as the page's edge crosses it
        wipedImage(ax, ax + art, fy, fy + art, false, !now.cover, [&](bool old) {   // (the magic: its frost's)
            if (SDL_Texture* c = mixCover(old, now.cover, now.prevCover)) g.image(c, ax, fy, art, art, rad, 1, 1 + 0.075f * ken, -ken * 0.3f, ken * 0.2f);
            else g.icon(ICON_MUSIC, ax + art / 2, fy + art / 2, std::min(72.f, art * 0.4f), pal.text3);
        });
    } else {
        if (now.prevCover && now.fade < 1) g.image(now.prevCover, ax, fy, art, art, rad, 1, 1 + 0.075f * ken, -ken * 0.3f, ken * 0.2f);
        if (now.cover) g.image(now.cover, ax, fy, art, art, rad, now.fade, 1 + 0.075f * ken, -ken * 0.3f, ken * 0.2f);
        else g.icon(ICON_MUSIC, ax + art / 2, fy + art / 2, std::min(72.f, art * 0.4f), pal.text3);
    }
    // Automix: the frost, the hint and the lap round the cover, as on Now playing (the small chip beside PLAYING is gone)
    if (mixing) {
        // Simple: above the cover (Minimal: on it), or with lyrics on screen in their place (drawCanvasLyrics); Full: on it.
        // Its own places: the hint's setting is Now playing's (App::nowHintAbove)
        const bool inside = !(simple && cvTo != 2);
        const bool inLyrics = simple && lyricsOn && cvTo != 2 && hintInLyrics(true);
        drawAutomixCover(ax, fy, art, restX + restArt / 2, restY + floatY + restArt / 2, restArt, rad, inside && !inLyrics);
        if (!inside && !inLyrics) {   // above the cover (the user, 2026-10-29); the Lyrics layout's small one: on the right, level with its middle
            constexpr float K = 1.1f;
            const float cw = 36 * K + float(g.measure(mixLabel, { int(std::round(css::Xs * K)), Weight::SemiBold }));
            if (cvTo != 1 || m > 0.5f)
                automixChip(mixLabel, restX + restArt / 2 - cw / 2, std::max(restY + floatY - 4 - 11 * K, 16 * K), K, automixCoverEase(), false, true);
            else automixChip(mixLabel, css::W - 8 * vw() - cw, restY + floatY + restArt / 2, K, automixCoverEase(), false, true);
        }
    }
    g.roundStroke(ax, fy, art, art, rad, 1, { 1, 1, 1 }, 0.06f);

    // ---- .cv-meta (+ lyrics): each layout's own, faded by its weight and drifting in by a few pixels
    const std::string kick = !has ? "NOTHING PLAYING" : isPlaying() ? "PLAYING" : "PAUSED";
    auto albumOf = [&](const TrackInfo& in) {
        std::string al = in.album;
        if (has && in.date.size() >= 4) al += (al.empty() ? "" : " · ") + in.date.substr(0, 4);
        return al;
    };
    const std::string title = has ? now.info.title : "Songate";
    // a Simple join (Settings › Appearance › Automix › On the Canvas): the outgoing song's title, artist, album and lyrics give
    // way to the next one's, from left to right, timed to the join (the user, 2026-10-29); else the song's own
    const bool give = simple && mixOld.on;
    auto infoOf = [&](bool old) -> const TrackInfo& { return old ? mixOld.info : now.info; };
    const Font kf{ int(clampf(0.85f * vw(), 11, 14)), Weight::Bold };
    for (int l = 0; l < 3; l++) {
        // the stage hiding them all: only the layout it returns to, as a ghost (its text ready); hidden interface drawn at
        // nothing cost Now playing frames on the console
        if (fc.ui <= 0.001f && l != cvTo) continue;
        float A = textA[l];
        // A hidden layout is drawn as a ghost (Gfx::setGhost): its text is ready for when it shows.
        const bool ghost = A < 0.01f, wasGhost = g.ghost();
        if (ghost) { g.setGhost(true); A = 0.001f; }   // ~0: no taps on its lyric lines, nothing visible
        const Stage& s = st[l];
        const float dy = (1 - A) * 14;
        // The drift is the moving part of every text position here: it glides sub-pixel (Gfx::setShift).
        textShift = dy;
        g.setShift(0, dy);
        if (l == 2) {
            // minimal: centred under the cover, no kicker
            const Font tf{ int(clampf(2.2f * vw(), 22, 40)), Weight::ExtraBold };
            auto block = [&](bool old) {
                const TrackInfo& in = infoOf(old);
                float ty = s.ay + s.art + 3.5f * vh() + dy;
                g.text(has ? in.title : title, tf, css::W / 2.f, ty, pal.text, A, Align::Center, int(css::W * 0.8f));
                ty += tf.size * 1.25f + 6;
                g.text(in.artist, { int(clampf(1.3f * vw(), 15, 24)), Weight::SemiBold }, css::W / 2.f, ty, pal.text, A, Align::Center, int(css::W * 0.8f));
                ty += 24 + 6;
                g.text(albumOf(in), { int(clampf(1.05f * vw(), 14, 19)) }, css::W / 2.f, ty, pal.text2, A, Align::Center, int(css::W * 0.8f));
            };
            if (give) {
                float wide = 0;
                for (bool o : { true, false })
                    wide = std::max({ wide, float(g.measure(infoOf(o).title, tf)), float(g.measure(infoOf(o).artist, { int(clampf(1.3f * vw(), 15, 24)), Weight::SemiBold })) });
                wide = std::min(wide, css::W * 0.8f);
                const float by0 = s.ay + s.art + 3.5f * vh() + dy;
                wiped(css::W / 2.f - wide / 2, css::W / 2.f + wide / 2, by0, by0 + tf.size * 1.25f + 6 + 24 + 6 + 20, block);
            } else {
                block(false);
            }
        } else if (l == 1) {
            // lyrics first: a small cover with the meta beside it, large lines below
            const float mx = s.ax + s.art + 28, mw = css::W - 8 * vw() - mx;
            const Font tf{ int(clampf(2 * vw(), 22, 40)), Weight::ExtraBold };
            const Font arf{ int(clampf(1.2f * vw(), 15, 24)), Weight::SemiBold };
            const float block = kf.size * 1.4f + 4 + tf.size * 1.1f + 4 + arf.size * 1.4f;
            float my = s.ay + (s.art - block) / 2 + dy;
            g.textSpaced(kick, kf, mx, my + kf.size * 0.7f, 0.16f, pal.accentInk, A);
            my += kf.size * 1.4f + 4;
            auto metaBlock = [&](bool old) {
                const TrackInfo& in = infoOf(old);
                g.text(has ? in.title : title, tf, mx, my - 3, pal.text, A, Align::Left, int(mw));
                g.text(in.artist, arf, mx, my + tf.size * 1.1f + 4, pal.text, A, Align::Left, int(mw));
            };
            if (give) {
                float wide = 0;
                for (bool o : { true, false }) wide = std::max({ wide, float(g.measure(infoOf(o).title, tf)), float(g.measure(infoOf(o).artist, arf)) });
                wiped(mx, mx + std::min(mw, wide), my - 4, my + tf.size * 1.1f + 4 + arf.size * 1.4f, metaBlock);
            }
            else metaBlock(false);
            my += tf.size * 1.1f + 4;
            // ending where the Cover layout's lyrics end (its side column's foot), for consistency (the user, 2026-10-29; it
            // ran to 80 % of the screen, past the progress line)
            const float ly = s.ay + s.art + 3 * vh() + dy, lh = st[0].sy + st[0].sh + dy - ly;
            if (lyricsOn) drawCanvasLyrics(8 * vw(), ly, css::W - 16 * vw(), lh, int(clampf(3.3f * vw(), 28, 66)), 1, A);
        } else {
            // cover: meta at the top of the side column, lyrics fill the rest; without lyrics the meta is centred
            const Font tf{ int(clampf(3.6f * vw(), 30, 72)), Weight::ExtraBold };
            const Font arf{ int(clampf(1.7f * vw(), 18, 32)), Weight::SemiBold };
            const Font alf{ int(clampf(1.05f * vw(), 14, 19)) };
            const std::vector<std::string> lines = g.wrap(title, tf, int(s.sw), 2);
            const float metaH = kf.size * 1.4f + 10 + lines.size() * std::round(tf.size * 1.02f) + 10 + arf.size * 1.35f + 6 + alf.size * 1.45f;
            float my = (lyricsOn ? s.sy : s.ay + (s.art - metaH) / 2) + dy;
            g.textSpaced(kick, kf, s.sx, my + kf.size * 0.7f, 0.16f, pal.accentInk, A);
            my += kf.size * 1.4f + 10;
            auto block = [&](bool old) {   // the next song's lines decide the layout
                const TrackInfo& in = infoOf(old);
                const std::vector<std::string> ls = old ? g.wrap(in.title, tf, int(s.sw), 2) : lines;
                float yy = my;
                for (const auto& ln : ls) {
                    g.text(ln, tf, s.sx - 2, yy - tf.size * 0.12f, pal.text, A, Align::Left, int(s.sw));
                    yy += std::round(tf.size * 1.02f);
                }
                yy = my + lines.size() * std::round(tf.size * 1.02f) + 10;
                g.text(in.artist, arf, s.sx, yy, pal.text, A, Align::Left, int(s.sw));
                g.text(albumOf(in), alf, s.sx, yy + arf.size * 1.35f + 6, pal.text2, A, Align::Left, int(s.sw));
            };
            if (give) {
                float wide = 0;
                for (bool o : { true, false }) {
                    for (const auto& ln : g.wrap(infoOf(o).title, tf, int(s.sw), 2)) wide = std::max(wide, float(g.measure(ln, tf)));
                    wide = std::max({ wide, float(g.measure(infoOf(o).artist, arf)), float(g.measure(albumOf(infoOf(o)), alf)) });
                }
                wiped(s.sx, s.sx + std::min(s.sw, wide), my, my + lines.size() * std::round(tf.size * 1.02f) + 10 + arf.size * 1.35f + 6 + alf.size * 1.45f,
                      block);
            }
            else block(false);
            my += lines.size() * std::round(tf.size * 1.02f) + 10 + arf.size * 1.35f + 6 + alf.size * 1.45f;
            if (lyricsOn) {
                const float ly = my + 4 * vh();
                drawCanvasLyrics(s.sx, ly, s.sw, s.sy + s.sh + dy - ly, int(clampf(2.05f * vw(), 20, 40)), 0, A);
            }
        }
        g.setGhost(wasGhost);
        textShift = 0;
        g.setShift(0, 0);
    }
    pageWipe_.on = false;   // (the controls: not wiped)

    // ---- .cv-chrome: hairline progress, transport, tools; fades out when idle
    const bool idle = canvasIdle > store.canvasIdleAfter && isPlaying();
    // Coming back from idle, the spectrum folds back first: the controls only return in its last moments,
    // so they never sit on top of the bars (going idle, they leave first and the bars rise after).
    canvasCtl += ((idle || canvasViz > 0.12f ? 0.f : 1.f) - canvasCtl) * (1 - std::exp(-dt / 0.12f));
    const float padX = 4 * vw(), bottom = css::H - 3.2f * vh();
    // .cv-progress sits just above the transport, as on PC: closer to the buttons than to the text above
    const float toolsCy = bottom - 21, transCy = toolsCy - 21 - 6 - 8 - 30, progCy = transCy - 30 - 14;
    const float ctlA = canvasCtl, slide = (1 - canvasCtl) * 14;
    const bool live = canvasCtl > 0.3f;

    // .cv-progress: 3 px hairline, accent fill with a soft glow
    const double dur = now.info.duration;
    const float pw = css::W - padX * 2;
    const Id pid = uid("cv.progress");
    float frac = dur > 0 ? float(clampf(float(player.position() / dur), 0, 1)) : 0;
    if (live) {
        ui.item(pid, { padX, progCy - 12, pw, 24 }, 6, Slider);
        if (const int st = ui.sliderStep(pid)) seekBy(st * 5);
        const float d = ui.sliderDrag(pid);
        if (d >= 0) frac = clampf((d - padX) / pw, 0, 1);
        if (ui.sliderReleased(pid) && dur > 0) player.seek(clampf((ui.lastDragX() - padX) / pw, 0, 1) * dur);
    }
    // Idle: the hairline slides into the room the controls leave and grows into a spectrum.
    // A constant pace over Settings → Canvas → Spectrum rise, eased at both ends (smootherstep).
    canvasViz = clampf(canvasViz + (idle ? 1.f : -1.6f) * dt / std::max(0.1f, store.canvasVizTime), 0, 1);   // back a little faster: a press waits on it
    const float vz = canvasViz * canvasViz * canvasViz * (canvasViz * (canvasViz * 6 - 15) + 10);
    const float base = progCy + vz * (css::H - 6 * vh() - progCy);
    if (vz < 0.999f) {
        const float a = 1 - vz;
        g.roundRect(padX, base - 1.5f, pw, 3, 1.5f, pal.text, 0.16f * a);
        g.roundRect(padX, base - 4.5f, pw * frac, 9, 4.5f, pal.accent, 0.18f * a);
        g.roundRect(padX, base - 1.5f, pw * frac, 3, 1.5f, pal.accent, a);
        if (ui.engaged(pid)) {   // picked up with A: a knob where ←/→ move it
            g.circle(padX + pw * frac, base, 9, pal.accent, a);
            g.circle(padX + pw * frac, base, 6, pal.text, a);
        }
    }
    if (vz > 0.001f) {
        // viz.js proportions: gap max(2, w/n·0.28); bars from the 56 bands, interpolated across the width
        const int n = std::max(2, spec.bands);   // Settings → Visualizer → Bars in the Canvas
        const float gap = std::max(2.f, std::round(pw / n * 0.28f)), bw = (pw - gap * (n - 1)) / n;
        const float maxH = 92, played = padX + pw * frac;
        for (int i = 0; i < n; i++) {
            const float v = spec.bars[i], pk = spec.peaks[i];
            const float h = 2 + (std::max(2.f, v * maxH) - 2) * vz;
            const float bx = padX + i * (bw + gap);
            const float a = vz * (bx + bw <= played ? 1.f : 0.4f);   // what has played stays bright: still a progress bar
            const float t = h / maxH;
            const Rgb top{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                           pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
            g.roundRectV(bx, base - h, bw, h, std::min({ bw / 2, 4.f, h / 2 }), top, pal.accent2, a);
            if (store.vizPeaks && pk * maxH > 3) g.rect(bx, base - pk * maxH * vz - 4, bw, 1.5f, pal.accent, 0.55f * a);
        }
    }
    // Faded out, the controls still run as a ghost: their text and icons stay ready, so the fade-in
    // does not begin by rendering them. Their slide is the moving part of their positions.
    const bool wasGhost = g.ghost();
    if (ctlA <= 0.01f) g.setGhost(true);
    g.setShift(0, slide);

    // .cv-time + .cv-transport
    const Font timeF{ 13, Weight::SemiBold };
    const float timeCy = transCy + slide;   // level with the buttons, ~43 px under the bar as on PC
    // in a Simple join the outgoing song's times (where it is, its length) until the swipe passes them: one swipe from the
    // left one to the right one, on the join's clock (the user, 2026-10-29: the next song's length showed at once)
    auto times = [&](bool old) {
        const double pos = old ? mixOld.posAt + (time - mixOld.at) : player.position();
        g.textC(fmtTime(pos), timeF, padX, timeCy, pal.text2, ctlA);
        g.textC(fmtTime(old ? mixOld.info.duration : dur), timeF, css::W - padX, timeCy, pal.text2, ctlA, Align::Right);
    };
    if (simple && mixing && mixOld.on) {
        wipeGroup_ = { true, padX, css::W - padX };
        wiped(padX, css::W - padX, timeCy - 10, timeCy + 10, times);
        groupMagic(timeCy - 12, timeCy + 12);
        wipeGroup_.on = false;
    } else {
        times(false);
    }
    // disabled: greyed like the layouts that cannot show, and neither focusable nor pressable
    auto cvBtn = [&](Id id, const char* icon, float cx, float cy, float size, bool big, bool on = false, bool disabled = false, uint8_t flags = 0) {
        bool hit = false;
        if (live && !disabled) hit = ui.item(id, { cx - size / 2, cy - size / 2, size, size }, size / 2, uint8_t((big ? DefaultFocus : 0) | flags));
        const bool hot = !disabled && (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing()));
        const float s = ui.pressed(id) ? 0.94f : 1.f;
        if (big) g.circle(cx, cy, size / 2 * s, pal.accent, ctlA);
        else g.circle(cx, cy, size / 2 * s, pal.text, (hot ? 0.16f : 0.08f) * ctlA);
        g.icon(icon, cx, cy, big ? 26 : 18, big ? pal.onAccent : on && !disabled ? pal.accentInk : pal.text, ctlA * (disabled ? 0.35f : 1.f));
        return hit;
    };
    const float mid = css::W / 2.f, tc = transCy + slide;
    hint(uid("cv.prev"), "Previous");
    hint(uid("cv.play"), isPlaying() ? "Pause" : "Play");
    hint(uid("cv.next"), "Next");
    hint(uid("cv.mute"), volumeShown <= 0.001f ? "Unmute" : "Mute");
    hint(uid("cv.sleep"), "Sleep timer");
    hint(uid("cv.lyrics"), store.canvasLyrics ? "Hide the lyrics" : "Show the lyrics");
    hint(uid("cv.eq"), "Equalizer (hold for its settings)");
    hint(uid("cv.close"), "Close the Canvas");
    if (cvBtn(uid("cv.prev"), ICON_PREV, mid - 30 - 14 - 21, tc, 42, false)) prev();
    if (cvBtn(uid("cv.play"), isPlaying() ? ICON_PAUSE : ICON_PLAY, mid, tc, 60, true)) togglePlay();
    if (cvBtn(uid("cv.next"), ICON_NEXT, mid + 30 + 14 + 21, tc, 42, false)) next(false);

    // .cv-tools: layout segment, volume, lyrics, close
    const Font segF{ 13, Weight::SemiBold };
    static const char* layouts[] = { "Cover", "Lyrics", "Minimal" };
    float segW = 6;
    for (const char* l : layouts) segW += g.measure(l, segF) + 28;
    const float volW = 42 + 4 + 120 + 12;   // mute, gap, range, padding (the thumb travels inside the range)
    // The sleep timer's moon: a pill with the time left while it runs.
    const double sleepLeftNow = sleepLeft();
    const Font sleepF{ 13, Weight::SemiBold };
    const float sleepW = sleepLeftNow > 0 ? 42 + g.measure("88:88", sleepF) + 4 : 42;
    // The lyrics' timing, while synced lyrics show (the Cover and Lyrics layouts, with the lyrics switch on)
    const bool syncShown = canvasLayoutShown() != 2 && store.canvasLyrics && lyricTimingShown();
    const Font syncF{ 13, Weight::SemiBold };
    const float syncW = syncShown ? lyricSyncControl(nullptr, 0, 0, 42, syncF, 0, false) : 0;
    const float totalW = segW + 8 + volW + 8 + sleepW + 8 + (syncShown ? syncW + 8 : 0) + 42 + 8 + 42 + 8 + 42;
    float x = mid - totalW / 2;
    const float cy = toolsCy + slide;
    g.roundRect(x, cy - 19, segW, 38, 19, pal.text, 0.08f * ctlA);
    float bx = x + 3;
    // While Minimal is forced (no lyrics), the layouts that cannot show are greyed and ignore presses,
    // and the one actually shown is the raised one.
    const unsigned off = canvasLayoutsOff();
    for (int i = 0; i < 3; i++) {
        const float w = g.measure(layouts[i], segF) + 28;
        const Id id = uid("cv.layout", i);
        const bool disabled = off & (1u << i);
        if (live && !disabled && ui.item(id, { bx, cy - 16, w, 32 }, 16)) { store.canvasLayout = i; persistSoon(0.3f); }
        const bool on = canvasLayoutShown() == i, hot = !disabled && ui.focused(id) && ui.showFocusRing();
        if (on || hot) g.roundRect(bx, cy - 16, w, 32, 16, pal.text, (on ? 0.16f : 0.08f) * ctlA);
        g.textC(layouts[i], segF, bx + w / 2, cy, on ? pal.text : pal.text2, ctlA * (disabled ? 0.35f : 1.f), Align::Center);
        bx += w;
    }
    x += segW + 8;
    // .cv-vol: mute button + range (track in --text, not the accent)
    g.roundRect(x, cy - 21, volW, 42, 21, pal.text, 0.08f * ctlA);
    {
        const Id mid2 = uid("cv.mute");
        if (live && ui.item(mid2, { x, cy - 21, 42, 42 }, 21)) {
            static float before = 1;
            if (volumeShown > 0.001f) { before = volumeShown; volumeShown = 0; }
            else volumeShown = before > 0.01f ? before : 1;
            player.setVolume(volumeShown);
        }
        g.icon(volumeShown <= 0.001f ? ICON_VOL_X : ICON_VOL, x + 21, cy, 18, pal.text, ctlA);
        const float rx = x + 42 + 4, rw = 120;
        const Id rid = uid("cv.vol");
        float v = volumeShown;
        if (live) {
            ui.item(rid, { rx, cy - 11, rw, 22 }, 11, Slider);
            if (const int st = ui.sliderStep(rid)) v = clampf(v + st * 0.05f, 0, 1);
            if (ui.sliderDrag(rid) >= 0) v = clampf((ui.sliderDrag(rid) - rx - 7) / (rw - 14), 0, 1);
        }
        if (v != volumeShown) { volumeShown = v; player.setVolume(v); }
        const float tx = rx + 7 + (rw - 14) * volumeShown;
        g.roundRect(rx, cy - 2, rw, 4, 2, pal.text, 0.22f * ctlA);
        g.roundRect(rx, cy - 2, tx - rx, 4, 2, pal.text, ctlA);
        g.circle(tx, cy + 1, 8, { 0, 0, 0 }, 0.3f * ctlA);
        if (ui.engaged(rid)) g.circle(tx, cy, 11, pal.accent, ctlA);   // picked up with A
        g.circle(tx, cy, 7, pal.text, ctlA);
    }
    x += volW + 8;
    {
        const Id sid = uid("cv.sleep");
        if (live && ui.item(sid, { x, cy - 21, sleepW, 42 }, 21)) openSleep();
        const bool hot = ui.pressed(sid) || (ui.focused(sid) && ui.showFocusRing());
        g.roundRect(x, cy - 21, sleepW, 42, 21, pal.text, (hot ? 0.16f : 0.08f) * ctlA);
        g.icon(ICON_MOON, x + 21, cy, 18, sleepLeftNow > 0 ? pal.accentInk : pal.text, ctlA);
        if (sleepLeftNow > 0) g.textC(fmtTime(std::ceil(sleepLeftNow)), sleepF, x + 38, cy, pal.accentInk, ctlA);
    }
    x += sleepW + 8;
    if (syncShown) {
        lyricSyncControl("cv.sync", x, cy, 42, syncF, ctlA, live);   // not pressable while the controls are hidden
        x += syncW + 8;
    }
    // The lyrics switch, greyed while the track has none (nothing it could show or hide)
    const bool noLyrics = now.track >= 0 && !now.loading && now.lyrics.empty();
    if (cvBtn(uid("cv.lyrics"), ICON_QUOTE, x + 21, cy, 42, false, store.canvasLyrics, noLyrics)) { store.canvasLyrics = !store.canvasLyrics; store.save(); }
    x += 42 + 8;
    // The equalizer, as the player bar's: lit while on; a press turns it on or off, a long press opens its settings
    if (cvBtn(uid("cv.eq"), ICON_EQ, x + 21, cy, 42, false, store.eq.on != 0, false, Hold)) toggleEq();
    if (live && ui.longPressed(uid("cv.eq"))) { openCanvas(false); openEqualizer(); }
    x += 42 + 8;
    if (cvBtn(uid("cv.close"), ICON_X, x + 21, cy, 42, false)) openCanvas(false);
    if (live) {
        // The D-pad on the bottom row: Up goes to the nearest of previous, play and next (the seek line spans the width
        // and was nearer by the layout); previous and next keep Left/Right to themselves (they dropped to the row below)
        const Id trans[3] = { uid("cv.prev"), uid("cv.play"), uid("cv.next") };
        const float tx[3] = { mid - 30 - 14 - 21, mid, mid + 30 + 14 + 21 };
        std::vector<Id> row = { uid("cv.layout", 0), uid("cv.layout", 1), uid("cv.layout", 2), uid("cv.mute"), uid("cv.vol"), uid("cv.sleep"),
                                uid("cv.sync", 0), uid("cv.sync", 1), uid("cv.sync", 2), uid("cv.lyrics"), uid("cv.eq"), uid("cv.close") };
        for (Id id : row) {
            const SDL_FRect r = ui.rectOf(id);
            if (r.w <= 0) continue;
            int k = 0;
            for (int j = 1; j < 3; j++)
                if (std::fabs(tx[j] - (r.x + r.w / 2)) < std::fabs(tx[k] - (r.x + r.w / 2))) k = j;
            ui.link(id, 0, -1, trans[k]);
        }
        ui.link(trans[0], -1, 0, 0);
        ui.link(trans[2], 1, 0, 0);
    }
    g.setGhost(wasGhost);
    g.setShift(0, 0);
}

/**
 * While the Canvas is closed it is drawn as a ghost every few frames: nothing shows and nothing
 * registers (a modal with no items of its own), but its text and icons are rendered ahead, a couple
 * a frame, so opening it does not start with a frame spent rendering them.
 */
void App::warmCanvas() {
    // Not in the first seconds: launch has enough to draw, and the Canvas's own font sizes can wait.
    if (canvas || time < 3 || ++warmTick_ % 4) return;
    const bool modal = ui.modal();
    ui.setModal(true);
    g.setGhost(true);
    drawCanvas();
    g.setGhost(false);
    g.unclip();
    ui.setModal(modal);
}
