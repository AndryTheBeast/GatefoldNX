/*
 * Gatefold NX · view_library.cpp
 * #view-library as Gatefold 1.4.2 has it: Albums | Tracks | Folders.
 *
 * - Albums: the album grid (view_grid.cpp), also the sidebar's own Albums tab.
 * - Tracks: every song in one list, sorted by artist, title, album, year, genre or length, either
 *   way round, filtered by what you type, with a letter index down the side. A plays the whole
 *   list from that song. Any of the four list views.
 * - Folders: the music folder's folders as tiles with their four covers; one opens it in Folders.
 *
 * No darker panel behind the lists, unlike the PC: they sit on the background like the rest.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <strings.h>
#include <thread>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"

namespace {

const char* const SortNames[] = { "By artist", "By title", "By album", "By year", "By genre", "By length" };

std::string lowerCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = char(tolower((unsigned char)c));
    return o;
}

/** The letter a name files under: A–Z, else '#' (digits, symbols, other scripts). */
char letterOf(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '.' || c == '(' || c == '[' || c == '\'' || c == '"') continue;
        const char u = char(toupper((unsigned char)c));
        return u >= 'A' && u <= 'Z' ? u : '#';
    }
    return '#';
}

/** The name without what letterOf skips at its start ("(I Just)…" files under I). */
const char* filedFrom(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '_' || s[i] == '.' || s[i] == '(' || s[i] == '[' || s[i] == '\'' || s[i] == '"')) i++;
    return s.c_str() + i;
}

/**
 * Names in the order the letter index files them: digits and symbols first (#), then A–Z, ignoring what
 * a name opens with; so the index reads # A B C… with each letter once. Sorting by the raw text put
 * "(Everything…" among the symbols while the index filed it under E.
 */
int filedCompare(const std::string& a, const std::string& b) {
    const char la = letterOf(a), lb = letterOf(b);
    if (la != lb) return la < lb ? -1 : 1;   // '#' sorts before 'A'
    return strcasecmp(filedFrom(a), filedFrom(b));
}

struct FolderTiles { int cols; float w, cover, h; };
FolderTiles folderTilesFor(float w) {
    const float inner = w - 32;
    const int cols = std::max(2, int(inner / 200));
    const float tw = inner / cols, cover = tw - 16;
    return { cols, tw, cover, 8 + cover + 10 + 20 + 18 + 16 };
}

}  // namespace

// ============================================================ Tracks: sorting and filtering

namespace {

/** What a sort needs of a track, copied on the main thread: the metadata thread keeps writing the library's own. */
struct SortRow {
    int idx;
    std::string title, artist, album, genre, date;   // artist: the album artist when there is one
    int trackNo;
    double duration;
};

void sortRows(std::vector<SortRow>& rows, int s, bool desc, const std::string& filter, std::vector<int>& list,
              std::vector<std::pair<char, int>>& letters) {
    // The filter looks in the field the list is sorted by (the title when sorting by length).
    const std::string f = lowerCopy(filter);
    auto field = [s](const SortRow& t) -> const std::string& {
        return s == 0 ? t.artist : s == 2 ? t.album : s == 3 ? t.date : s == 4 ? t.genre : t.title;
    };
    if (!f.empty()) std::erase_if(rows, [&](const SortRow& t) { return lowerCopy(field(t)).find(f) == std::string::npos; });
    auto byAlbumOrder = [&](const SortRow& a, const SortRow& b) {
        if (int c = filedCompare(a.album, b.album)) return c < 0;
        if (a.trackNo != b.trackNo) return a.trackNo < b.trackNo;
        return filedCompare(a.title, b.title) < 0;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const SortRow& a, const SortRow& b) {
        switch (s) {
        case 1: if (int c = filedCompare(a.title, b.title)) return c < 0; return filedCompare(a.artist, b.artist) < 0;
        case 2: return byAlbumOrder(a, b);
        case 3: if (int c = a.date.compare(b.date)) return c < 0;
                if (int c = filedCompare(a.artist, b.artist)) return c < 0;
                return byAlbumOrder(a, b);
        case 4: if (int c = filedCompare(a.genre, b.genre)) return c < 0;
                if (int c = filedCompare(a.artist, b.artist)) return c < 0;
                return byAlbumOrder(a, b);
        case 5: return a.duration < b.duration;
        default: if (int c = filedCompare(a.artist, b.artist)) return c < 0; return byAlbumOrder(a, b);
        }
    });
    if (desc) std::reverse(rows.begin(), rows.end());
    list.clear();
    for (const SortRow& r : rows) list.push_back(r.idx);
    // The letter index follows the sort's own field; year and length have none.
    letters.clear();
    if (s != 3 && s != 5)
        for (int i = 0; i < int(rows.size()); i++) {
            const SortRow& t = rows[i];
            const char c = letterOf(s == 1 ? t.title : s == 2 ? t.album : s == 4 ? t.genre : t.artist);
            if (letters.empty() || letters.back().first != c) letters.push_back({ c, i });
        }
}

}  // namespace

/** A sort running on a worker core, and the list it was asked for. */
struct TracksJob {
    std::atomic<bool> done{ false };
    unsigned gen = 0, epoch = 0;
    int sort = 0;
    bool desc = false;
    std::string filter;
    std::vector<int> list;
    std::vector<std::pair<char, int>> letters;
};

/**
 * Library → Tracks, sorted and filtered. A big library is sorted on a worker core (thousands of
 * tracks took long enough to show, and it ran again each time tags arrived): until it is done the
 * previous list stays, which is at worst a moment out of date.
 */
const std::vector<int>& App::sortedTracks() {
    TracksView& tv = tracksView;
    if (tv.job && tv.job->done) {
        const TracksJob& j = *tv.job;
        if (j.epoch == libEpoch) {   // the same scan: its indexes hold, even if tags moved on meanwhile
            tv.list = j.list;
            tv.letters = j.letters;
            tv.gen = j.gen;
            tv.sort = j.sort;
            tv.desc = j.desc;
            tv.filter = j.filter;
            tv.epoch = j.epoch;
        }
        tv.job.reset();
        if (tv.thread.joinable()) tv.thread.join();   // done already: returns at once
    }
    if (tv.epoch != libEpoch) {   // a rescan: the old indexes point at other tracks
        tv.list.clear();
        tv.letters.clear();
        tv.epoch = libEpoch;
        tv.gen = ~0u;
    }
    if (tv.gen == indexGen && tv.sort == store.tracksSort && tv.desc == store.tracksDesc && tv.filter == tracksFilter) return tv.list;
    if (tv.job) return tv.list;   // one at a time; the newest wish is started when it is done

    std::vector<SortRow> rows;
    rows.reserve(lib.tracks.size());
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const Track& t = lib.tracks[i];
        rows.push_back({ i, t.title, t.albumArtist.empty() ? t.artist : t.albumArtist, t.album, t.genre, t.date.substr(0, std::min<size_t>(4, t.date.size())),
                         t.trackNo, t.duration });
    }
    auto job = std::make_shared<TracksJob>();
    job->gen = indexGen;
    job->epoch = libEpoch;
    job->sort = store.tracksSort;
    job->desc = store.tracksDesc;
    job->filter = tracksFilter;
    if (rows.size() < 1000) {   // small: at once, as before
        sortRows(rows, job->sort, job->desc, job->filter, job->list, job->letters);
        job->done = true;
        tv.job = job;
        return sortedTracks();
    }
    tv.job = job;
    // Joined when its result is taken, never detached: libnx has no pthread_detach.
    tv.thread = std::thread([job, rows = std::move(rows)]() mutable {
        cores::moveHere(cores::Files);
        sortRows(rows, job->sort, job->desc, job->filter, job->list, job->letters);
        job->done = true;
    });
    return tv.list;
}

// ============================================================ the page

/** Folder tile: the four covers of what is inside, its name, and how much music. */
bool App::folderTile(Id id, int folder, float x, float y, float w, float cover) {
    const FolderNode& fo = lib.folders[folder];
    const float h = 8 + cover + 10 + 20 + 18 + 8;
    const bool hit = ui.item(id, { x, y, w, h }, 14);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float cx = x + 8, cy = y + 8;
    std::vector<int> inside;
    for (int i = fo.first; i < fo.last && i < fo.first + 400; i++) inside.push_back(i);
    if (inside.empty()) {
        g.roundRect(cx, cy, cover, cover, 12, pal.surface2);
        g.icon(ICON_FOLDER, cx + cover / 2, cy + cover / 2, cover * 0.3f, pal.text3);
    } else {
        for (int k = 0; k < std::min<int>(int(inside.size()), 40); k++) meta.wantTags(inside[k]);
        coverQuad(inside, cx, cy, cover, 12);
    }
    const int n = fo.last - fo.first;
    double secs = 0;
    for (int i = fo.first; i < fo.last; i++) secs += std::max(0.0, lib.tracks[i].duration);
    std::string sub = std::to_string(n) + (n == 1 ? " track" : " tracks");
    if (secs > 0) {
        const int m = int(secs / 60);
        sub += " · " + (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
    }
    g.text(fo.name, { css::Base, Weight::SemiBold }, cx, cy + cover + 10, pal.text, 1, Align::Left, int(cover));
    g.text(sub, { css::Sm }, cx, cy + cover + 32, pal.text3, 1, Align::Left, int(cover));
    return hit;
}

void App::drawAlbums(float x, float y, float w, float h) {
    const int loose = int(std::count_if(lib.tracks.begin(), lib.tracks.end(), [](const Track& t) { return t.albumIdx < 0; }));
    std::string sub = std::to_string(lib.albums.size()) + " albums · " + std::to_string(lib.tracks.size()) + " tracks";
    if (loose && meta.taggedCount() >= int(lib.tracks.size())) sub += " · " + std::to_string(loose) + " without an album tag";
    drawAlbumGrid(x, y, w, h, viewHead("Library", "Albums", sub, x, y, w));
}

void App::drawLibrary(float x, float y, float w, float h) {
    static const char* tabs[] = { "Albums", "Tracks", "Folders" };
    const int tab = store.libTab;
    std::string sub;
    if (tab == 0) sub = std::to_string(lib.albums.size()) + " albums · " + std::to_string(lib.tracks.size()) + " tracks";
    else if (tab == 1) {
        const std::vector<int>& list = sortedTracks();
        sub = fmtTotal(list);
        if (!tracksFilter.empty()) sub += " · matching “" + tracksFilter + "”";
    } else {
        const int root = lib.rootFolders.empty() ? -1 : lib.rootFolders[0];
        const int n = root >= 0 ? int(lib.folders[root].children.size()) : 0;
        sub = std::to_string(n) + (n == 1 ? " folder · " : " folders · ") + std::to_string(lib.tracks.size()) + " tracks";
    }
    // .view-head with the tab switch at its right (Albums | Tracks | Folders)
    float cy = y + 18 + 8;
    kicker("Library", x + 28, cy, pal.text3);
    cy += 10;
    g.text(tabs[tab], { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56 - 330));
    cy += 38;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56 - 330));
    int picked = -1;
    segmented(uid("lib.tab"), x + w - 28, y + 52, { "Albums", "Tracks", "Folders" }, tab, &picked);
    if (picked >= 0 && picked != tab) { store.libTab = picked; persistSoon(0.5f); }
    const float top = cy + 22 + 14;

    if (tab == 0) { drawAlbumGrid(x, y, w, h, top + 46); return; }
    if (tab == 1) { drawTracksTab(x, y, w, h, top); return; }
    drawFoldersTab(x, y, w, h, top);
}

void App::drawTracksTab(float x, float y, float w, float h, float top) {
    const std::vector<int>& list = sortedTracks();
    // the tools row: Play, Shuffle · filter, sort, direction, the four views
    bool clicked = false;
    float bx = x + 28;
    const float by = top;
    bx += button(uid("trk.play"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !list.empty()) playTracks(list, 0, "Library");
    bx += button(uid("trk.shuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
    if (clicked && !list.empty()) playTracks(list, 0, "Library", true);
    float rx = x + w - 28;
    int mode = store.viewTracks;
    rx -= viewSwitch("trk.view", rx, by + 17, mode) + 8;
    store.viewTracks = mode;
    // direction
    const Id did = uid("trk.dir");
    if (ui.item(did, { rx - 36, by, 36, 34 }, 9)) { store.tracksDesc = !store.tracksDesc; persistSoon(0.5f); }
    g.roundRect(rx - 36, by, 36, 34, 9, pal.text, ui.focused(did) && ui.showFocusRing() ? 0.11f : 0.065f);
    g.icon(store.tracksDesc ? ICON_SORT_UP : ICON_SORT_DOWN, rx - 18, by + 17, 17, pal.text2);
    rx -= 36 + 8;
    // sort: a select that cycles, as the album grid's
    const Font sf{ css::Sm };
    const float sw = g.measure(SortNames[store.tracksSort], sf) + 20 + 26;
    const Id sid = uid("trk.sort");
    if (ui.item(sid, { rx - sw, by, sw, 34 }, 8)) { store.tracksSort = (store.tracksSort + 1) % 6; persistSoon(0.5f); }
    g.roundRect(rx - sw, by, sw, 34, 8, pal.text, ui.focused(sid) && ui.showFocusRing() ? 0.11f : 0.065f);
    g.roundStroke(rx - sw, by, sw, 34, 8, 1, pal.text, 0.15f);
    g.textC(SortNames[store.tracksSort], sf, rx - sw + 10, by + 17, pal.text);
    g.icon(ICON_CHEV, rx - 16, by + 17, 14, pal.text3);
    rx -= sw + 8;
    // filter: the console keyboard (Gatefold filters as you type; here, as you finish)
    static const char* const FilterBy[] = { "Filter artists…", "Filter titles…", "Filter albums…", "Filter years…", "Filter genres…", "Filter titles…" };
    const std::string ft = tracksFilter.empty() ? FilterBy[std::clamp(store.tracksSort, 0, 5)] : tracksFilter;
    const float fw = std::min(220.f, std::max(120.f, rx - bx - 8));
    const Id fid = uid("trk.filter");
    if (ui.item(fid, { rx - fw, by, fw, 34 }, 8)) {
        std::string q = tracksFilter;
        static const char* const Header[] = { "Filter by artist", "Filter by title", "Filter by album", "Filter by year", "Filter by genre", "Filter by title" };
        if (keyboard(Header[std::clamp(store.tracksSort, 0, 5)], tracksFilter, q)) tracksFilter = q;
    }
    g.roundRect(rx - fw, by, fw, 34, 8, pal.text, ui.focused(fid) && ui.showFocusRing() ? 0.11f : 0.065f);
    g.roundStroke(rx - fw, by, fw, 34, 8, 1, pal.text, 0.15f);
    g.icon(ICON_SEARCH, rx - fw + 16, by + 17, 14, pal.text3);
    g.textC(ft, sf, rx - fw + 30, by + 17, tracksFilter.empty() ? pal.text3 : pal.text, 1, Align::Left, int(fw - 40));

    // the list, with the letter index down the right edge when the sort has letters
    const float lt = by + 34 + 14;
    const bool index = !tracksView.letters.empty() && store.viewTracks != int(ListMode::Grid);
    const float lw = w - (index ? 26 : 0);
    ScrollArea& sa = area("lib.tracks");
    sa.view = { x, lt, w, y + h - lt };
    ui.pushArea(&sa);
    g.clip(x, lt, w, y + h - lt);
    if (list.empty()) {
        g.textC(tracksFilter.empty() ? "No music found" : "No tracks match", { css::Lg, Weight::SemiBold }, x + w / 2, lt + 80, pal.text2, 1, Align::Center);
        sa.content = 0;
    } else {
        sa.content = drawTrackBody("trk", list, x, lt - sa.y, lw, lt, y + h - lt, ListMode(store.viewTracks), true,
                                   [this, &list](int i) { playTracks(list, i, "Library"); });
    }
    g.unclip();
    ui.popArea();
    if (index) {
        // #-to-Z down the side, only the letters present; a tap or A glides there
        const auto& L = tracksView.letters;
        const float top2 = lt + 8, avail = y + h - top2 - 8;
        const float step = std::min(22.f, avail / L.size());
        const std::vector<ListEntry> layout = listLayout(list, lw, ListMode(store.viewTracks));
        for (int i = 0; i < int(L.size()); i++) {
            const float ly = top2 + i * step;
            const Id id = uid("trk.az", L[i].first);
            if (ui.item(id, { x + w - 24, ly, 22, step }, 6)) {
                sa.target = listRowY(layout, L[i].second);
                sa.clamp();
            }
            const bool hot = ui.focused(id) && ui.showFocusRing();
            g.textC(std::string(1, L[i].first), { css::Xs, Weight::SemiBold }, x + w - 13, ly + step / 2, hot ? pal.text : pal.text2, 1, Align::Center);
        }
    }
}

void App::drawFoldersTab(float x, float y, float w, float h, float top) {
    const int root = lib.rootFolders.empty() ? -1 : lib.rootFolders[0];
    if (root < 0) {
        g.textC("No music folder", { css::Lg, Weight::SemiBold }, x + w / 2, top + 80, pal.text2, 1, Align::Center);
        return;
    }
    const FolderNode& node = lib.folders[root];
    bool clicked = false;
    float bx = x + 28;
    const std::vector<int> all = lib.tracksUnder(root);
    bx += button(uid("lfo.play"), ICON_PLAY, "Play", bx, top, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !all.empty()) playTracks(all, 0, "Music");
    bx += button(uid("lfo.shuffle"), ICON_SHUFFLE, "Shuffle", bx, top, false, &clicked) + 8;
    if (clicked && !all.empty()) playTracks(all, 0, "Music", true);
    g.icon(ICON_FOLDER, x + w - 28 - g.measure(node.path, { css::Sm, Weight::SemiBold }) - 14, top + 17, 15, pal.text2);
    g.textC(node.path, { css::Sm, Weight::SemiBold }, x + w - 28, top + 17, pal.text2, 1, Align::Right);

    const float lt = top + 34 + 14;
    const FolderTiles ft = folderTilesFor(w);
    ScrollArea& sa = area("lib.folders");
    sa.view = { x, lt, w, y + h - lt };
    const int n = int(node.children.size()), rows = (n + ft.cols - 1) / ft.cols;
    sa.content = rows * ft.h + 24;
    ui.pushArea(&sa);
    g.clip(x, lt, w, y + h - lt);
    for (int r = 0; r < rows; r++) {
        const float ry = lt + r * ft.h - sa.y;
        if (ry + ft.h < lt - 40 || ry > y + h + 40) continue;
        for (int c = 0; c < ft.cols && r * ft.cols + c < n; c++) {
            const int f = node.children[r * ft.cols + c];
            if (folderTile(uid("lfo.tile", f), f, x + 16 + c * ft.w, ry, ft.w, ft.cover)) navigate(View::Folders, f);
        }
    }
    g.unclip();
    ui.popArea();
}
