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
    const float ax = x + padH, ay = y + (h - art) / 2;
    const float ix = ax + art + gap;
    const bool has = now.track >= 0;

    // ---- .np-art-wrap: halo, shadow, cover crossfade, beat "breathing"
    // viz.js applyBeat: scale 1 + min(.03, beat·.07) from 50% 60%, halo .3 + min(.45, beat·1.1). Settings → Visualizer
    // scales both (Gatefold's full strength reads as a twitch on a handheld); the scale is eased so a kick swells.
    // Gatefold's halo behind the cover (and its beat flare) and its drop shadow are left out at the user's
    // request: on the console's dark backgrounds the halo read as a smudge and the shadow as a dark rim under
    // the cover. The cover sits on the background as the Canvas's does.
    const float s = art * (1 + coverBeat), sx = ax + (art - s) / 2, sy = ay + (art - s) * 0.6f;
    g.roundRect(sx, sy, s, s, 16, pal.surface);
    const float k = now.fade;
    if (now.prevCover && k < 1) g.image(now.prevCover, sx, sy, s, s, 16, 1 - k);
    if (now.cover) g.image(now.cover, sx, sy, s, s, 16, k);
    if (!now.cover) {
        // .art-empty
        g.icon(ICON_MUSIC, sx + s / 2, sy + s / 2 - 14, 64, pal.text3, 0.6f * (1 - (now.prevCover ? 1 - k : 0)));
        g.textC(has ? "No cover" : "", { css::Base }, sx + s / 2, sy + s / 2 + 38, pal.text3, 1, Align::Center);
    }
    g.roundStroke(sx, sy, s, s, 16, 1, pal.text, 0.08f);

    // ---- .np-info, measured first so the panel can take what is left
    // .np-title: Inter 760, clamp(30px, 4.2vw, 52px), line-height 1.04; a title that needs two lines steps down
    // to 42 px, because at 720 p every line it takes is a row of Up next.
    Font titleF{ css::Hero, Weight::ExtraBold };
    const std::string titleText = has ? now.info.title : "Gatefold";
    std::vector<std::string> title = g.wrap(titleText, titleF, int(iw), 2);
    if (title.size() > 1) { titleF.size = 42; title = g.wrap(titleText, titleF, int(iw), 2); }
    const float titleLH = std::round(titleF.size * 1.04f);
    std::string album = has ? now.info.album : "";
    if (has && now.info.date.size() >= 4) album += (album.empty() ? "" : " · ") + now.info.date.substr(0, 4);
    std::vector<std::string> badges;
    if (has) {
        if (!now.info.codec.empty()) badges.push_back(now.info.codec);
        char b[48];
        if (now.info.bitsPerSample && now.info.codec != "MP3") { snprintf(b, sizeof b, "%d-bit", now.info.bitsPerSample); badges.push_back(b); }
        if (now.info.sampleRate) { snprintf(b, sizeof b, "%g kHz", now.info.sampleRate / 1000.0); badges.push_back(b); }
        if (now.info.bitrateKbps) badges.push_back(std::to_string(now.info.bitrateKbps) + " kbps");
    }
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
    kicker(!has ? "Nothing playing" : isPlaying() ? "Playing" : "Paused", ix + 13 + 10, cy + 8, pal.accentInk, 0.14f);
    cy += 17 + 4;
    for (const auto& l : title) { g.text(l, titleF, ix - 2, cy - titleF.size * 0.08f, pal.text, 1, Align::Left, int(iw)); cy += titleLH; }
    cy += 2 + 4;
    // .np-artist / .np-album
    if (has) {
        g.text(now.info.artist, { 24, Weight::SemiBold }, ix, cy, pal.text, 1, Align::Left, int(iw));
        cy += 31 + 2;
        g.text(album, { css::Md }, ix, cy, pal.text2, 1, Align::Left, int(iw));
        cy += 22;
    } else {
        cy += 31 + 2 + 22;
    }
    // .np-badges: the codec chip is the accent one
    if (!badges.empty()) {
        cy += 10;
        float bx = ix;
        for (size_t i = 0; i < badges.size(); i++) bx += chip(badges[i], bx, cy + 11, i == 0) + 6;
        // The first genre, as a chip that opens its page
        const std::string& gs = lib.tracks[now.track].genre;
        const std::string g1 = gs.substr(0, gs.find(';'));
        if (!g1.empty()) {
            const Font gf{ css::Xs, Weight::SemiBold };
            const float gw = g.measureSpaced(g1, gf, 0.02f) + 16 + 18;
            const Id gid = uid("np.genre");
            if (ui.item(gid, { bx, cy, gw, 22 }, 6)) openGenre(g1);
            const bool hot = ui.pressed(gid) || (ui.focused(gid) && ui.showFocusRing());
            g.roundRect(bx, cy, gw, 22, 6, pal.text, hot ? 0.13f : 0.065f);
            g.roundStroke(bx, cy, gw, 22, 6, 1, pal.text, 0.09f);
            g.icon(ICON_TAG, bx + 13, cy + 11, 12, pal.text2);
            g.textSpaced(g1, gf, bx + 8 + 18, cy + 11, 0.02f, pal.text2);
        }
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
            if (clicked) openAlbum(t.albumIdx);
        }
        bx += button(uid("np.artist"), ICON_MIC, "Artist", bx, cy, false, &clicked) + 8;
        if (clicked) openArtist(t.artist);
        bx += button(uid("np.folder"), ICON_FOLDER, "Folder", bx, cy, false, &clicked) + 8;
        if (clicked) navigate(View::Folders, lib.folderByPath(t.folder));
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
    if (nextTracks.empty()) {
        g.textC(q.tracks.empty() ? "—" : "This is the last track of the playlist", { css::Base }, x + w / 2, y + 40, pal.text3, 1, Align::Center);
        return;
    }
    ui.pushArea(&sa);
    g.clip(x - 8, y, w + 16, h);
    for (int i = 0; i < int(nextTracks.size()); i++) {
        const float ry = y + 8 + i * 52 - sa.y;
        if (ry + 52 < y - 60 || ry > y + h + 60) continue;
        const int t = nextTracks[i];
        const Track& tr = lib.tracks[t];
        meta.wantTags(t);
        // .up-row: 40 px cover | title + artist · album | duration
        const Id id = uid("np.next", i);
        const int pos = (q.pos + 1 + i) % int(q.order.size());   // in play order (wrapped round with Repeat)
        if (ui.item(id, { x - 8, ry, w + 16, 52 }, 8)) playAt(pos);
        if (ui.focused(id)) { hoverTrack = t; hoverQueue = pos; hoverUpNext = true; }
        if (grab.upNext && grab.pos == pos) {   // picked up with the track menu's Move
            g.roundRect(x - 8, ry, w + 16, 52, 8, pal.accent, 0.14f);
            g.roundStroke(x - 8, ry, w + 16, 52, 8, 2, pal.accent, 0.95f);
        }
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x - 8, ry, w + 16, 52, 8, pal.text, 0.065f);
        coverBox(t, x, ry + 6, 40, 6);
        const std::string dur = tr.duration > 0 ? fmtTime(tr.duration) : "";
        const int dw = g.measure(dur, { css::Sm });
        const int tw = int(w - 52 - dw - 20);
        g.text(tr.title, { css::Base, Weight::SemiBold }, x + 52, ry + 7, pal.text, 1, Align::Left, tw);
        g.text(trackSub(t), { css::Sm }, x + 52, ry + 27, pal.text3, 1, Align::Left, tw);
        g.textC(dur, { css::Sm }, x + w, ry + 26, pal.text3, 1, Align::Right);
    }
    g.unclip();
    ui.popArea();
}

void App::drawLyrics(float x, float y, float w, float h) {
    ScrollArea& sa = area("np.panel");
    sa.view = { x, y, w, h };
    if (now.lyrics.empty()) {
        // .lyr-msg
        g.text("No lyrics", { css::Lg, Weight::SemiBold }, x, y + 28, pal.text2);
        g.text("Put an .lrc file with the track's name next to it, or a LYRICS tag.", { css::Base }, x, y + 56, pal.text3, 1, Align::Left, int(w));
        return;
    }
    // .lyr-bar
    g.textC(std::string(now.synced ? "Synced" : "Plain text") + " · " + now.lyricsSource, { css::Xs }, x, y + 14, pal.text3);
    const float top = y + 30, vh = h - 30;
    sa.view = { x, top, w, vh };

    const Font f = now.synced ? Font{ css::Lg + 1, Weight::Bold } : Font{ css::Base };
    const float lh = now.synced ? std::round((css::Lg + 1) * 1.32f) : 22;
    int cur = -1;
    if (now.synced) {
        const double pos = lyricsPos() + 0.15;
        for (int i = 0; i < int(now.lyrics.size()); i++) if (now.lyrics[i].t <= pos) cur = i;
    }
    // Lay out wrapped lines once per frame (cheap: wrap results are cached by Gfx).
    std::vector<std::pair<int, std::string>> rows;
    for (int i = 0; i < int(now.lyrics.size()); i++) {
        auto parts = now.lyrics[i].text.empty() ? std::vector<std::string>{ "♪" } : g.wrap(now.lyrics[i].text, f, int(w - 12), 4);
        for (auto& p : parts) rows.push_back({ i, p });
    }
    const float gapLine = now.synced ? 8 : 0;
    std::vector<float> rowY(rows.size());
    float yy = 0;
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0 && rows[r].first != rows[r - 1].first) yy += gapLine;
        rowY[r] = yy;
        yy += lh;
    }
    sa.content = yy + vh * 0.6f;
    // Scrolled by hand (touch, right stick, D-pad onto a line): left there for 4 s, then back to the line playing.
    if (now.synced && cur >= 0 && ui.now() - sa.manual > 4) {
        // .lyr-track follows the current line, a third of the way down
        for (size_t r = 0; r < rows.size(); r++) if (rows[r].first == cur) { sa.target = std::max(0.f, rowY[r] - vh * 0.32f); break; }
    }
    ui.pushArea(&sa);
    g.clip(x, top, w, vh);
    for (size_t r = 0; r < rows.size(); r++) {
        const float ry = top + rowY[r] - sa.y;
        if (ry + lh < top - 40 || ry > top + vh + 40) continue;
        const int li = rows[r].first;
        float a = 1;
        Rgb c = pal.text3;
        if (now.synced) {
            if (li == cur) c = pal.text;
            else if (li < cur) a = 0.42f;
            else a = std::abs(li - cur) <= 2 ? 0.75f : 0.6f;
            // Fade the edges like the CSS mask on .lyr
            const float e = std::min(sa.y > 1 ? ry - top : 1e9f, top + vh - ry - lh);
            a *= clampf(e / 24 + 0.15f, 0.15f, 1);
            if (r == 0 || rows[r - 1].first != li) {
                const Id id = uid("np.lyr", li);
                if (ui.item(id, { x, ry, w, lh }, 6) && now.lyrics[li].t >= 0) player.seek(now.lyrics[li].t);
            }
        } else {
            c = pal.text2;
        }
        if (now.synced && now.lyrics[li].text.empty()) lyricDots(x, ry + lh / 2, 7, li == cur ? pal.text : c, a, li == cur);   // a break
        else g.text(rows[r].second, f, x, ry, c, a, Align::Left, int(w - 12));
    }
    g.unclip();
    ui.popArea();
}

void App::drawPaletteTab(float x, float y, float w, float h) {
    ScrollArea& sa = area("np.panel");
    sa.view = { x, y, w, h };
    sa.content = 8 + 26 + 10 + 4 * 30 + 30;
    const Id id = uid("np.palette");
    ui.pushArea(&sa);
    ui.item(id, { x, y, w, 40 }, 8);
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
    ui.popArea();
}
