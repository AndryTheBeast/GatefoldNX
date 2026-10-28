/*
 * Gatefold NX · view_playlists.cpp
 * Playlists: every playlist on one page (the sidebar's "PLAYLISTS" header opens it), in the views the Folders page
 * has: tiles with four covers in the big ones, rows in the small ones. It is where they are put in order, as the
 * play queue's tracks are: + on one opens its menu, Move picks it up, the D-pad moves it, A or B puts it down
 * (App::grab); the sidebar follows. Liked stays first.
 */
#include <algorithm>
#include <strings.h>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

std::vector<int> App::playlistTracks(int ref) const { return listTracks(View::List, ref, {}); }

/** Pages are by index: a deleted playlist's pages leave the history (B never lands on it, or on the one that took its
 *  index), the ones after it move up one; its page, if shown, goes back. */
void App::erasePlaylist(int ref) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    store.playlists.erase(store.playlists.begin() + ref);
    store.save();
    auto gone = [&](const Route& r) { return r.view == View::List && r.ref == ref; };
    history.erase(std::remove_if(history.begin(), history.end(), gone), history.end());
    for (Route& r : history) if (r.view == View::List && r.ref > ref) r.ref--;
    if (gone(route)) back();
    else if (route.view == View::List && route.ref > ref) route.ref--;
}

void App::movePlaylist(int from, int to) {
    const int n = int(store.playlists.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const FolderPlaylist moved = store.playlists[size_t(from)];
    store.playlists.erase(store.playlists.begin() + from);
    store.playlists.insert(store.playlists.begin() + to, moved);
    // pages of playlists open in the history are by index: they move with them
    auto fix = [&](Route& r) {
        if (r.view != View::List || r.ref < 0) return;
        if (r.ref == from) r.ref = to;
        else if (from < to && r.ref > from && r.ref <= to) r.ref--;
        else if (to < from && r.ref >= to && r.ref < from) r.ref++;
    };
    fix(route);
    for (Route& r : history) fix(r);
    store.save();
}

void App::drawPlaylists(float x, float y, float w, float h) {
    const int n = int(store.playlists.size());
    plHover = -2;   // the playlist under focus this frame (+ opens its menu); −1 Liked
    const ListMode mode = ListMode(store.viewPlaylists == 0 ? 2 : store.viewPlaylists);   // an old "by album" reads as tiles
    const int held = grab.kind == Grab::Playlists ? grab.pos : -1;   // picked up with its menu's Move
    const bool tiles = mode == ListMode::Grid || mode == ListMode::Albums;
    const float rowH = mode == ListMode::List ? 52 : 64;

    ScrollArea& sa = area("playlists");
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .view-head
    int smart = 0, folder = 0;
    for (const auto& p : store.playlists) { smart += p.smart(); folder += p.folder(); }
    std::string sub = "Liked and " + std::to_string(n) + (n == 1 ? " playlist" : " playlists");
    if (smart) sub += "  ·  " + std::to_string(smart) + " smart";
    if (folder) sub += "  ·  " + std::to_string(folder) + (folder == 1 ? " from a folder" : " from folders");
    float cy = oy + 18 + 8;
    kicker("Library", x + 28, cy, pal.text3);
    cy += 10;
    g.text("Playlists", { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    g.text(held >= 0 ? "Moving “" + store.playlists[size_t(held)].name + "”: the D-pad moves it, A puts it down" : sub, { css::Sm }, x + 28, cy + 4,
           held >= 0 ? pal.accentInk : pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    bx += button(uid("pls.new"), ICON_PLUS, "New playlist", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) openNewList();
    {
        // By album would be the tiles again here (a playlist is not an album): three views
        int m = store.viewPlaylists == 0 ? 2 : store.viewPlaylists;
        viewSwitch("pls.view", x + w - 28, cy + 17, m, 1u);
        store.viewPlaylists = m;
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    // ref −1 is Liked; then the sidebar's playlists in their order
    auto info = [&](int ref, const std::vector<int>& t) {
        std::string s = std::to_string(t.size()) + (t.size() == 1 ? " track" : " tracks");
        if (ref >= 0) {
            const FolderPlaylist& p = store.playlists[size_t(ref)];
            if (p.smart()) s += " · smart";
            else if (p.folder()) s += " · folder";
        }
        return s;
    };
    auto nameOf = [&](int ref) { return ref < 0 ? std::string("Liked") : store.playlists[size_t(ref)].name; };
    auto keyOf = [&](int ref) { return ref < 0 ? int64_t(-1) : int64_t(uid(store.playlists[size_t(ref)].name + "\t" + store.playlists[size_t(ref)].path) & 0x7fffffff); };
    if (tiles) {
        const float inner = w - 32;
        const int cols = std::max(2, int(inner / 200));
        const float tw = inner / cols, cover = tw - 20, th = 10 + cover + 8 + 20 + 2 + 18 + 10;
        const int count = n + 1, rows = (count + cols - 1) / cols;
        if (held >= 0) grab.cols = cols;   // the D-pad moves a held tile along the grid
        for (int k = 0; k < count; k++) {
            const int ref = k - 1;
            const float tx = x + 16 + (k % cols) * tw, ty = cy + (k / cols) * th;
            const Id id = uid("pls.item", keyOf(ref));
            if (ty + th < y - 40 || ty > y + h + 40) {
                if (ui.item(id, { tx, ty, tw, th }, 14)) navigate(View::List, ref);
                if (ui.focused(id)) plHover = ref;
                continue;
            }
            const std::vector<int> t = playlistTracks(ref);
            const bool hit = ui.item(id, { tx, ty, tw, th }, 14);
            if (ui.focused(id)) plHover = ref;
            const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
            if (ref == held) {   // picked up: lifted, the accent around it (the queue's Move)
                g.roundRect(tx, ty, tw, th, 14, pal.accent, 0.14f);
                g.roundStroke(tx, ty, tw, th, 14, 2, pal.accent, 0.95f);
            } else if (hot) g.roundRect(tx, ty, tw, th, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
            if (!t.empty()) coverQuad(t, tx + 10, ty + 10, cover, 10);
            else {
                g.roundRect(tx + 10, ty + 10, cover, cover, 10, pal.surface2);
                g.icon(playlistIcon(ref), tx + 10 + cover / 2, ty + 10 + cover / 2, cover * 0.3f, pal.text3, 0.8f);
            }
            // the playlist's own icon, small, over the cover's corner: what the sidebar shows for it
            g.roundRect(tx + 18, ty + 18, 30, 30, 9, pal.bg, 0.72f);
            g.icon(playlistIcon(ref), tx + 33, ty + 33, 16, pal.text);
            if (hot && ref != held) {
                g.circle(tx + 10 + cover - 31, ty + 10 + cover - 31, 21, pal.accent);
                g.icon(ICON_CHEV, tx + 10 + cover - 30, ty + 10 + cover - 31, 18, pal.onAccent);
            }
            const float ly = ty + 10 + cover + 8;
            g.text(nameOf(ref), { css::Base, Weight::SemiBold }, tx + 10, ly, pal.text, 1, Align::Left, int(cover));
            g.text(info(ref, t), { css::Sm }, tx + 10, ly + 22, pal.text3, 1, Align::Left, int(cover));
            if (hit) navigate(View::List, ref);
        }
        cy += rows * th + 8;
    } else {
        const float cov = rowH - 4 - 16;
        if (held >= 0) grab.cols = 1;
        for (int k = 0; k <= n; k++) {
            const int ref = k - 1;
            const float ry = cy + k * rowH, rx = x + 14, rw = w - 28;
            const Id id = uid("pls.item", keyOf(ref));
            const bool visible = !(ry + rowH < y - rowH || ry > y + h + rowH);
            const float ctlW = 0;
            const bool hit = ui.item(id, { rx, ry, rw, rowH - 4 }, 10);
            if (hit) navigate(View::List, ref);
            if (ui.focused(id)) plHover = ref;
            if (!visible) continue;
            if (ref == held) {   // picked up: lifted, the accent around it (the queue's Move)
                g.roundRect(rx, ry, rw, rowH - 4, 10, pal.accent, 0.14f);
                g.roundStroke(rx, ry, rw, rowH - 4, 10, 2, pal.accent, 0.95f);
            } else if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
            const float mid = ry + (rowH - 4) / 2;
            const std::vector<int> t = playlistTracks(ref);
            g.roundRect(rx + 16, mid - cov / 2, cov, cov, 8, pal.surface2);
            g.icon(playlistIcon(ref), rx + 16 + cov / 2, mid, cov * 0.5f, ref < 0 ? pal.accentInk : pal.text2);
            const float tx = rx + 16 + cov + 14;
            const bool playing = q.name == nameOf(ref) && isPlaying();
            if (mode == ListMode::List) {
                g.textC(nameOf(ref), { css::Base, Weight::SemiBold }, tx, mid, playing ? pal.accentInk : pal.text, 1, Align::Left, int(rw * 0.5f));
                g.textC(info(ref, t), { css::Sm }, rx + rw - 48 - ctlW, mid, pal.text3, 1, Align::Right, int(rw * 0.3f));
            } else {
                g.text(nameOf(ref), { css::Base, Weight::SemiBold }, tx, mid - 19, playing ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 160 - ctlW));
                std::string s2 = info(ref, t);
                if (ref >= 0 && store.playlists[size_t(ref)].folder()) s2 += " · " + store.playlists[size_t(ref)].path;
                else if (ref >= 0 && store.playlists[size_t(ref)].smart()) s2 = smartSummary(store.playlists[size_t(ref)].path) + " · " + s2;
                g.text(s2, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 160 - ctlW));
            }
            if (playing) eqGlyph(rx + rw - 48 - ctlW - 13 - (mode == ListMode::List ? g.measure(info(ref, t), { css::Sm }) + 14 : 0), mid, pal.accentInk);
            g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
        }
        cy += (n + 1) * rowH;
    }
    sa.content = cy - oy + 24;
    g.unclip();
    ui.popArea();
}
