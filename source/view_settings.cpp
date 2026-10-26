/*
 * Gatefold NX · view_settings.cpp
 * #view-settings as Gatefold 1.4 lays it out: a "GATEFOLD / Settings" header, a
 * "Find a setting" field and the categories down the left; on the right the
 * chosen category's header (icon tile + title) over rounded cards of rows.
 * Rows put the label (and a hint, wrapped, never cut) on the left and the
 * control on the right: segmented choices, toggles, sliders, buttons.
 */
#include <algorithm>
#include <cmath>
#include <functional>

#include "app.hpp"
#include "icons.inc"

#ifndef GF_VERSION
#define GF_VERSION "1.0.0"
#endif

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

struct Row {
    std::string label, value, hint;   // value: shown after the label, like Gatefold's "Saturation 100%"
    float ctlW = 0;                   // the control's width, so the text knows how much room it has
    std::function<void(float rx, float cy)> control;
    float minH = 56;
    // a full-width block instead of label + control (the theme picker, the About header)
    float blockH = 0;
    std::function<void(float x, float y, float w)> block;
};
struct Card { std::string kicker; std::vector<Row> rows; };
struct Section { const char* name; const char* icon; std::vector<Card> cards; };

}  // namespace

/** Gatefold's .seg: a pill holding the options, the chosen one raised. Right-aligned at rx; returns its width. */
float App::segmented(Id id, float rx, float cy, const std::vector<std::string>& opts, int cur, int* picked, unsigned disabled) {
    const Font f{ css::Sm, Weight::SemiBold };
    float w = 6;
    for (const auto& o : opts) w += g.measure(o, f) + 26;
    const float h = 36, x0 = rx - w;
    g.roundRect(x0, cy - h / 2, w, h, 12, pal.text, 0.05f);
    g.roundStroke(x0, cy - h / 2, w, h, 12, 1, pal.text, 0.09f);
    float x = x0 + 3;
    for (int i = 0; i < int(opts.size()); i++) {
        const float ow = g.measure(opts[i], f) + 26;
        const Id oid = id + 0x9e3779b97f4a7c15ull * uint64_t(i + 1);
        const bool off = disabled & (1u << i);   // greyed: drawn, but not focusable or pressable
        if (!off && ui.item(oid, { x, cy - h / 2 + 3, ow, h - 6 }, 9) && picked) *picked = i;
        const bool on = i == cur, hot = !off && (ui.pressed(oid) || (ui.focused(oid) && ui.showFocusRing()));
        if (on) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, 9, pal.text, 0.13f);
        else if (hot) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, 9, pal.text, 0.065f);
        g.textC(opts[i], f, x + ow / 2, cy, on ? pal.text : pal.text2, off ? 0.35f : 1.f, Align::Center);
        x += ow;
    }
    return w;
}

void App::openFindSetting() {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        toast("The keyboard is not available in applet mode");
        return;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, "Find a setting");
    swkbdConfigSetHeaderText(&kbd, "Settings");
    swkbdConfigSetInitialText(&kbd, settingsQuery.c_str());
    char out[128] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return;
    settingsQuery = out;   // an empty answer clears the search
    area("settings.pane").target = area("settings.pane").y = 0;
}

void App::drawSettings(float x, float y, float w, float h) {
    const Font labelF{ css::Md, Weight::SemiBold }, hintF{ css::Sm }, valueF{ css::Md };

    // ------------------------------------------------------------ the rows, by category
    auto toggleRow = [&](const std::string& label, const std::string& hint, const char* key, bool& v) {
        return Row{ label, "", hint, 40, [this, key, &v](float rx, float cy) {
                       if (toggle(uid(key), rx - 40, cy, v)) { v = !v; persistSoon(0.3f); }
                   } };
    };
    auto sliderRow = [&](const std::string& label, const std::string& value, const std::string& hint, const char* key, float& v,
                         std::function<void()> changed = nullptr) {
        return Row{ label, value, hint, 180, [this, key, &v, changed](float rx, float cy) {
                       float nv = v;
                       slider(uid(key), rx - 180, cy, 180, v, &nv);
                       if (nv != v) { v = nv; persistSoon(0.8f); if (changed) changed(); }
                   } };
    };
    auto segRow = [&](const std::string& label, const std::string& hint, const char* key, std::vector<std::string> opts, int cur,
                      std::function<void(int)> pick, unsigned disabled = 0) {
        float sw = 6;
        for (const auto& o : opts) sw += g.measure(o, { css::Sm, Weight::SemiBold }) + 26;
        return Row{ label, "", hint, sw, [this, key, opts, cur, pick, disabled](float rx, float cy) {
                       int p = -1;
                       segmented(uid(key), rx, cy, opts, cur, &p, disabled);
                       if (p >= 0 && p != cur) { pick(p); persistSoon(0.3f); }
                   } };
    };
    auto buttonRow = [&](const std::string& label, const std::string& hint, const char* key, const char* icon, const std::string& text,
                         std::function<void()> act, bool danger = false) {
        const float bw = g.measure(text, { css::Sm, Weight::SemiBold }) + 32 + (icon ? 24 : 0);
        return Row{ label, "", hint, bw, [this, key, icon, text, act, bw, danger](float rx, float cy) {
                       bool c = false;
                       button(uid(key), icon, text, rx - bw, cy - 17, false, &c, false, danger);
                       if (c) act();
                   } };
    };
    auto infoRow = [&](const std::string& label, const std::string& hint, const std::string& value = {}) {
        // Values are counts that change (tags read, covers ready): fixed-width digits, so they do not jitter.
        const float vw = value.empty() ? 0 : g.measureNum(value, valueF);
        return Row{ label, "", hint, vw, value.empty() ? std::function<void(float, float)>() : [this, value](float rx, float cy) {
                                                              g.textNum(value, { css::Md }, rx, cy - g.lineHeight({ css::Md }) / 2.f, pal.text2, 1,
                                                                        Align::Right);
                                                          } };
    };
    auto pct = [](float v) { return std::to_string(int(std::lround(v * 100))) + "%"; };

    std::vector<Section> sections;
    static float satShown, trShown;   // Saturation 0..100% and Colour transition 0..3 s, as the slider's 0..1
    satShown = store.saturation;   // 0..100%: past 100% the palette changes no further
    trShown = store.colourTransition / 3.f;

    // Appearance
    std::vector<Row> sidebarRows;
    {
        // Settings → Appearance → Sidebar: each tab can be hidden; Now playing and Settings always stay
        // in the sidebar's order; the bits keep 0.4's meaning (Store::hiddenTabs)
        static const std::pair<const char*, int> tabs[] = { { "Playlist", 0 }, { "Albums", 5 }, { "Artists", 1 }, { "Genres", 2 }, { "Folders", 4 }, { "Library", 3 } };
        for (const auto& [name, bit] : tabs)
            sidebarRows.push_back({ name, "", bit == 0 ? "Now playing and Settings always stay" : "", 40, [this, bit = bit](float rx, float cy) {
                                       const bool shown = !(store.hiddenTabs & (1u << bit));
                                       if (toggle(uid("set.tab", bit), rx - 40, cy, shown)) {
                                           store.hiddenTabs ^= 1u << bit;
                                           persistSoon(0.3f);
                                       }
                                   } });
    }
    {
        // Interface colours: Gatefold's cards in one row (From the cover, Dark, Light, Custom). Grey is left out at the
        // user's request, to keep the page shorter; a saved Grey reads as Dark (Store::load).
        Row themes;
        themes.blockH = 150;
        themes.block = [this](float bx, float by, float bw) {
            struct Opt { const char* name; int id; Palette p; };
            const Palette custom = store.theme == 4 ? target : customPalette();
            const Opt opts[4] = { { "From the cover", 0, coverPal }, { "Dark", 1, paletteFixed(FixedTheme::Dark) },
                                  { "Light", 3, paletteFixed(FixedTheme::Light) }, { "Custom", 4, custom } };
            const float gap = 12, cw = (bw - gap * 3) / 4, ch = 150;
            for (int i = 0; i < 4; i++) {
                const float cx = bx + i * (cw + gap), cy = by;
                const Id id = uid("set.theme", opts[i].id);
                if (ui.item(id, { cx, cy, cw, ch }, 14)) { store.theme = opts[i].id; applyTheme(); persistSoon(0.3f); }
                const bool on = store.theme == opts[i].id, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                g.roundRect(cx, cy, cw, ch, 14, pal.text, hot ? 0.08f : 0.04f);
                g.roundStroke(cx, cy, cw, ch, 14, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.09f);
                // .theme-card .pv: a tiny window in that palette — sidebar strip, two text lines, an accent pill
                const Palette& p = opts[i].p;
                const float px = cx + 10, py = cy + 10, pw = cw - 20, ph = ch - 50;
                g.roundRect(px, py, pw, ph, 9, p.bg);
                if (i == 0) {
                    g.glow(px + pw * 0.2f, py + ph * 0.3f, pw * 0.6f, p.blobs[0], 0.9f);
                    g.glow(px + pw * 0.85f, py + ph * 0.8f, pw * 0.5f, p.blobs[2], 0.9f);
                    g.glow(px + pw * 0.7f, py + ph * 0.1f, pw * 0.35f, p.accent, 0.45f);
                }
                g.roundRect(px, py, pw * 0.24f, ph, 9, p.surface, 0.85f);
                const float tx = px + pw * 0.34f;
                g.roundRect(tx, py + ph * 0.24f, pw * 0.48f, 7, 3.5f, p.text);
                g.roundRect(tx, py + ph * 0.42f, pw * 0.32f, 6, 3, p.text3);
                g.roundRect(tx, py + ph * 0.62f, pw * 0.24f, 14, 7, p.accent);
                g.roundStroke(px, py, pw, ph, 9, 1, p.dark ? Rgb{ 1, 1, 1 } : Rgb{ 0, 0, 0 }, 0.06f);
                float lx = cx + 14;
                if (on) { g.circle(lx + 4, cy + ch - 20, 4, pal.accent); lx += 14; }
                g.textC(opts[i].name, { css::Base, Weight::SemiBold }, lx, cy + ch - 20, on ? pal.text : pal.text2, 1, Align::Left,
                        int(cx + cw - 10 - lx));
            }
        };
        static const std::vector<std::string> brightNames = { "Dark", "Light" };
        const Row brightness = segRow("Brightness",
                                      store.theme == 0 ? "From the cover, on a dark or a light background"
                                                       : "For From the cover; Dark and Light keep their own",
                                      "set.bright", brightNames, store.brightness, [this](int i) { store.brightness = i; applyTheme(); });
        const Row saturation = sliderRow("Saturation", std::to_string(int(std::lround(store.saturation * 100))) + "%", "How strongly the cover tints the interface",
                                         "set.sat", satShown, [this] { store.saturation = std::round(satShown * 100) / 100; applyTheme(); });
        char trV[16];
        snprintf(trV, sizeof trV, "%.1f s", double(store.colourTransition));
        const Row transition = sliderRow("Colour transition", trV, "How long the colours take to follow a new cover", "set.tr", trShown,
                                         [this] { store.colourTransition = std::round(trShown * 3 * 10) / 10; });
        // Cover palette: the eight colours read from the cover playing (.swatches)
        Row swatches{ "Cover palette", "", coverPal.swatchCount ? "" : "Play something to see its colours", 8 * 34.f, [this](float rx, float cy) {
                         const int n = coverPal.swatchCount;
                         for (int i = 0; i < n; i++) {
                             const float sx = rx - (n - i) * 34;
                             g.roundRect(sx, cy - 15, 28, 30, 8, coverPal.swatches[i].c);
                             g.roundStroke(sx, cy - 15, 28, 30, 8, 1, pal.text, 0.12f);
                         }
                     } };
        // Custom: your background and two accents, each from the cover playing or a few of Gatefold's own
        std::vector<Row> customRows;
        if (store.theme == 4) {
            auto chooser = [this](const char* label, const char* key, int* field, bool allowAuto) {
                std::vector<int> choices;
                if (allowAuto) choices.push_back(-1);
                for (int i = 0; i < coverPal.swatchCount; i++) {
                    const Rgb& c = coverPal.swatches[i].c;
                    choices.push_back(int(std::lround(c.r * 255)) << 16 | int(std::lround(c.g * 255)) << 8 | int(std::lround(c.b * 255)));
                }
                for (int v : { 0x16181f, 0xf4f1ea, 0x8fb4ff, 0xff5a6a, 0x4cd6a0, 0xffc857 }) choices.push_back(v);
                const float cw = choices.size() * 30.f;
                return Row{ label, "", "", cw, [this, key, field, choices](float rx, float cy) {
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const float sx = rx - (int(choices.size()) - i) * 30;
                                   const Id id = uid(key, i);
                                   if (ui.item(id, { sx, cy - 13, 26, 26 }, 13)) { *field = choices[i]; applyTheme(); persistSoon(0.3f); }
                                   const int v = choices[i];
                                   if (v < 0) {
                                       g.circle(sx + 13, cy, 12, pal.text, 0.1f);
                                       g.textC("A", { css::Xs, Weight::Bold }, sx + 13, cy, pal.text2, 1, Align::Center);
                                   } else {
                                       g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                   }
                                   const bool on = *field == v, hot = ui.focused(id) && ui.showFocusRing();
                                   if (on || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                               }
                           } };
            };
            customRows.push_back(chooser("Background", "set.cbg", &store.customBg, true));
            customRows.push_back(chooser("Accent", "set.cacc", &store.customAccent, false));
            customRows.push_back(chooser("Second accent", "set.cacc2", &store.customAccent2, true));
            customRows.push_back(infoRow("Your colours", "Kept readable: text and an accent too close to the background move only as far as they must. "
                                                         "A uses the cover's own."));
        }
        std::vector<Card> cards = { { "Interface colours", { themes, brightness, saturation, transition, swatches } } };
        if (!customRows.empty()) cards.push_back({ "Custom", customRows });   // only while Custom is chosen
        cards.push_back({ "Layout",
                          { toggleRow("Collapse the sidebar", "Icons only, more room for everything else", "set.side", store.sidebarCollapsed),
                            toggleRow("Background motion", "The slowly drifting colours behind every view and the Canvas", "set.motion",
                                      store.motion) } });
        cards.push_back({ "Sidebar", sidebarRows });
        sections.push_back({ "Appearance", ICON_SPARKLE, cards });
    }

    // Playback
    {
        // The background player (sysmodule): music on the HOME menu and in games, and its pause-for-apps.
        std::vector<Row> bgRows;
        const bool on = player.remote();
        bgRows.push_back(Row{ "Keep playing outside Gatefold", "",
                              on ? "Connected. The music carries on on the HOME menu, in games and after Gatefold closes"
                              : store.background ? "Not installed: copy atmosphere/contents/4200000047464E58 to the SD card and restart"
                                                 : "Off: the music stops when you leave Gatefold",
                              40, [this](float rx, float cy) {
                                  if (toggle(uid("set.bg"), rx - 40, cy, store.background && player.remote())) setBackground(!player.remote());
                              } });
        if (on && bgs.cfgRead) {
            // Each game event gets an action, like sys-tune's focus options.
            static const std::vector<std::string> acts = { "Nothing", "Play", "Resume", "Pause" };
            auto actRow = [&](const std::string& label, const std::string& hint, const char* key, int BgConfig::*field) {
                return segRow(label, hint, key, acts, bgs.cfg.*field, [this, field](int i) { bgs.cfg.*field = i; bg.setConfig(bgs.cfg); });
            };
            // Fades from 0 to 5 s, on a 0..1 slider.
            auto fadeRow = [this](const std::string& label, const char* key, float BgConfig::*field) {
                char v[16];
                snprintf(v, sizeof v, "%.1f s", double(bgs.cfg.*field));
                return Row{ label, v, "", 180, [this, key, field](float rx, float cy) {
                               const float cur = bgs.cfg.*field / 5.f;
                               float nv = cur;
                               slider(uid(key), rx - 180, cy, 180, cur, &nv);
                               if (nv != cur) { bgs.cfg.*field = std::round(nv * 50) / 10; bg.setConfig(bgs.cfg); }
                           } };
            };
            bgRows.push_back(actRow("When a game opens", "When it starts. Play picks a random folder and shuffles it",
                                    "set.bgopen", &BgConfig::onOpen));
            bgRows.push_back(actRow("On the HOME menu", "While a game waits in the background", "set.bghome", &BgConfig::onHome));
            bgRows.push_back(actRow("Back to the game", "Going back from the HOME menu to a game that never closed", "set.bgreturn",
                                    &BgConfig::onReturn));
            bgRows.push_back(actRow("When a game closes", "", "set.bgclose", &BgConfig::onClose));
            bgRows.push_back(fadeRow("Fade out", "set.bgfout", &BgConfig::fadeOut));
            bgRows.push_back(fadeRow("Fade in", "set.bgfin", &BgConfig::fadeIn));
            // The overlay's covers: made for each queue that reaches the player, or for everything here.
            {
                if (time - bgs.coverCountAt > 1) {
                    bgs.coverCountAt = time;
                    bgs.coverCount = 0;
                    for (const auto& t : lib.tracks) bgs.coverCount += thumbs.has(t.path);
                }
                // The count goes beside the label (fixed-width digits); the hint stays the same sentence, so its
                // lines do not reflow as the count rises.
                int made = 0, left = 0;
                thumbs.progress(made, left);
                Row covers = buttonRow("Covers for the overlay",
                                       left > 0 ? "Being made in the background. Keep Gatefold open until it finishes; the card above Settings shows how far"
                                                : "Made for each queue sent to the player, or for the whole library here",
                                       "set.bgcovers", ICON_REFRESH, left > 0 ? "Working" : "Make all", [this] {
                                           for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
                                           toast("Making them in the background: the card above Settings shows how far it is");
                                       });
                covers.value = std::to_string(bgs.coverCount) + " of " + std::to_string(lib.tracks.size());
                bgRows.push_back(covers);
            }
            bgRows.push_back(infoRow("Controls outside Gatefold", "The Gatefold overlay in Ultrahand's menu (ZL + ZR + D-pad down unless you changed it)"));
        }
        static const std::vector<std::string> orders = { "Default", "Repeat list", "Repeat track", "Shuffle" };
        sections.push_back(
            { "Playback", ICON_PLAY,
              { { "", { segRow("Order", "Also the first button in the player bar", "set.order", orders, int(store.order),
                               [this](int i) { setOrder(Order(i)); }),
                        Row{ "Stop after the current track", "", "Then playback stops until you start it again", 40,
                             [this](float rx, float cy) {
                                 if (toggle(uid("set.sac"), rx - 40, cy, stopAfterCurrent)) stopAfterCurrent = !stopAfterCurrent;
                             } },
                        Row{ "Volume", pct(volumeShown), "", 180,
                             [this](float rx, float cy) {
                                 float v = volumeShown;
                                 slider(uid("set.vol"), rx - 180, cy, 180, volumeShown, &v);
                                 if (v != volumeShown) { volumeShown = v; player.setVolume(v); }
                             } } } },
                { "Startup", { toggleRow("Resume where I left off", "The queue comes back paused at the same place in the same track",
                                         "set.resume", store.resume) } },
                { "Background player", bgRows } } });
    }

    // Library
    {
        const int tagged = meta.taggedCount();
        const int done = lib.tracks.empty() ? 100 : tagged * 100 / int(lib.tracks.size());
        std::string roots;
        for (const auto& r : lib.roots) roots += (roots.empty() ? "" : ", ") + r;
        sections.push_back(
            { "Library", ICON_GRID,
              { { "", { buttonRow("Music folder", roots.empty() ? "Not chosen yet" : roots, "set.folder", ICON_FOLDER, "Change",
                                  [this] {
                                      const size_t s = store.musicRoot.find_last_of('/');
                                      navigate(View::PickFolder, -1,
                                               store.musicRoot.empty() || s <= 5 ? std::string("sdmc:/") : store.musicRoot.substr(0, s));
                                  }),
                        infoRow("Tracks", "Albums and artists come from the tags, however the folders are organised",
                                std::to_string(lib.tracks.size()) + " · " + std::to_string(lib.albums.size()) + " albums"),
                        infoRow("Tags read", "Kept on the SD card for next time", std::to_string(done) + "%"),
                        infoRow("Covers ready", "Small copies of every cover, so lists open with their covers. Made in the background "
                                                "after the tags, and for new music after a rescan",
                                std::to_string(std::min(meta.coversCached(), int(lib.tracks.size()))) + " of " + std::to_string(lib.tracks.size())),
                        buttonRow("Rescan", "After adding or removing music", "set.rescan", ICON_REFRESH, "Rescan",
                                  [this] { rescan(); toast("Library rescanned: " + std::to_string(lib.tracks.size()) + " tracks"); }),
                        buttonRow("Read all tags again", "Forgets the tag cache and reads every file", "set.forget", nullptr, "Forget",
                                  [this] { meta.stop(); meta.clearDiskCache(); rescan(); toast("Reading all tags again"); }) } } } });
    }

    // Lyrics
    sections.push_back({ "Lyrics", ICON_QUOTE,
                         { { "", { toggleRow("Lyrics in the Canvas", "Also the quote button in the Canvas", "set.cvlyr", store.canvasLyrics),
                                   infoRow("Where lyrics come from",
                                           "An .lrc file with the track's name next to it, or the LYRICS / USLT tag. Synced lines follow "
                                           "the music; tap one to jump there.") } } } });

    // Canvas
    {
        static const std::vector<std::string> layouts = { "Cover", "Lyrics", "Minimal" };
        // Idle: when the controls fade, and how long the progress line takes to grow into the spectrum.
        char idleV[16], vizV[16];
        snprintf(idleV, sizeof idleV, "%.1f s", double(store.canvasIdleAfter));
        snprintf(vizV, sizeof vizV, "%.1f s", double(store.canvasVizTime));
        const Row idleAfterRow{ "Controls fade after", idleV, "Without a button or a touch", 180, [this](float rx, float cy) {
                                   const float cur = (store.canvasIdleAfter - 2) / 13;   // 2..15 s
                                   float nv = cur;
                                   slider(uid("set.cvidle"), rx - 180, cy, 180, cur, &nv);
                                   if (nv != cur) { store.canvasIdleAfter = std::round((2 + nv * 13) * 2) / 2; persistSoon(0.8f); }
                               } };
        const Row vizTimeRow{ "Spectrum rise", vizV, "How long the progress line takes to become the spectrum, and back", 180,
                              [this](float rx, float cy) {
                                  const float cur = (store.canvasVizTime - 0.2f) / 2.8f;   // 0.2..3 s
                                  float nv = cur;
                                  slider(uid("set.cvviz"), rx - 180, cy, 180, cur, &nv);
                                  if (nv != cur) { store.canvasVizTime = std::round((0.2f + nv * 2.8f) * 10) / 10; persistSoon(0.8f); }
                              } };
        sections.push_back(
            { "Canvas", ICON_CANVAS,
              { { "", { segRow("Layout",
                               canvasLayoutsOff() == 0b011 ? "This track has no lyrics, so the Canvas shows Minimal until one that has them"
                                                           : "Also at the bottom of the Canvas",
                               "set.cvlayout", layouts, canvasLayoutShown(), [this](int i) { store.canvasLayout = i; }, canvasLayoutsOff()),
                        toggleRow("Minimal when there are no lyrics", "Your layout comes back with the next track that has lyrics",
                                  "set.cvauto", store.canvasAutoMinimal) } },
                { "Cover motion",
                  { toggleRow("Float", "The cover rises and settles slowly", "set.cvfloat", store.canvasFloat),
                    toggleRow("Slow zoom", "A gentle Ken Burns zoom across the cover", "set.cvzoom", store.canvasZoom) } },
                { "When idle",
                  { idleAfterRow, vizTimeRow } } } });
    }

    // Visualizer
    {
        static const std::vector<std::string> bands = { "32", "56", "80" };
        const int bandIdx = store.vizBands <= 40 ? 0 : store.vizBands <= 64 ? 1 : 2;
        static const std::vector<std::string> cvBands = { "32", "56", "80", "112" };
        const int cvBandIdx = store.canvasBars <= 40 ? 0 : store.canvasBars <= 64 ? 1 : store.canvasBars <= 96 ? 2 : 3;
        sections.push_back(
            { "Visualizer", ICON_WAVE,
              { { "Spectrum",
                  { toggleRow("Spectrum on Now playing", "The Canvas turns its progress line into the same bars when idle", "set.spec",
                              store.showSpectrum),
                    sliderRow("Sensitivity", pct(store.vizSensitivity), "Higher for quiet recordings, lower if the bars hit the top",
                              "set.vsens", store.vizSensitivity),
                    sliderRow("Smoothing", pct(store.vizSmoothing), "Higher is calmer, lower follows every hit", "set.vsmooth",
                              store.vizSmoothing),
                    segRow("Bars on Now playing", "", "set.vbands", bands, bandIdx, [this](int i) { store.vizBands = i == 0 ? 32 : i == 1 ? 56 : 80; }),
                    segRow("Bars in the Canvas", "The spectrum the Canvas grows when it is idle", "set.cvbands", cvBands, cvBandIdx,
                           [this](int i) { store.canvasBars = i == 0 ? 32 : i == 1 ? 56 : i == 2 ? 80 : 112; }),
                    toggleRow("Peak caps", "The thin marks that fall slowly above the bars", "set.vpeaks", store.vizPeaks) } },
                { "Cover",
                  { sliderRow("Bass pulse", store.coverPulse < 0.01f ? "Off" : pct(store.coverPulse),
                              "How much the cover breathes with the bass. 100% is Gatefold's, 0 turns it off", "set.vpulse",
                              store.coverPulse),
                    toggleRow("Pulse in the Canvas too", "The full-screen cover breathes with the same strength", "set.vcvpulse",
                              store.canvasPulse) } } } });
    }

    // Controls
    {
        std::vector<Row> keys;
        const std::pair<const char*, const char*> map[] = { { "A", "Select" },           { "B", "Back" },        { "X", "Play / pause" },
                                                            { "Y", "Search" },           { "L  R", "Previous / next track" },
                                                            { "ZL  ZR", "Seek 10 seconds" }, { "−", "Canvas (full screen)" },
                                                            { "+", "Track menu: tags, cover, queue, delete" },
                                                            { "HOME", "Leave Gatefold (close it from the HOME menu)" },
                                                            { "Right stick", "Scroll" } };
        for (auto& [k, l] : map) {
            const Font f{ css::Sm, Weight::SemiBold };
            const float kw = g.measure(k, f) + 18;
            Row r{ l, "", "", kw, [this, k = std::string(k), kw, f](float rx, float cy) {
                      g.roundRect(rx - kw, cy - 13, kw, 26, 7, pal.text, 0.065f);
                      g.roundStroke(rx - kw, cy - 13, kw, 26, 7, 1, pal.text, 0.15f);
                      g.textC(k, f, rx - kw / 2, cy, pal.text2, 1, Align::Center);
                  } };
            r.minH = 44;
            keys.push_back(r);
        }
        const Row speedRow = sliderRow("Pointer speed", pct(store.pointerSpeed * 2),
                                       "The left stick moves a pointer; A taps under it", "set.ptrspeed", store.pointerSpeed);
        const Row accelRow = toggleRow("Pointer acceleration", "Slow for small moves, fast when the stick is pushed all the way",
                                       "set.ptraccel", store.pointerAccel);
        sections.push_back({ "Controls", ICON_SLIDERS,
                             { { "Buttons", keys },
                               { "Pointer", { speedRow, accelRow } },
                               { "Touch", { infoRow("Touch works everywhere", "Tap, drag lists with momentum, drag the seek bar and the volume") } } } });
    }

    // Storage and reset
    sections.push_back(
        { "Storage and reset", ICON_FOLDER,
          { { "", { infoRow("Saved in", std::string(Store::Dir) + ": settings, playlists, Liked, where you left off and the tag cache. "
                                                                  "Saved as you change things, so closing from the HOME menu loses nothing."),
                    buttonRow("Reset settings", "Colours, visualizer, Canvas and playback options. Your music folder, playlists and Liked stay.",
                              "set.reset", ICON_REFRESH, "Reset",
                              [this] {
                                  Store fresh;
                                  fresh.musicRoot = store.musicRoot;
                                  fresh.playlists = store.playlists;
                                  fresh.liked = store.liked;
                                  store = fresh;
                                  volumeShown = store.volume;
                                  player.setVolume(volumeShown);
                                  applyTheme();
                                  persistNow();
                                  toast("Settings reset");
                              },
                              true) } } } });

    // About
    {
        Row head;
        head.blockH = 76;
        head.block = [this](float bx, float by, float) {
            const Rgb fold{ pal.accent.r * 0.62f, pal.accent.g * 0.62f, pal.accent.b * 0.62f };
            g.roundRect(bx, by + 8, 60, 60, 16, pal.accent, 0.12f);
            g.glow(bx + 30, by + 38, 30, pal.glow, 0.5f);
            g.icon(ICON_BRAND_G, bx + 30, by + 38, 34, pal.accent);
            g.icon(ICON_BRAND_FOLD, bx + 30, by + 38, 34, fold);
            g.text("Gatefold for Nintendo Switch", { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
            g.text("Version " GF_VERSION " · by AndryTheBeast & DEVIROYrus", { css::Base }, bx + 76, by + 46, pal.text2);
        };
        sections.push_back(
            { "About", ICON_INFO,
              { { "", { head } },
                { "Diagnostics",
                  { toggleRow("Frame times", "A graph of the last two seconds of frames: green is on time, red took longer than a "
                                             "sixtieth of a second, the white part is the app's own work",
                              "set.frametimes", store.frameTimes) } },
                { "Credits",
                  { infoRow("Gatefold", "The design, colours, icons and app icon. © Gatefold contributors, MIT licence"),
                    infoRow("Inter", "The typeface. © The Inter Project Authors, SIL Open Font Licence"),
                    infoRow("FFmpeg", "AAC and ALAC decoding. LGPL 2.1"),
                    infoRow("SDL 2", "Graphics, sound, fonts and images: SDL, SDL_mixer, SDL_ttf, SDL_image. zlib licence"),
                    infoRow("libFLAC, mpg123, Opus, Tremor", "FLAC, MP3, Opus and Ogg Vorbis decoding. BSD and LGPL licences"),
                    infoRow("libnx and devkitPro", "The Nintendo Switch homebrew toolchain. ISC licence"),
                    infoRow("Background player", "Helix AAC decoder (RealNetworks RPSL), minimp3 (CC0), dr_flac (public domain), and "
                                                 "the overlay is built on sys-tune's, with libultrahand (GPL 2)") } } } });
    }
    settingsCat = std::clamp(settingsCat, 0, int(sections.size()) - 1);

    // ------------------------------------------------------------ left column
    const float lx = x + 24, lw = 214;
    float cy = y + 24;
    kicker("Gatefold", lx, cy + 6, pal.text3, 0.14f);
    cy += 16;
    g.text("Settings", { css::Xxl, Weight::ExtraBold }, lx - 1, cy, pal.text);
    cy += 50;
    // .st-search: opens the console keyboard; what it finds replaces the category on the right
    {
        const Id sid = uid("set.find");
        if (ui.item(sid, { lx, cy, lw, 38 }, 10)) openFindSetting();
        const bool hot = ui.focused(sid) && ui.showFocusRing();
        g.roundRect(lx, cy, lw, 38, 10, pal.text, hot ? 0.09f : 0.05f);
        g.roundStroke(lx, cy, lw, 38, 10, 1, pal.text, 0.12f);
        g.icon(ICON_SEARCH, lx + 20, cy + 19, 16, pal.text3);
        g.textC(settingsQuery.empty() ? "Find a setting" : settingsQuery, { css::Base }, lx + 38, cy + 19, settingsQuery.empty() ? pal.text3 : pal.text,
                1, Align::Left, int(lw - 50));
        cy += 38 + 12;
    }
    for (int i = 0; i < int(sections.size()); i++) {
        const Id id = uid("set.cat", i);
        const bool on = settingsQuery.empty() && settingsCat == i;
        if (ui.item(id, { lx, cy, lw, 38 }, 10, on ? DefaultFocus : 0)) {
            settingsCat = i;
            settingsQuery.clear();
            area("settings.pane").target = area("settings.pane").y = 0;
        }
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(lx, cy, lw, 38, 10, pal.accent, 0.14f);
        else if (hot) g.roundRect(lx, cy, lw, 38, 10, pal.text, 0.065f);
        g.icon(sections[i].icon, lx + 12 + 9, cy + 19, 18, on ? pal.accentInk : pal.text2);
        g.textC(sections[i].name, { css::Base, Weight::SemiBold }, lx + 12 + 18 + 12, cy + 19, on ? pal.accentInk : hot ? pal.text : pal.text2, 1,
                Align::Left, int(lw - 50));
        cy += 40;
    }

    // ------------------------------------------------------------ right pane
    const float px = lx + lw + 28, pw = x + w - 24 - px;
    settingsPaneX = px - 8;
    ScrollArea& sa = area("settings.pane");
    sa.view = { px - 8, y, x + w - (px - 8), h };
    ui.pushArea(&sa);
    g.clip(px - 8, y, x + w - (px - 8), h);
    float py = y + 22 - sa.y;

    // What to show: the category, or every row the search matches, grouped under its category.
    std::vector<Card> cards;
    std::string title = sections[settingsCat].name;
    const char* icon = sections[settingsCat].icon;
    if (!settingsQuery.empty()) {
        const std::string q = lowerAscii(settingsQuery);
        for (const auto& s : sections) {
            Card hit{ s.name, {} };
            for (const auto& c : s.cards)
                for (const auto& r : c.rows)
                    if (!r.block && lowerAscii(r.label + " " + r.hint + " " + c.kicker + " " + s.name).find(q) != std::string::npos)
                        hit.rows.push_back(r);
            if (!hit.rows.empty()) cards.push_back(hit);
        }
        title = "“" + settingsQuery + "”";
        icon = ICON_SEARCH;
    } else {
        cards = sections[settingsCat].cards;
    }

    // .st-head: a 44 px tile with the icon, the title beside it
    g.roundRect(px, py, 44, 44, 12, pal.accent, 0.16f);
    g.icon(icon, px + 22, py + 22, 22, pal.accentInk);
    g.textC(title, { 30, Weight::ExtraBold }, px + 44 + 16, py + 22, pal.text, 1, Align::Left, int(pw - 60));
    py += 44 + 20;
    if (cards.empty()) {
        g.text("Nothing matches. Press the search field to try other words.", { css::Base }, px, py + 8, pal.text3, 1, Align::Left, int(pw));
        py += 40;
    }

    // .st-group cards
    const float pad = 20, innerW = pw - pad * 2;
    for (const Card& c : cards) {
        struct Laid { const Row* r; std::vector<std::string> hint; float h, labelW; };
        std::vector<Laid> laid;
        float ch = c.kicker.empty() ? 6 : 42;
        for (const Row& r : c.rows) {
            Laid l{ &r, {}, 0, 0 };
            if (r.block) {
                l.h = r.blockH + 16;
            } else {
                l.labelW = innerW - (r.ctlW > 0 ? r.ctlW + 24 : 0);
                if (!r.hint.empty()) l.hint = g.wrap(r.hint, hintF, int(l.labelW), 4);
                const float textH = 21 + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                l.h = std::max(r.minH, textH + 28);
            }
            ch += l.h;
            laid.push_back(l);
        }
        ch += 6;
        // .st-group: surface at 55%, 1 px line, 18 px radius
        g.roundRect(px, py, pw, ch, 18, pal.surface, 0.55f);
        g.roundStroke(px, py, pw, ch, 18, 1, pal.text, 0.09f);
        float ry = py + (c.kicker.empty() ? 6 : 42);
        if (!c.kicker.empty()) kicker(c.kicker, px + pad, py + 26, pal.text3, 0.12f);
        for (size_t i = 0; i < laid.size(); i++) {
            const Laid& l = laid[i];
            const Row& r = *l.r;
            if (i > 0) g.rect(px + pad, ry, innerW, 1, pal.text, 0.09f);
            {   // every row registers, even scrolled out of view: the D-pad scrolls by moving focus onto it
                if (r.block) {
                    r.block(px + pad, ry + 8, innerW);
                } else {
                    const float textH = 21 + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                    float ty = ry + (l.h - textH) / 2;
                    const int lw2 = g.text(r.label, labelF, px + pad, ty, pal.text, 1, Align::Left, int(l.labelW));
                    if (!r.value.empty()) g.textNum(r.value, valueF, px + pad + lw2 + 7, ty, pal.text2);
                    ty += 24;
                    for (const auto& hl : l.hint) { g.text(hl, hintF, px + pad, ty, pal.text3); ty += 18; }
                    if (r.control) r.control(px + pw - pad, ry + l.h / 2);
                    else ui.item(uid("set.row", int64_t(std::hash<std::string>{}(r.label))), { px + 8, ry + 4, pw - 16, l.h - 8 }, 10);
                }
            }
            ry += l.h;
        }
        py += ch + 16;
    }
    sa.content = py + sa.y - y + 24;
    g.unclip();
    ui.popArea();
}
