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
 * Left out from the PC: High rating (no ratings here), your own foobar2000 query, and Lossless / Hi-Res (the Switch's
 * output gains nothing from them). The sound-analysis recipe is Mood ("smart:mood:<mood>", mood.hpp: our own analysis):
 * a kind of its own in the sidebar, the New playlist sheet and every page, hidden from the recipes here.
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <strings.h>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"
#include "plicons.inc"

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
        // Mood playlists (group 2: not among the recipes; New playlist › Mood playlist… opens its form)
        { "mood", PLI_MOOD, "Mood playlist", "Your library sorted by how it sounds: its energy, its beat, its key", 2, "Mood",
          { { mood::Happy, "Happy" }, { mood::Party, "Party" }, { mood::Relaxed, "Relaxed" }, { mood::Sad, "Sad" }, { mood::Aggressive, "Aggressive" } },
          mood::Happy },
    };
    return r;
}

const char* const kGroups[] = { "By listening", "By tags", "New playlist" };

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
    if (!strcmp(r.id, "mood")) return mood::name(atoi(arg.c_str()));
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
    if (c.statsGen == stats.gen() && c.indexGen == indexGen && c.moodGen == moodGen_ && c.day == hour) return c.tracks;
    c.statsGen = stats.gen();
    c.indexGen = indexGen;
    c.moodGen = moodGen_;
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
        if (id == "mood") return size_t(t) < moodIdx_.best.size() && moodIdx_.best[size_t(t)] == n;
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
    if (id == "mood")   // the strongest first (ordered again as more of the library is analysed)
        std::stable_sort(c.tracks.begin(), c.tracks.end(), [&](int a, int b) { return moodIdx_.score[size_t(a)] > moodIdx_.score[size_t(b)]; });
    else if (id == "recent") by([](const PlayStat* s) { return s ? s->added : 0; }, true);
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
    // a Mood playlist: its mood's own icon (the user, 2026-10-30: "the five should have their own"), as each recipe has; the
    // masks are the kind's (New playlist, its form)
    if (id == "mood") {
        static const char* const icons[mood::Count] = { PLI_SMILE, PLI_PARTY, PLI_LEAF, PLI_RAIN, PLI_FLAME };
        const int m = atoi(arg.c_str());
        return m >= 0 && m < mood::Count ? icons[m] : PLI_MOOD;
    }
    return r ? r->icon : PLI_SMART;
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
    if (id == "mood") return std::string(mood::name(n)) + ": " + mood::desc(n);
    return "Smart playlist";
}

// ============================================================ the sheet

/** The sleep timer's choices: minutes, in the order Settings' control shows them (after Off). */
static const int kSleepMinutes[] = { 15, 30, 45, 60, 90 };

const char* App::playlistIcon(int ref) const {
    if (ref < 0 || ref >= int(store.playlists.size())) return ICON_HEART;   // Liked
    const FolderPlaylist& p = store.playlists[size_t(ref)];
    if (const char* chosen = p.icon.empty() ? nullptr : playlistIconSvg(p.icon)) return chosen;
    return p.smart() ? smartIcon(p.path) : p.folder() ? ICON_FOLDER : p.automix ? ICON_SPARKLE : ICON_LIST;   // an Automix playlist: the two stars, its chip's
}

const char* App::playlistKind(int ref) const {
    if (ref < 0 || ref >= int(store.playlists.size())) return "Playlist";   // Liked
    const FolderPlaylist& p = store.playlists[size_t(ref)];
    return p.own() ? (p.automix ? "Automix playlist" : "Playlist") : isMoodPlaylist(p) ? "Mood playlist" : p.smart() ? "Smart playlist" : "Folder playlist";
}

/** Mood playlists' sorting (mood.hpp), built again when the analysis or the library changed: at most every 5 s while the
 *  library is analysed (the playlists reorder as it goes), at once when it is done; only while a Mood playlist exists. */
void App::moodTick() {
    if (moodThread_.joinable()) {   // a sort under way: taken in once it is done
        if (!moodReady_) return;
        moodThread_.join();
        moodIdx_ = std::move(moodNext_);
        moodGen_++;
    }
    bool any = false;
    for (const auto& p : store.playlists) if (isMoodPlaylist(p)) { any = true; break; }
    if (!any) { moodTotal_ = moodDone_ = 0; return; }
    if (!levels.loaded() || (levels.gen() == moodLevelsGen_ && libEpoch == moodLibEpoch_)) return;
    const bool libNew = libEpoch != moodLibEpoch_;
    if (!libNew && time - moodBuiltAt_ < 5 && moodDone_ + 1 < moodTotal_) return;   // (the last one: at once)
    moodLevelsGen_ = levels.gen();
    moodLibEpoch_ = libEpoch;
    moodBuiltAt_ = time;
    moodIn_.assign(lib.tracks.size(), mixan::Mood{});   // (invalid: not measured yet)
    int done = 0;
    for (size_t i = 0; i < lib.tracks.size(); i++) {
        const Level* l = levels.find(lib.tracks[i].path);
        if (!l || !l->moodTried) continue;
        done++;
        moodIn_[i] = l->mix.mood;
    }
    moodDone_ = done;
    moodTotal_ = int(lib.tracks.size());
    moodReady_ = false;
    moodThread_ = std::thread([this] {
        cores::moveHere(cores::Files);
        std::vector<const mixan::Mood*> per(moodIn_.size(), nullptr);
        for (size_t i = 0; i < moodIn_.size(); i++)
            if (moodIn_[i].valid) per[i] = &moodIn_[i];
        moodNext_.build(per);
        moodReady_ = true;
    });
}

void App::renamePlaylist(int ref) {
    if (ref < 0 || ref >= int(store.playlists.size()) || store.playlists[size_t(ref)].folder()) return;
    std::string name = store.playlists[size_t(ref)].name;
    if (!keyboard("Rename the playlist", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    if (name.empty() || name == store.playlists[size_t(ref)].name) return;
    // Playlists are kept by name (lists.txt, the background player's lists): two of one name merged into one at the next
    // launch, the other left empty
    for (int i = 0; i < int(store.playlists.size()); i++)
        if (i != ref && store.playlists[size_t(i)].name == name) { toast("There is already a playlist called “" + name + "”"); return; }
    if (name == "Liked") { toast("“Liked” is the name of your liked tracks"); return; }
    renameJoins(store.playlists[size_t(ref)].name, name);
    store.playlists[size_t(ref)].name = name;
    store.save();
}

/** The playlist page's icon button: the icons straight away (B closes them). */
void App::openPlaylistIcons(int ref) {
    openPlaylistMenu(ref);
    if (sheet.page != Sheet::PlMenu) return;
    sheet.page = Sheet::PlIcon;
    sheet.fromList = true;
}

void App::openPlaylistMenu(int ref) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::PlMenu;
    sheet.recipe = ref;
    sheet.name = store.playlists[size_t(ref)].name;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::openSleep() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Sleep;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::openJobMenu(int slot, bool fromList) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();   // from the list: where it came from
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Job;
    sheet.recipe = slot;
    sheet.fromList = fromList;
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::openJobList() {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Jobs;
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::openNewList() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::NewList;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

// ============================================================ the colour picker

namespace {

Rgb hsvToRgb(float h, float s, float v) {
    h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f) / 60.f;
    const int i = int(h);
    const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
    case 0: return { v, t, p };
    case 1: return { q, v, p };
    case 2: return { p, v, t };
    case 3: return { p, q, v };
    case 4: return { t, p, v };
    default: return { v, p, q };
    }
}

/** h is left as it was for a grey, which has none: the bar does not jump to red when the square reaches its edge. */
void rgbToHsv(const Rgb& c, float& h, float& s, float& v) {
    const float mx = std::max({ c.r, c.g, c.b }), mn = std::min({ c.r, c.g, c.b }), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;
    if (mx == c.r) h = 60 * std::fmod((c.g - c.b) / d + 6, 6.f);
    else if (mx == c.g) h = 60 * ((c.b - c.r) / d + 2);
    else h = 60 * ((c.r - c.g) / d + 4);
}

int packRgb(const Rgb& c) { return int(std::lround(c.r * 255)) << 16 | int(std::lround(c.g * 255)) << 8 | int(std::lround(c.b * 255)); }
Rgb unpackRgb(int v) { return { ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f }; }

std::string hexOf(int v) {
    char b[16];
    snprintf(b, sizeof b, "#%06X", unsigned(v & 0xFFFFFF));
    return b;
}

}  // namespace

SDL_Texture* App::hueTexture() {
    if (hueTex) return hueTex;
    uint8_t px[96 * 4];
    for (int i = 0; i < 96; i++) {
        const Rgb c = hsvToRgb(i * 360.f / 95.f, 1, 1);
        px[i * 4] = uint8_t(std::lround(c.r * 255));
        px[i * 4 + 1] = uint8_t(std::lround(c.g * 255));
        px[i * 4 + 2] = uint8_t(std::lround(c.b * 255));
        px[i * 4 + 3] = 255;
    }
    hueTex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 96, 1);
    if (hueTex) {
        SDL_UpdateTexture(hueTex, nullptr, px, 96 * 4);
        SDL_SetTextureBlendMode(hueTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(hueTex, SDL_ScaleModeLinear);
    }
    return hueTex;
}

/**
 * A colour wheel: the hue round the circle (red at the top, clockwise), white in the middle, and an edge
 * one pixel soft at the size it is drawn (made at about twice that, 24 px, so it stays smooth when shrunk).
 */
SDL_Texture* App::wheelTexture() {
    if (wheelTex) return wheelTex;
    constexpr int N = 52;
    static uint8_t px[N * N * 4];
    const float c = (N - 1) / 2.f, rad = N / 2.f - 0.5f;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            const float dx = i - c, dy = j - c, d = std::sqrt(dx * dx + dy * dy);
            const float cover = std::clamp((rad - d) / 2.f + 0.5f, 0.f, 1.f);   // 2 texels here: 1 px drawn
            float hue = std::atan2(dx, -dy) * 57.29578f;
            if (hue < 0) hue += 360;
            const float t = std::clamp(d / (rad * 0.95f), 0.f, 1.f);
            const Rgb col = hsvToRgb(hue, t * t * (3 - 2 * t) * 0.9f + 0.1f * t, 1);
            uint8_t* p = px + (j * N + i) * 4;
            p[0] = uint8_t(std::lround(col.r * 255));
            p[1] = uint8_t(std::lround(col.g * 255));
            p[2] = uint8_t(std::lround(col.b * 255));
            p[3] = uint8_t(std::lround(cover * 255));
        }
    wheelTex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, N, N);
    if (wheelTex) {
        SDL_UpdateTexture(wheelTex, nullptr, px, N * 4);
        SDL_SetTextureBlendMode(wheelTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(wheelTex, SDL_ScaleModeLinear);
    }
    return wheelTex;
}

void App::openColourPicker(int* field, const char* what, const Rgb& start) {
    const Id back = ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Colour;
    sheet.returnFocus = back;
    pick = ColourPick{};
    pick.field = field;
    pick.before = *field;
    pick.what = what;
    rgbToHsv(start, pick.h, pick.s, pick.v);
    ui.focusDefault();
}

void App::setPick(float h, float s, float v) {
    pick.h = std::clamp(h, 0.f, 359.9f);
    pick.s = std::clamp(s, 0.f, 1.f);
    pick.v = std::clamp(v, 0.f, 1.f);
    if (!pick.field) return;
    *pick.field = packRgb(hsvToRgb(pick.h, pick.s, pick.v));
    applyTheme();
}

void App::closeColourPicker(bool keep) {
    if (pick.field && !keep) { *pick.field = pick.before; applyTheme(); }
    if (pick.field && keep) persistSoon(0.3f);
    pick.field = nullptr;
    closeSheet();
}

/**
 * The picker, on the track menu's card: a saturation × brightness square for the hue, the hue bar under it, and
 * beside them the colour before and now and its hex code (the console keyboard). The square and the bar are
 * picked up with A like a slider (the square moves on both axes) and dragged by touch.
 */
void App::drawColourSheet() {
    ui.region(Region::Modal);
    if (!pick.field) { closeSheet(); return; }
    // The square again whenever the hue has moved.
    if (!svTex) {
        svTex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 48, 48);
        if (svTex) {   // blended: the shape's soft edge is in the vertices' alpha, which an opaque texture ignores
            SDL_SetTextureBlendMode(svTex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(svTex, SDL_ScaleModeLinear);
        }
        svTexHue = -1;
    }
    if (svTex && std::fabs(svTexHue - pick.h) > 0.2f) {
        static uint8_t px[48 * 48 * 4];
        for (int j = 0; j < 48; j++)
            for (int i = 0; i < 48; i++) {
                const Rgb c = hsvToRgb(pick.h, i / 47.f, 1 - j / 47.f);
                uint8_t* p = px + (j * 48 + i) * 4;
                p[0] = uint8_t(std::lround(c.r * 255)); p[1] = uint8_t(std::lround(c.g * 255)); p[2] = uint8_t(std::lround(c.b * 255)); p[3] = 255;
            }
        SDL_UpdateTexture(svTex, nullptr, px, 48 * 4);
        svTexHue = pick.h;
    }
    const Rgb now = hsvToRgb(pick.h, pick.s, pick.v);

    // ---- the card, as the track menu's
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.55f);
    const float w = 620, x = (css::W - w) / 2;
    const float headH = 24 + 64 + 18, bodyH = 16 + 220 + 14 + 20 + 16, footH = 64, h = headH + bodyH + footH + 16;
    const float y = (css::H - h) / 2;
    g.glow(x + w / 2, y + h / 2 + 30, w * 0.75f, { 0, 0, 0 }, 0.5f);
    g.roundRect(x, y, w, h, 20, pal.surface2);
    g.roundRect(x, y, w, h, 20, pal.text, 0.03f);
    g.roundStroke(x, y, w, h, 20, 1, pal.text, 0.12f);

    // ---- header: the tile is the colour itself
    const float hx = x + 24, hy = y + 24;
    g.roundRect(hx, hy, 64, 64, 14, now);   // no border: a line over a soft edge reads as a double edge
    kicker("Custom colour", hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    g.text(pick.what, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, textW);
    g.text("Pick it on the square and the bar, or type its hex code", { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    hint(uid("cp.close"), "Close");
    if (iconButton(uid("cp.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeColourPicker(false); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- the square: saturation → right, brightness ↑
    const float sx = x + 24, sy = y + headH + 16, sw = 320, sh = 220;
    {
        const Id id = uid("cp.sv");
        ui.item(id, { sx, sy, sw, sh }, 12, Slider | Slider2D | DefaultFocus);
        float s = pick.s, v = pick.v;
        if (const float dx = ui.sliderDrag(id); dx >= 0) {
            s = (dx - sx) / sw;
            v = 1 - (ui.sliderDragY(id) - sy) / sh;
        }
        if (const int st = ui.sliderStep(id)) s += st * 0.02f;
        if (const int st = ui.sliderStepY(id)) v -= st * 0.02f;
        if (s != pick.s || v != pick.v) setPick(pick.h, s, v);
        if (svTex) g.imageStretched(svTex, sx, sy, sw, sh, 12);
        const float kx = sx + pick.s * sw, ky = sy + (1 - pick.v) * sh;
        if (ui.engaged(id)) g.circle(kx, ky, 13, pal.accent);
        g.circle(kx, ky, 10, { 1, 1, 1 });
        g.circle(kx, ky, 7, now);
    }
    // ---- the hue bar
    const float bx = sx, by = sy + sh + 14, bw = sw, bh = 20;
    {
        const Id id = uid("cp.hue");
        ui.item(id, { bx, by - 4, bw, bh + 8 }, 10, Slider);
        float hue = pick.h;
        if (const float dx = ui.sliderDrag(id); dx >= 0) hue = std::clamp((dx - bx - bh / 2) / (bw - bh), 0.f, 1.f) * 359.9f;
        if (const int st = ui.sliderStep(id)) hue = std::fmod(hue + st * 4 + 360.f, 360.f);
        if (hue != pick.h) setPick(hue, pick.s, pick.v);
        if (SDL_Texture* hue = hueTexture()) g.imageStretched(hue, bx, by, bw, bh, bh / 2);
        const float kx = bx + bh / 2 + pick.h / 359.9f * (bw - bh);   // the knob stays on the bar at both ends
        if (ui.engaged(id)) g.circle(kx, by + bh / 2, 14, pal.accent);
        g.circle(kx, by + bh / 2, 11, { 1, 1, 1 });
        g.circle(kx, by + bh / 2, 8, hsvToRgb(pick.h, 1, 1));
    }

    // ---- beside them: before and now, and the hex code
    const float rx = sx + sw + 24, rw = x + w - 24 - rx, cw = (rw - 8) / 2;
    {
        // Before: pressing it goes back to it (an "A", the cover's own colour, has nothing to go back to here)
        const Id id = uid("cp.before");
        const bool hadOwn = pick.before >= 0;
        if (hadOwn && ui.item(id, { rx, sy, cw, 72 }, 12)) {
            float hh = pick.h, ss, vv;
            rgbToHsv(unpackRgb(pick.before), hh, ss, vv);
            setPick(hh, ss, vv);
        }
        if (hadOwn) g.roundRect(rx, sy, cw, 72, 12, unpackRgb(pick.before));
        else {
            g.roundRect(rx, sy, cw, 72, 12, pal.text, 0.06f);
            g.textC("From the cover", { css::Xs, Weight::SemiBold }, rx + cw / 2, sy + 36, pal.text2, 1, Align::Center);
        }
        g.roundRect(rx + cw + 8, sy, cw, 72, 12, now);
        g.textC("Before", { css::Xs }, rx + cw / 2, sy + 72 + 14, pal.text3, 1, Align::Center);
        g.textC("Now", { css::Xs }, rx + cw + 8 + cw / 2, sy + 72 + 14, pal.text3, 1, Align::Center);
    }
    {
        // the hex code, like Settings' search field: the console keyboard
        const float fy = sy + 72 + 34;
        kicker("Hex", rx, fy + 6, pal.text3, 0.12f);
        const Id id = uid("cp.hex");
        const float ty = fy + 18;
        if (ui.item(id, { rx, ty, rw, 44 }, 10)) {
            std::string in;
            if (keyboard("Colour, as a hex code", hexOf(packRgb(now)), in)) {
                std::string t;
                for (char c : in) if (c != '#' && c != ' ') t += c;
                if (t.size() == 3) t = std::string{ t[0], t[0], t[1], t[1], t[2], t[2] };
                char* end = nullptr;
                const long v = t.size() == 6 ? strtol(t.c_str(), &end, 16) : -1;
                if (v < 0 || !end || *end) {
                    toast("“" + in + "” is not a colour: six hex digits, like #FF5A6A");
                } else {
                    float hh = pick.h, ss, vv;
                    rgbToHsv(unpackRgb(int(v)), hh, ss, vv);
                    setPick(hh, ss, vv);
                }
            }
        }
        const bool hot = ui.focused(id) && ui.showFocusRing();
        g.roundRect(rx, ty, rw, 44, 10, pal.text, hot ? 0.09f : 0.05f);
        g.roundStroke(rx, ty, rw, 44, 10, 1, pal.text, 0.12f);
        g.roundRect(rx + 12, ty + 12, 20, 20, 6, now);
        g.textNum(hexOf(packRgb(now)), { css::Base, Weight::SemiBold }, rx + 42, ty + 22 - g.lineHeight({ css::Base }) / 2.f, pal.text);
        g.textC("Edit", { css::Xs }, rx + rw - 12, ty + 22, pal.text3, 1, Align::Right);
    }

    // ---- footer: Cancel, Use
    const float fy = y + h - 16 - 48;
    g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
    bool c = false;
    const Font bf{ css::Sm, Weight::SemiBold };
    const float uw = g.measure("Use", bf) + 32 + 24, kw = g.measure("Cancel", bf) + 32;
    button(uid("cp.use"), ICON_CHECK, "Use", x + w - 24 - uw, fy + 7, true, &c);
    if (c) { closeColourPicker(true); return; }
    button(uid("cp.cancel"), nullptr, "Cancel", x + w - 24 - uw - 8 - kw, fy + 7, false, &c);
    if (c) closeColourPicker(false);
}

void App::openActionList(int event) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::ActList;
    sheet.recipe = std::clamp(event, 0, 3);
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::openListActions(const std::string& name) {
    const Id back = ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::ListActions;
    sheet.name = name;
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::openEqPresets() {
    const Id back = ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::EqPreset;
    sheet.returnFocus = back;
    // The one in use takes the focus: the rows are "Yours", your presets, Custom, "Gatefold's", Gatefold's twelve
    const int own = int(store.eqUser.size()), yours = own || store.eq.hasCustom ? 1 : 0, custom = store.eq.hasCustom ? 1 : 0;
    const int p = store.eq.preset;
    sheet.queuePos = p >= GFEQ_PRESETS ? yours + (p - GFEQ_PRESETS)
                   : p < 0          ? yours + own
                                    : yours + own + custom + yours + p;
    ui.focusDefault();
}

void App::drawSmartSheet() {
    if (sheet.page == Sheet::Colour) { drawColourSheet(); return; }
    ui.region(Region::Modal);
    const Sheet::Page page = sheet.page;
    const Recipe* rc = page == Sheet::SmartForm && sheet.recipe >= 0 && sheet.recipe < int(recipes().size()) ? &recipes()[size_t(sheet.recipe)] : nullptr;
    if (page == Sheet::SmartForm && !rc) { sheet.page = Sheet::Smart; return; }
    const std::string spec = rc ? std::string("smart:") + rc->id + ":" + sheet.arg : std::string();

    // ---- the rows, laid out first so the card can be sized to them
    using Row = SheetRow;
    std::vector<Row> rows;
    std::vector<int> matched;
    const bool plPage = page == Sheet::PlMenu || page == Sheet::PlIcon;
    if (plPage && (sheet.recipe < 0 || sheet.recipe >= int(store.playlists.size()) || store.playlists[size_t(sheet.recipe)].name != sheet.name)) {
        closeSheet();   // the playlist went (or moved) while the sheet was open
        return;
    }
    constexpr int IconCols = 8;
    constexpr float IconTile = 58, IconRowH = 68;
    const bool filePage = page == Sheet::FileMenu || page == Sheet::FileInfo || page == Sheet::FileLink;
    const bool relPage = page == Sheet::Release || page == Sheet::Model;
    if (filePage) {
        fileSheetRows(rows);
        if (sheet.page != page) return;   // the file went
    } else if (relPage) {
        if (page == Sheet::Model) modelSheetRows(rows);   // view_updates.cpp: Download, Use, Go back, then its notes
        else updateSheetRows(rows);   // view_updates.cpp: Install / Go back, then the changelog
        if (sheet.page != page) return;
    } else if (page == Sheet::Report) {
        reportRows(rows);   // view_support.cpp: the report's form
    } else if (page == Sheet::Crashes) {
        crashRows(rows);    // … Atmosphère's crash reports
    } else if (page == Sheet::Factory) {
        factoryRows(rows);   // view_updates.cpp: a switch for each part, then Erase what is ticked / Erase everything
    } else if (page == Sheet::PlMenu) {
        const int ref = sheet.recipe;
        // On the Playlists page: Move, as a queue row's (the D-pad moves it, A or B puts it down)
        if (route.view == View::Playlists && store.playlists.size() > 1)
            rows.push_back({ Row::Action, ICON_SORT_DOWN, "Move", "", "", [this, ref] {
                                 closeSheet();
                                 grab = Grab{};
                                 grab.kind = Grab::Playlists;
                                 grab.pos = ref;
                                 toast(GF_MOVE_WORDS);
                             } });
        rows.push_back({ Row::Action, playlistIcon(ref), "Icon…", "Shown in the sidebar, also when it is folded", "", [this] {
                             sheet.page = Sheet::PlIcon;
                             ui.focusDefault();
                         } });
        if (!store.playlists[size_t(ref)].folder())
            rows.push_back({ Row::Action, ICON_TAG, "Rename", "", "", [this, ref] {
                                 closeSheet();
                                 renamePlaylist(ref);
                             } });
    } else if (page == Sheet::PlIcon) {
        const int n = int(std::size(kPlaylistIcons));
        rows.push_back({ Row::Grid });
        rows.back().h = ((n + IconCols - 1) / IconCols) * IconRowH + 4;
    } else if (page == Sheet::NewList) {
        rows.push_back({ Row::Action, ICON_LIST, "A playlist of your own", "Add tracks to it from the track menu (" GF_MENU_KEY ")", "", [this] {
                             closeSheet();
                             newPlaylist(-1);
                         } });
        rows.push_back({ Row::Action, ICON_SPARKLE, "Automix playlist…", "Pick songs; Songate orders them for the best joins", "",
                         [this] { openAutomixPicker(); } });
        rows.push_back({ Row::Action, PLI_SMART, "Smart playlist…", "Fills itself from your library and keeps up to date", "", [this] {
                             sheet.page = Sheet::Smart;
                             ui.focusDefault();
                         } });
        rows.push_back({ Row::Action, PLI_MOOD, "Mood playlist…", "Your library sorted by how it sounds: Happy, Party, Relaxed…", "", [this] {
                             for (int i = 0; i < int(recipes().size()); i++)
                                 if (!strcmp(recipes()[size_t(i)].id, "mood")) {
                                     const Recipe& p = recipes()[size_t(i)];
                                     sheet.recipe = i;
                                     sheet.arg = std::to_string(p.def);
                                     sheet.name = defaultName(p, sheet.arg);
                                     sheet.nameTouched = false;
                                     sheet.page = Sheet::SmartForm;
                                     ui.focusDefault();
                                 }
                         } });
        rows.push_back({ Row::Action, ICON_DOWNLOAD, "Import a playlist file…", "An .m3u or .m3u8 file anywhere " GF_ON_CARD, "",
                         [this] { openPlaylistPicker(); } });
    } else if (page == Sheet::PlFile) {
        // One playlist file: play it as it is, or keep it as a playlist of your own
        auto name = [b = baseName(sheet.dir)] { return b.substr(0, b.find_last_of('.')); };
        rows.push_back({ Row::Action, ICON_PLAY, "Play", "In the file's order", "", [this, name] {
                             const std::vector<int> t = sheet.fileTracks;
                             if (t.empty()) { toast("None of its tracks are in your library"); return; }
                             closeSheet();
                             playTracks(t, 0, name());
                         } });
        rows.push_back({ Row::Action, ICON_SHUFFLE, "Shuffle", "", "", [this, name] {
                             const std::vector<int> t = sheet.fileTracks;
                             if (t.empty()) { toast("None of its tracks are in your library"); return; }
                             closeSheet();
                             playTracks(t, 0, name(), true);
                         } });
        rows.push_back({ Row::Action, ICON_PLUS, "Import as a playlist", "A playlist of your own, in the sidebar; the file is not changed", "",
                         [this, name] {
                             const std::vector<int> t = sheet.fileTracks;
                             const int missing = sheet.fileMissing;
                             if (t.empty()) { toast("None of its tracks are in your library"); return; }
                             const int p = store.newPlaylist(name());
                             for (int tr : t) store.playlists[size_t(p)].tracks.push_back(lib.tracks[size_t(tr)].path);
                             store.save();
                             closeSheet();
                             if (route.view == View::PickPlaylist) leavePlaylistPicker();   // B on the playlist goes back to where + was
                             navigate(View::List, p);
                             toast("Imported “" + store.playlists[size_t(p)].name + "”: " + std::to_string(t.size()) + " tracks" +
                                   (missing ? ", " + std::to_string(missing) + " not in your library" : std::string()));
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
    } else if (page == Sheet::Jobs) {
        // Everything running, as the stack shows it: each opens its own menu (or its Settings row, for the ones that cannot wait).
        for (int i = 0; i < JobSlots; i++) {
            if (jobCards[i].shown <= 0.01f) continue;
            const Job& j = jobCards[i].job;
            const std::string count = j.total > 0 ? jobCount(j) : "Working";
            const std::string pct = j.total > 0 ? std::to_string(j.done * 100 / std::max(1, j.total)) + "%" : "";
            rows.push_back({ Row::Action, j.stop == Stoppable::Levels ? ICON_WAVE : j.stop == Stoppable::None ? ICON_REFRESH : ICON_IMAGE, j.label,
                             count, pct, [this, i] {
                                 const Job& jj = jobCards[i].job;
                                 if (jj.stop != Stoppable::None) { openJobMenu(i, true); return; }
                                 const Job copy = jj;
                                 closeSheet();
                                 showSetting(copy);
                             } });
        }
        if (rows.empty()) { closeSheet(); return; }   // all done while the list was open
    } else if (page == Sheet::Job) {
        // A progress card's menu: the job as the card last showed it (it may finish while the menu is open).
        const Job& j = jobCards[std::clamp(sheet.recipe, 0, JobSlots - 1)].job;
        const char* still = j.stop == Stoppable::Covers ? "Covers for what is on screen are still made"
                          : j.stop == Stoppable::Thumbs ? "The track playing still gets its cover"
                                                        : "The track playing and the next ones are still measured";
        rows.push_back({ Row::Action, ICON_X, "Stop until I resume it", std::string(still) + "; Resume is in Settings", "", [this, s = j.stop] {
                             closeSheet();
                             setJobStopped(s, true);
                         } });
        rows.push_back({ Row::Action, ICON_GEAR, "Open in Settings", std::string(j.settingsCat == SetPlayback ? "Playback" : "Library") + " · " + (j.row ? j.row : ""), "",
                         [this, j] {
                             closeSheet();
                             showSetting(j);
                         } });
    } else if (page == Sheet::ActList) {
        // Every playlist; the one chosen plays (faded in) at that event
        const int ev = sheet.recipe;
        uint64_t& want = bgs.cfg.lists[ev];
        std::vector<std::pair<std::string, size_t>> lists = { { "Liked", store.liked.size() } };
        for (const auto& p : store.playlists) lists.push_back({ p.name, p.own() ? p.tracks.size() : size_t(0) });
        for (size_t i = 0; i < lists.size(); i++) {
            const std::string name = lists[i].first;
            const FolderPlaylist* pl = i ? &store.playlists[i - 1] : nullptr;
            const std::string detail = pl && pl->smart() ? "Smart" : pl && !pl->own() ? "Folder" : std::to_string(lists[i].second);
            rows.push_back({ Row::Choice, nullptr, name, "", detail, [this, ev, name] {
                                 bgs.cfg.lists[ev] = listHash(name);
                                 int* acts[4] = { &bgs.cfg.onOpen, &bgs.cfg.onHome, &bgs.cfg.onClose, &bgs.cfg.onReturn };
                                 *acts[ev] = 4;
                                 bg.setConfig(bgs.cfg);
                                 writeBgPlaylists();   // the player reads it at the event: it must have this one now
                                 closeSheet();
                             } });
            rows.back().h = 44;
            rows.back().on = want == listHash(name);
        }
    } else if (page == Sheet::ListActions) {
        // The four events, each on or off for this playlist; off gives the event its usual action back
        static const char* labels[4] = { "When a game opens", "On the HOME menu", "When a game closes", "Back to the game" };
        const uint64_t h = listHash(sheet.name);
        int* acts[4] = { &bgs.cfg.onOpen, &bgs.cfg.onHome, &bgs.cfg.onClose, &bgs.cfg.onReturn };
        for (int ev : { 0, 1, 3, 2 }) {   // in the order Settings shows them
            const bool on = *acts[ev] == 4 && bgs.cfg.lists[ev] == h;
            const std::string other = *acts[ev] == 4 && bgs.cfg.lists[ev] != h && !listName(bgs.cfg.lists[ev]).empty()
                                          ? "Now: " + listName(bgs.cfg.lists[ev]) : "";
            rows.push_back({ Row::Choice, nullptr, labels[ev], "", other, [this, ev, h, on] {
                                 int* a[4] = { &bgs.cfg.onOpen, &bgs.cfg.onHome, &bgs.cfg.onClose, &bgs.cfg.onReturn };
                                 if (on) {   // its usual action back: the first-run default (BgConfig)
                                     const BgConfig d;
                                     const int usual[4] = { d.onOpen, d.onHome, d.onClose, d.onReturn };
                                     *a[ev] = usual[ev];
                                 }
                                 else { *a[ev] = 4; bgs.cfg.lists[ev] = h; }
                                 bg.setConfig(bgs.cfg);
                                 writeBgPlaylists();
                             } });
            rows.back().h = 46;
            rows.back().on = on;
        }
    } else if (page == Sheet::EqPreset) {
        // Yours first (each the whole sound it keeps), and Custom (the curve last moved by hand); then Gatefold's,
        // which set the ten bands (bass, treble and the preamp stay).
        if (!store.eqUser.empty() || store.eq.hasCustom) { rows.push_back({ Row::Group, nullptr, "Yours" }); rows.back().h = 34; }
        for (int k = 0; k < int(store.eqUser.size()); k++) {
            const bool editedNow = store.eq.preset == GFEQ_PRESETS + k && store.eq.edited;   // A on it: back to it as saved
            rows.push_back({ Row::Choice, nullptr, store.eqUser[size_t(k)].name, "", editedNow ? "Edited" : "", [this, k] {
                                 GfbgEq e = store.eq;
                                 gfEqApplyUser(&e, &store.eqUser[size_t(k)], k);
                                 setEq(e);
                                 closeSheet();
                             } });
            rows.back().h = 44;
            rows.back().on = store.eq.preset == GFEQ_PRESETS + k;
        }
        if (store.eq.hasCustom) {
            rows.push_back({ Row::Choice, nullptr, "Custom", "", "Not saved", [this] {
                                 GfbgEq e = store.eq;
                                 gfEqApplyCustom(&e);
                                 setEq(e);
                                 closeSheet();
                             } });
            rows.back().h = 44;
            rows.back().on = store.eq.preset < 0;
        }
        if (rows.size()) { rows.push_back({ Row::Group, nullptr, "Songate's" }); rows.back().h = 34; }
        for (int i = 0; i < GFEQ_PRESETS; i++) {
            rows.push_back({ Row::Choice, nullptr, kGfEqPresets[i].name, "", "", [this, i] {
                                 GfbgEq e = store.eq;
                                 gfEqApplyPreset(&e, i);
                                 setEq(e);
                                 closeSheet();
                             } });
            rows.back().h = 44;
            rows.back().on = store.eq.preset == i;
        }
    } else if (page == Sheet::Sleep) {
        const bool on = sleepLeft() > 0;
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
    } else if (page == Sheet::SmartForm) {
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
    const bool ask = page == Sheet::Confirm;
    const bool footer = page == Sheet::SmartForm || page == Sheet::Report || ask;
    const float headH = 24 + 64 + 18, footH = footer ? 64 : 0, maxBody = css::H - 60 - headH - footH - 16;
    float bodyH = 8;
    for (const auto& r : rows) bodyH += r.h;
    // the question and its lines, as the track menu's Delete
    const std::vector<std::string> askLines = ask ? g.wrap(sheet.askText, { css::Sm }, int(w - 56), 14) : std::vector<std::string>();
    std::string warn = sheet.askWarn;
    for (char& c : warn) c = char(toupper((unsigned char)c));
    const std::vector<std::string> warnLines = ask && !warn.empty() ? g.wrap(warn, { css::Sm, Weight::Bold }, int(w - 56), 4) : std::vector<std::string>();
    const float askH = 40 + askLines.size() * 19 + 14 + (warnLines.empty() ? 0 : 12 + warnLines.size() * 19);
    if (ask) bodyH = askH;
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
    const bool sleep = page == Sheet::Sleep, job = page == Sheet::Job;
    const bool jobs = page == Sheet::Jobs;
    int running = 0;
    for (const JobCard& c : jobCards) running += c.shown > 0.01f;
    const double left = sleep ? sleepLeft() : 0;
    const Job& jb = jobCards[std::clamp(sheet.recipe, 0, JobSlots - 1)].job;
    const bool plFile = page == Sheet::PlFile, eqp = page == Sheet::EqPreset;
    const bool actl = page == Sheet::ActList, lacts = page == Sheet::ListActions;
    static const char* evNames[4] = { "When a game opens", "On the HOME menu", "When a game closes", "Back to the game" };
    std::string plName;
    if (plFile) { plName = baseName(sheet.dir); plName = plName.substr(0, plName.find_last_of('.')); }
    const FolderPlaylist* plp = plPage ? &store.playlists[size_t(sheet.recipe)] : nullptr;
    const char* fileIcon = nullptr;
    std::string fileKick, fileTitle, fileSub;
    if (filePage) fileSheetHeader(fileIcon, fileKick, fileTitle, fileSub);
    if (page == Sheet::Model) modelSheetHeader(fileIcon, fileKick, fileTitle, fileSub);
    else if (relPage) updateSheetHeader(fileIcon, fileKick, fileTitle, fileSub);
    const bool facPage = page == Sheet::Factory;
    if (facPage) {
        fileIcon = ICON_X;
        fileKick = "Factory reset";
        fileTitle = "Choose what to erase";
        fileSub = "Your music files and backups always stay";
    }
    const bool supPage = page == Sheet::Report || page == Sheet::Crashes;
    if (page == Sheet::Report) reportHeader(fileIcon, fileKick, fileTitle, fileSub);
    if (page == Sheet::Crashes) {
        fileIcon = ICON_BUG;
        fileKick = "Support";
        fileTitle = "Crash reports";
        fileSub = "Atmosphère's, newest first";
    }
    const bool ownHead = filePage || relPage || facPage || supPage;   // pages that say their own header
    if (filePage && sheet.preview) g.image(sheet.preview, hx, hy, 64, 64, 14);   // an image shows itself
    else if (ownHead) g.icon(fileIcon, hx + 32, hy + 32, 28, pal.accentInk);
    else
    g.icon(plp ? playlistIcon(sheet.recipe) : ask && sheet.askIcon ? sheet.askIcon : eqp ? ICON_EQ : actl || lacts ? ICON_GAME : rc ? rc->icon : plFile ? ICON_LIST : sleep ? ICON_MOON : jobs ? ICON_REFRESH : job ? (jb.stop == Stoppable::Levels ? ICON_WAVE : ICON_IMAGE) : page == Sheet::Smart ? PLI_SMART : ICON_LIST,
           hx + 32, hy + 32, 28, pal.accentInk);
    const std::string kick = ownHead ? fileKick : plp ? playlistKind(sheet.recipe) : ask ? sheet.askKicker : eqp ? "Equalizer" : actl ? "Background player" : lacts ? "Games" : rc ? kGroups[rc->group] : plFile ? "Playlist file" : sleep ? "Sleep timer" : job || jobs ? "In the background" : page == Sheet::Smart ? "New playlist" : "Playlists";
    const std::string title = ownHead ? fileTitle : plp ? plp->name : ask ? sheet.askTitle
                            : eqp ? "Preset"
                            : actl ? evNames[std::clamp(sheet.recipe, 0, 3)]
                            : lacts ? sheet.name
                            : rc ? rc->title
                            : sleep ? (left > 0 ? "Pauses in " + fmtTime(std::ceil(left)) : stopAfterCurrent ? "Stops after this track" : "Pause the music in…")
                            : plFile ? plName
                            : job ? jb.label
                            : jobs ? "Running now"
                            : page == Sheet::Smart ? "Smart playlist" : "New playlist";
    const std::string sub = ownHead ? fileSub : page == Sheet::PlIcon ? "Its icon in the sidebar: A picks one" : plp ? "More for this playlist" : ask ? sheet.askSub
                          : eqp ? "Yours keep the whole sound; Songate's set the ten bands only"
                          : actl ? "The playlist it plays, faded in"
                          : lacts ? "The game events that play it; off, they do what they usually do"
                          : rc ? rc->desc
                          : sleep ? "It fades out over its last 10 seconds, then pauses where you were"
                          : plFile ? std::to_string(sheet.fileTracks.size()) + (sheet.fileTracks.size() == 1 ? " track" : " tracks") +
                                         (sheet.fileMissing ? " · " + std::to_string(sheet.fileMissing) + " not in your library" : std::string())
                          : job ? std::to_string(jb.done) + " of " + std::to_string(jb.total) + " done"
                          : jobs ? std::to_string(running) + " jobs at once: pick one to stop it or see its setting"
                          : page == Sheet::Smart ? "Pick a recipe: it keeps itself up to date as you listen" : "Your own list, or one that fills itself";
    kicker(kick, hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    g.text(title, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, textW);
    // A job's count rises while the card is open: fixed-width digits, as the progress cards draw it, so nothing shifts
    if (job || jobs) g.textNum(sub, { css::Sm }, hx + 64 + 16, hy + 44, pal.text3);
    else g.text(sub, { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    hint(uid("sp.close"), "Close");
    if (iconButton(uid("sp.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeSheet(); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- body
    const float by = y + headH;
    ScrollArea& sa = area("sheet", int(page) * 100 + sheet.recipe);
    sa.view = { x, by, w, bodyH };
    ui.pushArea(&sa);
    g.clip(x, by, w, bodyH);
    float ry = by + 8 - sa.y;
    if (ask) {
        g.text(sheet.askQuestion, { css::Lg, Weight::Bold }, x + 24, ry + 8, pal.text, 1, Align::Left, int(w - 48));
        float ty = ry + 40;
        for (const auto& l : askLines) {
            g.text(l, { css::Sm }, x + 24, ty, pal.text3);
            ty += 19;
        }
        if (!warnLines.empty()) {
            ty += 12;
            for (const auto& l : warnLines) {
                g.text(l, { css::Sm, Weight::Bold }, x + 24, ty, pal.text);
                ty += 19;
            }
        }
        ry += askH - 8;
    }
    bool first = !footer;   // the form starts on Create: A makes it as shown, the D-pad reaches the rest
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        const Id id = uid("sp.row", int64_t(i) + 1000 * int(page));
        const bool visible = ry + r.h > by - 80 && ry < by + bodyH + 80;
        switch (r.kind) {
        case Row::Group:
            if (!r.label.empty()) {
                float kx = x + 24;
                if (r.icon) { g.icon(r.icon, kx + 8, ry + r.h - 12, 16, pal.accentInk); kx += 26; }   // a changelog's section
                kicker(r.label, kx, ry + r.h - 12, pal.text3, 0.08f);
            }
            break;
        case Row::Text: {
            // Wrapped lines after an icon (or a dot): a changelog's items. Focusable, so the D-pad scrolls through them.
            ui.item(id, { x + 12, ry, w - 24, r.h }, 10);
            if (!visible) break;
            const float lx = x + 24;
            if (r.icon) g.icon(r.icon, lx + 9, ry + 7 + 10, 16, r.on ? pal.accentInk : pal.text2);
            else g.circle(lx + 9, ry + 7 + 10, 2.5f, pal.text3);
            float ty = ry + 7;
            for (const auto& l : g.wrap(r.sub, { css::Sm }, int(w - 48 - 48), 12)) {
                g.text(l, { css::Sm }, lx + 32, ty, r.on ? pal.text : pal.text2);
                ty += 19;
            }
            break;
        }
        case Row::Grid: {
            // The playlist's icons: tiles like the Settings category tiles, the one in use in the accent and focused first
            const int ref = sheet.recipe;
            FolderPlaylist& p = store.playlists[size_t(ref)];
            const char* now = playlistIcon(ref);
            const float gap = (w - 48 - IconCols * IconTile) / (IconCols - 1);
            const int n = int(std::size(kPlaylistIcons));
            for (int k = 0; k < n; k++) {
                const float tx = x + 24 + (k % IconCols) * (IconTile + gap), ty = ry + 2 + (k / IconCols) * IconRowH;
                const Id tid = uid("sp.icon", k);
                const bool on = kPlaylistIcons[k].svg == now;
                const bool hit = ui.item(tid, { tx, ty, IconTile, IconTile }, 14, on ? DefaultFocus : 0);
                if (ty + IconTile > by - 4 && ty < by + bodyH + 4) {
                    const bool hot = ui.pressed(tid) || (ui.focused(tid) && ui.showFocusRing());
                    g.roundRect(tx, ty, IconTile, IconTile, 14, on ? pal.accent : pal.text, on ? 0.18f : hot ? 0.1f : 0.05f);
                    if (on) g.roundStroke(tx, ty, IconTile, IconTile, 14, 1.5f, pal.accent, 0.6f);
                    g.icon(kPlaylistIcons[k].svg, tx + IconTile / 2, ty + IconTile / 2, 24, on ? pal.accentInk : hot ? pal.text : pal.text2);
                }
                if (hit) {
                    // its kind's own icon is kept as "none chosen", so it follows the kind (a smart recipe's)
                    const char* def = p.smart() ? smartIcon(p.path) : p.folder() ? ICON_FOLDER : ICON_LIST;
                    p.icon = kPlaylistIcons[k].svg == def ? std::string() : kPlaylistIcons[k].key;
                    store.save();
                    closeSheet();
                    break;
                }
            }
            break;
        }
        case Row::Setting: {
            g.textC(r.label, { css::Base, Weight::SemiBold }, x + 24, ry + r.h / 2, pal.text2);
            if (r.pick) {   // its own options (Support's form)
                int pick = -1;
                segmented(uid("sp.seg", int(i)), x + w - 24, ry + r.h / 2, r.opts, r.cur, &pick);
                if (pick >= 0) r.pick(pick);
                break;
            }
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
            // Mood: the library is analysed for it (from its first Mood playlist on): the songs come as it goes
            const bool moodWait = rc->group == 2 && moodDone_ < int(lib.tracks.size());
            g.textC(moodWait && matched.empty() ? "Songs arrive as your library is analysed"
                    : matched.empty() ? (stats0 ? "Nothing yet: plays are counted from now on" : "Nothing matches yet")
                    : std::string(matched.size() == 1 ? "track matches" : "tracks match") + (moodWait ? " so far" : " now"),
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
        case Row::Toggle: {
            // A part to erase (the factory reset): the whole row switches it, as Settings' switch rows
            const bool hit = ui.item(id, { x + 12, ry, w - 24, r.h }, 10, first ? DefaultFocus : 0);
            first = false;
            if (visible) {
                const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                if (hot) g.roundRect(x + 12, ry, w - 24, r.h, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
                const int tw = int(w - 48 - 40 - 16);
                g.text(r.label, { css::Base, Weight::SemiBold }, x + 24, ry + r.h / 2 - 19, pal.text, 1, Align::Left, tw);
                g.text(r.sub, { css::Sm }, x + 24, ry + r.h / 2 + 2, pal.text3, 1, Align::Left, tw);
                const float sx = x + w - 24 - 40, sy = ry + r.h / 2;   // App::toggle's switch
                g.roundRect(sx, sy - 11, 40, 22, 11, r.on ? pal.accent : pal.text, r.on ? 1.f : 0.16f);
                g.circle(r.on ? sx + 29 : sx + 11, sy, 8, r.on ? pal.onAccent : pal.text);
            }
            if (hit && r.act) r.act();
            break;
        }
        case Row::Action:
        case Row::Name:
        case Row::Choice: {
            // A list (the jobs, the playlist files) come back to from one of its rows: that row takes the focus (sheet.queuePos), else the first.
            const bool def = (page == Sheet::Jobs || page == Sheet::EqPreset) && sheet.queuePos >= 0 ? int(i) == sheet.queuePos : first;
            // an action without anything to do only informs (Files' "Kept by Gatefold"): not focusable
            const bool focusable = r.kind != Row::Action || r.act;
            const bool hit = focusable && ui.item(id, { x + 12, ry, w - 24, r.h }, 10, def ? DefaultFocus : 0);
            if (focusable) first = false;
            if (!visible) { if (hit && r.act) r.act(); break; }
            const bool hot = focusable && (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing()));
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
                const bool danger = facPage && r.icon == ICON_X;   // the factory reset's Erase rows, in the danger red
                const Rgb ink = danger ? dangerInk() : pal.text;
                g.icon(r.icon, lx + 9, ry + r.h / 2, 18, danger ? (r.act ? ink : pal.text3) : pal.text2);
                lx += 18 + 14;
                const int dw = r.detail.empty() ? 0 : std::min(120, g.measure(r.detail, { css::Sm }));
                const int tw = int(x + w - 24 - lx - dw - 16);
                const Rgb labelInk = danger && !r.act ? pal.text3 : ink;   // nothing ticked yet: greyed
                if (r.sub.empty()) {   // one line: centred
                    g.textC(r.label, { css::Base, Weight::SemiBold }, lx, ry + r.h / 2, labelInk, 1, Align::Left, tw);
                } else {
                    g.text(r.label, { css::Base, Weight::SemiBold }, lx, ry + r.h / 2 - 19, labelInk, 1, Align::Left, tw);
                    if (page == Sheet::Jobs) g.textNum(r.sub, { css::Sm }, lx, ry + r.h / 2 + 2, pal.text3);   // counts that rise
                    else g.text(r.sub, { css::Sm }, lx, ry + r.h / 2 + 2, pal.text3, 1, Align::Left, tw);
                }
                if (dw && page == Sheet::Jobs)
                    g.textNum(r.detail, { css::Sm }, x + w - 24, ry + r.h / 2 - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
                else if (dw) g.textC(r.detail, { css::Sm }, x + w - 24, ry + r.h / 2, pal.text3, 1, Align::Right, 120);
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

    const float fy = y + h - 16 - 48;
    g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
    bool c = false;
    const Font bf{ css::Sm, Weight::SemiBold };
    if (ask) {
        // ---- footer: Cancel, then the red button. Cancel is where focus lands: a stray A press must not delete anything
        // A safe question (askSafe: installing, allowing) has its button in the accent; a second choice may sit beside it.
        const float dw = g.measure(sheet.askButton, bf) + 32 + 24, kw = g.measure("Cancel", bf) + 32;
        button(uid("sp.yes"), sheet.askYesIcon ? sheet.askYesIcon : ICON_X, sheet.askButton, x + w - 24 - dw, fy + 7, sheet.askSafe, &c, false,
               !sheet.askSafe, sheet.askNoCancel ? DefaultFocus : 0);
        if (c) {
            std::function<void()> yes = std::move(sheet.askYes);
            closeSheet();
            if (yes) yes();
            return;
        }
        float left = x + w - 24 - dw - 8;
        if (!sheet.askAlt.empty()) {
            const float aw = g.measure(sheet.askAlt, bf) + 32;
            button(uid("sp.alt"), nullptr, sheet.askAlt, left - aw, fy + 7, false, &c);
            if (c) {
                std::function<void()> alt = std::move(sheet.askAltYes);
                closeSheet();
                if (alt) alt();
                return;
            }
            left -= aw + 8;
        }
        if (!sheet.askAlt2.empty()) {
            const float aw = g.measure(sheet.askAlt2, bf) + 32;
            button(uid("sp.alt2"), nullptr, sheet.askAlt2, left - aw, fy + 7, false, &c);
            if (c) {
                std::function<void()> alt = std::move(sheet.askAlt2Yes);
                closeSheet();
                if (alt) alt();
                return;
            }
            left -= aw + 8;
        }
        if (sheet.askNoCancel) return;
        button(uid("sp.no"), nullptr, "Cancel", left - kw, fy + 7, false, &c, false, false, DefaultFocus);
        if (c) closeSheet();
        return;
    }

    if (page == Sheet::Report) { reportFooter(x, w, fy); return; }   // view_support.cpp: Send, Cancel

    // ---- footer: Cancel, Create
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
        toast(std::string(rc->group == 2 ? "Made the mood playlist “" : "Made the smart playlist “") + made + "”");
        return;
    }
    button(uid("sp.back"), nullptr, "Back", x + w - 24 - cw - 8 - kw, fy + 7, false, &c);
    if (c) { sheet.page = rc->group == 2 ? Sheet::NewList : Sheet::Smart; ui.focusDefault(); }   // (Mood's form: from New playlist)
}
