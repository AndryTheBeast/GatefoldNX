/*
 * Gatefold NX · view_automix.cpp
 * Automix playlists (the user's idea, 2026-10-28): + › Automix playlist opens a picker (Tracks, Albums, Artists, Genres,
 * Folders, a tick on each row; an album, artist, genre or folder ticks all of its tracks); Done names it, and Gatefold
 * orders it for the best joins (automix::order, from each track's analysis). It is ordered again whenever tracks are
 * added, and once the tracks not analysed yet are (they are analysed first meanwhile), until it is moved by hand; its page
 * then offers Order again. A track can be pinned to play first. Over every page, as the join editor.
 */
#include <algorithm>

#include "app.hpp"
#include "icons.inc"

/** Orders an Automix playlist (store.playlists[ref]) for the best joins; tracks gone from the library stay at its end. */
automix::Model App::mixModel() const {
#ifdef GF_DEV
    automix::Model m;
    if (automix::modelFromName(store.automixModel, m)) return m;   // Settings › Debug: models compared on the console
#endif
    return automix::Latest;   // a release: the newest (a choice saved by a dev build is ignored)
}

void App::orderAutomixList(int ref, bool announce) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    FolderPlaylist& p = store.playlists[size_t(ref)];
    if (!p.own() || !p.automix) return;
    std::vector<std::string> here, gone;
    std::vector<mixan::Info> info;
    bool waiting = false;
    for (const auto& path : p.tracks) {
        if (!pathIndex.count(path)) gone.push_back(path);
        else here.push_back(path);
    }
    // by path first: the same tracks give the same order, whatever order they stood in (the user, 2026-10-28: "every time I
    // click on the reorganize button it gives a completely different sequence": ties went by where the tracks stood)
    std::sort(here.begin(), here.end());
    // only the analysed ones are ordered (and shown); the others wait after them, and each joins the order where it fits
    // best once analysed (automixListsTick): the mix builds up song by song (the user, 2026-10-28)
    std::vector<std::string> later;
    {
        std::vector<std::string> ready;
        for (const auto& path : here) (automixReady(path) ? ready : later).push_back(path);
        here.swap(ready);
    }
    waiting = !later.empty();
    for (const auto& path : here) info.push_back(levels.find(path)->mix);
    std::vector<const mixan::Info*> ptr;
    for (const auto& i : info) ptr.push_back(&i);
    int first = -1;
    for (size_t i = 0; i < here.size(); i++) if (!p.pin.empty() && here[i] == p.pin) first = int(i);
    const std::vector<int> ord = automix::order(ptr, first, mixModel());
    std::vector<std::string> out;
    for (int i : ord) out.push_back(here[size_t(i)]);
    out.insert(out.end(), later.begin(), later.end());
    out.insert(out.end(), gone.begin(), gone.end());
    p.tracks = out;
    p.waiting = waiting;
    p.manual = false;
    store.save();
    if (announce) toast(waiting ? "Ordered “" + p.name + "”; again once all of its tracks are analysed" : "Ordered “" + p.name + "” for Automix");
}

bool App::automixReady(const std::string& path) const {
    const Level* l = levels.find(path);
    return l && l->mix.seconds != 0;
}

/**
 * Every second: in an Automix playlist still being analysed, each track the worker has finished leaves the waiting ones
 * (after the ordered ones, not shown) and goes where it joins best between two of them (the gain of putting it there:
 * the two new joins less the one it breaks; never before a pinned first track); once all are in, the whole playlist is
 * ordered once more. An insertion is a row of scores, cheap on the console; ordering again each time would not be.
 */
void App::automixListsTick() {
    if (time - automixTickAt < 1) return;
    automixTickAt = time;
    for (int i = 0; i < int(store.playlists.size()); i++) {
        FolderPlaylist& p = store.playlists[size_t(i)];
        if (!p.automix || !p.waiting || p.manual) continue;
        // the ordered ones (ready, in front), then the waiting ones
        size_t head = 0;
        while (head < p.tracks.size() && pathIndex.count(p.tracks[head]) && automixReady(p.tracks[head])) head++;
        bool moved = false, all = true;
        for (size_t k = head; k < p.tracks.size();) {
            const std::string path = p.tracks[k];
            if (!pathIndex.count(path)) { k++; continue; }
            if (!automixReady(path)) { all = false; k++; continue; }
            const mixan::Info& x = levels.find(path)->mix;
            auto info = [&](size_t j) { return &levels.find(p.tracks[j])->mix; };
            const size_t from = !p.pin.empty() && head > 0 && p.tracks[0] == p.pin ? 1 : 0;
            size_t at = head;   // at the end, unless somewhere between is better
            const automix::Model mm = mixModel();
            double best = head > 0 ? automix::joinScore(info(head - 1), &x, mm) : 0;
            for (size_t j = from; j < head; j++) {   // before p.tracks[j]
                const double g = (j > 0 ? automix::joinScore(info(j - 1), &x, mm) : 0) + automix::joinScore(&x, info(j), mm) -
                                 (j > 0 ? automix::joinScore(info(j - 1), info(j), mm) : 0);
                if (g > best) { best = g; at = j; }
            }
            p.tracks.erase(p.tracks.begin() + long(k));
            p.tracks.insert(p.tracks.begin() + long(at), path);
            head++;
            k++;
            moved = true;
        }
        if (all) {
            orderAutomixList(i, false);
            toast("“" + p.name + "” is ordered for Automix: all of its songs are analysed");
        } else if (moved && time - automixSavedAt >= 5) {
            automixSavedAt = time;
            store.save();
        }
    }
}

/** The tracks of Automix playlists not analysed yet (up to `max`): the levelling worker takes them first. */
std::vector<std::string> App::automixWaitingTracks(size_t max) const {
    std::vector<std::string> out;
    for (const auto& p : store.playlists) {
        if (!p.own() || !p.automix) continue;
        for (const auto& path : p.tracks) {
            if (out.size() >= max) return out;
            const Level* l = levels.find(path);
            if (pathIndex.count(path) && (!l || l->mix.seconds == 0)) out.push_back(path);
        }
    }
    return out;
}

/** The queue came from an Automix playlist (q.name is the playlist's): only then does Automix join its tracks. */
bool App::automixQueue() const {
    if (q.name.empty()) return false;
    for (const auto& p : store.playlists)
        if (p.own() && p.automix && p.name == q.name) return true;
    return false;
}

void App::automixProgress(int& done, int& total) const {
    done = total = 0;
    for (const auto& p : store.playlists) {
        if (!p.own() || !p.automix) continue;
        for (const auto& path : p.tracks) {
            if (!pathIndex.count(path)) continue;
            const Level* l = levels.find(path);
            total++;
            if (l && l->mix.seconds != 0) done++;
        }
    }
}

void App::openAutomixPicker() {
    closeSheet();
    ap = AutomixPick{};
    ap.open = true;
    ap.sel.assign(lib.tracks.size(), 0);
}

void App::closeAutomixPicker() { ap.open = false; ap.sel.clear(); ap.sel.shrink_to_fit(); }

void App::drawAutomixPicker() {
    if (!ap.open) return;
    if (ap.sel.size() != lib.tracks.size()) ap.sel.assign(lib.tracks.size(), 0);   // the library changed under it
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.6f);
    const float W = 1080, H = 676, X = (css::W - W) / 2, Y = (css::H - H) / 2, pad = 24, ix = X + pad, iw = W - pad * 2;
    g.roundRect(X, Y, W, H, 18, pal.surface);
    g.roundStroke(X, Y, W, H, 18, 1, pal.text, 0.08f);

    // ---- header
    g.roundRect(ix, Y + pad, 40, 40, 10, pal.accent, 0.14f);
    g.icon(ICON_LINK, ix + 20, Y + pad + 20, 20, pal.accentInk);
    kicker("New Automix playlist", ix + 54, Y + pad + 7, pal.accentInk);
    g.text("Pick the songs; Gatefold orders them for the best joins", { css::Lg, Weight::Bold }, ix + 54, Y + pad + 16, pal.text, 1, Align::Left,
           int(iw - 54 - 260));
    if (iconButton(uid("ap.x"), ICON_X, X + W - pad - 17, Y + pad + 20)) { closeAutomixPicker(); return; }
    int count = 0;
    for (char s : ap.sel) count += s;
    {
        const Font bf{ css::Sm, Weight::SemiBold };
        const float dw = 16 + 24 + g.measure("Done", bf) + 16;
        bool done = false;
        button(uid("ap.done"), ICON_CHECK, "Done", X + W - pad - 34 - 12 - dw, Y + pad + 3, count > 0, &done);
        if (done) {
            if (!count) { toast("Pick a few songs first"); return; }
            std::string name = "Automix";
            if (!keyboard("Name the Automix playlist", name, name)) return;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) name = "Automix";
            name = store.freePlaylistName(name);
            FolderPlaylist p{ name, "", {} };
            for (size_t i = 0; i < ap.sel.size(); i++) if (ap.sel[i]) p.tracks.push_back(lib.tracks[i].path);
            p.automix = true;
            store.playlists.push_back(p);
            const int ref = int(store.playlists.size()) - 1;
            closeAutomixPicker();
            orderAutomixList(ref, true);
            mixView = true;   // its joins shown (Mix)
            navigate(View::List, ref);
            return;
        }
    }

    // ---- the tabs, and how many are ticked
    const float ty = Y + pad + 62;
    {
        int picked = -1;
        segmented(uid("ap.tab"), ix + iw, ty + 18, { "Tracks", "Albums", "Artists", "Genres", "Folders" }, ap.tab, &picked);
        if (picked >= 0 && picked != ap.tab) ap.tab = picked;
        char b[64];
        snprintf(b, sizeof b, "%d song%s ticked", count, count == 1 ? "" : "s");
        g.textC(b, { css::Md, Weight::SemiBold }, ix, ty + 18, count ? pal.accentInk : pal.text2, 1);
    }

    // ---- the rows: only those on screen are drawn (a library of thousands)
    struct RowRef { const std::vector<int>* tracks; int one; std::string title, sub; };
    auto rowCount = [&]() -> int {
        switch (ap.tab) {
        case 0: return int(lib.tracks.size());
        case 1: return int(lib.albums.size());
        case 2: return int(artists.size());
        case 3: return int(genres.size());
        default: return std::max(0, int(lib.folders.size()) - 1);   // every folder but the music folder itself
        }
    };
    auto rowAt = [&](int i) -> RowRef {
        switch (ap.tab) {
        case 0: { const Track& t = lib.tracks[size_t(i)]; return { nullptr, i, t.title, t.artist + (t.album.empty() ? "" : " · " + t.album) }; }
        case 1: { const Album& a = lib.albums[size_t(i)]; return { &a.tracks, -1, a.title, a.artist }; }
        case 2: return { &artists[size_t(i)].tracks, -1, artists[size_t(i)].name, "" };
        case 3: return { &genres[size_t(i)].tracks, -1, genres[size_t(i)].name, "" };
        default: { const FolderNode& f = lib.folders[size_t(i) + 1]; return { nullptr, -2 - (i + 1), f.name, f.path }; }
        }
    };
    const float lx = ix, ly = ty + 46, lw = iw, lh = Y + H - pad - ly, RH = 48;
    ScrollArea& sa = area("ap.list", ap.tab);
    sa.view = { lx, ly, lw, lh };
    const int n = rowCount();
    sa.content = n * RH;
    ui.pushArea(&sa);
    g.clip(lx, ly, lw, lh);
    const int i0 = std::max(0, int(sa.y / RH) - 1), i1 = std::min(n, int((sa.y + lh) / RH) + 2);
    for (int i = i0; i < i1; i++) {
        const RowRef r = rowAt(i);
        const float y = ly - sa.y + i * RH;
        // its tracks: one, a list, or a folder's range
        std::vector<int> folderTracks;
        const std::vector<int>* list = r.tracks;
        if (r.one <= -2) { folderTracks = lib.tracksUnder(-2 - r.one); list = &folderTracks; }
        int on = 0, all = 0;
        if (r.one >= 0) { on = ap.sel[size_t(r.one)]; all = 1; }
        else for (int t : *list) { on += ap.sel[size_t(t)]; all++; }
        const Id id = uid("ap.r", ap.tab * 1000000 + i);
        if (ui.item(id, { lx, y, lw, RH - 4 }, 10)) {
            const char v = on == all ? 0 : 1;   // all ticked: untick them all; else tick them all
            if (r.one >= 0) ap.sel[size_t(r.one)] = v;
            else for (int t : *list) ap.sel[size_t(t)] = v;
        }
        const bool hot = ui.focused(id) && ui.showFocusRing();
        if (hot || ui.pressed(id)) g.roundRect(lx, y, lw, RH - 4, 10, pal.text, 0.06f);
        // the tick: full, part (a dash), or empty
        const float bx = lx + 14, by = y + (RH - 4) / 2 - 10;
        if (on && on == all) {
            g.roundRect(bx, by, 20, 20, 5, pal.accent);
            g.icon(ICON_CHECK, bx + 10, by + 10, 14, pal.onAccent);
        } else {
            g.roundStroke(bx, by, 20, 20, 5, 1.5f, pal.text, 0.35f);
            if (on) g.roundRect(bx + 5, by + 9, 10, 2.5f, 1, pal.accent);
        }
        g.text(r.title.empty() ? "Unknown" : r.title, { css::Base, Weight::SemiBold }, lx + 48, y + 6, pal.text, 1, Align::Left, int(lw - 48 - 140));
        const std::string sub = r.one >= 0 ? r.sub : (r.sub.empty() ? "" : r.sub + " · ") + std::to_string(all) + (all == 1 ? " track" : " tracks");
        g.text(sub, { css::Sm }, lx + 48, y + 26, pal.text3, 1, Align::Left, int(lw - 48 - 140));
        if (on && r.one < 0) {
            char b[32];
            snprintf(b, sizeof b, "%d of %d", on, all);
            g.textC(b, { css::Sm, Weight::SemiBold }, lx + lw - 16, y + (RH - 4) / 2, pal.accentInk, 1, Align::Right);
        }
    }
    if (!n) g.textC("Nothing here yet", { css::Md, Weight::SemiBold }, lx + lw / 2, ly + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}
