/*
 * Gatefold NX · view_grid.cpp
 * #view-library (album tiles), #view-artists and the artist page, and the same
 * two views for genres. Albums, artists and genres come from tags, whatever the
 * folder layout is.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {
constexpr float RowH = 40;

struct GridLayout {
    int cols;
    float tileW, tileH, x0;
};
GridLayout gridFor(float x, float w) {
    const float inner = w - 36;
    const int cols = std::max(2, int(inner / 200));
    const float tw = inner / cols;
    const float cover = tw - 20;
    return { cols, tw, 10 + cover + 8 + 20 + 2 + 18 + 10, x + 18 };
}
}  // namespace

/** .tile: 10 px padding, 10 px-rounded cover (round for artists), title, subtitle. */
bool App::tile(Id id, int coverTrack, float x, float y, float w, bool round, const std::string& title, const std::string& sub) {
    const float cover = w - 20;
    const float h = 10 + cover + 8 + 20 + 2 + 18 + 10;
    const bool hit = ui.item(id, { x, y, w, h }, 14);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float rad = round ? cover / 2 : 10;
    // Gatefold's shadow under the cover (0 10px 24px -14px) is left out at the user's request, as on Now playing
    coverBox(coverTrack, x + 10, y + 10, cover, rad);
    g.roundStroke(x + 10, y + 10, cover, cover, rad, 1, pal.text, 0.09f);
    if (hot && !round) {
        // .tile .cov .play — shown on hover in Gatefold, on focus here
        g.circle(x + 10 + cover - 31, y + 10 + cover - 31, 21, pal.accent);
        g.icon(ICON_PLAY, x + 10 + cover - 30, y + 10 + cover - 31, 18, pal.onAccent);
    }
    const float ty = y + 10 + cover + 8;
    const float tx = round ? x + w / 2 : x + 10;
    const Align al = round ? Align::Center : Align::Left;
    g.text(title, { css::Base, Weight::SemiBold }, tx, ty, pal.text, 1, al, int(cover));
    g.text(sub, { css::Sm }, tx, ty + 22, pal.text3, 1, al, int(cover));
    return hit;
}

/** The album grid below a header ending at headBottom: the sidebar's Albums and Library → Albums. */
void App::drawAlbumGrid(float x, float y, float w, float h, float headBottom) {
    // .lib-tools: the sort select, as a button that cycles
    static const char* sorts[] = { "By artist", "By title", "By year", "By folder" };
    const Font sf{ css::Sm };
    const float sw = g.measure(sorts[librarySort], sf) + 20 + 26, sx = x + w - 28 - sw, sy = headBottom - 14 - 32;
    const Id sid = uid("lib.sort");
    if (ui.item(sid, { sx, sy, sw, 32 }, 8)) {
        librarySort = (librarySort + 1) % 4;
        rebuildIndex();
    }
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 8, pal.text, hot ? 0.11f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 8, 1, pal.text, 0.15f);
    g.textC(sorts[librarySort], sf, sx + 10, sy + 16, pal.text);
    g.icon(ICON_CHEV, sx + sw - 16, sy + 16, 14, pal.text3);
    const bool reading = meta.taggedCount() < int(lib.tracks.size());
    if (reading) {
        const int pct = lib.tracks.empty() ? 100 : meta.taggedCount() * 100 / int(lib.tracks.size());
        g.textC("Reading tags · " + std::to_string(pct) + "%", { css::Xs }, sx - 14, sy + 16, pal.text3, 1, Align::Right);
    }

    if (lib.albums.empty()) {
        const char* msg = lib.tracks.empty() ? "No music found" : reading ? "Reading tags…" : "No album tags";
        const std::string hint = lib.tracks.empty()
                                   ? "Choose your music folder in Settings."
                                   : reading ? "Albums appear as their tags are read. Your music is already in Folders, Artists and search."
                                             : "These files have no album tags. Browse them in Folders or Artists, or search with Y.";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, headBottom + 90, pal.text2, 1, Align::Center);
        g.textC(hint, { css::Base }, x + w / 2, headBottom + 120, pal.text3, 1, Align::Center, int(w - 80));
        if (lib.tracks.empty()) {
            bool c = false;
            const float bw = g.measure("Choose music folder", { css::Sm, Weight::SemiBold }) + 56;
            button(uid("lib.pick"), ICON_FOLDER, "Choose music folder", x + (w - bw) / 2, headBottom + 150, true, &c, false, false, DefaultFocus);
            if (c) navigate(View::PickFolder, -1, "sdmc:/");
        }
        return;
    }

    const GridLayout gl = gridFor(x, w);
    ScrollArea& sa = area("library");
    sa.view = { x, headBottom, w, y + h - headBottom };
    const int rows = (int(albumOrder.size()) + gl.cols - 1) / gl.cols;
    sa.content = rows * gl.tileH + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(albumOrder.size())) break;
            const Album& al = lib.albums[albumOrder[i]];
            if (tile(uid("lib.tile", int(uid(al.key) & 0x7fffffff)), al.tracks[0], gl.x0 + c * gl.tileW, headBottom + r * gl.tileH - sa.y, gl.tileW, false,
                     al.title, al.artist))
                openAlbum(albumOrder[i]);
        }
    g.unclip();
    ui.popArea();
}

void App::drawArtists(float x, float y, float w, float h, bool genre) {
    const std::vector<Artist>& list = genre ? genres : artists;
    const float headBottom = viewHead("Library", genre ? "Genres" : "Artists",
                                      genre && list.empty() ? "No genre tags yet" : std::to_string(list.size()) + (genre ? " genres" : " artists"), x, y, w);
    const GridLayout gl = gridFor(x, w);
    ScrollArea& sa = area(genre ? "genres" : "artists");
    sa.view = { x, headBottom, w, y + h - headBottom };
    const int rows = (int(list.size()) + gl.cols - 1) / gl.cols;
    sa.content = rows * gl.tileH + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(list.size())) break;
            const Artist& ar = list[i];
            const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
            std::string sub = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
            if (!ar.albums.empty()) sub = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums") + (genre ? " · " + sub : "");
            // artists are round, as in Gatefold; a genre is shown by the cover of its first album, square
            if (tile(uid(genre ? "gen.tile" : "art.tile", int(uid(ar.name) & 0x7fffffff)), cover, gl.x0 + c * gl.tileW, headBottom + r * gl.tileH - sa.y,
                     gl.tileW, !genre, ar.name, sub))
                genre ? openGenre(ar.name) : openArtist(ar.name);
        }
    g.unclip();
    ui.popArea();
}

void App::drawArtist(float x, float y, float w, float h, bool genre) {
    const int ai = genre ? routeGenre() : routeArtist();
    if (ai < 0) { viewHead(genre ? "Genre" : "Artist", route.query, "Not in the library", x, y, w); return; }
    const Artist& ar = genre ? genres[ai] : artists[ai];
    const int lead = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
    AlbumArt& art = meta.art(lead);
    const Palette& ap = art.state == AlbumArt::Ready ? art.pal : pal;

    const GridLayout gl = gridFor(x, w);
    const float heroH = 26 + 220 + 22;
    const int rows = (int(ar.albums.size()) + gl.cols - 1) / gl.cols;
    const float gridH = ar.albums.empty() ? 0 : 10 + rows * gl.tileH;
    ScrollArea& sa = area(genre ? "genre" : "artist", int(uid(ar.name) & 0x7fffffff));
    sa.view = { x, y, w, h };
    sa.content = heroH + gridH + 46 + 30 + 8 + ar.tracks.size() * RowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    g.rectV(x, oy, w, heroH, ap.accent, 0.26f, ap.accent, 0);
    const float cx = x + 30, cyTop = oy + 26;
    g.glow(cx + 110, cyTop + 150, 170, ap.glow, 0.6f);
    coverBox(lead, cx, cyTop, 220, genre ? 16 : 110);

    const float tx = cx + 220 + 28, tw = w - (tx - x) - 30;
    float ty = cyTop + 220 - 34 - 16 - 22 - 8 - 48 - 6 - 18;
    kicker(genre ? "Genre" : "Artist", tx, ty + 8, pal.text3);
    ty += 24;
    g.text(ar.name, { 46, Weight::ExtraBold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw));
    ty += 48 + 8;
    std::string meta2 = fmtTotal(ar.tracks);
    if (!ar.albums.empty()) meta2 = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album · " : " albums · ") + meta2;
    g.text(meta2, { css::Base }, tx, ty, pal.text2, 1, Align::Left, int(tw));
    ty += 22 + 16;
    float bx = tx;
    bool clicked = false;
    bx += button(uid("ar.play"), ICON_PLAY, "Play", bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) playTracks(ar.tracks, 0, ar.name);
    bx += button(uid("ar.shuffle"), ICON_SHUFFLE, "Shuffle", bx, ty, false, &clicked) + 8;
    if (clicked) playTracks(ar.tracks, 0, ar.name, true);

    g.clip(x, y, w, h);
    float gy = oy + heroH + 10;
    for (int i = 0; i < int(ar.albums.size()); i++) {
        const Album& al = lib.albums[ar.albums[i]];
        const std::string sub = al.year.empty() ? std::to_string(al.tracks.size()) + " tracks" : al.year;
        const float ty2 = gy + (i / gl.cols) * gl.tileH;
        if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
        if (tile(uid("ar.tile", int(uid(al.key) & 0x7fffffff)), al.tracks[0], gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, false, al.title, sub))
            openAlbum(ar.albums[i]);
    }

    // Songs: everything by this artist, also tracks that belong to no album
    float sy = oy + heroH + gridH + 16;
    g.text("Songs", { css::Lg, Weight::Bold }, x + 30, sy, pal.text);
    sy += 30;
    listHead(x, sy, w, true);
    const float top = sy + 30 + 8;
    for (int i = 0; i < int(ar.tracks.size()); i++) {
        const float ry = top + i * RowH;
        if (ry + RowH < y - RowH || ry > y + h + RowH) continue;
        if (trackRow(uid("row", i), ar.tracks[i], i + 1, x + 14, ry, w - 28, RowH, true)) playTracks(ar.tracks, i, ar.name);
    }
    g.unclip();
    ui.popArea();
}
