/*
 * Gatefold NX · view_grid.cpp
 * #view-library (album tiles), #view-artists and the artist page, and the same
 * two views for genres. Albums, artists and genres come from tags, whatever the
 * folder layout is.
 */
#include <algorithm>
#include <unordered_map>
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
bool App::tile(Id id, int coverTrack, float x, float y, float w, bool round, const std::string& title, const std::string& sub,
               const std::vector<int>* quad, uint8_t flags) {
    const float cover = w - 20;
    const float h = 10 + cover + 8 + 20 + 2 + 18 + 10;
    const bool hit = ui.item(id, { x, y, w, h }, 14, flags);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float rad = round ? cover / 2 : 10;
    // Gatefold's shadow under the cover (0 10px 24px -14px) is left out at the user's request, as on Now playing
    if (quad) coverQuad(*quad, x + 10, y + 10, cover, rad);   // it draws its own outline
    else {
        coverBox(coverTrack, x + 10, y + 10, cover, rad);
        g.roundStroke(x + 10, y + 10, cover, cover, rad, 1, pal.text, 0.09f);
    }
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
            const Id tid = uid("lib.tile", int(uid(al.key) & 0x7fffffff));
            // The first album is where the page opens (not the sort button above it); Up from the top row goes to
            // the sort button, which the layout put out of reach from the left (the search field was nearer)
            if (r == 0) ui.link(tid, 0, -1, sid);
            if (tile(tid, al.tracks[0], gl.x0 + c * gl.tileW, headBottom + r * gl.tileH - sa.y, gl.tileW, false, al.title, al.artist, nullptr,
                     i == 0 ? DefaultFocus : 0))
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
            // artists are round, as in Gatefold; a genre shows the covers of its first four albums, as Folders does
            if (tile(uid(genre ? "gen.tile" : "art.tile", int(uid(ar.name) & 0x7fffffff)), cover, gl.x0 + c * gl.tileW, headBottom + r * gl.tileH - sa.y,
                     gl.tileW, !genre, ar.name, sub, genre ? &ar.tracks : nullptr))
                genre ? openGenre(ar.name) : openArtist(ar.name);
        }
    g.unclip();
    ui.popArea();
}

/** The artists with tracks in a genre, by name, with how many of those tracks are theirs. */
const std::vector<std::pair<int, int>>& App::genreArtists(int gi) {
    if (gi == genreArtistsFor_ && indexGen == genreArtistsGen_) return genreArtists_;
    genreArtistsFor_ = gi;
    genreArtistsGen_ = indexGen;
    genreArtists_.clear();
    std::unordered_map<int, int> count;   // index into artists → tracks in this genre
    for (int t : genres[gi].tracks) {
        std::string key = lib.tracks[t].artist.empty() ? "Unknown artist" : lib.tracks[t].artist;
        for (char& c : key) c = char(tolower((unsigned char)c));
        if (auto it = artistByKey.find(key); it != artistByKey.end()) count[it->second]++;
    }
    for (const auto& [a, n] : count) genreArtists_.push_back({ a, n });
    // artists is sorted by name already, so its index orders them
    std::sort(genreArtists_.begin(), genreArtists_.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return genreArtists_;
}

void App::drawArtist(float x, float y, float w, float h, bool genre) {
    const int ai = genre ? routeGenre() : routeArtist();
    if (ai < 0) { viewHead(genre ? "Genre" : "Artist", route.query, "Not in the library", x, y, w); return; }
    const Artist& ar = genre ? genres[ai] : artists[ai];
    const int lead = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];

    const GridLayout gl = gridFor(x, w);
    const float heroH = 26 + 220 + 22;
    // One tab at a time under a switch, as in Library: a genre has Albums, Artists and Tracks, an artist Albums and
    // Tracks. Tabs are numbered alike (0 Albums, 1 Artists, 2 Tracks); with no albums, Albums is greyed and Tracks shows.
    int& chosen = genre ? genreTab : artistTab;
    int tab = std::clamp(chosen, 0, 2);
    if (!genre && tab == 1) tab = 0;
    if (ar.albums.empty() && tab == 0) tab = 2;
    const std::vector<std::pair<int, int>>* inGenre = tab == 1 ? &genreArtists(ai) : nullptr;
    const float switchH = 56;   // the switch's row
    const int gridItems = tab == 1 ? int(inGenre->size()) : int(ar.albums.size());
    const int rows = (gridItems + gl.cols - 1) / gl.cols;
    const float gridH = (tab == 2 || gridItems == 0) ? 0 : 10 + rows * gl.tileH;
    const float songsH = (tab == 0 || tab == 1) ? 0 : 46 + 30 + 8 + ar.tracks.size() * RowH;
    ScrollArea& sa = area(genre ? "genre" : "artist", int(uid(ar.name) & 0x7fffffff));
    sa.view = { x, y, w, h };
    sa.content = heroH + switchH + gridH + songsH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // Gatefold tints the hero with the cover's accent and haloes the cover; on the console the tint covered
    // only the content area and clashed with the sidebar and player bar, so the page keeps the app's background.
    const float cx = x + 30, cyTop = oy + 26;
    if (genre) coverQuad(ar.tracks, cx, cyTop, 220, 16);
    else coverBox(lead, cx, cyTop, 220, 110);

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
    {
        // .seg as in Library (Albums | Tracks | Folders): what this page holds, one view at a time
        const float sy = oy + heroH + 22;
        std::string count;
        if (tab == 0) count = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums");
        else if (tab == 1) count = std::to_string(inGenre->size()) + (inGenre->size() == 1 ? " artist" : " artists");
        else count = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
        g.textC(count, { css::Lg, Weight::Bold }, x + 30, sy, pal.text);
        // On Tracks the four list views (as in Library → Tracks) take the right edge and the switch sits left of them
        float rx = x + w - 30;
        if (tab == 2) {
            int mode = store.viewGenre;
            rx -= viewSwitch(genre ? "gen.view" : "ar.view", rx, sy, mode) + 10;
            store.viewGenre = mode;
        }
        int picked = -1;
        const std::vector<std::string> names = genre ? std::vector<std::string>{ "Albums", "Artists", "Tracks" } : std::vector<std::string>{ "Albums", "Tracks" };
        const int shown = genre ? tab : (tab == 2 ? 1 : 0);
        segmented(uid(genre ? "gen.tab" : "ar.tab"), rx, sy, names, shown, &picked, ar.albums.empty() ? 1u : 0u);
        if (picked >= 0 && picked != shown) chosen = genre ? picked : (picked == 1 ? 2 : 0);
        gy = oy + heroH + switchH;
    }
    if (tab == 1) {
        // Artists: round, as in the Artists tab; the count is their tracks in this genre
        for (int i = 0; i < int(inGenre->size()); i++) {
            const Artist& p = artists[(*inGenre)[i].first];
            const int n = (*inGenre)[i].second;
            const float ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            const int cover = p.albums.empty() ? p.tracks[0] : lib.albums[p.albums[0]].tracks[0];
            if (tile(uid("gen.artist", int(uid(p.name) & 0x7fffffff)), cover, gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, true, p.name,
                     std::to_string(n) + (n == 1 ? " track here" : " tracks here")))
                openArtist(p.name);
        }
    } else if (tab != 2) {
        for (int i = 0; i < int(ar.albums.size()); i++) {
            const Album& al = lib.albums[ar.albums[i]];
            const std::string sub = al.year.empty() ? std::to_string(al.tracks.size()) + " tracks" : al.year;
            const float ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            if (tile(uid("ar.tile", int(uid(al.key) & 0x7fffffff)), al.tracks[0], gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, false, al.title, sub))
                openAlbum(ar.albums[i]);
        }
    }

    // The Tracks tab: the same list as Library → Tracks, in the view chosen beside the switch
    if (tab == 2) {
        const float top = oy + heroH + switchH;
        // false: a playlist's columns (no Genre column on a genre's own page)
        const float bodyH = drawTrackBody(genre ? "gen" : "ar", ar.tracks, x, top, w, y, h, ListMode(store.viewGenre), false,
                                          [&](int i) { playTracks(ar.tracks, i, ar.name); });
        sa.content = heroH + switchH + bodyH + 24;
    }
    g.unclip();
    ui.popArea();
}
