/*
 * Gatefold NX · view_lists.cpp
 * Track lists: #view-playlist (play queue, Liked, folder playlists, search),
 * #view-album, and the Folders browser that Gatefold NX adds.
 */
#include <algorithm>
#include <cmath>
#include <strings.h>
#include <sys/stat.h>

#include "app.hpp"
#include "icons.inc"

namespace {
constexpr float RowH = 40;   // .vrow is 36 px on the PC; a little taller for fingers
}

void App::drawTrackList(float x, float y, float w, float h) {
    const View v = route.view;
    const std::vector<int> tracks = listTracks(v, route.ref, route.query);

    std::string kick = "Playlist", title, sub, playName;
    const char* placeholder = ICON_LIST;
    if (v == View::Queue) {
        title = q.name.empty() ? "Nothing queued" : q.name;
        playName = q.name;
    } else if (v == View::Search) {
        kick = "Search";
        title = "“" + route.query + "”";
        playName = "Search: " + route.query;
        placeholder = ICON_SEARCH;
    } else if (route.ref < 0) {
        title = playName = "Liked";
        placeholder = ICON_HEART;
    } else if (route.ref < int(store.playlists.size())) {
        const FolderPlaylist& p = store.playlists[route.ref];
        kick = p.own() ? "Playlist" : p.smart() ? "Smart playlist" : "Folder playlist";
        title = playName = p.name;
        placeholder = playlistIcon(route.ref);
    }
    sub = fmtTotal(tracks);
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && !store.playlists[route.ref].own()) {
        const FolderPlaylist& p = store.playlists[route.ref];
        sub = (p.smart() ? smartSummary(p.path) : p.path) + "  ·  " + sub;
    }

    ScrollArea& sa = area(v == View::Search ? "search" : "list", int(v) * 1000 + route.ref);
    const float headH = 18 + 120 + 14;
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;
    const ListMode mode = ListMode(store.viewPlaylist);

    // .pl-head: cover, kicker, title, sub, actions
    const float cx = x + 28, cyTop = oy + 18;
    if (!tracks.empty()) {
        coverQuad(tracks, cx, cyTop, 120, 12);   // .pl-cover.quad (no halo, as on the other pages)
    } else {
        g.roundRect(cx, cyTop, 120, 120, 12, pal.surface2);
        g.icon(placeholder, cx + 60, cyTop + 60, 40, pal.text3, 0.7f);
    }
    const float tx = cx + 120 + 22, tw = w - (tx - x) - 28;
    kicker(kick, tx, cyTop + 8, pal.text3);
    g.text(title, { css::Xxl, Weight::Bold }, tx, cyTop + 16, pal.text, 1, Align::Left, int(tw));
    g.text(sub, { css::Sm }, tx, cyTop + 58, pal.text3, 1, Align::Left, int(tw));
    float bx = tx;
    const float by = cyTop + 120 - 34;
    bool clicked = false;
    // The page's playlist, as it was when the frame began: Export and Open folder go to another page part way through
    // (route.ref is then the picker's −1 or a folder's index), and the buttons after them read the playlist again
    const int ref = route.ref;
    const bool ownPage = v == View::List && ref >= 0 && ref < int(store.playlists.size());
    // The actions after Play and Shuffle shrink to round icon buttons when their labels would run into the views
    // (a folder playlist with Games has seven); the playlist's icon button is always one.
    bool compact = false;
    {
        const Font bf{ css::Sm, Weight::SemiBold };
        auto bw = [&](const char* label) { return 16 + 24 + g.measure(label, bf) + 16 + 8.f; };
        float need = bw("Play") + bw("Shuffle");
        if (v == View::Queue) need += bw("Current track") + bw("Clear");
        else if (v == View::Search) need += bw("New search");
        else if (ownPage && !store.playlists[ref].folder()) need += bw("Delete") + bw("Rename");
        else if (ref >= 0) need += bw("Open folder") + bw("Delete");
        if (v == View::List && bgShown()) need += bw("Games");
        if (v != View::Search) need += bw("Export");
        if (ownPage) need += 34 + 8;
        compact = tx + need > x + w - 28 - (6 + 38 * 4) - 16;
    }
    // A secondary action: labelled, or (compact) round with its icon only
    auto action = [&](const char* key, const char* icon, const char* label, bool toggled = false, bool danger = false) {
        if (!compact) {
            bool c = false;
            bx += button(uid(key), icon, label, bx, by, false, &c, toggled, danger) + 8;
            return c;
        }
        const Id id = uid(key);
        const bool c = ui.item(id, { bx, by, 34, 34 }, 17);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        g.roundRect(bx, by, 34, 34, 17, toggled ? pal.accent : pal.text, toggled ? 0.18f : hot ? 0.11f : 0.065f);
        g.roundStroke(bx, by, 34, 34, 17, 1, toggled ? pal.accent : pal.text, toggled ? 0.5f : 0.15f);
        g.icon(icon, bx + 17, by + 17, 16, danger ? dangerInk() : toggled ? pal.accentInk : pal.text);
        bx += 34 + 8;
        return c;
    };
    bx += button(uid("pl.play"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !tracks.empty()) { if (v == View::Queue) playAt(0); else playTracks(tracks, 0, playName); }
    bx += button(uid("pl.shuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
    if (clicked && !tracks.empty()) playTracks(tracks, 0, playName, true);
    if (v == View::Queue) {
        clicked = action("pl.jump", ICON_DISC, "Current track");
        const int cur = q.pos;   // the list is in play order
        if (clicked && cur >= 0) {
            // Gatefold's "Current track": the list glides there (the row need not exist yet, it is
            // only drawn once on screen) and the focus lands on it when it arrives.
            sa.target = headH + listRowY(listLayout(tracks, w, mode), cur) - (h - RowH) / 2;
            sa.clamp();
            listJump = uid("pl.r", cur);
        }
        if (!tracks.empty()) {
            if (action("pl.clear", ICON_X, "Clear")) {
                queueClear();
                toast("Cleared the playlist");
            }
        }
    } else if (v == View::Search) {
        if (action("pl.search", ICON_SEARCH, "New search")) openSearch();
    } else if (v == View::List && ref >= 0 && ref < int(store.playlists.size()) && !store.playlists[ref].folder()) {
        if (action("pl.rename", ICON_TAG, "Rename")) renamePlaylist(ref);
    } else if (ref >= 0 && ref < int(store.playlists.size())) {
        if (action("pl.folder", ICON_FOLDER, "Open folder")) navigate(View::Folders, lib.folderByPath(store.playlists[ref].path));
    }

    // Games: the game events that play this playlist (the background player), lit while any does
    if (v == View::List && bgShown()) {
        const uint64_t h = listHash(title);
        const bool used = (bgs.cfg.onOpen == 4 && bgs.cfg.lists[0] == h) || (bgs.cfg.onHome == 4 && bgs.cfg.lists[1] == h) ||
                          (bgs.cfg.onClose == 4 && bgs.cfg.lists[2] == h) || (bgs.cfg.onReturn == 4 && bgs.cfg.lists[3] == h);
        if (action("pl.games", ICON_GAME, "Games", used)) openListActions(title);
    }

    // Export, as an .m3u8 in a folder chosen on the SD card (m3u.hpp): any list here but search results
    if (v != View::Search) {
        if (action("pl.export", ICON_DOWNLOAD, "Export")) openExportPicker(playName.empty() ? title : playName, tracks);   // as listed: the queue in play order
    }

    // The playlist's sidebar icon, as a round button wearing it: the icon grid opens at once
    if (ownPage) {
        const Id iid = uid("pl.icon");
        if (ui.item(iid, { bx, by, 34, 34 }, 17)) openPlaylistIcons(ref);
        const bool hot = ui.pressed(iid) || (ui.focused(iid) && ui.showFocusRing());
        g.roundRect(bx, by, 34, 34, 17, pal.accent, hot ? 0.24f : 0.14f);
        g.roundStroke(bx, by, 34, 34, 17, 1, pal.accent, 0.45f);
        g.icon(playlistIcon(ref), bx + 17, by + 17, 17, pal.accentInk);
        bx += 34 + 8;
    }

    // Delete, always the last of the actions (asked first on every playlist, folder ones too)
    if (ownPage && store.playlists[ref].folder()) {
        if (action("pl.remove", ICON_X, "Delete", false, true)) confirmRemoveFolderPlaylist(ref, int(tracks.size()));
    } else if (ownPage) {
        if (action("pl.remove", ICON_X, "Delete", false, true)) {
            // asked first, as a track's Delete is (the sheet's card)
            const FolderPlaylist& p = store.playlists[ref];
            const bool smart = p.smart();
            const std::string count = std::to_string(tracks.size()) + (tracks.size() == 1 ? " track" : " tracks");
            confirm(smart ? ICON_SPARKLE : ICON_LIST, "Delete", p.name, smart ? "Smart playlist · " + count : "Playlist · " + count,
                    "Delete this playlist?",
                    smart ? "It leaves the sidebar. Your tracks, their play counts and Liked are not touched."
                          : "It leaves the sidebar, and its order is lost. The tracks stay on the SD card and in your library.",
                    "Delete", [this, ref, name = p.name] {
                        if (ref < 0 || ref >= int(store.playlists.size()) || store.playlists[ref].name != name) return;
                        erasePlaylist(ref);
                        toast("Deleted the playlist “" + name + "”");
                    });
        }
    }

    // the four views, right of the actions (.pl-modes)
    {
        int m = store.viewPlaylist;
        viewSwitch("pl.view", x + w - 28, by + 17, m);
        store.viewPlaylist = m;
    }

    // the tracks (no panel behind them: the background shows through, as everywhere here)
    const float ly = oy + headH;
    if (tracks.empty()) {
        listColumns(x, ly, w, mode == ListMode::Grid ? ListMode::List : mode, false);
        const char* msg = v == View::Search ? "No tracks match" : v == View::Queue ? "Play something from the library or a folder"
                        : ref < 0 ? "Tracks you like appear here"
                        : ref < int(store.playlists.size()) && store.playlists[ref].smart() ? "Nothing matches yet"
                                                                                                         : "This playlist has no tracks";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, ly + 90, pal.text2, 1, Align::Center);
    }
    g.clip(x, y, w, h);
    bodyGrab = v == View::Queue && grab.kind == Grab::Queue && !grab.upNext ? grab.pos : -1;
    const float bodyH = drawTrackBody("pl", tracks, x, ly, w, y, h, mode, false, [&](int i) {
        if (v == View::Queue) playAt(i);   // i is the position in play order
        else playTracks(tracks, i, playName);
    });
    bodyGrab = -1;
    if (v == View::Queue) {
        if (bodyFocus >= 0) hoverQueue = bodyFocus;
        if (grab.pos >= 0 && grab.kind == Grab::Queue && !grab.upNext) grab.cols = bodyCols;
    }
    sa.content = headH + bodyH;
    g.unclip();
    if (listJump && ui.hasItem(listJump) && std::fabs(sa.y - sa.target) < RowH) {
        ui.focus(listJump);
        listJump = 0;
    }
    ui.popArea();
}

/** A folder playlist's Delete (its page, or the Folders page's "In playlists"): the same card as your own playlists'. */
/** The first launch (the user, 2026-10-27: always asked): the card's Music folder offered, with the guided tour or without
 *  (the user, 2026-10-28), the picker under the card for another. No Cancel: Another folder, B or × leave the picker. */
void App::welcomeCard(const std::string& found) {
    confirm(ICON_FOLDER, "Welcome to Gatefold", "Where is your music?", "On this SD card: " + found, "Read your music from " + found + "?",
            "Gatefold makes albums, artists and genres from the tags of the files in one folder and the folders inside it, "
            "whatever their layout. Settings › Library can change it later.",
            "Use it and take the tour", [this, found] {
                tourAsk = false;
                tourPending = true;   // App::update starts it once the music is read (tour.cpp)
                useMusicFolder(found);
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_SPARKLE;
    sheet.askNoCancel = true;
    sheet.askAlt = "Just use it";
    sheet.askAltYes = [this, found] { tourAsk = false; useMusicFolder(found); };
    sheet.askAlt2 = "Another folder";
    sheet.askAlt2Yes = [] {};   // the picker is already under the card (the tour is offered once a folder is chosen: tourAsk)
}

void App::confirmRemoveFolderPlaylist(int ref, int count) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    const FolderPlaylist& p = store.playlists[ref];
    confirm(ICON_FOLDER, "Delete", p.name, "Folder playlist · " + std::to_string(count) + (count == 1 ? " track" : " tracks"),
            "Delete this playlist?", "It leaves the sidebar. The folder and its tracks stay on the SD card; Folders can add it again.",
            "Delete", [this, ref, name = p.name, path = p.path] {
                if (ref >= int(store.playlists.size()) || store.playlists[ref].name != name || store.playlists[ref].path != path) return;
                erasePlaylist(ref);
                toast("Deleted the playlist “" + name + "”");
            });
}

void App::drawAlbum(float x, float y, float w, float h) {
    const int a = routeAlbum();
    if (a < 0) { viewHead("Album", "Not in the library", "Its tags changed or the music folder was rescanned.", x, y, w); return; }
    const Album& al = lib.albums[a];

    ScrollArea& sa = area("album", a);
    const float heroH = 26 + 220 + 22;
    sa.view = { x, y, w, h };
    sa.content = heroH + 30 + 8 + al.tracks.size() * RowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .album-hero::before (the album's accent, fading down) and the cover's halo are left out: on the console the
    // tint covered only the content area and clashed with the sidebar and player bar. The app's background stays.
    const float cx = x + 30, cyTop = oy + 26;
    coverBox(al.tracks[0], cx, cyTop, 220, 12);

    // h1 + .album-meta + .album-actions, bottom-aligned with the cover
    const float tx = cx + 220 + 28, tw = w - (tx - x) - 30;
    const Font h1{ 46, Weight::ExtraBold };
    const std::vector<std::string> lines = g.wrap(al.title, h1, int(tw), 2);
    float ty = cyTop + 220 - 34 - 16 - 22 - 8 - lines.size() * 48 - 6 - 18;
    kicker("Album", tx, ty + 8, pal.text3);
    ty += 18 + 6;
    for (const auto& l : lines) { g.text(l, h1, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 48; }
    ty += 8;
    float mx = tx;
    if (!al.artist.empty()) mx += g.text(al.artist, { css::Base, Weight::SemiBold }, mx, ty, pal.text);
    std::string rest = (al.year.empty() ? "" : " · " + al.year) + " · " + fmtTotal(al.tracks);
    g.text(rest, { css::Base }, mx, ty, pal.text2, 1, Align::Left, int(tx + tw - mx));
    ty += 22 + 16;

    float bx = tx;
    bool clicked = false;
    const std::string name = al.title;
    bx += button(uid("al.play"), ICON_PLAY, "Play", bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) playTracks(al.tracks, 0, name);
    bx += button(uid("al.shuffle"), ICON_SHUFFLE, "Shuffle", bx, ty, false, &clicked) + 8;
    if (clicked) playTracks(al.tracks, 0, name, true);
    bx += button(uid("al.artist"), ICON_MIC, "Artist", bx, ty, false, &clicked) + 8;
    if (clicked) openArtist(al.artist);
    bx += button(uid("al.folder"), ICON_FOLDER, "Folder", bx, ty, false, &clicked) + 8;
    if (clicked) navigate(View::Folders, lib.folderByPath(lib.tracks[al.tracks[0]].folder));
    {
        const std::string& gs = lib.tracks[al.tracks[0]].genre;
        const std::string g1 = gs.substr(0, gs.find(';'));
        if (!g1.empty()) {
            bx += button(uid("al.genre"), ICON_TAG, g1, bx, ty, false, &clicked) + 8;
            if (clicked) openGenre(g1);
        }
    }

    const float ly = oy + heroH;
    listHead(x, ly, w, false);
    g.clip(x, std::max(y, ly + 30), w, y + h - std::max(y, ly + 30));
    const float top = ly + 30 + 8;
    for (int i = 0; i < int(al.tracks.size()); i++) {
        const float ry = top + i * RowH;
        if (ry + RowH < y - RowH || ry > y + h + RowH) continue;
        if (trackRow(uid("row", i), al.tracks[i], i + 1, x + 14, ry, w - 28, RowH, false)) playTracks(al.tracks, i, name);
    }
    g.unclip();
    ui.popArea();
}

void App::drawFolders(float x, float y, float w, float h) {
    if (lib.folders.empty()) {
        viewHead("Folders", "No music folder", "Put your music in sdmc:/music and choose Rescan in Settings.", x, y, w);
        return;
    }
    int f = route.ref;
    if (f < 0 || f >= int(lib.folders.size())) f = route.ref = lib.rootFolders[0];
    const FolderNode& node = lib.folders[f];
    const bool isRoot = node.parent < 0;
    const int total = node.last - node.first;
    // By album groups a folder's own tracks; a folder of sub-folders only would show the tiles twice, so it has three views
    const bool ownTracks = !lib.folders[f].tracks.empty();
    const int shownMode = !ownTracks && store.viewFolders == 0 ? 2 : store.viewFolders;
    const ListMode mode = ListMode(shownMode);

    ScrollArea& sa = area("folders", f);
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .view-head
    std::string sub = node.path;
    if (!node.children.empty()) sub += "  ·  " + std::to_string(node.children.size()) + (node.children.size() == 1 ? " folder" : " folders");
    sub += "  ·  " + std::to_string(total) + (total == 1 ? " track" : " tracks");
    float cy = oy + 18 + 8;
    kicker("Folders", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? "Music" : node.name, { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    // Actions: play everything below, keep it as a playlist, go up; the four views on the right
    float bx = x + 28;
    bool clicked = false;
    const std::string name = isRoot ? "Music" : node.name;
    const std::vector<int> all = lib.tracksUnder(f);
    bx += button(uid("fo.play"), ICON_PLAY, "Play", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !all.empty()) playTracks(all, 0, name);
    bx += button(uid("fo.shuffle"), ICON_SHUFFLE, "Shuffle", bx, cy, false, &clicked) + 8;
    if (clicked && !all.empty()) playTracks(all, 0, name, true);
    const int pl = store.playlistByPath(node.path);
    bx += button(uid("fo.addpl"), pl >= 0 ? ICON_LIST : ICON_PLUS, pl >= 0 ? "In playlists" : "Add as playlist", bx, cy, false, &clicked, pl >= 0) + 8;
    if (clicked) {
        if (pl >= 0) {
            confirmRemoveFolderPlaylist(pl, total);   // asks first, as on the playlist's own page
        } else {
            // Two folders of one name ("CD1" of two albums) would be one list to the game actions and the overlay
            const std::string as = store.freePlaylistName(name);
            store.playlists.push_back({ as, node.path, {} });
            toast("Added “" + as + "” to playlists");
            store.save();
        }
    }
    if (!node.children.empty()) {
        bx += button(uid("fo.addall"), ICON_PLUS, "Add all as playlists", bx, cy, false, &clicked) + 8;
        if (clicked) addSubfoldersAsPlaylists(f);
    }
    if (!isRoot) {
        bx += button(uid("fo.up"), ICON_BACK, "Up", bx, cy, false, &clicked) + 8;
        if (clicked) { navigate(View::Folders, node.parent, {}, false); ui.focus(uid("fo.dir", f)); }
    }
    {
        int m = shownMode;
        viewSwitch("fo.view", x + w - 28, cy + 17, m, ownTracks ? 0u : 1u);
        if (m != shownMode) store.viewFolders = m;   // a folder without tracks leaves a chosen "by album" as it is
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    cy += drawSubfolders("fo.dir", node, x, cy, w, y, h, mode);

    // Tracks directly in this folder, in the chosen view
    float bodyH = 0;
    if (!node.tracks.empty()) {
        cy += 16;
        bodyH = drawTrackBody("fo", node.tracks, x, cy, w, y, h, mode, false, [&](int i) { playTracks(node.tracks, i, name); });
    } else if (node.children.empty()) {
        g.textC("This folder is empty", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    }
    sa.content = cy - oy + bodyH + 24;
    g.unclip();
    ui.popArea();
}

/** A folder's sub-folders in the chosen view (Folders, and Library → Folders): tiles with four covers in the big
 *  views, rows with one in the small ones. Returns the height drawn. */
float App::drawSubfolders(const char* idp, const FolderNode& node, float x, float cy, float w, float y, float h, ListMode mode) {
    const int nSub = int(node.children.size());
    if (!nSub) return 0;
    if (mode == ListMode::Grid || mode == ListMode::Albums) {
        // .tile with the folder's four covers (Gatefold 1.4.1's Folders)
        const float inner = w - 32;
        const int cols = std::max(2, int(inner / 200));
        const float tw = inner / cols, cover = tw - 16, th = 8 + cover + 10 + 20 + 18 + 16;
        const int rows = (nSub + cols - 1) / cols;
        for (int r = 0; r < rows; r++) {
            const float ry = cy + r * th;
            if (ry + th < y - 40 || ry > y + h + 40) continue;
            for (int c = 0; c < cols && r * cols + c < nSub; c++) {
                const int ch = node.children[r * cols + c];
                if (folderTile(uid(idp, ch), ch, x + 16 + c * tw, ry, tw, cover)) navigate(View::Folders, ch);
            }
        }
        return rows * th + 8;
    }
    // rows: a cover (or the folder icon), the name, what is inside, a chevron
    const float rowH = mode == ListMode::List ? 48 : 60, cov = rowH - 4 - 16;
    for (int i = 0; i < nSub; i++) {
        const int c = node.children[i];
        const FolderNode& ch = lib.folders[c];
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const Id id = uid(idp, c);
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10)) navigate(View::Folders, c);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (ch.last > ch.first && meta.art(ch.first).state == AlbumArt::Ready) {
            coverBox(ch.first, rx + 16, mid - cov / 2, cov, 6);
        } else {
            if (ch.last > ch.first) meta.wantTags(ch.first);
            g.roundRect(rx + 16, mid - cov / 2, cov, cov, 6, pal.surface2);
            g.icon(ICON_FOLDER, rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
        }
        const int n = ch.last - ch.first;
        std::string info = std::to_string(n) + (n == 1 ? " track" : " tracks");
        if (!ch.children.empty()) info = std::to_string(ch.children.size()) + (ch.children.size() == 1 ? " folder · " : " folders · ") + info;
        if (store.playlistByPath(ch.path) >= 0) info += " · in playlists";
        const float tx = rx + 16 + cov + 14;
        if (mode == ListMode::List) {
            g.textC(ch.name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
            g.textC(info, { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.35f));
        } else {
            g.text(ch.name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 140));
            g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 140));
        }
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    return nSub * rowH;
}

/** "Add all as playlists" (Folders, Library → Folders): every sub-folder of a folder becomes a folder playlist;
 *  those already in are left as they are, a taken name gets "<name> 2". Asks first, with the count. */
void App::addSubfoldersAsPlaylists(int f) {
    if (f < 0 || f >= int(lib.folders.size())) return;
    std::vector<int> add;
    for (int c : lib.folders[f].children)
        if (store.playlistByPath(lib.folders[c].path) < 0) add.push_back(c);
    if (add.empty()) { toast("Every folder here is in playlists already"); return; }
    const int n = int(add.size());
    const std::string count = std::to_string(n) + (n == 1 ? " folder" : " folders");
    confirm(ICON_LIST, "Folders", lib.folders[f].parent < 0 ? "Music" : lib.folders[f].name, count + " not in playlists yet",
            "Add " + count + " as playlists?",
            "Each becomes a playlist in the sidebar, named after its folder, with the tracks in it and in the folders inside it. "
            "Folders already in playlists stay as they are.",
            "Add all", [this, f, path = lib.folders[f].path] {
                if (f >= int(lib.folders.size()) || lib.folders[f].path != path) return;   // rescanned meanwhile
                int added = 0;
                for (int c : lib.folders[f].children) {
                    const FolderNode& ch = lib.folders[c];
                    if (store.playlistByPath(ch.path) >= 0) continue;
                    store.playlists.push_back({ store.freePlaylistName(ch.name), ch.path, {} });
                    added++;
                }
                store.save();
                toast("Added " + std::to_string(added) + (added == 1 ? " playlist" : " playlists"));
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_PLUS;
}

/**
 * Browsing the SD card's folders: for the music folder ("Use this folder"); as View::PickPlaylist, for a playlist
 * file to play or import (+ → Import a playlist file…: .m3u and .m3u8 files are listed after the folders); as
 * View::PickExport, for the folder an exported playlist goes in ("Export here"; the files there are shown, not picked).
 */
void App::drawPicker(float x, float y, float w, float h) {
    const bool exporting = route.view == View::PickExport;
    const bool files = route.view == View::PickPlaylist || exporting;
    const View self = route.view;
    std::string path = route.query.empty() ? "sdmc:/" : route.query;
    const bool isRoot = path == "sdmc:/" || path == "sdmc:";
    auto join = [](const std::string& a, const std::string& b) { return a.back() == '/' ? a + b : a + "/" + b; };
    auto parent = [&](const std::string& p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : p.substr(0, s);
    };
    // Listing the card is cheap but not free: keep the last listing.
    static std::string listedPath;
    static bool listedFiles = false;
    static unsigned listedGen = ~0u;
    static std::vector<std::string> dirs, lists;
    if (listedPath != path || listedFiles != files || listedGen != pickerGen) {
        listedGen = pickerGen;
        dirs = listDirs(path);
        lists = files ? listPlaylistFiles(path) : std::vector<std::string>();
        listedPath = path;
        listedFiles = files;
    }
    const int nDirs = int(dirs.size()), nRows = nDirs + int(lists.size());

    ScrollArea& sa = area(exporting ? "pickex" : files ? "pickpl" : "picker", int(uid(path) & 0x7fffffff));
    const float headH = 18 + 8 + 10 + 38 + 22 + 16 + 34 + 16;
    const float rowH = 52;
    sa.view = { x, y, w, h };
    sa.content = headH + nRows * rowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    float cy = oy + 18 + 8;
    const bool choosing = !files && folderFor >= 0;   // Settings → Storage and reset: a browser's start folder
    kicker(exporting ? "Export “" + exportName + "” to" : files ? "Import a playlist file" : choosing ? folderPickName() : "Music folder", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? "SD card" : baseName(path), { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    std::string sub = path + "  ·  " + std::to_string(dirs.size()) + (dirs.size() == 1 ? " folder" : " folders");
    if (files) sub += lists.empty() ? std::string("  ·  no playlist files")
                                    : "  ·  " + std::to_string(lists.size()) + (lists.size() == 1 ? " playlist file" : " playlist files");
    else if (choosing) sub += folderPickField().empty() ? std::string("  ·  now its own default") : "  ·  now " + folderPickField();
    else if (!store.musicRoot.empty()) sub += "  ·  now reading " + store.musicRoot;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    if (choosing) {
        bx += button(uid("pk.use"), ICON_CHECK, "Choose this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        // Backups inside Gatefold's own settings folder would be copied into themselves, and cleared by a fresh start
        const std::string cfg = Store::Dir;
        if (clicked && folderFor == 4 && strncasecmp(path.c_str(), cfg.c_str(), cfg.size()) == 0 && (path.size() == cfg.size() || path[cfg.size()] == '/')) {
            toast("Backups cannot go in Gatefold's own settings folder");
            clicked = false;
        }
        if (clicked) {
            ui.popArea();
            folderPickField() = path;
            const std::string what = folderPickName();
            folderFor = -1;
            persistSoon(0.3f);
            back();
            toast(what + ": " + (path == "sdmc:/" ? std::string("the SD card") : path));
            return;
        }
    } else if (!files) {
        bx += button(uid("pk.use"), ICON_FOLDER, "Use this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        if (clicked) { ui.popArea(); useMusicFolder(path); return; }
    }
    if (exporting) {
        bx += button(uid("pk.export"), ICON_DOWNLOAD, "Export here", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked) { ui.popArea(); exportPlaylist(path); return; }
        bx += button(uid("pk.mkdir"), ICON_PLUS, "New folder", bx, cy, false, &clicked) + 8;
        if (clicked) {
            std::string name;
            if (keyboard("New folder", "", name)) {
                while (!name.empty() && name.back() == ' ') name.pop_back();
                while (!name.empty() && name[0] == ' ') name.erase(0, 1);
                const std::string made = join(path, name);
                struct stat st;
                if (name.empty()) {
                    // nothing typed: nothing made
                } else if (name[0] == '.' || name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    toast("A folder name cannot start with a dot or hold / \\ : * ? \" < > |");
                } else if (stat(made.c_str(), &st) == 0) {
                    toast("“" + name + "” is already here");
                } else if (mkdir(made.c_str(), 0777) != 0) {
                    toast("Could not make the folder “" + name + "”");
                } else {
                    listedPath.clear();   // list it again
                    ui.popArea();
                    navigate(self, -1, made, false);
                    toast("Made the folder “" + name + "”");
                    return;
                }
            }
        }
    }
    if (!isRoot) {
        bx += button(uid("pk.up"), ICON_BACK, "Up", bx, cy, false, &clicked, false, false, files && !exporting && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) { navigate(self, -1, parent(path), false); ui.focus(uid("pk.dir", int(uid(path) & 0x7fffffff))); }
    }
    if (files || choosing || !store.musicRoot.empty()) {
        bx += button(uid("pk.cancel"), ICON_X, "Cancel", bx, cy, false, &clicked, false, false, files && !exporting && isRoot && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) {
            ui.popArea();
            folderFor = -1;
            if (files) leavePlaylistPicker();
            else back();
            return;
        }
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    for (int i = 0; i < nRows; i++) {
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const bool isFile = i >= nDirs;
        const std::string& name = isFile ? lists[size_t(i - nDirs)] : dirs[size_t(i)];
        const std::string child = join(path, name);
        const Id id = uid("pk.dir", int(uid(child) & 0x7fffffff));
        // exporting: the files already there are shown (a name taken is asked about), not picked
        const bool pickable = !(exporting && isFile);
        if (pickable && ui.item(id, { rx, ry, rw, rowH - 4 }, 10, (isRoot || files) && !exporting && i == 0 ? DefaultFocus : 0)) {
            if (isFile) openPlaylistFile(child);
            else navigate(self, -1, child, !files && !choosing);   // the playlist browsers and a chosen folder: one entry in the history
        }
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (isFile) {
            // a playlist file: the accent tile with the list icon, its name without the extension, then the extension
            const size_t dot = name.find_last_of('.');
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, pal.accent, pickable ? 0.16f : 0.08f);
            g.icon(ICON_LIST, rx + 34, mid, 18, pickable ? pal.accentInk : pal.text3);
            g.textC(name.substr(0, dot), { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, pickable ? pal.text : pal.text3, 1, Align::Left, int(rw - 220));
            g.textC("Playlist file · ." + name.substr(dot + 1), { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        } else {
            const bool current = !files && strcasecmp(child.c_str(), (choosing ? folderPickField() : store.musicRoot).c_str()) == 0;
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, current ? pal.accent : pal.surface2, current ? 0.25f : 1.f);
            g.icon(ICON_FOLDER, rx + 34, mid, 18, current ? pal.accentInk : pal.text2);
            g.textC(name, { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, current ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 160));
            if (current) g.textC(choosing ? "chosen now" : "current music folder", { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        }
        if (pickable) g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    if (nRows == 0)
        g.textC(files ? "No folders or playlist files here" : "No folders here", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}
