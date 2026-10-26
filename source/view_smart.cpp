/*
 * Gatefold NX · view_smart.cpp
 * Smart playlists, after Gatefold 1.4.2's smartpl.js: ready recipes instead of a query language,
 * each with one simple setting, the number of tracks that match right now, and a live playlist
 * that keeps itself up to date. The plays come from the statistics (stats.hpp), the rest from tags.
 *
 * A smart playlist is a FolderPlaylist whose path is "smart:<recipe>:<setting>". + in the sidebar
 * opens the sheet: your own playlist or a smart one, then the recipes (.sp-card), then the chosen
 * recipe (.sp-form): its setting, the count with five of the tracks, the name, and Create.
 *
 * Left out from the PC: High rating (no ratings here), your own foobar2000 query, the sound-analysis
 * recipes, and Lossless / Hi-Res (the Switch's output gains nothing from them).
 */
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <strings.h>

#include "app.hpp"
#include "icons.inc"

namespace {

struct Recipe {
    const char* id;
    const char* icon;
    const char* title;
    const char* desc;
    int group;                                         // 0 by listening, 1 by tags
    const char* label;                                 // the setting's label; nullptr: none
    std::vector<std::pair<int, std::string>> opts;     // a segmented setting
    int def = 0;
    bool genre = false;                                // the setting is a genre from the library
};

const std::vector<Recipe>& recipes() {
    static const std::vector<Recipe> r = {
        { "recent", ICON_INBOX, "Recently added", "New in the library, newest first", 0, "Added in the last",
          { { 7, "7 days" }, { 30, "30 days" }, { 90, "3 months" } }, 30 },
        { "top", ICON_HEART_F, "Most played", "What you play most, by number of plays", 0, "Played at least",
          { { 5, "5 times" }, { 10, "10 times" }, { 25, "25 times" } }, 10 },
        { "played", ICON_REFRESH, "Recently played", "What you listened to lately, latest first", 0, "Played in the last",
          { { 7, "7 days" }, { 30, "30 days" }, { 90, "3 months" } }, 7 },
        { "forgot", ICON_HISTORY, "Forgotten favourites", "Played many times, but not in six months", 0, nullptr, {}, 0 },
        { "never", ICON_UNHEARD, "Never played", "Everything in the library you have not heard yet", 0, nullptr, {}, 0 },
        { "genre", ICON_TAG, "Genre", "One genre from your library", 1, "Genre", {}, 0, true },
        { "decade", ICON_CALENDAR, "Decade", "Music of one decade, by release date", 1, "Decade",
          { { 1950, "50s" }, { 1960, "60s" }, { 1970, "70s" }, { 1980, "80s" }, { 1990, "90s" }, { 2000, "00s" }, { 2010, "10s" }, { 2020, "20s" } },
          1990 },
        { "long", ICON_HOURGLASS, "Long tracks", "Five minutes or more: suites, mixes, epics", 1, nullptr, {}, 0 },
    };
    return r;
}

const char* const kGroups[] = { "By listening", "By tags" };

const Recipe* recipeOf(const std::string& id) {
    for (const auto& r : recipes()) if (id == r.id) return &r;
    return nullptr;
}

/** "smart:<recipe>:<setting>" → recipe and setting. */
void parse(const std::string& spec, std::string& id, std::string& arg) {
    const size_t a = spec.find(':'), b = a == std::string::npos ? a : spec.find(':', a + 1);
    id = a == std::string::npos ? std::string() : spec.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    arg = b == std::string::npos ? std::string() : spec.substr(b + 1);
}

std::string decadeName(int d) { return d >= 2000 ? "The " + std::to_string(d) + "s" : "The " + std::to_string(d % 100) + "s"; }

/** The name a new playlist gets until you type one. */
std::string defaultName(const Recipe& r, const std::string& arg) {
    if (r.genre) return arg.empty() ? "Genre" : arg;
    if (!strcmp(r.id, "decade")) return decadeName(atoi(arg.c_str()));
    return r.title;
}

bool hasGenre(const std::string& genres, const std::string& want) {
    size_t at = 0;
    while (at <= genres.size()) {
        size_t end = genres.find(';', at);
        if (end == std::string::npos) end = genres.size();
        size_t s = at, e = end;
        while (s < e && genres[s] == ' ') s++;
        while (e > s && genres[e - 1] == ' ') e--;
        if (e - s == want.size() && strncasecmp(genres.c_str() + s, want.c_str(), want.size()) == 0) return true;
        at = end + 1;
    }
    return false;
}

constexpr int64_t Day = 86400;

}  // namespace

// ============================================================ the tracks

const std::vector<int>& App::smartTracks(const std::string& spec) const {
    SmartCache& c = smartCache_[spec];
    const int64_t now = int64_t(std::time(nullptr)), hour = now / 3600;   // "the last 7 days" moves on by itself
    if (c.statsGen == stats.gen() && c.indexGen == indexGen && c.day == hour) return c.tracks;
    c.statsGen = stats.gen();
    c.indexGen = indexGen;
    c.day = hour;
    c.tracks.clear();
    std::string id, arg;
    parse(spec, id, arg);
    const int n = atoi(arg.c_str());
    auto st = [&](int t) { return stats.find(lib.tracks[t].path); };
    auto keep = [&](int t) -> bool {
        const PlayStat* s = st(t);
        const Track& tr = lib.tracks[t];
        if (id == "recent") return s && s->added >= now - int64_t(n) * Day;
        if (id == "top") return s && s->plays >= n;
        if (id == "played") return s && s->last >= now - int64_t(n) * Day;
        if (id == "forgot") return s && s->plays >= 5 && s->last < now - 182 * Day;
        if (id == "never") return !s || s->plays == 0;
        if (id == "genre") return !arg.empty() && hasGenre(tr.genre, arg);
        if (id == "decade") { const int y = atoi(tr.date.substr(0, 4).c_str()); return y >= n && y < n + 10; }
        if (id == "long") return tr.duration >= 300;
        return false;
    };
    for (int i = 0; i < int(lib.tracks.size()); i++) if (keep(i)) c.tracks.push_back(i);

    // As an album would play: album artist, year, album, track number (foobar2000's ALBUM_ORDER in smartpl.js).
    auto albumLess = [&](int a, int b) {
        const Track &x = lib.tracks[a], &y = lib.tracks[b];
        const std::string& ax = x.albumArtist.empty() ? x.artist : x.albumArtist;
        const std::string& ay = y.albumArtist.empty() ? y.artist : y.albumArtist;
        if (int r = strcasecmp(ax.c_str(), ay.c_str())) return r < 0;
        if (x.date != y.date) return x.date < y.date;
        if (int r = strcasecmp(x.album.c_str(), y.album.c_str())) return r < 0;
        if (x.trackNo != y.trackNo) return x.trackNo < y.trackNo;
        return x.path < y.path;
    };
    auto by = [&](auto key, bool desc) {
        std::stable_sort(c.tracks.begin(), c.tracks.end(), [&](int a, int b) {
            const int64_t ka = key(st(a)), kb = key(st(b));
            if (ka != kb) return desc ? ka > kb : ka < kb;
            return albumLess(a, b);
        });
    };
    if (id == "recent") by([](const PlayStat* s) { return s ? s->added : 0; }, true);
    else if (id == "top") by([](const PlayStat* s) { return s ? int64_t(s->plays) : 0; }, true);
    else if (id == "played") by([](const PlayStat* s) { return s ? s->last : 0; }, true);
    else if (id == "forgot") by([](const PlayStat* s) { return s ? s->last : 0; }, false);
    else if (id == "decade") std::stable_sort(c.tracks.begin(), c.tracks.end(), [&](int a, int b) {
        const std::string &da = lib.tracks[a].date, &db = lib.tracks[b].date;
        if (da.substr(0, 4) != db.substr(0, 4)) return da < db;
        return albumLess(a, b);
    });
    else std::stable_sort(c.tracks.begin(), c.tracks.end(), albumLess);
    return c.tracks;
}

const char* App::smartIcon(const std::string& spec) const {
    std::string id, arg;
    parse(spec, id, arg);
    const Recipe* r = recipeOf(id);
    return r ? r->icon : ICON_SPARKLE;
}

std::string App::smartSummary(const std::string& spec) const {
    std::string id, arg;
    parse(spec, id, arg);
    const int n = atoi(arg.c_str());
    const std::string days = n == 90 ? "3 months" : std::to_string(n) + " days";
    if (id == "recent") return "Added in the last " + days;
    if (id == "top") return "Played " + std::to_string(n) + " times or more";
    if (id == "played") return "Played in the last " + days;
    if (id == "forgot") return "Played 5 times or more, not in six months";
    if (id == "never") return "Not played yet";
    if (id == "genre") return "Genre: " + arg;
    if (id == "decade") return "Released " + std::to_string(n) + "–" + std::to_string(n + 9);
    if (id == "long") return "Five minutes or longer";
    return "Smart playlist";
}

// ============================================================ the sheet

/** The sleep timer's choices: minutes, in the order Settings' control shows them (after Off). */
static const int kSleepMinutes[] = { 15, 30, 45, 60, 90 };

void App::openSleep() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Sleep;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::openNewList() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::NewList;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::drawSmartSheet() {
    ui.region(Region::Modal);
    const Sheet::Page page = sheet.page;
    const Recipe* rc = page == Sheet::SmartForm && sheet.recipe >= 0 && sheet.recipe < int(recipes().size()) ? &recipes()[size_t(sheet.recipe)] : nullptr;
    if (page == Sheet::SmartForm && !rc) { sheet.page = Sheet::Smart; return; }
    const std::string spec = rc ? std::string("smart:") + rc->id + ":" + sheet.arg : std::string();

    // ---- the rows, laid out first so the card can be sized to them
    struct Row {
        enum Kind { Action, Group, Setting, Count, Preview, Name, Choice } kind;
        const char* icon = nullptr;
        std::string label, sub, detail;
        std::function<void()> act;
        float h = 58;
        bool on = false;
    };
    std::vector<Row> rows;
    std::vector<int> matched;
    if (page == Sheet::NewList) {
        rows.push_back({ Row::Action, ICON_LIST, "A playlist of your own", "Add tracks to it from the track menu (+)", "", [this] {
                             closeSheet();
                             newPlaylist(-1);
                         } });
        rows.push_back({ Row::Action, ICON_SPARKLE, "Smart playlist…", "Fills itself from your library and keeps up to date", "", [this] {
                             sheet.page = Sheet::Smart;
                             ui.focusDefault();
                         } });
    } else if (page == Sheet::Smart) {
        for (int gr = 0; gr < 2; gr++) {
            rows.push_back({ Row::Group, nullptr, kGroups[gr] });
            rows.back().h = 34;
            for (int i = 0; i < int(recipes().size()); i++) {
                const Recipe& r = recipes()[size_t(i)];
                if (r.group != gr) continue;
                // The count for its usual setting (a genre has none until one is chosen)
                std::string count;
                if (!r.genre) {
                    const size_t n = smartTracks(std::string("smart:") + r.id + ":" + std::to_string(r.def)).size();
                    count = std::to_string(n) + (n == 1 ? " track" : " tracks");
                }
                rows.push_back({ Row::Action, r.icon, r.title, r.desc, count, [this, i] {
                                     const Recipe& p = recipes()[size_t(i)];
                                     sheet.recipe = i;
                                     sheet.arg = p.genre ? (genres.empty() ? std::string() : genres[0].name) : std::to_string(p.def);
                                     sheet.name = defaultName(p, sheet.arg);
                                     sheet.nameTouched = false;
                                     sheet.page = Sheet::SmartForm;
                                     ui.focusDefault();
                                 } });
            }
        }
    } else if (page == Sheet::Sleep) {
        const bool on = sleepLeft() > 0;
#ifdef GF_DEV
        // Dev builds only (the Makefile's DEV): a minute, for testing the end of the timer on the console.
        rows.push_back({ Row::Choice, nullptr, "1 minute (test)", "", "", [this] {
                             stopAfterCurrent = false;
                             setSleep(60.0, 9);
                             toast("The music pauses in 1 minute");
                             closeSheet();
                         } });
        rows.back().h = 46;
        rows.back().on = on && sleepChoice == 9;
#endif
        for (int k = 0; k < 5; k++) {
            rows.push_back({ Row::Choice, nullptr, std::to_string(kSleepMinutes[k]) + " minutes", "", "", [this, k] {
                                 stopAfterCurrent = false;
                                 setSleep(kSleepMinutes[k] * 60.0, k + 1);
                                 toast("The music pauses in " + std::to_string(kSleepMinutes[k]) + " minutes");
                                 closeSheet();
                             } });
            rows.back().h = 46;
            rows.back().on = on && sleepChoice == k + 1;
        }
        rows.push_back({ Row::Choice, nullptr, "End of this track", "", "", [this] {
                             setSleep(0, 0);
                             stopAfterCurrent = true;
                             toast("Stop after the current track");
                             closeSheet();
                         } });
        rows.back().h = 46;
        rows.back().on = !on && stopAfterCurrent;
        rows.push_back({ Row::Choice, nullptr, "Off", "", "", [this, on] {
                             setSleep(0, 0);
                             stopAfterCurrent = false;
                             if (on) toast("Sleep timer off");
                             closeSheet();
                         } });
        rows.back().h = 46;
        rows.back().on = !on && !stopAfterCurrent;
    } else {
        if (!rc->opts.empty()) { rows.push_back({ Row::Setting, nullptr, rc->label }); rows.back().h = 56; }
        matched = smartTracks(spec);
        rows.push_back({ Row::Count });
        rows.back().h = 50;
        for (size_t i = 0; i < std::min<size_t>(5, matched.size()); i++) {
            rows.push_back({ Row::Preview, nullptr, lib.tracks[size_t(matched[i])].title, lib.tracks[size_t(matched[i])].artist });
            rows.back().h = 26;
        }
        if (!matched.empty()) { rows.push_back({ Row::Group, nullptr, "" }); rows.back().h = 10; }
        rows.push_back({ Row::Name, nullptr, "Name", "", sheet.name, [this] {
                             std::string n = sheet.name;
                             if (keyboard("Name the playlist", n, n)) {
                                 while (!n.empty() && n.back() == ' ') n.pop_back();
                                 if (!n.empty()) { sheet.name = n; sheet.nameTouched = true; }
                             }
                         } });
        rows.back().h = 48;
        if (rc->genre) {
            rows.push_back({ Row::Group, nullptr, genres.empty() ? "No genres in the tags yet" : rc->label });
            rows.back().h = 38;
            for (const auto& gn : genres) {
                rows.push_back({ Row::Choice, nullptr, gn.name, "", std::to_string(gn.tracks.size()), [this, name = gn.name] {
                                     sheet.arg = name;
                                     if (!sheet.nameTouched) sheet.name = name;
                                 } });
                rows.back().h = 42;
                rows.back().on = sheet.arg == gn.name;
            }
        }
    }

    // ---- the card, as the track menu's (drawSheet)
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.55f);
    const float w = 600, x = (css::W - w) / 2;
    const bool footer = page == Sheet::SmartForm;
    const float headH = 24 + 64 + 18, footH = footer ? 64 : 0, maxBody = css::H - 60 - headH - footH - 16;
    float bodyH = 8;
    for (const auto& r : rows) bodyH += r.h;
    bodyH = std::min(bodyH + 8, maxBody);
    const float h = headH + bodyH + footH + 16, y = (css::H - h) / 2;
    g.glow(x + w / 2, y + h / 2 + 30, w * 0.75f, { 0, 0, 0 }, 0.5f);
    g.roundRect(x, y, w, h, 20, pal.surface2);
    g.roundRect(x, y, w, h, 20, pal.text, 0.03f);
    g.roundStroke(x, y, w, h, 20, 1, pal.text, 0.12f);

    // ---- header: the icon tile (.sp-ic), kicker, title, a line under it, close
    const float hx = x + 24, hy = y + 24;
    g.roundRect(hx, hy, 64, 64, 14, pal.accent, 0.16f);
    g.roundStroke(hx, hy, 64, 64, 14, 1, pal.accent, 0.3f);
    const bool sleep = page == Sheet::Sleep;
    const double left = sleep ? sleepLeft() : 0;
    g.icon(rc ? rc->icon : sleep ? ICON_MOON : page == Sheet::Smart ? ICON_SPARKLE : ICON_LIST, hx + 32, hy + 32, 28, pal.accentInk);
    const std::string kick = rc ? kGroups[rc->group] : sleep ? "Sleep timer" : page == Sheet::Smart ? "New playlist" : "Playlists";
    const std::string title = rc ? rc->title
                            : sleep ? (left > 0 ? "Pauses in " + fmtTime(std::ceil(left)) : stopAfterCurrent ? "Stops after this track" : "Pause the music in…")
                            : page == Sheet::Smart ? "Smart playlist" : "New playlist";
    const std::string sub = rc ? rc->desc
                          : sleep ? "It fades out over its last 10 seconds, then pauses where you were"
                          : page == Sheet::Smart ? "Pick a recipe: it keeps itself up to date as you listen" : "Your own list, or one that fills itself";
    kicker(kick, hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    g.text(title, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, textW);
    g.text(sub, { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    if (iconButton(uid("sp.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeSheet(); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- body
    const float by = y + headH;
    ScrollArea& sa = area("sheet", int(page) * 100 + sheet.recipe);
    sa.view = { x, by, w, bodyH };
    ui.pushArea(&sa);
    g.clip(x, by, w, bodyH);
    float ry = by + 8 - sa.y;
    bool first = !footer;   // the form starts on Create: A makes it as shown, the D-pad reaches the rest
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        const Id id = uid("sp.row", int64_t(i) + 1000 * int(page));
        const bool visible = ry + r.h > by - 80 && ry < by + bodyH + 80;
        switch (r.kind) {
        case Row::Group:
            if (!r.label.empty()) kicker(r.label, x + 24, ry + r.h - 12, pal.text3, 0.08f);
            break;
        case Row::Setting: {
            g.textC(r.label, { css::Base, Weight::SemiBold }, x + 24, ry + r.h / 2, pal.text2);
            std::vector<std::string> labels;
            int cur = 0;
            for (size_t k = 0; k < rc->opts.size(); k++) {
                labels.push_back(rc->opts[k].second);
                if (std::to_string(rc->opts[k].first) == sheet.arg) cur = int(k);
            }
            int pick = -1;
            segmented(uid("sp.seg"), x + w - 24, ry + r.h / 2, labels, cur, &pick);
            if (pick >= 0) {
                sheet.arg = std::to_string(rc->opts[size_t(pick)].first);
                if (!sheet.nameTouched) sheet.name = defaultName(*rc, sheet.arg);
            }
            break;
        }
        case Row::Count: {
            // .sp-count: the number, big, and what it means
            const std::string n = std::to_string(matched.size());
            const float nw = float(g.measure(n, { css::Xl, Weight::ExtraBold }));
            g.textC(n, { css::Xl, Weight::ExtraBold }, x + 24, ry + r.h / 2, matched.empty() ? pal.text3 : pal.accentInk);
            const bool stats0 = rc->group == 0 && matched.empty();
            g.textC(matched.empty() ? (stats0 ? "Nothing yet: plays are counted from now on" : "Nothing matches yet")
                                    : std::string(matched.size() == 1 ? "track matches" : "tracks match") + " now",
                    { css::Base }, x + 24 + nw + 10, ry + r.h / 2 + 1, pal.text2);
            break;
        }
        case Row::Preview:
            if (visible) {
                const int tw = int((w - 48 - 14) * 0.6f);
                g.textC(r.label, { css::Sm, Weight::SemiBold }, x + 38, ry + r.h / 2, pal.text, 1, Align::Left, tw);
                g.textC(r.sub, { css::Sm }, x + w - 24, ry + r.h / 2, pal.text3, 1, Align::Right, int(w - 48 - 14 - tw - 16));
                g.circle(x + 28, ry + r.h / 2, 2, pal.text3);
            }
            break;
        case Row::Action:
        case Row::Name:
        case Row::Choice: {
            const bool hit = ui.item(id, { x + 12, ry, w - 24, r.h }, 10, first ? DefaultFocus : 0);
            first = false;
            if (!visible) { if (hit && r.act) r.act(); break; }
            const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
            if (hot) g.roundRect(x + 12, ry, w - 24, r.h, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
            float lx = x + 24;
            if (r.kind == Row::Choice) {
                if (r.on) g.icon(ICON_CHECK, lx + 9, ry + r.h / 2, 18, pal.accentInk);
                lx += 18 + 14;
                g.textC(r.label, { css::Base, r.on ? Weight::SemiBold : Weight::Regular }, lx, ry + r.h / 2, r.on ? pal.accentInk : pal.text, 1,
                        Align::Left, int(w - 48 - 32 - 80));
                g.textC(r.detail, { css::Sm }, x + w - 24, ry + r.h / 2, pal.text3, 1, Align::Right, 80);
            } else if (r.kind == Row::Name) {
                g.textC(r.label, { css::Base, Weight::SemiBold }, lx, ry + r.h / 2, pal.text2);
                g.textC(r.detail, { css::Base }, x + w - 24, ry + r.h / 2, pal.text, 1, Align::Right, int(w - 48 - 150));
            } else {
                g.icon(r.icon, lx + 9, ry + r.h / 2, 18, pal.text2);
                lx += 18 + 14;
                const int dw = r.detail.empty() ? 0 : std::min(120, g.measure(r.detail, { css::Sm }));
                const int tw = int(x + w - 24 - lx - dw - 16);
                g.text(r.label, { css::Base, Weight::SemiBold }, lx, ry + r.h / 2 - 19, pal.text, 1, Align::Left, tw);
                g.text(r.sub, { css::Sm }, lx, ry + r.h / 2 + 2, pal.text3, 1, Align::Left, tw);
                if (dw) g.textC(r.detail, { css::Sm }, x + w - 24, ry + r.h / 2, pal.text3, 1, Align::Right, 120);
            }
            if (hit && r.act) r.act();
            break;
        }
        }
        if (sheet.page != page) break;   // the row moved to another page (or closed)
        ry += r.h;
    }
    sa.content = ry + sa.y - by + 8;
    g.unclip();
    ui.popArea();
    if (sheet.page != page || !footer) return;

    // ---- footer: Cancel, Create
    const float fy = y + h - 16 - 48;
    g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
    bool c = false;
    const Font bf{ css::Sm, Weight::SemiBold };
    const float cw = g.measure("Create", bf) + 32 + 24, kw = g.measure("Back", bf) + 32;
    button(uid("sp.create"), ICON_CHECK, "Create", x + w - 24 - cw, fy + 7, true, &c, false, false, DefaultFocus);
    if (c && rc->genre && sheet.arg.empty()) { toast("Choose a genre first"); c = false; }
    if (c) {
        const int p = store.newPlaylist(sheet.name.empty() ? defaultName(*rc, sheet.arg) : sheet.name);
        store.playlists[size_t(p)].path = spec;
        store.save();
        const std::string made = store.playlists[size_t(p)].name;
        closeSheet();
        navigate(View::List, p);
        toast("Made the smart playlist “" + made + "”");
        return;
    }
    button(uid("sp.back"), nullptr, "Back", x + w - 24 - cw - 8 - kw, fy + 7, false, &c);
    if (c) { sheet.page = Sheet::Smart; ui.focusDefault(); }
}
