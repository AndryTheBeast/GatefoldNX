/*
 * Gatefold NX · view_lists.cpp
 * Track lists: #view-playlist (play queue, Liked, folder playlists, search),
 * #view-album, and the Folders browser that Gatefold NX adds.
 */
#include <algorithm>
#include <cmath>
#include <strings.h>

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
        const bool own = store.playlists[route.ref].own();
        kick = own ? "Playlist" : "Folder playlist";
        title = playName = store.playlists[route.ref].name;
        placeholder = own ? ICON_LIST : ICON_FOLDER;
    }
    sub = fmtTotal(tracks);
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && !store.playlists[route.ref].own())
        sub = store.playlists[route.ref].path + "  ·  " + sub;

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
    bx += button(uid("pl.play"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !tracks.empty()) { if (v == View::Queue) playAt(0); else playTracks(tracks, 0, playName); }
    bx += button(uid("pl.shuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
    if (clicked && !tracks.empty()) playTracks(tracks, 0, playName, true);
    if (v == View::Queue) {
        bx += button(uid("pl.jump"), ICON_DISC, "Current track", bx, by, false, &clicked) + 8;
        const int cur = currentQueuePos();
        if (clicked && cur >= 0) {
            // Gatefold's "Current track": the list glides there (the row need not exist yet, it is
            // only drawn once on screen) and the focus lands on it when it arrives.
            sa.target = headH + listRowY(listLayout(tracks, w, mode), cur) - (h - RowH) / 2;
            sa.clamp();
            listJump = uid("pl.r", cur);
        }
    } else if (v == View::Search) {
        bx += button(uid("pl.search"), ICON_SEARCH, "New search", bx, by, false, &clicked) + 8;
        if (clicked) openSearch();
    } else if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].own()) {
        bx += button(uid("pl.rename"), ICON_TAG, "Rename", bx, by, false, &clicked) + 8;
        if (clicked) {
            std::string name = store.playlists[route.ref].name;
            if (keyboard("Rename the playlist", name, name) && !name.empty()) { store.playlists[route.ref].name = name; store.save(); }
        }
        bx += button(uid("pl.remove"), ICON_X, "Delete", bx, by, false, &clicked, false, true) + 8;
        if (clicked) {
            toast("Deleted the playlist “" + store.playlists[route.ref].name + "”");
            store.playlists.erase(store.playlists.begin() + route.ref);
            store.save();
            ui.popArea();
            back();
            return;
        }
    } else if (route.ref >= 0 && route.ref < int(store.playlists.size())) {
        bx += button(uid("pl.folder"), ICON_FOLDER, "Open folder", bx, by, false, &clicked) + 8;
        if (clicked) navigate(View::Folders, lib.folderByPath(store.playlists[route.ref].path));
        bx += button(uid("pl.remove"), ICON_X, "Remove", bx, by, false, &clicked, false, true) + 8;
        if (clicked) {
            toast("Removed “" + store.playlists[route.ref].name + "” from playlists");
            store.playlists.erase(store.playlists.begin() + route.ref);
            store.save();
            ui.popArea();
            back();
            return;
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
                        : route.ref < 0 ? "Tracks you like appear here" : "This playlist has no tracks";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, ly + 90, pal.text2, 1, Align::Center);
    }
    g.clip(x, y, w, h);
    const float bodyH = drawTrackBody("pl", tracks, x, ly, w, y, h, mode, false, [&](int i) {
        if (v == View::Queue) {
            const int k = int(std::find(q.order.begin(), q.order.end(), i) - q.order.begin());
            playAt(k);
        } else {
            playTracks(tracks, i, playName);
        }
    });
    sa.content = headH + bodyH;
    g.unclip();
    if (listJump && ui.hasItem(listJump) && std::fabs(sa.y - sa.target) < RowH) {
        ui.focus(listJump);
        listJump = 0;
    }
    ui.popArea();
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
    const ListMode mode = ListMode(store.viewFolders);
    // The big views show sub-folders as tiles with four covers; the small ones as rows with one.
    const bool tiles = mode == ListMode::Grid || mode == ListMode::Albums;
    const float folderRowH = mode == ListMode::List ? 48 : 60;

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
            store.playlists.erase(store.playlists.begin() + pl);
            toast("Removed “" + name + "” from playlists");
        } else {
            store.playlists.push_back({ name, node.path, {} });
            toast("Added “" + name + "” to playlists");
        }
        store.save();
    }
    if (!isRoot) {
        bx += button(uid("fo.up"), ICON_BACK, "Up", bx, cy, false, &clicked) + 8;
        if (clicked) { navigate(View::Folders, node.parent, {}, false); ui.focus(uid("fo.dir", f)); }
    }
    {
        int m = store.viewFolders;
        viewSwitch("fo.view", x + w - 28, cy + 17, m);
        store.viewFolders = m;
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    const int nSub = int(node.children.size());
    if (tiles && nSub) {
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
                if (folderTile(uid("fo.dir", ch), ch, x + 16 + c * tw, ry, tw, cover)) navigate(View::Folders, ch);
            }
        }
        cy += rows * th + 8;
    } else {
        // rows: a cover (or the folder icon), the name, what is inside, a chevron
        const float cov = folderRowH - 4 - 16;
        for (int i = 0; i < nSub; i++) {
            const int c = node.children[i];
            const FolderNode& ch = lib.folders[c];
            const float ry = cy + i * folderRowH, rx = x + 14, rw = w - 28;
            if (ry + folderRowH < y - folderRowH || ry > y + h + folderRowH) continue;
            const Id id = uid("fo.dir", c);
            if (ui.item(id, { rx, ry, rw, folderRowH - 4 }, 10)) navigate(View::Folders, c);
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, folderRowH - 4, 10, pal.text, 0.065f);
            const float mid = ry + (folderRowH - 4) / 2;
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
        cy += nSub * folderRowH;
    }

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

/** Choosing the music folder: browse the SD card's folders, then "Use this folder". */
void App::drawPicker(float x, float y, float w, float h) {
    std::string path = route.query.empty() ? "sdmc:/" : route.query;
    const bool isRoot = path == "sdmc:/" || path == "sdmc:";
    auto join = [](const std::string& a, const std::string& b) { return a.back() == '/' ? a + b : a + "/" + b; };
    auto parent = [&](const std::string& p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : p.substr(0, s);
    };
    // Listing the card is cheap but not free: keep the last listing.
    static std::string listedPath;
    static std::vector<std::string> dirs;
    if (listedPath != path) { dirs = listDirs(path); listedPath = path; }

    ScrollArea& sa = area("picker", int(uid(path) & 0x7fffffff));
    const float headH = 18 + 8 + 10 + 38 + 22 + 16 + 34 + 16;
    const float rowH = 52;
    sa.view = { x, y, w, h };
    sa.content = headH + dirs.size() * rowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    float cy = oy + 18 + 8;
    kicker("Music folder", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? "SD card" : baseName(path), { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    std::string sub = path + "  ·  " + std::to_string(dirs.size()) + (dirs.size() == 1 ? " folder" : " folders");
    if (!store.musicRoot.empty()) sub += "  ·  now reading " + store.musicRoot;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    bx += button(uid("pk.use"), ICON_FOLDER, "Use this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
    if (clicked) { ui.popArea(); useMusicFolder(path); return; }
    if (!isRoot) {
        bx += button(uid("pk.up"), ICON_BACK, "Up", bx, cy, false, &clicked) + 8;
        if (clicked) { navigate(View::PickFolder, -1, parent(path), false); ui.focus(uid("pk.dir", int(uid(path) & 0x7fffffff))); }
    }
    if (!store.musicRoot.empty()) {
        bx += button(uid("pk.cancel"), ICON_X, "Cancel", bx, cy, false, &clicked) + 8;
        if (clicked) { ui.popArea(); back(); return; }
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    for (int i = 0; i < int(dirs.size()); i++) {
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const std::string child = join(path, dirs[i]);
        const Id id = uid("pk.dir", int(uid(child) & 0x7fffffff));
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10, isRoot && i == 0 ? DefaultFocus : 0)) navigate(View::PickFolder, -1, child);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        const bool current = strcasecmp(child.c_str(), store.musicRoot.c_str()) == 0;
        g.roundRect(rx + 16, mid - 18, 36, 36, 8, current ? pal.accent : pal.surface2, current ? 0.25f : 1.f);
        g.icon(ICON_FOLDER, rx + 34, mid, 18, current ? pal.accentInk : pal.text2);
        g.textC(dirs[i], { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, current ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 160));
        if (current) g.textC("current music folder", { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    if (dirs.empty()) g.textC("No folders here", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}
