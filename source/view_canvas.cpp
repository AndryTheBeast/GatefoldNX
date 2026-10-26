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

constexpr float vw = css::W / 100.f, vh = css::H / 100.f, vmax = std::max(vw, vh);
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

/** CSS `ease-in-out infinite alternate` over a period: 0 → 1 → 0 … */
float alternate(double t, float period) {
    const float ph = std::fmod(float(t) / period, 2.f), k = ph < 1 ? ph : 2 - ph;
    return 0.5f - 0.5f * std::cos(k * 3.14159265f);
}

}  // namespace

void App::drawCanvasLyrics(float x, float y, float w, float h, int size, int layout, float alpha) {
    ScrollArea& sa = area("cv.lyrics", layout);
    sa.view = { x, y, w, h };
    if (!now.synced) {
        // .ln.plain: UI font, regular weight; scrolls by touch and the right stick
        const Font f{ int(clampf(1.25f * vw, 16, 22)) };
        float total = 0;
        for (const auto& l : now.lyrics) total += 26 * g.wrap(l.text.empty() ? " " : l.text, f, int(w), 3).size();
        sa.content = total;
        if (!g.ghost()) ui.pushArea(&sa);   // a hidden layout's lyrics take no touch or stick
        g.clip(x - 10, y, w + 20, h);
        float ly = y - sa.y;
        for (const auto& l : now.lyrics)
            for (const auto& part : g.wrap(l.text.empty() ? " " : l.text, f, int(w), 3)) {
                if (ly + 26 > y && ly < y + h) g.text(part, f, x, ly, pal.text2, alpha);
                ly += 26;
            }
        g.unclip();
        if (!g.ghost()) ui.popArea();
        return;
    }
    // .lyr-canvas .ln: 760 weight, line-height 1.24, padding .32em; the current line sits a little above the middle
    const Font f{ size, Weight::ExtraBold };
    const float lineH = std::round(size * 1.24f), pad = std::round(size * 0.32f);
    const double pos = lyricsPos() + 0.15;
    int cur = -1;
    for (int i = 0; i < int(now.lyrics.size()); i++) if (now.lyrics[i].t <= pos) cur = i;

    std::vector<std::vector<std::string>> parts(now.lyrics.size());
    std::vector<float> top(now.lyrics.size());
    float yy = h * 0.45f;   // room above the first line, so it can sit at the focus point too
    for (size_t i = 0; i < now.lyrics.size(); i++) {
        parts[i] = now.lyrics[i].text.empty() ? std::vector<std::string>{ "♪" } : g.wrap(now.lyrics[i].text, f, int(w - 20), 3);
        top[i] = yy;
        yy += pad * 2 + lineH * parts[i].size();
    }
    sa.content = yy + h;
    const int focusLine = std::max(cur, 0);
    const float blockH = pad * 2 + lineH * (parts.empty() ? 1 : parts[focusLine].size());
    // Scrolled by hand: left there for 4 s, then back to the line playing.
    if (ui.now() - sa.manual > 4) sa.target = top.empty() ? 0 : std::max(0.f, top[focusLine] + blockH / 2 - h * 0.45f);

    if (!g.ghost()) ui.pushArea(&sa);   // a hidden layout's lyrics take no touch or stick
    // text-shadow: 0 0 40px glow — drawn before the clip so it fades out instead of ending at the box edge
    if (cur >= 0) {
        const float by = y + top[cur] - sa.y, bh = pad * 2 + lineH * parts[cur].size();
        const float rel = (by + bh / 2 - y) / h;
        const float m = rel < 0.18f ? clampf(rel / 0.18f, 0, 1) : rel > 0.72f ? clampf((1 - rel) / 0.28f, 0, 1) : 1.f;
        const float tw = std::min(w, float(g.measure(parts[cur][0], f)));
        g.glow(x + tw / 2, by + bh / 2, std::max(tw * 0.42f, bh), pal.glow, 0.16f * m * alpha);
    }
    g.clip(x - 10, y, w + 20, h);
    for (size_t i = 0; i < now.lyrics.size(); i++) {
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
        if (alpha > 0.5f && ui.item(uid("cv.ln", int(i)), { x, by, w, bh }, 8) && now.lyrics[i].t >= 0) player.seek(now.lyrics[i].t);
        float ly = by + pad;
        if (now.lyrics[i].text.empty()) {   // a break: Gatefold's three dots, sized with the lines (7 px at the panel's size)
            lyricDots(x, ly + lineH / 2, std::clamp(std::round(size * 0.24f), 7.f, 14.f), pal.text, a, d == 0);
            continue;
        }
        for (const auto& line : parts[i]) {
            g.text(line, f, x, ly, pal.text, a, Align::Left, int(w));
            ly += lineH;
        }
    }
    g.unclip();
    if (!g.ghost()) ui.popArea();
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
    const int layout = canvasLayoutShown();
    auto progress = [&] { return clampf(float(time - cvStart) / 0.9f, 0, 1); };
    auto inOut = [](float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; };
    auto weights = [&](float e, float* w) { for (int i = 0; i < 3; i++) w[i] = cvFrom[i] + ((i == cvTo ? 1.f : 0.f) - cvFrom[i]) * e; };
    if (layout != cvTo) {
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
    }
    const bool lyricsOn = store.canvasLyrics && !now.lyrics.empty();

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
    blur(now.prevBlur, 0.62f * (1 - now.fade));
    blur(now.blur, 0.62f * now.fade);
    const float g1 = alternate(t, 60), g3 = alternate(t, 76);
    g.glow(-20 * vmax + 35 * vmax + 14 * vmax * g1, -24 * vmax + 35 * vmax + 10 * vmax * g1, 35 * vmax * (1 + 0.15f * g1), pal.blobs[0], 0.5f);
    g.glow(css::W + 22 * vmax - 35 * vmax + 16 * vmax * g3, css::H + 30 * vmax - 35 * vmax - 12 * vmax * g3, 35 * vmax * (1 + 0.1f * g3),
           pal.blobs[2], 0.5f);
    g.vignette(css::W * 0.5f, css::H * 0.45f, css::W * 1.2f, css::H * 0.9f, pal.bg, 0.7f);
    g.rectV(0, css::H * 0.7f, css::W, css::H * 0.3f, pal.bg, 0, pal.bg, 0.55f);

    // ---- .cv-stage, per layout
    struct Stage { float art = 0, ax = 0, ay = 0, sx = 0, sy = 0, sw = 0, sh = 0; };
    auto stage = [&](int l) {
        Stage s;
        if (l == 1) {
            s.art = std::min({ 16 * vh, 12 * vw, 200.f });
            s.ax = 8 * vw;
            s.ay = 7 * vh;
        } else if (l == 2) {
            s.art = std::min({ 54 * vh, 62 * vw, 900.f });
            const float top = 4 * vh, bottom = css::H - 25 * vh;   // the text ends well above the seek bar
            const float textH = 3.5f * vh + 34 + 6 + 24 + 6 + 20;
            s.ax = (css::W - s.art) / 2;
            s.ay = top + (bottom - top - s.art - textH) / 2;
        } else {
            s.art = std::min({ 62 * vh, 44 * vw, 860.f, css::H - 190.f });
            const float colW = std::min(46 * vw, 760.f), gap = clampf(6 * vw, 32, 110);
            const float padT = 5 * vh, padB = std::max(19 * vh, 170.f), padX = 6 * vw;
            const float total = s.art + gap + colW;
            s.ax = padX + (css::W - padX * 2 - total) / 2;
            s.ay = padT + (css::H - padT - padB - s.art) / 2;
            s.sx = s.ax + s.art + gap;
            s.sw = colW;
            s.sh = std::min(78 * vh, s.art * 1.15f);
            s.sy = padT + (css::H - padT - padB - s.sh) / 2;
        }
        return s;
    };
    const Stage st[3] = { stage(0), stage(1), stage(2) };
    float art = 0, ax = 0, ay = 0;
    for (int i = 0; i < 3; i++) { art += st[i].art * wts[i]; ax += st[i].ax * wts[i]; ay += st[i].ay * wts[i]; }

    // .cv-art: a slow float, the cover with a Ken Burns zoom, a deep shadow. Both motions can be turned off in Settings → Canvas.
    const float floatY = store.motion && store.canvasFloat ? -5 + 11 * alternate(t, 14) : 0;
    const float rad = clampf(1.2f * vw, 10, 18);
    // Optional bass pulse (Settings → Visualizer), scaled from 50% 60% like the Now playing cover
    if (store.canvasPulse && coverBeat > 0) {
        const float grown = art * (1 + coverBeat);
        ax -= (grown - art) / 2;
        ay -= (grown - art) * 0.6f;
        art = grown;
    }
    const float fy = ay + floatY;
    // 0 50px 120px -30px rgb(0 0 0 / .65), scaled with the cover so the small one in the lyrics layout is not drowned
    g.glow(ax + art / 2, fy + art / 2 + art * 0.11f, art * 0.66f, { 0, 0, 0 }, 0.65f * std::min(1.f, art / 300));
    g.roundRect(ax, fy, art, art, rad, pal.surface2);
    const float ken = store.motion && store.canvasZoom ? alternate(t, 42) : 0;
    if (now.prevCover && now.fade < 1) g.image(now.prevCover, ax, fy, art, art, rad, 1 - now.fade, 1 + 0.075f * ken, -ken * 0.3f, ken * 0.2f);
    if (now.cover) g.image(now.cover, ax, fy, art, art, rad, now.fade, 1 + 0.075f * ken, -ken * 0.3f, ken * 0.2f);
    else g.icon(ICON_MUSIC, ax + art / 2, fy + art / 2, std::min(72.f, art * 0.4f), pal.text3);
    g.roundStroke(ax, fy, art, art, rad, 1, { 1, 1, 1 }, 0.06f);

    // ---- .cv-meta (+ lyrics): each layout's own, faded by its weight and drifting in by a few pixels
    const std::string kick = !has ? "NOTHING PLAYING" : isPlaying() ? "PLAYING" : "PAUSED";
    std::string album = now.info.album;
    if (has && now.info.date.size() >= 4) album += (album.empty() ? "" : " · ") + now.info.date.substr(0, 4);
    const std::string title = has ? now.info.title : "Gatefold";
    const Font kf{ int(clampf(0.85f * vw, 11, 14)), Weight::Bold };

    for (int l = 0; l < 3; l++) {
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
            float ty = s.ay + s.art + 3.5f * vh + dy;
            const Font tf{ int(clampf(2.2f * vw, 22, 40)), Weight::ExtraBold };
            g.text(title, tf, css::W / 2.f, ty, pal.text, A, Align::Center, int(css::W * 0.8f));
            ty += tf.size * 1.25f + 6;
            g.text(now.info.artist, { int(clampf(1.3f * vw, 15, 24)), Weight::SemiBold }, css::W / 2.f, ty, pal.text, A, Align::Center,
                   int(css::W * 0.8f));
            ty += 24 + 6;
            g.text(album, { int(clampf(1.05f * vw, 14, 19)) }, css::W / 2.f, ty, pal.text2, A, Align::Center, int(css::W * 0.8f));
        } else if (l == 1) {
            // lyrics first: a small cover with the meta beside it, large lines below
            const float mx = s.ax + s.art + 28, mw = css::W - 8 * vw - mx;
            const Font tf{ int(clampf(2 * vw, 22, 40)), Weight::ExtraBold };
            const Font arf{ int(clampf(1.2f * vw, 15, 24)), Weight::SemiBold };
            const float block = kf.size * 1.4f + 4 + tf.size * 1.1f + 4 + arf.size * 1.4f;
            float my = s.ay + (s.art - block) / 2 + dy;
            g.textSpaced(kick, kf, mx, my + kf.size * 0.7f, 0.16f, pal.accentInk, A);
            my += kf.size * 1.4f + 4;
            g.text(title, tf, mx, my - 3, pal.text, A, Align::Left, int(mw));
            my += tf.size * 1.1f + 4;
            g.text(now.info.artist, arf, mx, my, pal.text, A, Align::Left, int(mw));
            const float ly = s.ay + s.art + 3 * vh + dy, lh = css::H - 20 * vh - ly;
            if (lyricsOn) drawCanvasLyrics(8 * vw, ly, css::W - 16 * vw, lh, int(clampf(3.3f * vw, 28, 66)), 1, A);
        } else {
            // cover: meta at the top of the side column, lyrics fill the rest; without lyrics the meta is centred
            const Font tf{ int(clampf(3.6f * vw, 30, 72)), Weight::ExtraBold };
            const Font arf{ int(clampf(1.7f * vw, 18, 32)), Weight::SemiBold };
            const Font alf{ int(clampf(1.05f * vw, 14, 19)) };
            const std::vector<std::string> lines = g.wrap(title, tf, int(s.sw), 2);
            const float metaH = kf.size * 1.4f + 10 + lines.size() * std::round(tf.size * 1.02f) + 10 + arf.size * 1.35f + 6 + alf.size * 1.45f;
            float my = (lyricsOn ? s.sy : s.ay + (s.art - metaH) / 2) + dy;
            g.textSpaced(kick, kf, s.sx, my + kf.size * 0.7f, 0.16f, pal.accentInk, A);
            my += kf.size * 1.4f + 10;
            for (const auto& ln : lines) {
                g.text(ln, tf, s.sx - 2, my - tf.size * 0.12f, pal.text, A, Align::Left, int(s.sw));
                my += std::round(tf.size * 1.02f);
            }
            my += 10;
            g.text(now.info.artist, arf, s.sx, my, pal.text, A, Align::Left, int(s.sw));
            my += arf.size * 1.35f + 6;
            g.text(album, alf, s.sx, my, pal.text2, A, Align::Left, int(s.sw));
            my += alf.size * 1.45f;
            if (lyricsOn) {
                const float ly = my + 4 * vh;
                drawCanvasLyrics(s.sx, ly, s.sw, s.sy + s.sh + dy - ly, int(clampf(2.05f * vw, 20, 40)), 0, A);
            }
        }
        g.setGhost(wasGhost);
        textShift = 0;
        g.setShift(0, 0);
    }

    // ---- .cv-chrome: hairline progress, transport, tools; fades out when idle
    const bool idle = canvasIdle > store.canvasIdleAfter && isPlaying();
    // Coming back from idle, the spectrum folds back first: the controls only return in its last moments,
    // so they never sit on top of the bars (going idle, they leave first and the bars rise after).
    canvasCtl += ((idle || canvasViz > 0.12f ? 0.f : 1.f) - canvasCtl) * (1 - std::exp(-dt / 0.12f));
    const float padX = 4 * vw, bottom = css::H - 3.2f * vh;
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
    const float base = progCy + vz * (css::H - 6 * vh - progCy);
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
    g.textC(fmtTime(player.position()), timeF, padX, timeCy, pal.text2, ctlA);
    g.textC(fmtTime(dur), timeF, css::W - padX, timeCy, pal.text2, ctlA, Align::Right);
    // disabled: greyed like the layouts that cannot show, and neither focusable nor pressable
    auto cvBtn = [&](Id id, const char* icon, float cx, float cy, float size, bool big, bool on = false, bool disabled = false) {
        bool hit = false;
        if (live && !disabled) hit = ui.item(id, { cx - size / 2, cy - size / 2, size, size }, size / 2, big ? DefaultFocus : 0);
        const bool hot = !disabled && (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing()));
        const float s = ui.pressed(id) ? 0.94f : 1.f;
        if (big) g.circle(cx, cy, size / 2 * s, pal.accent, ctlA);
        else g.circle(cx, cy, size / 2 * s, pal.text, (hot ? 0.16f : 0.08f) * ctlA);
        g.icon(icon, cx, cy, big ? 26 : 18, big ? pal.onAccent : on && !disabled ? pal.accentInk : pal.text, ctlA * (disabled ? 0.35f : 1.f));
        return hit;
    };
    const float mid = css::W / 2.f, tc = transCy + slide;
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
    const float totalW = segW + 8 + volW + 8 + sleepW + 8 + 42 + 8 + 42;
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
    // The lyrics switch, greyed while the track has none (nothing it could show or hide)
    const bool noLyrics = now.track >= 0 && !now.loading && now.lyrics.empty();
    if (cvBtn(uid("cv.lyrics"), ICON_QUOTE, x + 21, cy, 42, false, store.canvasLyrics, noLyrics)) { store.canvasLyrics = !store.canvasLyrics; store.save(); }
    x += 42 + 8;
    if (cvBtn(uid("cv.close"), ICON_X, x + 21, cy, 42, false)) openCanvas(false);
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
