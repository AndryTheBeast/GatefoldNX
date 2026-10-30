/*
 * Gatefold NX · view_now.cpp
 * #view-now: cover on the left, .np-info on the right (kicker, title, artist,
 * album, badges, actions, spectrum, tabs, panel), and the full-screen Canvas.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
}  // namespace

void App::drawNow(float x, float y, float w, float h) {
    // .np grid: padding clamp(16px, 3vh, 40px) clamp(20px, 4vw, 64px); gap clamp(24px, 4vw, 64px)
    const float padH = clampf(css::W * 0.04f, 20, 64), gap = clampf(css::W * 0.04f, 24, 64);
    const float art = std::min({ css::H * 0.52f, css::W * 0.40f, 620.f });
    const float iw = std::min(760.f, w - padH * 2 - art - gap);
    const bool has = now.track >= 0;
    // Automix joining (automixFocus): the cover glides from its place to the middle of the screen and grows, frosted with
    // the hint, while the info (and the sidebar and bars: App::draw) fade out; as the join ends it glides back and they return
    const int mode = store.mixAnimNow;   // Settings › Appearance › Automix: Full, Immersive, Simple
    const Focus fc = automixFocus(mode);
    const float m = fc.move;
    // Full: to the middle of the screen, 1.35× (the interface steps aside: App::draw); Immersive: to the middle of the page,
    // 1.2× (the sidebar and the bars stay; only the info fades); Simple: in place
    const bool imm = mode == 1;
    const float big = imm ? std::min(h - 48.f, art * 1.2f) : std::min(css::H - 140.f, art * 1.35f);
    const float art2 = art + (big - art) * m;
    const float rx = x + padH, ry = y + (h - art) / 2;   // at rest
    const float gx = imm ? x + w / 2 : css::W / 2.f, gy = imm ? y + h / 2 : css::H / 2.f;
    const float ax = rx + (gx - big / 2 - rx) * m, ay = ry + (gy - big / 2 - ry) * m;
    const float ix = x + padH + art + gap + 28 * (1 - fc.ui);   // the info at its own place, drifting right as it fades
    // Simple: the whole page changes from the outgoing song to the next from left to right, the cover too (the user,
    // 2026-10-29): each section across its own width, all on the join's clock (App::pageWipe_)
    struct PageOff { PageWipe& p; ~PageOff() { p.on = false; } } pageOff{ pageWipe_ };
    if (mode == 2 && has && mixOld.on) pageWipe_.on = true;
    // the info column, from the title to Up next or the lyrics, swipes as one (App::wipeGroup_)
    struct GroupOff { WipeGroup& w; ~GroupOff() { w.on = false; } } groupOff{ wipeGroup_ };
    if (pageWipe_.on) wipeGroup_ = { true, ix, ix + iw };
    if (m > 0) {   // drawn last by App::draw, over the bars and outside the page's clip
        glideCover_ = { ax, ay, art2, art2 };
        glideOn_ = true;
    } else {
        drawNowCover(ax, ay, art2);
    }
    if (fc.ui <= 0.001f) return;   // the info fully faded: not drawn at all (App::draw)
    if (fc.ui < 1) g.setFade(fc.ui);

    // ---- .np-info, measured first so the panel can take what is left
    // .np-title: Inter 760, clamp(30px, 4.2vw, 52px), line-height 1.04; a title that needs two lines steps down
    // to 42 px, because at 720 p every line it takes is a row of Up next.
    Font titleF{ css::Hero, Weight::ExtraBold };
    const std::string titleText = has ? now.info.title : "Songate";
    std::vector<std::string> title = g.wrap(titleText, titleF, int(iw), 2);
    if (title.size() > 1) { titleF.size = 42; title = g.wrap(titleText, titleF, int(iw), 2); }
    const float titleLH = std::round(titleF.size * 1.04f);
    auto badgesOf = [&](const TrackInfo& in) {
        std::vector<std::string> bs;
        if (!in.codec.empty()) bs.push_back(in.codec);
        char b[48];
        if (in.bitsPerSample && in.codec != "MP3") { snprintf(b, sizeof b, "%d-bit", in.bitsPerSample); bs.push_back(b); }
        if (in.sampleRate) { snprintf(b, sizeof b, "%g kHz", in.sampleRate / 1000.0); bs.push_back(b); }
        if (in.bitrateKbps) bs.push_back(std::to_string(in.bitrateKbps) + " kbps");
        return bs;
    };
    std::vector<std::string> badges;
    if (has) badges = badgesOf(now.info);
    // Gatefold's column is sized for a desktop window; on 720 p the fixed rows are tightened and the spectrum
    // shrinks (then folds away) so .np-panel keeps at least four Up next rows and five synced lyric lines.
    const float padT = 16, padB = 10;
    const float titleH = 4 + title.size() * titleLH + 2;
    auto fixedH = [&](float vizH) {
        return 17 + titleH + 4 + 31 + 2 + 22 + (badges.empty() ? 0 : 10 + 22) + (has ? 12 + 34 : 0) + (vizH > 0 ? 14 + vizH : 0) + 8 + 35 + 6;
    };
    const float avail = h - padT - padB;
    // What the panel must keep: four .up-rows, but for a two-line title three rows and the whole Palette tab
    // (drawPaletteTab's height) are enough, and the bars keep their height rather than shrinking to a sliver.
    const float paletteH = 8 + 26 + 10 + 4 * 30 + 8 + 20;
    const float wantPanel = title.size() > 1 ? std::max(8 + 3 * 52.f, paletteH) : 8 + 4 * 52;
    const float vizH = store.showSpectrum ? clampf(avail - fixedH(0) - 14 - wantPanel, 28, 46) : 0;
    const float panelH = std::max(96.f, avail - fixedH(vizH));
    float cy = y + padT;

    // .np-kicker
    eqGlyph(ix, cy + 8, pal.accentInk);
    const std::string state = !has ? "Nothing playing" : isPlaying() ? "Playing" : "Paused";
    kicker(state, ix + 13 + 10, cy + 8, pal.accentInk, 0.14f);
    cy += 17 + 4;
    // the title, .np-artist and .np-album; in a Simple join (Settings › Appearance › Automix) the outgoing song's give way
    // to the next one's from left to right, timed to the join (the user, 2026-10-29). The next one's title decides the layout
    auto meta = [&](bool old) {
        const TrackInfo& in = old ? mixOld.info : now.info;
        Font tf = titleF;
        std::vector<std::string> tl = title;
        if (old) {
            tf = { css::Hero, Weight::ExtraBold };
            tl = g.wrap(in.title, tf, int(iw), 2);
            if (tl.size() > 1) { tf.size = 42; tl = g.wrap(in.title, tf, int(iw), 2); }
        }
        float yy = cy;
        for (const auto& l : tl) { g.text(l, tf, ix - 2, yy - tf.size * 0.08f, pal.text, 1, Align::Left, int(iw)); yy += std::round(tf.size * 1.04f); }
        if (!has) return;
        yy = cy + title.size() * titleLH + 2 + 4;
        std::string al = in.album;
        if (in.date.size() >= 4) al += (al.empty() ? "" : " · ") + in.date.substr(0, 4);
        g.text(in.artist, { 24, Weight::SemiBold }, ix, yy, pal.text, 1, Align::Left, int(iw));
        g.text(al, { css::Md }, ix, yy + 31 + 2, pal.text2, 1, Align::Left, int(iw));
    };
    if (mode == 2 && has && mixOld.on) {
        // (across the texts themselves, the wider song's: across the whole column a short title changed in a moment)
        float wide = 0;
        for (const TrackInfo* in : { &mixOld.info, &now.info }) {
            for (const auto& l : g.wrap(in->title, titleF, int(iw), 2)) wide = std::max(wide, float(g.measure(l, titleF)));
            wide = std::max({ wide, float(g.measure(in->artist, { 24, Weight::SemiBold })), float(g.measure(in->album, { css::Md })) + 60 });
        }
        wiped(ix, ix + std::min(iw, wide), cy, cy + title.size() * titleLH + 6 + 31 + 2 + 22, meta);
    }
    else meta(false);
    cy += title.size() * titleLH + 2 + 4 + 31 + 2 + 22;
    // .np-badges: the codec chip is the accent one
    if (!badges.empty()) {
        cy += 10;
        auto badgeRow = [&](bool old) {   // (in a Simple join, the outgoing song's give way to the next one's)
            const std::vector<std::string> bs = old ? badgesOf(mixOld.info) : badges;
            float bx = ix;
            for (size_t i = 0; i < bs.size(); i++) bx += chip(bs[i], bx, cy + 11, i == 0) + 6;
            // The first genre, as a chip that opens its page
            const int tr = old ? mixOld.track : now.track;
            if (tr < 0 || tr >= int(lib.tracks.size())) return;
            const std::string& gs = lib.tracks[tr].genre;
            const std::string g1 = gs.substr(0, gs.find(';'));
            if (!g1.empty()) {
                const Font gf{ css::Xs, Weight::SemiBold };
                const float gw = g.measureSpaced(g1, gf, 0.02f) + 16 + 18;
                const Id gid = uid("np.genre");
                if (!old && ui.item(gid, { bx, cy, gw, 22 }, 6)) openGenre(g1);
                const bool hot = !old && (ui.pressed(gid) || (ui.focused(gid) && ui.showFocusRing()));
                g.roundRect(bx, cy, gw, 22, 6, pal.text, hot ? 0.13f : 0.065f);
                g.roundStroke(bx, cy, gw, 22, 6, 1, pal.text, 0.09f);
                g.icon(ICON_TAG, bx + 13, cy + 11, 12, pal.text2);
                g.textSpaced(g1, gf, bx + 8 + 18, cy + 11, 0.02f, pal.text2);
            }
        };
        if (pageWipe_.on) wiped(ix, ix + iw, cy, cy + 22, badgeRow);
        else badgeRow(false);
        cy += 22;
    }
    // .np-actions
    if (has) {
        cy += 12;
        const Track& t = lib.tracks[now.track];
        const bool liked = store.isLiked(t.path);
        float bx = ix;
        bool clicked = false;
        bx += button(uid("np.like"), liked ? ICON_HEART_F : ICON_HEART, liked ? "Liked" : "Add to Liked", bx, cy, false, &clicked, liked, false,
                     DefaultFocus) + 8;
        if (clicked) { store.toggleLiked(t.path); store.save(); toast(liked ? "Removed from Liked" : "Added to Liked"); }
        if (t.albumIdx >= 0) {
            bx += button(uid("np.album"), ICON_GRID, "Album", bx, cy, false, &clicked) + 8;
            if (clicked) goToAlbum(t.albumIdx);
        }
        bx += button(uid("np.artist"), ICON_MIC, "Artist", bx, cy, false, &clicked) + 8;
        if (clicked) openArtist(t.artist);
        if (t.subsonicId.empty()) {
            bx += button(uid("np.folder"), ICON_FOLDER, "Folder", bx, cy, false, &clicked) + 8;
            if (clicked && tabFor(Store::TabFolders, "folder")) navigate(View::Folders, lib.folderByPath(t.folder));
        }
        {
            // .np-actions .more: the track menu (also + on the controller)
            const Id mid = uid("np.more");
            if (ui.item(mid, { bx, cy, 34, 34 }, 17)) openTrackMenu(now.track);
            const bool hot = ui.pressed(mid) || (ui.focused(mid) && ui.showFocusRing());
            g.roundRect(bx, cy, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
            g.roundStroke(bx, cy, 34, 34, 17, 1, pal.text, 0.15f);
            g.icon(ICON_MORE, bx + 17, cy + 17, 16, pal.text);
        }
        cy += 34;
    }
    // #viz
    const float vizTop = cy;   // (the Lyrics tab's swipe light stops above it)
    if (vizH > 0) {
        cy += 14;
        drawSpectrum(ix, cy, iw, vizH);
        cy += vizH;
    }
    // .np-tabs
    cy += 8;
    static const char* tabs[] = { "Up next", "Lyrics", "Palette" };
    float tx = ix;
    const Font tabF{ css::Sm, Weight::SemiBold };
    for (int i = 0; i < 3; i++) {
        const float tw = g.measure(tabs[i], tabF) + 24;
        const Id id = uid("np.tab", i);
        if (ui.item(id, { tx, cy, tw, 35 }, 6)) { npTab = i; area("np.panel").target = area("np.panel").y = 0; }
        const bool on = npTab == i, hot = ui.focused(id) && ui.showFocusRing();
        g.textC(tabs[i], tabF, tx + 12, cy + 17, on || hot ? pal.text : pal.text3);
        if (on) g.roundRect(tx + 12, cy + 34, tw - 24, 2, 1, pal.accent);
        tx += tw + 4;
    }
    g.rect(ix, cy + 35, iw, 1, pal.text, 0.09f);
    cy += 35 + 6;

    // The next track's title, artist and album in these fonts, rendered ahead a couple a frame (Gfx::warm),
    // so a track change does not begin with a frame spent rendering them.
    if (has) {
        const std::vector<int> nx = upNext(1);
        if (!nx.empty() && nx[0] != now.track) {
            const Track& n = lib.tracks[nx[0]];
            Font tf{ css::Hero, Weight::ExtraBold };
            std::vector<std::string> lines = g.wrap(n.title, tf, int(iw), 2);
            if (lines.size() > 1) { tf.size = 42; lines = g.wrap(n.title, tf, int(iw), 2); }
            for (const auto& l : lines) g.warm(l, tf, int(iw));
            g.warm(n.artist, { 24, Weight::SemiBold }, int(iw));
            std::string al = n.album;
            if (n.date.size() >= 4) al += (al.empty() ? "" : " · ") + n.date.substr(0, 4);
            g.warm(al, { css::Md }, int(iw));
        }
    }

    // .np-panel
    const float ph = std::min(panelH, y + h - padB - cy);
    if (npTab == 0) drawUpNext(ix, cy, iw, ph);
    else if (npTab == 1) drawLyrics(ix, cy, iw, ph);
    else drawPaletteTab(ix, cy, iw, ph);
    // the column's swipe light: down to the panel's foot, but on the Lyrics tab only to above the waveform (its lyrics fade
    // behind the hint instead: the user, 2026-10-29)
    groupMagic(y + padT, npTab == 1 ? vizTop : cy + ph);
    g.setFade(1);
}

/**
 * Now playing's cover at (x, y), `art` wide: the cover crossfade (the new one over the old, so a frosted or glass layer
 * over them never thins mid-way), its bass pulse, and Automix's frost and hint during a join.
 * viz.js applyBeat: scale 1 + min(.03, beat·.07) from 50% 60%, halo .3 + min(.45, beat·1.1). Settings → Visualizer scales
 * both (Gatefold's full strength reads as a twitch on a handheld); the scale is eased so a kick swells. Gatefold's halo
 * behind the cover (and its beat flare) and its drop shadow are left out at the user's request: on the console's dark
 * backgrounds the halo read as a smudge and the shadow as a dark rim under the cover.
 */
void App::drawNowCover(float ax, float ay, float art) {
    const bool has = now.track >= 0;
    const float s = art * (1 + coverBeat), sx = ax + (art - s) / 2, sy = ay + (art - s) * 0.6f;
    g.roundRect(sx, sy, s, s, 16, pal.surface);
    const float k = now.fade;
    if (pageWipe_.on) {
        // a Simple join: the outgoing cover gives way to the next one as the page's edge crosses it (the user, 2026-10-29)
        wipedImage(sx, sx + s, sy, sy + s, false, !now.cover, [&](bool old) {   // (the magic: its frost's, over it)
            if (SDL_Texture* c = mixCover(old, now.cover, now.prevCover)) g.image(c, sx, sy, s, s, 16, 1);
            else {
                g.icon(ICON_MUSIC, sx + s / 2, sy + s / 2 - 14, 64, pal.text3, 0.6f);
                g.textC("No cover", { css::Base }, sx + s / 2, sy + s / 2 + 38, pal.text3, 1, Align::Center);
            }
        });
    } else {
        if (now.prevCover && k < 1) g.image(now.prevCover, sx, sy, s, s, 16, 1);
        if (now.cover) g.image(now.cover, sx, sy, s, s, 16, k);
        if (!now.cover) {
            // .art-empty
            g.icon(ICON_MUSIC, sx + s / 2, sy + s / 2 - 14, 64, pal.text3, 0.6f * (1 - (now.prevCover ? 1 - k : 0)));
            g.textC(has ? "No cover" : "", { css::Base }, sx + s / 2, sy + s / 2 + 38, pal.text3, 1, Align::Center);
        }
    }
    if (has && time < mixUntil) {
        // the hint holds still; above the cover rather than on it in Simple, or as Settings › Appearance › Automix › The hint
        // says (the user, 2026-10-29)
        // … or in the lyrics' place, when they are on screen (drawLyrics)
        const bool above = nowHintAbove();
        // (with or without lyrics: the tab's place is the hint's either way, "No lyrics" fading as they would; the user,
        // 2026-10-29)
        const bool inLyrics = npTab == 1 && hintInLyrics(false);
        drawAutomixCover(sx, sy, s, ax + art / 2, ay + art / 2, art, 16, !above && !inLyrics);
        if (above && !inLyrics) {
            constexpr float K = 1.3f;
            const float cw = 36 * K + float(g.measure(mixLabel, { int(std::round(css::Xs * K)), Weight::SemiBold }));
            automixChip(mixLabel, ax + art / 2 - cw / 2, std::max(ay - 26, 18 * K), K, automixCoverEase(), false, true);
        }
    }
    g.roundStroke(sx, sy, s, s, 16, 1, pal.text, 0.08f);
}

float App::mixWipe() const {
    if (!mixOld.on || time < mixFrom) return 0;
    // on the join's own clock, as the lap round the cover (drawAutomixCover): the swipes end as it closes ("the swipe
    // animation should be fully timed with Automix", the user, 2026-10-29; an eased curve looked done well before it)
    if (mixUntil <= mixFrom) return 1;
    return float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
}

App::Focus App::automixFocus(int mode) const {
    Focus f;
    if (now.track < 0 || mixUntil <= mixShownAt || time < mixShownAt || time >= mixUntil) return f;
    auto clamp01 = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
    // the cover's pulse rests through every join, eased over 0.4 s each way (the user, 2026-10-28)
    const float q = std::min(clamp01((time - mixShownAt) / 0.4), clamp01((mixUntil - time) / 0.4));
    f.quiet = q * q * (3 - 2 * q);
    if (mode == 2 || !store.motion || mixShowLen < 3) return f;   // Simple: nothing moves   // too short to carry the glide: the frost alone
    // as the Canvas's layout morph (view_canvas.cpp): 0.9 s, cubic in and out for the move; what leaves fades over the first
    // 40 % (quadratic), what returns over the last 40 % (cubic ease-out)
    auto inOut = [](float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; };
    const float pin = clamp01((time - mixShownAt) / 0.9), pout = clamp01((time - (mixUntil - 0.9)) / 0.9);
    f.move = inOut(pin) * (1 - inOut(pout));
    const float l = 1 - clamp01(pin / 0.4);
    f.ui = l * l;
    if (pout > 0) {   // (from wherever it had gone: a show cut short, endJoinShow, may end before it left)
        const float r = clamp01((pout - 0.6) / 0.4);
        f.ui = std::max(f.ui, 1 - (1 - r) * (1 - r) * (1 - r));
    }
    return f;
}

void App::drawSpectrum(float x, float y, float w, float h) {
    // viz.js drawSpectrum: 56 bars, gap max(2, w/n·0.28), gradient accent-2 (bottom) → accent (top), peak caps at 55%
    const int n = spec.bands;
    const float gap = std::max(2.f, std::round(w / n * 0.28f));
    const float bw = (w - gap * (n - 1)) / n;
    if (bw <= 0) return;
    for (int i = 0; i < n; i++) {
        const float bh = std::max(2.f, spec.bars[i] * h * 0.94f);
        const float bx = x + i * (bw + gap);
        const float t = bh / h;
        const Rgb top{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                       pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
        g.roundRectV(bx, y + h - bh, bw, bh, std::min({ bw / 2, 4.f, bh / 2 }), top, pal.accent2);
    }
    for (int i = 0; i < n && store.vizPeaks; i++) {
        const float py = y + h - spec.peaks[i] * h * 0.94f - 3;
        g.rect(x + i * (bw + gap), std::max(y, py), bw, 1.5f, pal.accent, 0.55f);
    }
}

void App::drawUpNext(float x, float y, float w, float h) {
    ScrollArea& sa = area("np.panel");
    const std::vector<int> nextTracks = upNext(15);
    sa.view = { x - 8, y, w + 16, h };
    sa.content = 8 + nextTracks.size() * 52.f + 8;
    // a Simple join (Settings › Appearance › Automix): the outgoing song's Up next gives way to the next one's, from left
    // to right, timed to the join (the user, 2026-10-29)
    const bool give = store.mixAnimNow == 2 && mixOld.on && !mixOld.upNext.empty();
    if (nextTracks.empty() && !give) {
        g.textC(q.tracks.empty() ? "—" : "This is the last track of the playlist", { css::Base }, x + w / 2, y + 40, pal.text3, 1, Align::Center);
        return;
    }
    ui.pushArea(&sa);
    g.clip(x - 8, y, w + 16, h);
    auto rows = [&](const std::vector<int>& list, bool live) {   // live: the list playing (the old one only shows)
        for (int i = 0; i < int(list.size()); i++) {
            const float ry = y + 8 + i * 52 - sa.y;
            if (ry + 52 < y - 60 || ry > y + h + 60) continue;
            const int t = list[i];
            const Track& tr = lib.tracks[t];
            meta.wantTags(t);
            // .up-row: 40 px cover | title + artist · album | duration
            if (live) {
                const Id id = uid("np.next", i);
                const int pos = (q.pos + 1 + i) % int(q.order.size());   // in play order (wrapped round with Repeat)
                if (ui.item(id, { x - 8, ry, w + 16, 52 }, 8)) playAt(pos);
                if (ui.focused(id)) { hoverTrack = t; hoverQueue = pos; hoverUpNext = true; }
                if (grab.kind == Grab::Queue && grab.upNext && grab.pos == pos) {   // picked up with the track menu's Move
                    g.roundRect(x - 8, ry, w + 16, 52, 8, pal.accent, 0.14f);
                    g.roundStroke(x - 8, ry, w + 16, 52, 8, 2, pal.accent, 0.95f);
                }
                if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x - 8, ry, w + 16, 52, 8, pal.text, 0.065f);
            }
            coverBox(t, x, ry + 6, 40, 6);
            const std::string dur = tr.duration > 0 ? fmtTime(tr.duration) : "";
            const int dw = g.measure(dur, { css::Sm });
            const int tw = int(w - 52 - dw - 20);
            g.text(tr.title, { css::Base, Weight::SemiBold }, x + 52, ry + 7, pal.text, 1, Align::Left, tw);
            g.text(trackSub(t), { css::Sm }, x + 52, ry + 27, pal.text3, 1, Align::Left, tw);
            g.textC(dur, { css::Sm }, x + w, ry + 26, pal.text3, 1, Align::Right);
        }
    };
    if (give) wiped(x - 8, x + w + 8, y, y + h, [&](bool old) { rows(old ? mixOld.upNext : nextTracks, !old); });
    else rows(nextTracks, true);
    g.unclip();
    ui.popArea();
}

void App::drawLyricsOf(float x, float y, float w, float h, const std::vector<LyricLine>& lyrics, bool synced, const std::string& source,
                       double at, ScrollArea& sa, bool live) {
    sa.view = { x, y, w, h };
    if (lyrics.empty()) {
        // .lyr-msg
        g.text("No lyrics", { css::Lg, Weight::SemiBold }, x, y + 28, pal.text2);
        g.text("Put an .lrc file with the track's name next to it, or a LYRICS tag.", { css::Base }, x, y + 56, pal.text3, 1, Align::Left, int(w));
        return;
    }
    // .lyr-bar: what they are, and for synced ones the timing control on the right
    g.textC(std::string(synced ? "Synced" : "Plain text") + " · " + source, { css::Xs }, x, y + 14, pal.text3);
    if (live && lyricTimingShown()) {
        const Font cf{ css::Xs, Weight::SemiBold };
        const float cw = lyricSyncControl(nullptr, 0, 0, 24, cf, 0, false);   // measured first, then drawn against the right edge
        lyricSyncControl("np.sync", x + w - cw, y + 14, 24, cf, 1, true);
    }
    const float top = y + 30, vh = h - 30;
    sa.view = { x, top, w, vh };

    const Font f = synced ? Font{ css::Lg + 1, Weight::Bold } : Font{ css::Base };
    const float lh = synced ? std::round((css::Lg + 1) * 1.32f) : 22;
    int cur = -1;
    if (synced) {
        const double pos = at + 0.15;
        for (int i = 0; i < int(lyrics.size()); i++) if (lyrics[i].t <= pos) cur = i;
    }
    // Lay out wrapped lines once per frame (cheap: wrap results are cached by Gfx).
    std::vector<std::pair<int, std::string>> rows;
    for (int i = 0; i < int(lyrics.size()); i++) {
        auto parts = lyrics[i].text.empty() ? std::vector<std::string>{ "♪" } : g.wrap(lyrics[i].text, f, int(w - 12), 4);
        for (auto& p : parts) rows.push_back({ i, p });
    }
    const float gapLine = synced ? 8 : 0;
    std::vector<float> rowY(rows.size());
    float yy = 0;
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0 && rows[r].first != rows[r - 1].first) yy += gapLine;
        rowY[r] = yy;
        yy += lh;
    }
    sa.content = yy + vh * 0.6f;
    // Scrolled by hand (touch, right stick, D-pad onto a line): left there for 4 s, then back to the line playing.
    if (synced && (!live || ui.now() - sa.manual > 4)) {
        // .lyr-track follows the current line, a third of the way down; before the first line, the top (it kept the scroll
        // of the song before: an Automix join's next lyrics sat out of sight until their first line, 2026-10-29)
        if (cur < 0) sa.target = 0;
        for (size_t r = 0; r < rows.size(); r++) if (rows[r].first == cur) { sa.target = std::max(0.f, rowY[r] - vh * 0.32f); break; }
    }
    if (!live) sa.y = sa.target;   // (the outgoing song's: no hand on it, it stays on its line)
    if (live) ui.pushArea(&sa);
    g.clip(x, top, w, vh);
    for (size_t r = 0; r < rows.size(); r++) {
        const float ry = top + rowY[r] - sa.y;
        if (ry + lh < top - 40 || ry > top + vh + 40) continue;
        const int li = rows[r].first;
        float a = 1;
        Rgb c = pal.text3;
        if (synced) {
            if (li == cur) c = pal.text;
            else if (li < cur) a = 0.42f;
            else a = std::abs(li - cur) <= 2 ? 0.75f : 0.6f;
            // Fade the edges like the CSS mask on .lyr
            const float e = std::min(sa.y > 1 ? ry - top : 1e9f, top + vh - ry - lh);
            a *= clampf(e / 24 + 0.15f, 0.15f, 1);
            if (live && (r == 0 || rows[r - 1].first != li)) {
                const Id id = uid("np.lyr", li);
                if (ui.item(id, { x, ry, w, lh }, 6)) seekLyric(li);
            }
        } else {
            c = pal.text2;
        }
        if (synced && lyrics[li].text.empty()) lyricDots(x, ry + lh / 2, 7, li == cur ? pal.text : c, a, li == cur);   // a break
        else g.text(rows[r].second, f, x, ry, c, a, Align::Left, int(w - 12));
    }
    g.unclip();
    if (live) ui.popArea();
}

void App::drawLyrics(float x, float y, float w, float h) {
    // a Simple join (Settings › Appearance › Automix): the lyrics fade out, the hint takes their place, the next song's fade
    // back in (App::hintInLyrics, as the Canvas: the user, 2026-10-29); the outgoing song's until the join's middle
    if (store.mixAnimNow == 2) {
        const float e = automixCoverEase();
        if (e > 0.002f) {
            const float f0 = g.fade();
            g.setFade(f0 * (1 - e));
            if (mixOld.on && mixWipe() < 0.5f)
                drawLyricsOf(x, y, w, h, mixOld.lyrics, mixOld.synced, mixOld.lyricsSource, mixOld.lyricsAt + (time - mixOld.at), area("np.panel.old"), false);
            else drawLyricsOf(x, y, w, h, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("np.panel"), true);
            g.setFade(f0);
            if (hintInLyrics(false)) lyricsHint(x, y, w, h, 1.6f);   // (no lyrics: still their place, as drawNowCover's inLyrics)
            return;
        }
    }
    drawLyricsOf(x, y, w, h, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("np.panel"), true);
}

/**
 * The lyrics' timing, as a pill like the Canvas' layout switch: "Earlier", the nudge, "Later". Each press moves
 * them 0.1 s; the value in the middle puts them back in time with the file. key nullptr: only measures.
 */
float App::lyricSyncControl(const char* key, float x, float cy, float h, const Font& f, float alpha, bool live) {
    const float n = lyricNudge();
    char v[24];
    if (std::fabs(n) < 0.05f) snprintf(v, sizeof v, "0.0 s");
    else snprintf(v, sizeof v, "%s%.1f s", n > 0 ? "+" : "−", double(std::fabs(n)));
    const char* labels[3] = { "Earlier", v, "Later" };
    float ws[3], total = 0;
    for (int i = 0; i < 3; i++) {
        // the middle sized for the widest value, so the pill does not change width as it moves
        ws[i] = (i == 1 ? std::max(g.measure("+10.0 s", f), g.measure(v, f)) : g.measure(labels[i], f)) + h * 0.8f;
        total += ws[i];
    }
    if (!key) return total;
    g.roundRect(x, cy - h / 2, total, h, h / 2, pal.text, 0.08f * alpha);
    float bx = x;
    for (int i = 0; i < 3; i++) {
        const Id id = uid(key, i);
        if (live && ui.item(id, { bx, cy - h / 2, ws[i], h }, h / 2)) nudgeLyrics(i == 0 ? -0.1f : i == 2 ? 0.1f : 0.f);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (hot) g.roundRect(bx, cy - h / 2, ws[i], h, h / 2, pal.text, 0.1f * alpha);
        const bool value = i == 1, moved = std::fabs(n) >= 0.05f;
        g.textC(labels[i], f, bx + ws[i] / 2, cy, value ? (moved ? pal.accentInk : pal.text3) : pal.text2, alpha, Align::Center);
        bx += ws[i];
    }
    return total;
}

void App::drawPaletteTab(float x, float y, float w, float h) {
    // a Simple join: the outgoing song's colours give way to the next one's from left to right with the column's swipe
    // (the user, 2026-10-29; they switched as the next cover came in)
    if (store.mixAnimNow == 2 && mixOld.on) {
        wiped(x, x + w, y, y + h, [&](bool old) { drawPaletteOf(x, y, w, h, old ? mixOld.palette : target, !old); });
        return;
    }
    drawPaletteOf(x, y, w, h, target, true);
}

void App::drawPaletteOf(float x, float y, float w, float h, const Palette& target, bool live) {
    ScrollArea& sa = area("np.panel");
    sa.view = { x, y, w, h };
    sa.content = 8 + 26 + 10 + 4 * 30 + 30;
    const Id id = uid("np.palette");
    if (live) {
        ui.pushArea(&sa);
        ui.item(id, { x, y, w, 40 }, 8);
    }
    g.clip(x, y, w, h);
    float cy = y + 8 - sa.y;
    // .swatches: widths follow each cluster's share of the cover
    float sx = x;
    for (int i = 0; i < target.swatchCount; i++) {
        const float sw = std::max(16.f, std::round(target.swatches[i].share * 160));
        g.roundRect(sx, cy, sw, 26, 7, target.swatches[i].c);
        g.roundStroke(sx, cy, sw, 26, 7, 1, pal.text, 0.09f);
        sx += sw + 6;
    }
    cy += 26 + 10;
    auto hex = [](const Rgb& c) {
        char b[8];
        snprintf(b, sizeof b, "#%02x%02x%02x", int(c.r * 255 + 0.5f), int(c.g * 255 + 0.5f), int(c.b * 255 + 0.5f));
        return std::string(b);
    };
    const std::pair<const char*, Rgb> roles[8] = { { "Background", target.bg }, { "Surface", target.surface },     { "Accent", target.accent },
                                                   { "Accent · text", target.accentInk }, { "Second accent", target.accent2 }, { "Glow", target.glow },
                                                   { "Text", target.text },             { "Secondary text", target.text2 } };
    const float colW = (w - 24) / 2;
    for (int i = 0; i < 8; i++) {
        const float rx = x + (i % 2) * (colW + 24), ry = cy + (i / 2) * 30;
        g.roundRect(rx, ry + 4, 22, 22, 6, roles[i].second);
        g.roundStroke(rx, ry + 4, 22, 22, 6, 1, pal.text, 0.09f);
        g.textC(roles[i].first, { css::Base }, rx + 32, ry + 15, pal.text2);
        g.textC(hex(roles[i].second), { css::Sm }, rx + colW, ry + 15, pal.text3, 1, Align::Right);
    }
    cy += 4 * 30 + 8;
    char desc[96];
    snprintf(desc, sizeof desc, "Dark scheme · %s · average lightness %d%%",
             target.monochrome ? "monochrome cover" : ("clusters: " + std::to_string(target.clusters)).c_str(), int(target.avgL * 100));
    g.text(desc, { css::Sm }, x, cy, pal.text3);
    g.unclip();
    if (live) ui.popArea();
}
