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
#include <ctime>
#include <functional>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

#ifndef GF_VERSION
#define GF_VERSION "2.0.0 BETA"
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
    Id focus = 0;   // its control, for showSetting (buttons; other rows are only scrolled to and lit)
    bool lifted = false;   // picked up with Move (a sidebar tab): drawn as the queue's held row
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

std::string App::eqPresetName(int preset) const {
    if (preset >= 0 && preset < GFEQ_PRESETS) return kGfEqPresets[preset].name;
    if (preset >= GFEQ_PRESETS && preset - GFEQ_PRESETS < int(store.eqUser.size())) return store.eqUser[size_t(preset - GFEQ_PRESETS)].name;
    return "Custom";
}

void App::saveEqPreset(int into) {
    std::string name = "My preset " + std::to_string(store.eqUser.size() + 1);
    if (into >= int(store.eqUser.size())) into = -1;
    if (into >= 0) name = store.eqUser[size_t(into)].name;   // updated where it is, under its name
    else if (!keyboard("Name the preset", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    if (name.size() > 63) name.resize(63);
    GfEqUser u{};
    snprintf(u.name, sizeof u.name, "%s", name.c_str());
    u.preamp = store.eq.preamp;
    u.bass = store.eq.bass;
    u.treble = store.eq.treble;
    for (int b = 0; b < GFEQ_BANDS; b++) u.bands[b] = store.eq.bands[b];
    int k = into;
    if (k < 0)
        for (int i = 0; i < int(store.eqUser.size()); i++) if (name == store.eqUser[size_t(i)].name) k = i;   // the same name: replaced
    if (k < 0) {
        if (store.eqUser.size() >= GFEQ_USER_MAX) { toast("That is a hundred presets already: delete one first"); return; }
        store.eqUser.push_back(u);
        k = int(store.eqUser.size()) - 1;
    } else {
        store.eqUser[size_t(k)] = u;
    }
    store.saveEqPresets();
    GfbgEq e = store.eq;
    e.preset = int8_t(GFEQ_PRESETS + k);
    e.edited = 0;
    setEq(e);
    ui.focus(uid("set.eqpreset"));   // the Save row goes with the change: without this the focus fell back to Find a setting
    toast(into >= 0 ? "Updated the preset “" + name + "”" : "Saved the preset “" + name + "”");
}

void App::renameEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    std::string name = store.eqUser[size_t(k)].name;
    if (!keyboard("Rename the preset", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    snprintf(store.eqUser[size_t(k)].name, sizeof store.eqUser[size_t(k)].name, "%s", name.c_str());
    store.saveEqPresets();
}

void App::deleteEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    const std::string name = store.eqUser[size_t(k)].name;
    confirm(ICON_EQ, "Equalizer", name, "Your preset", "Delete this preset?", "It leaves the preset list. The sound playing now stays as it is, as Custom.",
            "Delete", [this, k, name] {
                if (k >= int(store.eqUser.size()) || name != store.eqUser[size_t(k)].name) return;
                store.eqUser.erase(store.eqUser.begin() + k);
                store.saveEqPresets();
                GfbgEq e = store.eq;
                if (e.preset == GFEQ_PRESETS + k) gfEqMakeCustom(&e);      // the one on: its sound stays, as Custom
                else if (e.preset > GFEQ_PRESETS + k) e.preset--;           // the ones after it move up
                setEq(e);
                ui.focus(uid("set.eqpreset"));   // its rows go with it
                toast("Deleted the preset “" + name + "”");
            });
}

/** The equalizer buttons' short press (player bar, Canvas). */
void App::toggleEq() {
    GfbgEq e = store.eq;
    e.on = !e.on;
    setEq(e);
    toast(e.on ? "Equalizer on: " + eqPresetName(e.preset) : std::string("Equalizer off"));
}

/** A long press on an equalizer button: Settings → Equalizer, its switch focused. */
void App::openEqualizer() {
    settingsCat = SetEqualizer;
    settingsQuery.clear();
    area("settings.pane").target = area("settings.pane").y = 0;
    navigate(View::Settings);
    settingsJump = "Equalizer";   // drawSettings focuses its switch and lights the row, as showSetting does
    settingsJumpAt = time;
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
    // A category chosen last frame: its page was drawn then, so its first setting is known now
    if (settingsFocusFirst) {
        settingsFocusFirst = false;
        if (const Id first = ui.firstIn(&area("settings.pane"))) ui.focus(first);
    }

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
                         std::function<void()> act, bool danger = false, float minW = 0) {
        const float bw = std::max(minW, float(g.measure(text, { css::Sm, Weight::SemiBold }) + 32 + (icon ? 24 : 0)));
        Row r{ label, "", hint, bw, [this, key, icon, text, act, bw, danger](float rx, float cy) {
                  bool c = false;
                  button(uid(key), icon, text, rx - bw, cy - 17, false, &c, false, danger, 0, bw);
                  if (c) act();
              } };
        r.focus = uid(key);
        return r;
    };
    // Two buttons on one row (the equalizer's Save / Save new): the first is the row's focus
    struct Btn { const char* key; const char* icon; std::string text; std::function<void()> act; bool danger = false; };
    auto buttonsRow = [&](const std::string& label, const std::string& hint, std::vector<Btn> btns) {
        float total = 0;
        std::vector<float> ws;
        for (const Btn& b : btns) {
            ws.push_back(g.measure(b.text, { css::Sm, Weight::SemiBold }) + 32 + (b.icon ? 24 : 0));
            total += ws.back() + (total > 0 ? 8 : 0);
        }
        Row r{ label, "", hint, total, [this, btns, ws, total](float rx, float cy) {
                  float bx = rx - total;
                  for (size_t i = 0; i < btns.size(); i++) {
                      bool c = false;
                      button(uid(btns[i].key), btns[i].icon, btns[i].text, bx, cy - 17, false, &c, false, btns[i].danger);
                      if (c) btns[i].act();
                      bx += ws[i] + 8;
                  }
              } };
        if (!btns.empty()) r.focus = uid(btns[0].key);
        return r;
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
    auto oneDecimal = [](double v) { char b[16]; snprintf(b, sizeof b, "%.1f", v); return std::string(b); };

    std::vector<Section> sections;
    static float satShown, trShown;   // Saturation 0..100% and Colour transition 0..3 s, as the slider's 0..1
    satShown = store.saturation;   // 0..100%: past 100% the palette changes no further
    trShown = store.colourTransition / 3.f;

    // Appearance
    std::vector<Row> sidebarRows;
    {
        // Settings → Appearance → Sidebar: the tabs in the sidebar's order (store.tabOrder), each shown or hidden with its
        // switch (Store::hiddenTabs; Now playing always shows) and put in order as a queue row is: Move picks it up, the
        // D-pad moves it, A or B puts it down (App::grab). The button keeps its id per tab, so the focus moves with it.
        const Font bf{ css::Sm, Weight::SemiBold };
        const float mw = g.measure("Move", bf) + 32 + 24;
        const float rw = 34;   // Remove: a round icon button
        int lines = 0;
        for (int i = 0; i < int(store.tabOrder.size()); i++) {
            const int tab = store.tabOrder[size_t(i)];
            const unsigned bit = Store::tabBit(tab);
            const bool held = grab.kind == Grab::Tabs && grab.pos == i;
            if (Store::isLine(tab)) {   // one of your lines: Move it, or Remove it
                lines++;
                // The same columns as the tabs' rows: Move where their Move is, the cross centred where their switch is
                Row r{ "Line", "", held ? "The D-pad moves it, A puts it down" : "A thin line in the sidebar between the tabs around it",
                       40 + 16 + mw, [this, tab, i, mw, rw](float rx, float cy) {
                           bool c = false;
                           {   // Remove: round, the red cross only (as the playlist page's compact Delete)
                               const float cx = rx - 20 - rw / 2;
                               const Id did = uid("set.tabdel", tab);
                               c = ui.item(did, { cx, cy - 17, 34, 34 }, 17);
                               const bool hot = ui.pressed(did) || (ui.focused(did) && ui.showFocusRing());
                               g.roundRect(cx, cy - 17, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
                               g.roundStroke(cx, cy - 17, 34, 34, 17, 1, pal.text, 0.15f);
                               g.icon(ICON_X, cx + 17, cy, 16, dangerInk());
                           }
                           if (c) {
                               if (i < int(store.tabOrder.size()) && store.tabOrder[size_t(i)] == tab) store.tabOrder.erase(store.tabOrder.begin() + i);
                               persistSoon(0.3f);
                               return;
                           }
                           button(uid("set.tabmove", tab), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                           if (c) {
                               grab = Grab{};
                               grab.kind = Grab::Tabs;
                               grab.pos = i;
                               toast("Move it with the D-pad, then press A");
                           }
                       } };
                r.lifted = held;
                r.minH = 48;
                sidebarRows.push_back(r);
                continue;
            }
            const char* hint = held                    ? "The D-pad moves it, A puts it down"
                             : tab == Store::TabNow    ? "Always shown. Settings stays at the bottom"
                             : tab == Store::TabFiles  ? "The whole SD card: open, rename, copy, move and delete, and what fits each file"
                                                       : "";
            Row r{ Store::tabName(tab), "", hint, 40 + 16 + mw, [this, tab, bit, i, mw](float rx, float cy) {
                      if (bit) {
                          const bool shown = !(store.hiddenTabs & bit);
                          if (toggle(uid("set.tab", int(bit)), rx - 40, cy, shown)) {
                              store.hiddenTabs ^= bit;
                              persistSoon(0.3f);
                          }
                      }
                      bool c = false;
                      button(uid("set.tabmove", tab), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                      if (c) {
                          grab = Grab{};
                          grab.kind = Grab::Tabs;
                          grab.pos = i;
                          toast("Move it with the D-pad, then press A");
                      }
                  } };
            r.lifted = held;
            sidebarRows.push_back(r);
        }
        // A new line goes in at the end, picked up, so it is placed straight away
        if (lines < Store::MaxLines) {
            Row add = buttonRow("Add a line", "Separates the tabs in groups, as you like (up to four)", "set.tabaddline", ICON_PLUS, "Add", [this] {
                int id = Store::TabLine;
                while (std::find(store.tabOrder.begin(), store.tabOrder.end(), id) != store.tabOrder.end()) id++;
                store.tabOrder.push_back(id);
                persistSoon(0.3f);
                grab = Grab{};
                grab.kind = Grab::Tabs;
                grab.pos = int(store.tabOrder.size()) - 1;
                toast("Move it with the D-pad, then press A");
            });
            sidebarRows.push_back(add);
        } else {
            sidebarRows.push_back(infoRow("Add a line", "Four lines at most: remove one to add another"));
        }
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
        // Custom has its own: Dark, Light, or the background exactly as picked
        static const std::vector<std::string> customBrightNames = { "Dark", "Light", "As picked" };
        const Row brightness =
            store.theme == 4
                ? segRow("Brightness", "Your colours on a dark or a light background, like a cover's; or your background, moved only as far as text needs to stay readable",
                         "set.cbright", customBrightNames, store.customMode, [this](int i) { store.customMode = i; applyTheme(); })
                : segRow("Brightness",
                         "From the cover, on a dark or a light background",
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
            // The last swatch opens the colour picker; it shows your own colour, ringed, when it is none of the others.
            auto chooser = [this](const char* label, const char* hint, const char* key, int* field, bool allowAuto, Rgb shown) {
                std::vector<int> choices;
                if (allowAuto) choices.push_back(-1);
                for (int i = 0; i < coverPal.swatchCount; i++) {
                    const Rgb& c = coverPal.swatches[i].c;
                    choices.push_back(int(std::lround(c.r * 255)) << 16 | int(std::lround(c.g * 255)) << 8 | int(std::lround(c.b * 255)));
                }
                for (int v : { 0x16181f, 0xf4f1ea, 0x8fb4ff, 0xff5a6a, 0x4cd6a0, 0xffc857 }) choices.push_back(v);
                const float cw = (choices.size() + 1) * 30.f;
                return Row{ label, "", hint, cw, [this, label, key, field, choices, shown](float rx, float cy) {
                               const int n = int(choices.size()) + 1;
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const float sx = rx - (n - i) * 30;
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
                               // the picker: a colour wheel, or your own colour in the middle of it
                               const float sx = rx - 30;
                               const Id id = uid(key, 99);
                               if (ui.item(id, { sx, cy - 13, 26, 26 }, 13))
                                   openColourPicker(field, label, *field >= 0 ? Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f } : shown);
                               const bool own = *field >= 0 && std::find(choices.begin(), choices.end(), *field) == choices.end();
                               if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - 12, 24, 24, 0);
                               if (own) {
                                   g.circle(sx + 13, cy, 8.5f, { 0, 0, 0 }, 0.35f);
                                   g.circle(sx + 13, cy, 7, Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f });
                               }
                               const bool hot = ui.focused(id) && ui.showFocusRing();
                               if (own || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                           } };
            };
            customRows.push_back(chooser("Background", "Behind every page and the bars", "set.cbg", &store.customBg, true, pal.bg));
            customRows.push_back(chooser("Accent", "Buttons, sliders and what is playing", "set.cacc", &store.customAccent, false, pal.accent));
            customRows.push_back(chooser("Second accent", "Where gradients start: the seek bar, the spectrum", "set.cacc2", &store.customAccent2,
                                         true, pal.accent2));
            customRows.push_back(infoRow("Your colours", "Kept readable: text and an accent too close to the background move only as far as they must. "
                                                         "A uses the cover's own; the colour wheel picks any colour."));
        }
        // Brightness only where it does something: From the cover and Custom (Dark and Light are their own)
        std::vector<Row> colourRows = { themes };
        if (store.theme == 0 || store.theme == 4) colourRows.push_back(brightness);
        for (const Row& r : { saturation, transition, swatches }) colourRows.push_back(r);
        std::vector<Card> cards = { { "Interface colours", colourRows } };
        if (!customRows.empty()) cards.push_back({ "Custom", customRows });   // only while Custom is chosen
        cards.push_back({ "Layout",
                          { toggleRow("Collapse the sidebar", "Icons only, more room for everything else", "set.side", store.sidebarCollapsed),
                            toggleRow("Background motion", "The slowly drifting colours behind every view and the Canvas", "set.motion",
                                      store.motion) } });
        // How an Automix join shows (the user, 2026-10-29), on Now playing and on the Canvas, each its own; where the hint
        // sits, only for Now playing's Simple (the user, 2026-10-29: every other animation has its own place)
        std::vector<Row> mixRows = { segRow("On Now playing",
                                   store.mixAnimNow == 0   ? "The interface steps aside and the cover glides to the middle, frosted with the hint"
                                   : store.mixAnimNow == 1 ? "The cover glides to the middle of the page; the sidebar and the player bar stay"
                                                           : "Everything stays in place while the page changes from one song to the next, left to right",
                                   "set.mixnow", { "Full", "Immersive", "Simple" }, store.mixAnimNow, [this](int i) { store.mixAnimNow = i; store.save(); }),
                            segRow("On the Canvas",
                                   store.mixAnimCanvas == 0 ? "The cover glides to the middle, frosted with the hint"
                                                            : "Everything stays in place while the page changes from one song to the next, left to right",
                                   "set.mixcanvas", { "Full", "Simple" }, store.mixAnimCanvas, [this](int i) { store.mixAnimCanvas = i; store.save(); }) };
        if (store.mixAnimNow == 2)
            mixRows.push_back(segRow("The hint on Now playing",
                                     store.mixHint == 1 ? "On the cover; on the Lyrics tab, in the lyrics' place"
                                                        : "Above the cover; on the Lyrics tab, in the lyrics' place",
                                     "set.mixhint", { "Automatic", "On the cover", "Above" }, store.mixHint, [this](int i) { store.mixHint = i; store.save(); }));
        cards.push_back({ "Automix", mixRows });
        cards.push_back({ "Sidebar", sidebarRows });
        cards.push_back({ "Title bar",
                          { toggleRow("Battery percentage", "Beside the battery, top right", "set.batpct", store.batteryPercent),
                            toggleRow("Battery time", "How long to empty, or to full while charging; shown after a minute, steadier as it goes",
                                      "set.batest", store.batteryEstimate) } });
        sections.push_back({ "Appearance", ICON_SPARKLE, cards });
    }

    // Playback
    {
        // The background player (sysmodule): music on the HOME menu and in games, and its pause-for-apps.
        std::vector<Row> bgRows;
        const bool on = player.remote();
        bgRows.push_back(Row{ "Keep playing outside Songate", "",
                              on ? "Connected. The music carries on on the HOME menu, in games and after Songate closes"
                              : store.background ? "Not installed: copy atmosphere/contents/4200000047464E58 to the SD card and restart"
                                                 : "Off: the music stops when you leave Songate",
                              40, [this](float rx, float cy) {
                                  if (toggle(uid("set.bg"), rx - 40, cy, store.background && player.remote())) setBackground(!player.remote());
                              } });
        if (bgShown()) {
            // Each game event gets an action, like sys-tune's focus options.
            // Playlist (GfbgAct_Playlist, 4): one of your playlists, chosen on the row under it.
            static const std::vector<std::string> acts = { "Nothing", "Play", "Resume", "Pause", "Playlist" };
            auto actRow = [&](const std::string& label, const std::string& hint, const char* key, int BgConfig::*field, int event) {
                bgRows.push_back(segRow(label, hint, key, acts, std::clamp(bgs.cfg.*field, 0, 4), [this, field, event](int i) {
                    bgs.cfg.*field = i;
                    bg.setConfig(bgs.cfg);
                    if (i == 4 && !bgs.cfg.lists[event]) openActionList(event);   // none yet: choose one now
                }));
                if (bgs.cfg.*field != 4) return;
                const std::string name = listName(bgs.cfg.lists[event]);
                static const char* keys[4] = { "set.bglist0", "set.bglist1", "set.bglist2", "set.bglist3" };   // ids that outlive the frame
                bgRows.push_back(buttonRow("Its playlist", name.empty() ? "None chosen yet: until then the music resumes" : name, keys[event],
                                           ICON_LIST, "Choose", [this, event] { openActionList(event); }));
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
            actRow("When a game opens", "When it starts. Play picks a random folder and shuffles it; Playlist plays one of yours",
                   "set.bgopen", &BgConfig::onOpen, 0);
            actRow("On the HOME menu", "While a game waits in the background", "set.bghome", &BgConfig::onHome, 1);
            actRow("Back to the game", "Going back from the HOME menu to a game that never closed", "set.bgreturn", &BgConfig::onReturn, 3);
            actRow("When a game closes", "", "set.bgclose", &BgConfig::onClose, 2);
            bgRows.push_back(segRow("Action playlists", bgs.cfg.listStart ? "Shuffled afresh each time a game event plays one"
                                                                          : "Each carries on at the track and time it was left at",
                                    "set.bglstart", { "Continue", "Shuffle" }, bgs.cfg.listStart ? 1 : 0, [this](int i) {
                                        bgs.cfg.listStart = i;
                                        bg.setConfig(bgs.cfg);
                                    }));
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
                // Stop / Resume while some are missing; Make all once every one is made (or none was asked for).
                const bool stopped = store.stopThumbs;
                Row covers = buttonRow("Covers for the overlay",
                                       stopped ? "Stopped. Only the track playing gets one until you resume"
                                       : left > 0 ? "Being made in the background. Keep Songate open until it finishes; the card above Settings shows how far"
                                                  : "Made for each queue sent to the player, or for the whole library here",
                                       "set.bgcovers", stopped ? ICON_PLAY : left > 0 ? ICON_X : ICON_REFRESH,
                                       stopped ? "Resume" : left > 0 ? "Stop" : "Make all", [this, stopped, left] {
                                           if (stopped) { setJobStopped(Stoppable::Thumbs, false); return; }
                                           if (left > 0) { setJobStopped(Stoppable::Thumbs, true); return; }
                                           for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
                                           toast("Making them in the background: the card above Settings shows how far it is");
                                       });
                covers.value = std::to_string(bgs.coverCount) + " of " + std::to_string(lib.tracks.size());
                bgRows.push_back(covers);
            }
            bgRows.push_back(infoRow("Controls outside Gatefold", "The Songate overlay in Ultrahand's menu (L + D-pad down + R3 unless you changed it)"));
        }
        // Measured: the count beside the label; Stop / Resume while the library is not all measured.
        const size_t measured = std::min(levels.count(), lib.tracks.size());
        const std::string measuredHint =
            store.levelMode == 0 ? "Starts once levelling is on, with the track playing first (Automix playlists' tracks: always)"
            : store.stopLevels       ? "Stopped. Only the track playing and the next ones are measured until you resume"
            : levels.secondsPerTrack() > 0 ? "In the background while Songate is open, about " + oneDecimal(levels.secondsPerTrack()) + " s a track"
                                     : "In the background while Songate is open, the track playing first";
        Row measuredRow = store.levelMode && (store.stopLevels || measured < lib.tracks.size())
                              ? buttonRow("Measured", measuredHint, "set.lvlstop", store.stopLevels ? ICON_PLAY : ICON_X,
                                          store.stopLevels ? "Resume" : "Stop",
                                          [this] { setJobStopped(Stoppable::Levels, !store.stopLevels); })
                              : infoRow("Measured", measuredHint);
        measuredRow.value = std::to_string(measured) + " of " + std::to_string(lib.tracks.size());
        static const std::vector<std::string> orders = { "Default", "Repeat list", "Repeat track", "Shuffle" };
        sections.push_back(
            { "Playback", ICON_PLAY,
              { { "", { segRow("Order", automixQueue() ? "Also the first button in the player bar. An Automix playlist plays in its own order: no Shuffle"
                                                    : "Also the first button in the player bar", "set.order", orders, int(store.order),
                               [this](int i) { setOrder(Order(i)); }, automixQueue() ? 1u << 3 : 0u),
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
                { "Volume levelling",
                  { segRow("Volume levelling", "Every track about as loud as the next. Measured here; the files are not changed",
                           "set.level", { "Off", "Track", "Album", "Auto" }, store.levelMode,
                           [this](int i) { store.levelMode = i; persistSoon(0.3f); }),
                    segRow("Target loudness", "Quiet tracks are raised only as far as their peaks allow", "set.lvltarget",
                           { "−18", "−14", "−11 LUFS" }, store.levelTarget <= -16 ? 0 : store.levelTarget <= -12 ? 1 : 2,
                           [this](int i) {
                               static const int t[] = { -18, -14, -11 };
                               store.levelTarget = t[i];
                               persistSoon(0.3f);
                           },
                           store.levelMode ? 0u : 0x7u),
                    measuredRow } },
                { "Crossfade",
                  { segRow("Crossfade", "The end of a track fades into the next one. A track changed by hand fades in 0.3 s; Stop after "
                                        "this track, Repeat track and a sleep timer's end never fade. Automix playlists mix theirs instead",

                           "set.xfade", { "Off", "2", "4", "6", "8", "12 s" },
                           store.crossfade <= 0 ? 0 : store.crossfade <= 2 ? 1 : store.crossfade <= 4 ? 2 : store.crossfade <= 6 ? 3
                                                    : store.crossfade <= 8 ? 4 : 5,
                           [this](int i) {
                               static const float secs[] = { 0, 2, 4, 6, 8, 12 };
                               store.crossfade = secs[i];
                           }),
                    toggleRow("Not within an album", "Tracks of one album that follow each other keep their join, as live and "
                                                     "gapless albums need. Automix playlists still mix them",
                              "set.xfadealbum", store.xfadeNotInAlbum) } },
                { "Startup", { toggleRow("Resume where I left off", "The queue comes back paused at the same place in the same track",
                                         "set.resume", store.resume) } },
                { "Sleep",
                  { segRow("Sleep timer", "The music fades out and pauses; also the moon in the player bar", "set.sleep",
                           { "Off", "15", "30", "45", "60", "90 min" }, sleepLeft() > 0 ? sleepChoice : 0,
                           [this](int i) {
                               static const int mins[] = { 0, 15, 30, 45, 60, 90 };
                               setSleep(mins[i] * 60.0, i);
                           }),
                    segRow("Then sleep the console", "When the timer ends with Songate open, a card counts down; Stay awake keeps it on",
                           "set.sleepcon", { "Off", "10 s", "30 s", "1 min" },
                           store.sleepConsole <= 0 ? 0 : store.sleepConsole <= 10 ? 1 : store.sleepConsole <= 30 ? 2 : 3,
                           [this](int i) {
                               static const int secs[] = { 0, 10, 30, 60 };
                               store.sleepConsole = secs[i];
                               persistSoon(0.3f);
                           }),
                    toggleRow("Keep the console awake", "While Songate is open the screen does not dim and the console does not sleep by "
                                                        "itself. After a sleep timer ends, it may again.",
                              "set.awake", store.keepAwake),
                    segRow("Dim the screen", store.keepAwake ? "After this long without input; any button brings it back"
                                                             : "With Keep the console awake on; otherwise the console dims by itself",
                           "set.dim", { "Off", "1", "2", "5", "10 min" },
                           store.dimAfter <= 0 ? 0 : store.dimAfter <= 1 ? 1 : store.dimAfter <= 2 ? 2 : store.dimAfter <= 5 ? 3 : 4,
                           [this](int i) {
                               static const int mins[] = { 0, 1, 2, 5, 10 };
                               store.dimAfter = mins[i];
                               persistSoon(0.3f);
                           },
                           store.keepAwake ? 0u : 0x1Fu) } },
                { "Background player", bgRows } } });
    }

    // Equalizer (gfeq.hpp: the same sound in Gatefold's own player and the background player)
    {
        auto dbText = [](float v) {
            char b[16];
            if (std::fabs(v) < 0.05f) return std::string("0 dB");
            snprintf(b, sizeof b, "%+.1f dB", double(v));
            return std::string(b);
        };
        // A ±12 dB value on a flat slider, in steps of 0.5 dB
        auto dbRow = [&](const std::string& label, const std::string& hint, const char* key, float GfbgEq::*field) {
            return Row{ label, dbText(store.eq.*field), hint, 180, [this, key, field](float rx, float cy) {
                           const float cur = (store.eq.*field + 12) / 24;
                           float nv = cur;
                           slider(uid(key), rx - 180, cy, 180, cur, &nv);
                           if (nv != cur) {
                               GfbgEq e = store.eq;
                               e.*field = std::round((nv * 24 - 12) * 2) / 2;
                               if (e.preset >= GFEQ_PRESETS) gfEqKeepCustom(&e);   // your preset keeps these too: now an edit of it
                               setEq(e);
                           }
                       } };
        };
        const bool adv = store.eq.advanced != 0;
        const int own = store.eq.preset - GFEQ_PRESETS;   // ≥ 0: one of your saved presets
        const bool edited = own >= 0 && store.eq.edited;  // ... changed since it was saved
        const std::string preset = eqPresetName(store.eq.preset) + (edited ? " (edited)" : "");
        Row onRow{ "Equalizer", "", "Shapes everything Songate and the background player play. A strong boost lowers the level a little so it "
                                   "does not distort: the preamp brings it back up. Also the equalizer button in the player bar and the Canvas: a press turns it on or off, a long press opens this page",
                   40, [this](float rx, float cy) {
                       if (toggle(uid("set.eqon"), rx - 40, cy, store.eq.on != 0)) {
                           GfbgEq e = store.eq;
                           e.on = !e.on;
                           setEq(e);
                       }
                   } };
        onRow.focus = uid("set.eqon");
        Row presetRow = buttonRow("Preset", edited      ? "Changed since you saved it: save the change into it, or as a new preset"
                                            : own >= 0  ? "Yours: the bands, bass, treble and the preamp as you saved them"
                                            : store.eq.preset >= 0 ? (store.eq.hasCustom ? "Sets the ten bands; Custom keeps your own curve"
                                                                                         : "Sets the ten bands")
                                                                   : "Your own curve, kept when you choose a preset", "set.eqpreset",
                                  nullptr, preset, [this] { openEqPresets(); });
        presetRow.value = "";
        // Under the preset: Save while the curve is your own, Rename and Delete while one of yours is on
        std::vector<Row> presetRows = { presetRow };
        const Row saveNew = buttonRow("Save as a new preset", "Keeps this sound, with bass, treble and the preamp, under a name in the preset list",
                                      "set.eqsave", ICON_PLUS, "Save new", [this] { saveEqPreset(); });
        if (store.eq.preset < 0) {
            presetRows.push_back(saveNew);
        } else if (own >= 0 && own < int(store.eqUser.size())) {
            if (edited) {   // your preset, changed: into it, or as a new one (one row, two buttons)
                presetRows.push_back(buttonsRow("Save the change", "Into “" + std::string(store.eqUser[size_t(own)].name) +
                                                                       "”, or as a new preset under its own name",
                                                { { "set.equpdate", ICON_CHECK, "Save", [this, own] { saveEqPreset(own); } },
                                                  { "set.eqsave", ICON_PLUS, "Save new", [this] { saveEqPreset(); } } }));
            }
            presetRows.push_back(buttonRow("Rename this preset", "", "set.eqrename", ICON_TAG, "Rename", [this, own] { renameEqPreset(own); }));
            presetRows.push_back(buttonRow("Delete this preset", "The sound stays as it is, as Custom", "set.eqdelete", ICON_X, "Delete",
                                           [this, own] { deleteEqPreset(own); }, true));
        }
        std::vector<Card> cards;
        cards.push_back({ "", { onRow,
                                segRow("Mode", adv ? "The ten bands and a preamp, as well as the preset, bass and treble"
                                                   : "A preset, bass and treble. Advanced adds the ten bands and a preamp",
                                       "set.eqmode", { "Simple", "Advanced" }, adv ? 1 : 0,
                                       [this](int i) {
                                           GfbgEq e = store.eq;
                                           e.advanced = uint8_t(i);
                                           setEq(e);
                                       }),
                                } });
        for (const Row& r : presetRows) cards.back().rows.push_back(r);
        cards.push_back({ "Tone", { dbRow("Bass", "Below about 100 Hz, under the muddy part, so a boost stays clean", "set.eqbass", &GfbgEq::bass),
                                    dbRow("Treble", "Above about 10 kHz: air and cymbals", "set.eqtreble", &GfbgEq::treble) } });
        if (adv) {
            // The ten bands, upright, with their value above and their frequency below
            Row bands;
            bands.blockH = 214;
            bands.block = [this](float bx, float by, float bw) {
                static const char* names[GFEQ_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
                const float col = bw / GFEQ_BANDS, top = by + 30, hh = 150;
                for (int i = 0; i < GFEQ_BANDS; i++) {
                    const float cx = bx + col * (i + 0.5f);
                    const Id id = uid("set.eqband", i);
                    const float cur = (store.eq.bands[i] + 12) / 24;
                    float nv = cur;
                    vslider(id, cx, top, hh, cur, &nv, i > 0 ? uid("set.eqband", i - 1) : 0, i + 1 < GFEQ_BANDS ? uid("set.eqband", i + 1) : 0);
                    const float v = store.eq.bands[i];
                    char b[8];
                    snprintf(b, sizeof b, std::fabs(v) < 0.05f ? "0" : "%+.0f", double(v));
                    g.textC(b, { css::Xs, Weight::SemiBold }, cx, by + 12, std::fabs(v) < 0.05f ? pal.text3 : pal.accentInk, 1, Align::Center);
                    g.textC(names[i], { css::Xs }, cx, top + hh + 18, pal.text3, 1, Align::Center);
                    if (nv != cur) {
                        GfbgEq e = store.eq;
                        e.bands[i] = std::round(nv * 24 - 12);
                        gfEqKeepCustom(&e);   // moved by hand: Custom, kept for the preset sheet
                        setEq(e);
                    }
                }
            };
            cards.push_back({ "Bands", { bands, dbRow("Preamp", "Everything louder or quieter, at any volume; past full scale the limiter holds the peaks", "set.eqpre",
                                                      &GfbgEq::preamp) } });
        }
        cards.push_back({ "", { buttonRow("Reset", "Flat, with bass, treble and the preamp at 0 dB", "set.eqreset", ICON_REFRESH, "Reset",
                                          [this] {
                                              GfbgEq e = store.eq;
                                              gfEqApplyPreset(&e, 0);
                                              e.bass = e.treble = e.preamp = 0;
                                              setEq(e);
                                              toast("Equalizer reset to Flat");
                                          }) } });
        sections.push_back({ "Equalizer", ICON_EQ, cards });
    }

    // Library
    {
        const int tagged = meta.taggedCount();
        const int done = lib.tracks.empty() ? 100 : tagged * 100 / int(lib.tracks.size());
        std::string roots;
        for (const auto& r : lib.roots) roots += (roots.empty() ? "" : ", ") + r;
        // Covers ready: the count beside the label; Stop / Resume while the cover cache is being filled.
        const int n = int(lib.tracks.size()), cached = std::min(meta.coversCached(), n);
        const bool sweeping = meta.sweepPos() > 0 && meta.sweepPos() < n && cached < n;
        const std::string coversHint = store.stopCovers ? "Stopped. Covers are still made for what is on screen; Resume caches the rest"
                                                        : "Small copies of every cover, so lists open with their covers. Made in the background "
                                                          "after the tags, and for new music after a rescan";
        Row coversRow = coversHeld && !store.stopCovers && cached < n   // held at launch, or Not now on the covers question
                            ? buttonRow("Covers ready", "Not made yet: covers are made for what is on screen; Make them caches the rest",
                                        "set.covstop", ICON_PLAY, "Make them", [this] { appCoverNotice = 2; releaseCovers(); })
                        : store.stopCovers || sweeping
                            ? buttonRow("Covers ready", coversHint, "set.covstop", store.stopCovers ? ICON_PLAY : ICON_X,
                                        store.stopCovers ? "Resume" : "Stop", [this] { setJobStopped(Stoppable::Covers, !store.stopCovers); })
                            : infoRow("Covers ready", coversHint);
        coversRow.value = std::to_string(cached) + " of " + std::to_string(n);
        sections.push_back(
            { "Library", ICON_GRID,
              { { "", { buttonRow("Music folder", roots.empty() ? "Not chosen yet" : roots, "set.folder", ICON_FOLDER, "Change",
                                  [this] {
                                      folderFor = -1;
                                      const size_t s = store.musicRoot.find_last_of('/');
                                      navigate(View::PickFolder, -1,
                                               store.musicRoot.empty() || s <= 5 ? std::string("sdmc:/") : store.musicRoot.substr(0, s));
                                  }),
                        infoRow("Tracks", "Albums and artists come from the tags, however the folders are organised",
                                std::to_string(lib.tracks.size()) + " · " + std::to_string(lib.albums.size()) + " albums"),
                        infoRow("Tags read", "Kept on the SD card for next time", std::to_string(done) + "%"),
                        coversRow,
                        buttonRow("Rescan", "After adding or removing music", "set.rescan", ICON_REFRESH, "Rescan",
                                  [this] { rescan(); toast("Library rescanned: " + std::to_string(lib.tracks.size()) + " tracks"); }),
                        buttonRow("Read all tags again", "Forgets the tag cache and reads every file", "set.forget", nullptr, "Forget",
                                  [this] { meta.stop(); meta.clearDiskCache(); rescan(); toast("Reading all tags again"); }) } } } });
    }

    // Lyrics
    sections.push_back({ "Lyrics", ICON_QUOTE,
                         { { "", { toggleRow("Lyrics in the Canvas", "Also the quote button in the Canvas", "set.cvlyr", store.canvasLyrics),
                                   toggleRow("Timing control", "Earlier · 0.0 s · Later above synced lyrics, on Now playing and in the Canvas: "
                                                               "each track keeps its own. Timings already set still apply when it is off",
                                             "set.lyrtime", store.lyricTiming),
                                   segRow("Show it for", "Where the lyrics come from", "set.lyrtimefor", { "Tags", ".lrc files", "Both" },
                                          store.lyricTimingFor, [this](int i) { store.lyricTimingFor = i; }, store.lyricTiming ? 0u : 0x7u),
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
                              "How much the cover breathes with the bass. 100% is the default, 0 turns it off", "set.vpulse",
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
                                                            { "HOME", "Leave Songate (close it from the HOME menu)" },
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

    // Storage and reset: where each browser opens (view_storage.cpp), what Gatefold keeps on the card, and the reset
    std::vector<Row> folderRows, keptRows;
    {
        struct F { const char* label; const char* dflt; std::string* field; };
        const F fs[5] = { { "Files opens in", "The top of the SD card", &store.filesDir },
                          { "Importing playlists opens in", "The top of the SD card; then, while Songate is open, where you last picked one",
                            &store.importDir },
                          { "Exporting playlists opens in", "The music folder's Playlists folder (or the music folder); then where you last exported",
                            &store.exportDir },
                          { "The cover picker opens in", "The track's own folder (Change the cover… in the track menu)", &store.coverDir },
                          { "Backups go to", "sdmc:/config/gatefold-nx-backups (Settings › Updates makes one before each install)", &store.backupDir } };
        static const char* chooseKeys[5] = { "set.dir0", "set.dir1", "set.dir2", "set.dir3", "set.dir4" };
        static const char* defaultKeys[5] = { "set.dirdef0", "set.dirdef1", "set.dirdef2", "set.dirdef3", "set.dirdef4" };
        for (int i = 0; i < 5; i++) {
            std::string* field = fs[i].field;
            std::vector<Btn> btns = { { chooseKeys[i], ICON_FOLDER, "Choose", [this, i] { chooseFolder(i); } } };
            if (!field->empty())
                btns.push_back({ defaultKeys[i], nullptr, "Default", [this, field] {
                                    field->clear();
                                    persistSoon(0.3f);
                                    toast("Back to its own default");
                                } });
            folderRows.push_back(buttonsRow(fs[i].label, field->empty() ? std::string(fs[i].dflt) : *field, btns));
        }
        struct K { const char* label; const char* hint; };
        const K ks[6] = { { "Covers cache", "Small copies of every cover, so lists open with their covers (config/gatefold-nx/art)" },
                          { "Covers for the overlay", "What the overlay shows for each track (thumbs)" },
                          { "Loudness measurements", "Volume levelling's, Automix's and the moods' measurements of each track (loudness.tsv, mix.tsv, mood.tsv, gains.bin)" },
                          { "Tag cache", "The tags read from your files, so launch does not read them again (tags.tsv)" },
                          { "Backups", "Your settings as they were before each install or restore (Backups go to, above)" },
                          { "Downloads", "Release zips kept from an install that did not finish (updates)" } };
        static const char* clearKeys[6] = { "set.clr0", "set.clr1", "set.clr2", "set.clr3", "set.clr4", "set.clr5" };
        for (int i = 0; i < 6; i++) {
            Row r = buttonRow(ks[i].label, ks[i].hint, clearKeys[i], ICON_X, "Clear", [this, i] { clearStorage(i); }, true);
            r.value = storageValue(i);
            keptRows.push_back(r);
        }
    }
    // Storage (the SD card: where browsers open, Files, what Gatefold keeps); Reset is its own category after Updates
    sections.push_back({ "Storage", ICON_FOLDER,
                         { { "Where browsers open", folderRows },
                           { "Files", { toggleRow("Show hidden files", "Names that start with a dot, in Files", "set.hidden", store.showHidden) } },
                           { "Kept on the SD card", keptRows } } });
    // Reset's buttons are one column, one width (the user, 2026-10-28: Reset sat narrower than Back up, Restore and Choose)
    float resetW = 0;
    for (const char* t : { "Back up", "Restore", "Reset", "Choose" }) resetW = std::max(resetW, float(g.measure(t, { css::Sm, Weight::SemiBold })) + 32 + 24);
    Section resetSection{ "Reset", ICON_REFRESH,
          { { "", { infoRow("Saved in", std::string(Store::Dir) + ": settings, playlists, Liked, where you left off and the tag cache. "
                                                                  "Saved as you change things, so closing from the HOME menu loses nothing."),
                    buttonRow("Back up the settings now", "A copy of all of it in " + backupRoot() + ", to put back below",
                              "set.backupnow", ICON_HISTORY, "Back up", [this] {
                                  persistNow();
                                  Store::flush();
                                  // On the worker, in the progress stack: the folder can hold far more than Gatefold's own
                                  // files (a folder copied in by hand), and copying them on the main thread froze the app
                                  if (!startFileWork("Backing up the settings", backupRoot(), false, [this, root = backupRoot()](FileWork& w) {
                                          w.total = 0;
                                          const std::string b = updater.backupNow(root, "by hand");
                                          if (b.empty()) w.result = "Could not back up the settings";
                                          else w.result = "Backed up in " + b.substr(b.find_last_of('/') + 1);
                                      }, [this] { backupsAt = storage.at = -100; }))
                                      return;
                                  fileWork.cat = SetReset;
                              }, false, resetW),
                    buttonRow("Reset settings", "Every setting as it came, the background player's too. Your music folder, playlists, Liked, "
                                                "tag edits, covers and saved presets stay",
                              "set.reset", ICON_REFRESH, "Reset", [this] { resetSettings(); }, true, resetW),
                    buttonRow("Factory reset", "Erase what you choose, your playlists, Liked and statistics among them, or everything, as if Songate were newly installed. Your music files and backups stay",
                              "set.factory", ICON_X, "Choose", [this] { openFactory(); }, true, resetW) } } } };

    // Updates (view_updates.cpp, updater.hpp): off until allowed; then releases with their changelogs, install or go back
    {
        std::vector<Card> cards;
        const std::string me = Updater::thisVersion();
        Row onRow{ "Updates", "", store.updates ? "Songate goes online only when you check, install or go back (and when it starts, if you turn "
                                                  "that on), to GitHub and nowhere else"
                                                : "Off: Songate never connects to the internet. On, it can look for new versions on GitHub, "
                                                  "show what changed, install one, or go back to an earlier one",
                   40, [this](float rx, float cy) {
                       if (toggle(uid("set.updon"), rx - 40, cy, store.updates != 0)) {
                           if (store.updates) {
                               store.updates = 0;
                               updater.cancel();
                               persistSoon(0.3f);
                               toast("Updates off: Songate does not go online");
                           } else {
                               allowUpdates();   // asks first, saying what going online means
                           }
                       }
                   } };
        onRow.focus = uid("set.updon");
        std::vector<Row> top = { onRow };
        if (store.updates) {
            top.push_back(toggleRow("Check when Songate starts", "A newer version is announced once, at the bottom of the screen", "set.updstart",
                                    store.updateAtStart));
            std::string hint;
            if (updater.busy()) hint = updateStatus();
            else if (!updater.error.empty()) hint = "Last try: " + updater.error;
            else if (updater.checkedAt) {
                char b[64];
                const time_t t = time_t(updater.checkedAt);
                strftime(b, sizeof b, "Last checked %d %b, %H:%M", localtime(&t));
                const int n = updater.newer(store.betas);
                hint = std::string(b) + (n >= 0 ? " · " + updater.releases[size_t(n)].version + " is out" : " · up to date");
            } else hint = "Lists every release below, with what changed";
            // Beta versions (the user, 2026-10-29: "a beta toggle so users can test features early"; asked first, with the
            // warning: App::allowBetas)
            Row betaRow{ "Beta versions", "", store.betas ? "Listed apart below. They can be unstable and have bugs: please report them"
                                                          : "Try new features before they are released. Off: only releases are listed and offered",
                         40, [this](float rx, float cy) {
                             if (toggle(uid("set.updbeta"), rx - 40, cy, store.betas)) {
                                 if (store.betas) {
                                     store.betas = false;
                                     persistSoon(0.3f);
                                     toast("Beta versions off: only releases are offered");
                                 } else {
                                     allowBetas();
                                 }
                             }
                         } };
            betaRow.focus = uid("set.updbeta");
            top.push_back(betaRow);
            const bool busy = updater.busy();
            top.push_back(buttonRow("Check for updates", hint, "set.updcheck", busy ? ICON_X : ICON_REFRESH, busy ? "Stop" : "Check", [this, busy] {
                if (busy) updater.cancel();
                else checkUpdates(false);
            }));
        }
        top.push_back(infoRow("This version", "The app, the background player and the overlay come from one release and are updated together. "
                                              "Background player: " + std::string(player.remote() ? "connected" : "not connected (off, missing, or of another version)"),
                              me + " · protocol " + std::to_string(Updater::thisProtocol())));
        cards.push_back({ "", top });
        // Installed, waiting for a restart: the background player (if its file changed), then Gatefold
        const UpdInstalled& ins = updater.installed;
        if (!ins.version.empty() && (ins.appPending || ins.bgChanged || ins.overlayPending || ins.fresh)) {
            std::string what = Updater::releaseName(ins.version) + (ins.appPending ? " starts when this one closes" : " is in");   // (its own name: 1.x was Gatefold NX)
            if (ins.bgChanged) what += "; the background player restarts with it (the music stops for a moment)";
            if (ins.overlayPending) what += "; the overlay is replaced once Ultrahand lets go of it (close the Songate overlay)";
            if (ins.fresh) what += "; your settings are cleared (they are in the backup)";
            Row fin = buttonRow("Restart to finish", what, "set.updfinish", ICON_REFRESH, "Restart now", [this] { finishUpdate(); });
            fin.value = ins.from + " to " + ins.version;
            cards.push_back({ "Installed", { fin } });
        }
        // Every release: Details opens its changelog and Install / Go back
        if (store.updates || !updater.releases.empty()) {
            std::vector<Row> rel, betas;
            for (int i = 0; i < int(updater.releases.size()); i++) {
                const UpdRelease& r = updater.releases[size_t(i)];
                if (r.beta && !store.betas) continue;   // betas only with Beta versions on, and apart
                const int cmp = Updater::compare(r.version, me);
                char mb[24];
                snprintf(mb, sizeof mb, "%.1f MB", double(r.zipSize) / (1024.0 * 1024.0));
                const std::string hint = r.date + " · " + (cmp > 0 ? "newer" : cmp < 0 ? "earlier" : "this version") + " · " + mb +
                                         (r.manifest ? "" : " · without in-app updates");
                const float bw = g.measure("Details", { css::Sm, Weight::SemiBold }) + 32;
                Row row{ Updater::releaseName(r.version), "", hint, bw, [this, i, bw](float rx, float cy) {
                            bool c = false;
                            button(uid("set.updrel", i), nullptr, "Details", rx - bw, cy - 17, false, &c);
                            if (c) openRelease(i);
                        } };
                row.focus = uid("set.updrel", i);
                if (cmp == 0) row.value = "· installed";
                (r.beta ? betas : rel).push_back(row);
            }
            if (rel.empty()) rel.push_back(infoRow("No releases listed yet", "Check for updates reads them from GitHub"));
            cards.push_back({ "Releases", rel });
            if (store.betas) {
                if (betas.empty()) betas.push_back(infoRow("No beta versions right now", "Check for updates lists them here when there are"));
                cards.push_back({ "Beta versions", betas });
            }
        }
        // Automix's models (the user, 2026-10-30): the built-in one, the downloaded ones, and those the model repository lists;
        // Details opens each one's sheet (Download and use, Use, Go back)
        {
            std::vector<Row> mods;
            const std::vector<ModelRow> list = modelList();
            const std::string inUse = mixName();
            for (int i = 0; i < int(list.size()); i++) {
                const ModelRow& r = list[size_t(i)];
                const UpdModel* m = r.offer >= 0 ? &updater.models[size_t(r.offer)] : nullptr;
                const std::string hint = r.builtin ? "Built in · comes with Songate"
                                       : r.downloaded ? "Downloaded" + (m ? " · " + m->date : std::string())
                                       : m && Updater::modelUsable(*m) ? m->date + " · available · " + std::to_string(std::max<int64_t>(1, m->size / 1024)) + " KB"
                                                                      : "Needs a newer Songate";
                const float bw = g.measure("Details", { css::Sm, Weight::SemiBold }) + 32;
                Row row{ r.name, "", hint, bw, [this, i, bw, name = r.name](float rx, float cy) {
                            bool c = false;
                            button(uid("set.model", i), nullptr, "Details", rx - bw, cy - 17, false, &c);
                            if (c) openModel(name);
                        } };
                row.focus = uid("set.model", i);
                if (r.name == inUse) row.value = "· in use";
                mods.push_back(row);
            }
            cards.push_back({ "Automix models", mods });
        }
        cards.push_back({ "Backups", { infoRow("Before every install", "Songate backs up your settings first. They are put back from Settings › Reset") } });
        sections.push_back({ "Updates", ICON_DOWNLOAD, cards });
    }
    // Reset: the backups (made by hand, before each install, before a restore) are listed here and put back from here,
    // between Back up and the two red ones, last: Reset settings and Factory reset
    {
        std::vector<Row>& first = resetSection.cards[0].rows;
        const std::vector<Row> resetRows(first.end() - 2, first.end());
        first.erase(first.end() - 2, first.end());
        {
            std::vector<Row> bk = { infoRow("Your backups", "Made by hand above, and before every install. Kept in " + backupRoot() +
                                                                " (Settings › Storage chooses where new ones go)") };
            const std::string pend = updater.restorePending();
            if (!pend.empty())
                bk.push_back(buttonRow("A restore waits", "When Songate closes: " + pend, "set.updrestx", ICON_X, "Cancel", [this] {
                    updater.cancelRestore();
                    toast("The restore is cancelled");
                }));
            const auto& list = updateBackups();
            for (int i = 0; i < int(list.size()) && i < 20; i++) {
                const UpdBackup& b = list[size_t(i)];
                const float bw = resetW;
                Row row{ b.name, "", b.note, bw, [this, i, bw](float rx, float cy) {
                            bool c = false;
                            button(uid("set.updbk", i), ICON_HISTORY, "Restore", rx - bw, cy - 17, false, &c, false, false, 0, bw);
                            if (c && i < int(backupsShown.size())) restoreBackup(backupsShown[size_t(i)]);
                        } };
                row.focus = uid("set.updbk", i);
                bk.push_back(row);
            }
            resetSection.cards.push_back({ "Backups", bk });
        }
        resetSection.cards.push_back({ "", resetRows });
    }
    sections.push_back(resetSection);

    // Support (the user, 2026-10-30: "Settings › Support"): a report from the console to Songate's reports on GitHub, through
    // a form (view_support.cpp, report.hpp); Atmosphère's crash reports; what a report says of this console. Just above About.
    {
        std::vector<Card> cards;
        std::vector<Row> rep;
        if (report::available()) {
            // off until allowed, as Updates (the user, 2026-10-30): the prompt says what is sent and where (allowReports)
            Row onRow{ "Reports", "", store.reports ? "Sent only when you press Send, to Songate's private reports on GitHub, and nowhere else"
                                                    : "Off: Songate sends nothing. On, you can report a bug or suggest a feature from here",
                       40, [this](float rx, float cy) {
                           if (toggle(uid("set.repon"), rx - 40, cy, store.reports)) {
                               if (store.reports) {
                                   store.reports = false;
                                   persistSoon(0.3f);
                                   toast("Reports off: Songate sends nothing");
                               } else {
                                   allowReports();
                               }
                           }
                       } };
            onRow.focus = uid("set.repon");
            rep.push_back(onRow);
            if (store.reports) {
                rep.push_back(buttonRow("Report a bug", "A short form: what happened, and where", "set.repbug", ICON_BUG, "Report",
                                        [this] { openReport(0); }));
                rep.push_back(buttonRow("Suggest a feature", "Something Songate could do, sent the same way", "set.repidea", PLI_BULB, "Suggest",
                                        [this] { openReport(1); }));
            }
        } else {
            rep.push_back(infoRow("Reports cannot be sent from this build", "Open an issue on GitHub instead: " ISSUES_URL));
        }
        cards.push_back({ "Report", rep });
        // read on a worker the first time this page shows (supportScan): never the card on the main thread
        const bool read = supportAsked_ && !supportThread_.joinable();
        std::vector<Row> cr;
        if (!read) cr.push_back(infoRow("Atmosphère's crash reports", "Reading them…"));
        else if (supportCrashes_.empty()) cr.push_back(infoRow("Atmosphère's crash reports", "None on this console"));
        else
            cr.push_back(buttonRow("Atmosphère's crash reports", std::to_string(supportCrashes_.size()) + (supportCrashes_.size() == 1 ? " report" : " reports") +
                                       ", the latest " + supportCrashes_.front().when, "set.crashes", ICON_BUG, "See them", [this] {
                                           if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                                           const Id back = ui.focusedId();
                                           sheet = Sheet{};
                                           sheet.page = Sheet::Crashes;
                                           sheet.returnFocus = back;
                                           ui.focusDefault();
                                       }));
        cards.push_back({ "Crash reports", cr });
        // what a report says of this console, each as it is now (never the files' names: the switch's prompt says so)
        std::string playing = "Nothing";
        if (now.track >= 0 && !now.info.codec.empty()) {
            playing = now.info.codec;
            if (now.info.sampleRate > 0) { char r[24]; snprintf(r, sizeof r, " · %.1f kHz", now.info.sampleRate / 1000.0); playing += r; }
        }
        cards.push_back({ "Sent with a report",
                          { infoRow("Songate", "", Updater::thisVersion()),
                            infoRow("Firmware", "", read ? supportSys_.firmware : std::string("…")),
                            infoRow("Atmosphère", "", read ? supportSys_.atmosphere : std::string("…")),
                            infoRow("Background player", "", player.remote() ? "Connected" : "Not connected"),
                            infoRow("Playing", "Its format only, never its name", playing),
                            infoRow("Library", "", std::to_string(lib.tracks.size()) + " tracks") } });
        sections.push_back({ "Support", ICON_HELP, cards });
    }

#ifdef GF_DEV
    // Debug: dev builds only (the Makefile's DEV), never in a release: the testing tools
    sections.push_back(
        { "Debug", ICON_BOLT,
          { { "Testing",
              { toggleRow("Frame times", "A graph of the last two seconds of frames: green is on time, red took longer than a "
                                         "sixtieth of a second, the white part is the app's own work",
                          "set.frametimes", store.frameTimes),
                buttonRow("1-minute sleep timer", "The end of the sleep timer without the wait: the fade, the pause and the card",
                          "set.sleep1", ICON_MOON, sleepLeft() > 0 && sleepChoice == 9 ? "Running" : "Start", [this] {
                              stopAfterCurrent = false;
                              setSleep(60.0, 9);
                              toast("The music pauses in 1 minute");
                          }),
                infoRow("Background player memory", "Free on its heap (1.5 MB in all). A crossfade needs 320 KB of it for the second "
                                                    "track, else the join stays gapless",
                        player.remote() ? std::to_string(bg.state().heapFree / 1024) + " KB" : std::string("Not connected")) } } } });
    {   // Automix's models, to compare on the console (the user, 2026-10-29; automix.hpp): a release plays the newest
        std::vector<std::string> models;
        for (int i = 0; i < int(automix::Model::Count); i++) {
            const std::string n = automix::name(automix::Model(i));
            models.push_back(n.rfind("Automix ", 0) == 0 ? n.substr(8) : n);   // "V1.0.0"
        }
        sections.back().cards.push_back(
            { "Automix", { segRow("Model", "How the joins are planned and an Automix playlist ordered. The next join uses it; Order again "
                                           "on a playlist to see its order by it. A release plays the newest",
                                  "set.mixmodel", models, int(mixModel()), [this](int i) {
                                      store.automixModel = automix::name(automix::Model(i));
                                      store.save();
                                      toast(std::string(automix::name(automix::Model(i))) + " for the next join");
                                  }) } });
    }
#endif

    // About
    {
        Row head;
        head.blockH = 76;
        head.block = [this](float bx, float by, float) {
            const Rgb note{ pal.accent.r + (pal.text.r - pal.accent.r) * 0.45f, pal.accent.g + (pal.text.g - pal.accent.g) * 0.45f, pal.accent.b + (pal.text.b - pal.accent.b) * 0.45f };   // (as the sidebar's mark)
            g.roundRect(bx, by + 8, 60, 60, 16, pal.accent, 0.12f);
            g.glow(bx + 30, by + 38, 30, pal.glow, 0.5f);
            g.icon(ICON_BRAND_ARCH, bx + 30, by + 38, 34, pal.accent);
            g.icon(ICON_BRAND_NOTE, bx + 30, by + 38, 34, note);
            g.text("Songate for Nintendo Switch", { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
            g.text("Version " GF_VERSION " · by AndryTheBeast", { css::Base }, bx + 76, by + 46, pal.text2);
        };
        sections.push_back(
            { "About", ICON_INFO,
              { { "", { head } },
                { "Tour", { buttonRow("Take the tour", "Every part of Songate on its own screens, with examples: about 30 short steps, in chapters",
                                      "set.tour", ICON_SPARKLE, "Start", [this] { startTour(); }) } },
                { "Credits",
                  // the project it began as (the user, 2026-10-29: mentioned only as that, respectfully and factually; the
                  // licence requires the notice: LICENSE-gatefold)
                  { infoRow("Gatefold", "Songate began as a recreation of Gatefold, DEVIROYrus's interface for foobar2000. Its palette "
                                        "algorithm, icons, app icon and typeface come from that project. © Gatefold contributors, MIT licence"),
                    infoRow("Inter", "The typeface. © The Inter Project Authors, SIL Open Font Licence"),
                    infoRow("FFmpeg", "Decoding in Songate's own player: AAC, ALAC, FLAC, MP3, Ogg Vorbis, Opus and WAV. LGPL 2.1"),
                    infoRow("SDL 2", "Graphics, sound, fonts and images: SDL, SDL_mixer, SDL_ttf, SDL_image. zlib licence"),
                    infoRow("libFLAC, mpg123, Opus, Tremor", "SDL_mixer's decoders, for a file FFmpeg cannot open. BSD and LGPL licences"),
                    infoRow("libcurl and zlib", "Updates: the connection to GitHub, over the console's own secure connection. curl and zlib licences"),
                    infoRow("libnx and devkitPro", "The Nintendo Switch homebrew toolchain. ISC licence"),
                    infoRow("Background player", "Helix AAC decoder (RealNetworks RPSL), minimp3 (CC0), dr_flac (public domain), and "
                                                 "the overlay is built on sys-tune's, with libultrahand (GPL 2)") } } } });
    }
    // Storage goes with Library (your music, then where it and Gatefold's own files are): built later, placed after it
    {
        auto st = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Storage"; });
        if (st != sections.end()) {
            Section s = std::move(*st);
            sections.erase(st);
            auto li = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Library"; });
            sections.insert(li == sections.end() ? sections.end() : li + 1, std::move(s));
        }
    }
    // Reset above Updates (the user, 2026-10-30), as App::SetReset and SetUpdates number them
    {
        auto rs = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Reset"; });
        if (rs != sections.end()) {
            Section s = std::move(*rs);
            sections.erase(rs);
            auto up = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Updates"; });
            sections.insert(up, std::move(s));
        }
    }
    settingsCat = std::clamp(settingsCat, 0, int(sections.size()) - 1);
    // Pages that read the SD card do it once when they open: Storage and reset's sizes, Updates' backups
    if (settingsCat != settingsCatSeen_) {
        settingsCatSeen_ = settingsCat;
        if (!strcmp(sections[size_t(settingsCat)].name, "Support")) supportScan();   // its reads, on a worker
        if (settingsCat == SetStorage && time - storage.at > 60) storage.at = -100;   // sizes kept a minute (a Clear measures again)
        if (settingsCat == SetReset) backupsAt = -100;
    }

    // ------------------------------------------------------------ left column
    const float lx = x + 24, lw = 214;
    float cy = y + 24;
    kicker("Songate", lx, cy + 6, pal.text3, 0.14f);
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
    // Eleven categories and more sit a little closer; past what fits (Support, a dev build's Debug: About went under the
    // player bar, the user, 2026-10-30) the list scrolls, the focused category kept in view (Ui, as every scrolled list)
    const float step = sections.size() > 10 ? 36 : 40, catH = step - 2;
    ScrollArea& ca = area("settings.cats");
    const float listTop = cy;
    ca.view = { lx - 8, listTop, lw + 16, y + h - listTop };
    ca.content = sections.size() * step + 8;
    ca.clamp();
    ui.pushArea(&ca);
    g.clip(lx - 8, listTop, lw + 16, y + h - listTop);
    cy -= ca.y;
    for (int i = 0; i < int(sections.size()); i++) {
        const Id id = uid("set.cat", i);
        const bool on = settingsQuery.empty() && settingsCat == i;
        if (ui.item(id, { lx, cy, lw, catH }, 10, on ? DefaultFocus : 0)) {
            settingsCat = i;
            settingsQuery.clear();
            area("settings.pane").target = area("settings.pane").y = 0;
            settingsFocusFirst = true;   // the page's first setting takes the focus (the user: as an artist's page does)
        }
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(lx, cy, lw, catH, 10, pal.accent, 0.14f);
        else if (hot) g.roundRect(lx, cy, lw, catH, 10, pal.text, 0.065f);
        g.icon(sections[i].icon, lx + 12 + 9, cy + catH / 2, 18, on ? pal.accentInk : pal.text2);
        g.textC(sections[i].name, { css::Base, Weight::SemiBold }, lx + 12 + 18 + 12, cy + catH / 2, on ? pal.accentInk : hot ? pal.text : pal.text2, 1,
                Align::Left, int(lw - 50));
        cy += step;
    }
    g.unclip();
    ui.popArea();

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
            if (r.lifted) {   // held to be moved: lifted, the accent around it (the queue's Move)
                g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.14f);
                g.roundStroke(px + 8, ry + 4, pw - 16, l.h - 8, 12, 2, pal.accent, 0.95f);
            }
            // showSetting: for its first frames the page scrolls to the row and focuses its control; the row is lit
            // in the accent and fades over a second and a half.
            if (settingsQuery.empty() && !settingsJump.empty() && r.label == settingsJump) {
                const double since = time - settingsJumpAt;
                if (since < 0.15) {
                    sa.target = ry - y + sa.y - 90;
                    sa.clamp();
                    if (r.focus) ui.focus(r.focus);
                }
                const float lit = float(std::clamp(1.0 - (since - 0.5) / 1.2, 0.0, 1.0));
                if (lit > 0) g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.16f * lit);
                else settingsJump.clear();
            }
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
