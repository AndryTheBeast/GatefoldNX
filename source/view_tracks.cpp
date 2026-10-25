/*
 * Gatefold NX · view_tracks.cpp
 * The four ways Gatefold 1.4.2 shows a list of tracks, shared by the playlists, the queue,
 * Folders and Library → Tracks: grouped by album (.vhead over .vrow), with covers (.vrow.wc),
 * as tiles (.vtile) and as a plain list (.vrow). The switch between them is the four-icon
 * control (i-v-albums, i-v-covers, i-v-grid, i-v-list). Also the 4-cover mosaic of playlists
 * and folders (.pl-cover.quad).
 *
 * Gatefold puts a darker panel behind these lists; here they sit on the background itself,
 * as the rest of the Switch build does.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {

constexpr float RowH = 40;        // .vrow, a little taller than the PC's 36 for fingers
constexpr float CovRowH = 60;     // .vrow.wc: --rowc 56, + 4
constexpr float GroupH = 76;      // .vhead: --head 72, + 4
constexpr float ColHeadH = 38;    // .list-head 30 + 8 below

/** The grid's columns: tiles about 180 px wide, at least two. */
struct Tiles { int cols; float w, cover, h; };
Tiles tilesFor(float w) {
    const float inner = w - 32;
    const int cols = std::max(2, int(inner / 180));
    const float tw = inner / cols, cover = tw - 16;
    return { cols, tw, cover, 8 + cover + 10 + 20 + 18 + 14 };
}

}  // namespace

// ============================================================ layout

std::vector<App::ListEntry> App::listLayout(const std::vector<int>& tracks, float w, ListMode mode) const {
    std::vector<ListEntry> out;
    float y = 0;
    if (mode == ListMode::Grid) {
        const Tiles t = tilesFor(w);
        for (int i = 0; i < int(tracks.size()); i += t.cols) { out.push_back({ ListEntry::TileRow, i, y, t.h }); y += t.h; }
        return out;
    }
    out.push_back({ ListEntry::ColumnHead, -1, y, ColHeadH });
    y += ColHeadH;
    int lastAlbum = -2;
    for (int i = 0; i < int(tracks.size()); i++) {
        if (mode == ListMode::Albums) {
            // a header whenever the album changes, as Gatefold groups consecutive tracks
            const int a = lib.tracks[tracks[i]].albumIdx;
            if (a != lastAlbum || a < 0) { out.push_back({ ListEntry::GroupHead, i, y, GroupH }); y += GroupH; }
            lastAlbum = a;
        }
        const float h = mode == ListMode::Covers ? CovRowH : RowH;
        out.push_back({ ListEntry::Row, i, y, h });
        y += h;
    }
    return out;
}

float App::listRowY(const std::vector<ListEntry>& layout, int index) const {
    for (const auto& e : layout) {
        if (e.kind == ListEntry::Row && e.index == index) return e.y;
        if (e.kind == ListEntry::TileRow && index >= e.index && index < e.index + 64) {
            // the tile row holding it (rows hold cols tiles; the next row starts further on)
            const auto next = &e + 1;
            if (next == layout.data() + layout.size() || index < next->index) return e.y;
        }
    }
    return 0;
}

// ============================================================ pieces

/** The four-icon view switch (.pl-modes), right-aligned at rx; returns its width. */
float App::viewSwitch(const char* key, float rx, float cy, int& mode) {
    static const char* icons[4] = { ICON_V_ALBUMS, ICON_V_COVERS, ICON_V_GRID, ICON_V_LIST };
    const float bw = 38, h = 36, w = 6 + bw * 4, x0 = rx - w;
    g.roundRect(x0, cy - h / 2, w, h, 12, pal.text, 0.05f);
    g.roundStroke(x0, cy - h / 2, w, h, 12, 1, pal.text, 0.09f);
    for (int i = 0; i < 4; i++) {
        const float bx = x0 + 3 + i * bw;
        const Id id = uid(key, i);
        if (ui.item(id, { bx, cy - h / 2 + 3, bw, h - 6 }, 9)) { mode = i; persistSoon(0.5f); }
        const bool on = mode == i, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(bx, cy - h / 2 + 3, bw, h - 6, 9, pal.text, 0.13f);
        else if (hot) g.roundRect(bx, cy - h / 2 + 3, bw, h - 6, 9, pal.text, 0.065f);
        g.icon(icons[i], bx + bw / 2, cy, 17, on ? pal.text : pal.text2);
    }
    return w;
}

/**
 * .pl-cover.quad: the covers of the first four albums, two by two; one cover when there are
 * fewer than four different ones.
 */
void App::coverQuad(const std::vector<int>& tracks, float x, float y, float size, float rad, float alpha) {
    std::vector<int> picks;
    std::vector<int> seen;
    for (int t : tracks) {
        const int a = lib.tracks[t].albumIdx;
        const int key = a >= 0 ? a : -1 - t;   // tracks without an album each count as their own
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        picks.push_back(t);
        if (picks.size() == 4) break;
        if (seen.size() > 64) break;
    }
    if (picks.size() < 4) {
        if (!picks.empty()) coverBox(picks[0], x, y, size, rad, alpha);
        else { g.roundRect(x, y, size, size, rad, pal.surface2, alpha); g.icon(ICON_LIST, x + size / 2, y + size / 2, size * 0.36f, pal.text3, alpha); }
        return;
    }
    const float half = size / 2;
    // Each quarter rounds only its outer corner, and reaches half a pixel over the middle so no
    // seam shows: the four read as one cover.
    for (int i = 0; i < 4; i++) {
        const int col = i % 2, row = i / 2;
        const float qx = x + col * half - (col ? 0.5f : 0), qy = y + row * half - (row ? 0.5f : 0);
        const float r4[4] = { col == 1 && row == 0 ? rad : 0, col == 1 && row == 1 ? rad : 0, col == 0 && row == 1 ? rad : 0, col == 0 && row == 0 ? rad : 0 };
        AlbumArt& a = meta.art(picks[i]);
        if (a.state == AlbumArt::Ready) g.imageCorners(a.tex, qx, qy, half + 0.5f, half + 0.5f, r4, alpha);
        else g.roundRectCorners(qx, qy, half + 0.5f, half + 0.5f, r4, pal.surface2, alpha);
    }
    g.roundStroke(x, y, size, size, rad, 1, pal.text, 0.09f * alpha);
}

/** Column labels for the row modes (.list-head). */
void App::listColumns(float x, float y, float w, ListMode mode, bool extended) {
    const Font f{ css::Xs, Weight::SemiBold };
    const float pad = 30, gap = 14;
    float cx = x + pad;
    const float cy = y + 15;
    const float inner = w - pad * 2;
    g.textSpaced("#", f, cx + 44, cy, 0.08f, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    if (mode == ListMode::Covers) {
        // .vrow.wc: # | cover + title / artist | album | (year | genre) | time
        const float fixed = 44 + 64 + gap * (extended ? 5 : 3) + (extended ? 64 : 0);
        const float fr = (inner - fixed) / (extended ? 4.f : 3.f);
        g.textSpaced("TITLE", f, cx, cy, 0.08f, pal.text3);
        cx += fr * 2 + gap;
        g.textSpaced("ALBUM", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
        if (extended) {
            g.textSpaced("YEAR", f, cx, cy, 0.08f, pal.text3);
            cx += 64 + gap;
            g.textSpaced("GENRE", f, cx, cy, 0.08f, pal.text3);
            cx += fr + gap;
        }
    } else {
        const float fixed = 44 + 64 + gap * 4;
        const float fr = (inner - fixed) / 3.6f;
        g.textSpaced("TITLE", f, cx, cy, 0.08f, pal.text3);
        cx += fr * 1.6f + gap;
        g.textSpaced("ARTIST", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
        g.textSpaced("ALBUM", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
    }
    g.textSpaced("TIME", f, x + w - pad, cy, 0.08f, pal.text3, 1, Align::Right);
    g.rect(x, y + 29, w, 1, pal.text, 0.09f);
}

/** .vrow.wc: number, cover, title over artist, album (and year, genre), length. */
bool App::coverRow(Id id, int track, int number, float x, float y, float w, float h, bool extended) {
    const Track& t = lib.tracks[track];
    meta.wantTags(track);
    const bool hit = ui.item(id, { x, y, w, h - 4 }, 10);
    if (ui.focused(id)) hoverTrack = track;
    const bool playing = now.track == track && player.loaded();
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (playing) g.roundRect(x, y, w, h - 4, 10, pal.accent, 0.09f);
    if (hot) g.roundRect(x, y, w, h - 4, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float pad = 16, gap = 14, inner = w - pad * 2;
    const float fixed = 44 + 64 + gap * (extended ? 5 : 3) + (extended ? 64 : 0);
    const float fr = (inner - fixed) / (extended ? 4.f : 3.f);
    const float cy = y + (h - 4) / 2;
    float cx = x + pad;
    if (playing) eqGlyph(cx + 44 - 13, cy, pal.accentInk);
    else g.textC(std::to_string(number), { css::Sm }, cx + 44, cy, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    const float cov = h - 4 - 16;
    coverBox(track, cx, cy - cov / 2, cov, 7);
    const float tx = cx + cov + 12, tw = fr * 2 - cov - 12;
    g.text(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, tx, cy - 20, playing ? pal.accentInk : pal.text, 1, Align::Left, int(tw));
    g.text(t.artist, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(tw));
    cx += fr * 2 + gap;
    g.textC(t.album, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    cx += fr + gap;
    if (extended) {
        g.textC(t.date.substr(0, 4), { css::Base }, cx, cy, pal.text2, 1, Align::Left, 64);
        cx += 64 + gap;
        g.textC(t.genre, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    }
    g.textC(t.duration > 0 ? fmtTime(t.duration) : "", { css::Sm }, x + w - pad, cy, pal.text3, 1, Align::Right);
    return hit;
}

/** .vtile: the cover, title and artist; the play glyph on focus, an equaliser on the one playing. */
bool App::trackTile(Id id, int track, float x, float y, float w, float cover) {
    const Track& t = lib.tracks[track];
    meta.wantTags(track);
    const float h = 8 + cover + 10 + 20 + 18 + 6;
    const bool hit = ui.item(id, { x, y, w, h }, 14);
    if (ui.focused(id)) hoverTrack = track;
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    const bool playing = now.track == track && player.loaded();
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float cx = x + 8, cy = y + 8;
    coverBox(track, cx, cy, cover, 12);
    if (playing) {
        g.roundRect(cx, cy, cover, cover, 12, { 0, 0, 0 }, 0.38f);
        eqGlyph(cx + cover / 2 - 9, cy + cover / 2, { 1, 1, 1 });
    } else if (hot) {
        g.circle(cx + cover - 31, cy + cover - 31, 21, pal.accent);
        g.icon(ICON_PLAY, cx + cover - 31, cy + cover - 31, 18, pal.onAccent);
    }
    g.text(t.title, { css::Base, Weight::SemiBold }, cx, cy + cover + 10, playing ? pal.accentInk : pal.text, 1, Align::Left, int(cover));
    g.text(t.artist, { css::Sm }, cx, cy + cover + 32, pal.text3, 1, Align::Left, int(cover));
    return hit;
}

/** .vhead: the album's cover, title, "artist · year · n tracks", and Album to play it whole. */
void App::groupHead(const char* key, int index, int track, float x, float y, float w) {
    const Track& t = lib.tracks[track];
    const float cov = GroupH - 4 - 20, cy = y + (GroupH - 4) / 2 + 2;
    coverBox(track, x + 16, cy - cov / 2, cov, 8);
    const float tx = x + 16 + cov + 14;
    std::string album = t.album.empty() ? baseName(t.folder) : t.album;
    std::string meta3 = t.albumArtist.empty() ? t.artist : t.albumArtist;
    if (t.date.size() >= 4) meta3 += " · " + t.date.substr(0, 4);
    int n = 0;
    if (t.albumIdx >= 0) n = int(lib.albums[t.albumIdx].tracks.size());
    if (n) meta3 += " · " + std::to_string(n) + (n == 1 ? " track" : " tracks");
    const float bw = g.measure("Album", { css::Sm, Weight::SemiBold }) + 32 + 24;
    g.text(album, { css::Md, Weight::Bold }, tx, cy - 22, pal.text, 1, Align::Left, int(w - (tx - x) - bw - 30));
    g.text(meta3, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(w - (tx - x) - bw - 30));
    if (t.albumIdx >= 0) {
        bool clicked = false;
        button(uid(std::string(key) + ".grp", index), ICON_PLAY, "Album", x + w - 16 - bw, cy - 17, false, &clicked);
        if (clicked) playTracks(lib.albums[t.albumIdx].tracks, 0, album);
    }
}

// ============================================================ the body

/**
 * Draws tracks from top (in the scroll area's coordinates, already offset) in the given mode,
 * only what lies in [clipY, clipY + clipH). play(i) runs when row i is chosen. Returns the height.
 */
float App::drawTrackBody(const char* key, const std::vector<int>& tracks, float x, float top, float w, float clipY, float clipH, ListMode mode,
                         bool extended, const std::function<void(int)>& play) {
    const std::vector<ListEntry> layout = listLayout(tracks, w, mode);
    const Tiles tl = tilesFor(w);
    float height = 0;
    for (const auto& e : layout) {
        height = std::max(height, e.y + e.h);
        const float ey = top + e.y;
        if (ey + e.h < clipY - 80 || ey > clipY + clipH + 80) continue;
        switch (e.kind) {
        case ListEntry::ColumnHead: listColumns(x, ey, w, mode, extended); break;
        case ListEntry::GroupHead: groupHead(key, e.index, tracks[e.index], x + 14, ey, w - 28); break;
        case ListEntry::Row: {
            const Id id = uid(std::string(key) + ".r", e.index);
            const bool hit = mode == ListMode::Covers ? coverRow(id, tracks[e.index], e.index + 1, x + 14, ey, w - 28, e.h, extended)
                                                      : trackRow(id, tracks[e.index], e.index + 1, x + 14, ey, w - 28, e.h, true);
            if (hit) play(e.index);
            break;
        }
        case ListEntry::TileRow:
            for (int c = 0; c < tl.cols && e.index + c < int(tracks.size()); c++) {
                const int i = e.index + c;
                if (trackTile(uid(std::string(key) + ".r", i), tracks[i], x + 16 + c * tl.w, ey, tl.w, tl.cover)) play(i);
            }
            break;
        }
    }
    return height + 24;
}
