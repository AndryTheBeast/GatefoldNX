/*
 * Gatefold NX · view_updates.cpp
 * Settings → Updates (its rows are in view_settings.cpp): asking before anything goes online, checking GitHub, a
 * release's changelog in the sheet (Sheet::Release) with Install / Go back, the question that says all an install
 * does, finishing it (the background player and Gatefold start again), and the settings backups. The work itself is
 * the Updater's (updater.hpp), on a worker.
 */
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <strings.h>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {

// Two more in Gatefold's line style (plicons.inc's) for the changelog: a fix, and privacy
#define UPD_SVG R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">)SVG"
inline constexpr char UPD_WRENCH[] = UPD_SVG R"SVG(<path d="M14.7 6.3a4 4 0 0 0 5 5L21 10a5.5 5.5 0 0 1-7.3 6.4l-6.1 6.1a2.1 2.1 0 0 1-3-3l6.1-6.1A5.5 5.5 0 0 1 17 3l-1.3 1.3a4 4 0 0 0-1 2z"/></svg>)SVG";
inline constexpr char UPD_SHIELD[] = UPD_SVG R"SVG(<path d="M12 3l7.5 3v5.5c0 4.6-3.2 8.2-7.5 9.5-4.3-1.3-7.5-4.9-7.5-9.5V6z"/><path d="M9 12l2.2 2.2L15.5 10"/></svg>)SVG";

std::string megabytes(int64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return b;
}

/** "2026-10-27" → "27 October 2026". */
std::string longDate(const std::string& ymd) {
    static const char* months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return ymd;
    return std::to_string(d) + " " + months[m - 1] + " " + std::to_string(y);
}

}  // namespace

std::string App::backupRoot() const {
    // Never inside config/gatefold-nx (a settings.txt edited by hand): a backup would copy itself into itself
    const std::string cfg = Store::Dir;
    const bool inside = strncasecmp(store.backupDir.c_str(), cfg.c_str(), cfg.size()) == 0 &&
                        (store.backupDir.size() == cfg.size() || store.backupDir[cfg.size()] == '/');
    return store.backupDir.empty() || inside ? std::string("sdmc:/config/gatefold-nx-backups") : store.backupDir;
}

const std::vector<UpdBackup>& App::updateBackups() {
    if (backupsAt < -50) {   // listed once each time the page opens, and after an install or a Clear (not every few seconds)
        backupsAt = time;
        backupsShown = updater.backups(backupRoot());
    }
    return backupsShown;
}

const char* App::changelogIcon(const std::string& name) const {
    if (name == "fix") return UPD_WRENCH;
    if (name == "shield") return UPD_SHIELD;
    if (name == "download") return ICON_DOWNLOAD;
    if (name == "palette") return ICON_SPARKLE;
    if (name == "volume") return ICON_VOL;
    if (name == "shuffle") return ICON_SHUFFLE;
    if (name == "info") return ICON_INFO;
    if (name == "search") return ICON_SEARCH;
    if (name == "image") return ICON_IMAGE;
    if (name == "refresh") return ICON_REFRESH;
    if (name == "check") return ICON_CHECK;
    if (name == "tag") return ICON_TAG;
    if (name == "x") return ICON_X;
    if (name == "play") return ICON_PLAY;
    if (name == "canvas") return ICON_CANVAS;
    if (name == "settings" || name == "gear") return ICON_GEAR;
    if (name == "quote" || name == "lyrics") return ICON_QUOTE;
    for (const auto& k : kPlaylistIcons) if (name == k.key) return k.svg;   // music, headphones, eq, folder, moon, …
    return nullptr;   // a dot
}

std::string App::updateStatus() const {
    const int64_t d = updater.done(), t = updater.total();
    switch (updater.phase()) {
    case Updater::Checking: return "Asking GitHub for the releases…";
    case Updater::Downloading:
        return "Downloading " + updater.working() + (t > 0 ? " · " + megabytes(d).substr(0, megabytes(d).size() - 3) + " of " + megabytes(t) : std::string());
    case Updater::Verifying: return "Checking the download" + (t > 0 ? " · " + std::to_string(int(d * 100 / std::max<int64_t>(1, t))) + "%" : std::string());
    case Updater::Unpacking: return "Unpacking · " + std::to_string(d) + " of " + std::to_string(t) + " files";
    case Updater::BackingUp: return "Backing up your settings";
    case Updater::Installing: return "Putting the new files in place";
    default: return {};
    }
}

/** Updates are off until allowed here: what going online means, said before anything does. */
void App::allowUpdates() {
    confirm(UPD_SHIELD, "Updates", "Connect to the internet?", "GitHub · AndryTheBeast/GatefoldNX", "Allow Gatefold to go online?",
            "Gatefold connects to GitHub only when you check for updates or install one, and at launch if you turn that on. "
            "GitHub sees your console's IP address and Gatefold's version, nothing else. You can turn this off at any time.",
            "Allow", [this] {
                store.updates = 1;
                persistSoon(0.3f);
                toast("Updates allowed: Gatefold goes online only when you ask");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

void App::checkUpdates(bool quiet) {
    if (!store.updates) { if (!quiet) allowUpdates(); return; }
    if (updater.busy()) return;
    updateQuiet = quiet;
    updater.check();
}

void App::updateTick() {
    if (!updater.poll()) return;
    if (updater.justChecked) {
        const int n = updater.newer();
        if (updateQuiet) {   // the check at launch: a toast only for a newer version, once each
            if (updater.error.empty() && n >= 0 && updater.releases[size_t(n)].version != store.updateToasted) {
                store.updateToasted = updater.releases[size_t(n)].version;
                persistSoon(0.3f);
                toast("Gatefold " + store.updateToasted + " is out: Settings › Updates");
            }
        } else if (!updater.error.empty()) {
            toast("Could not check: " + updater.error);
        } else {
            toast(n >= 0 ? "Gatefold " + updater.releases[size_t(n)].version + " is out" : "Gatefold is up to date");
        }
        updateQuiet = false;
        return;
    }
    backupsAt = -100;
    if (updater.justInstalled) {
        if (sheet.page == Sheet::Release) closeSheet();
        toast("Installed " + updater.installed.version + ": restart to finish");
        settingsCat = SetUpdates;
        settingsQuery.clear();
        if (route.view == View::Settings) area("settings.pane").target = 0;
    } else if (!updater.error.empty()) {
        toast("Not installed: " + updater.error);
    }
}

void App::openRelease(int i) {
    if (i < 0 || i >= int(updater.releases.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Release;
    sheet.recipe = i;
    sheet.name = updater.releases[size_t(i)].version;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::updateSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const UpdRelease& r = updater.releases[size_t(sheet.recipe)];
    const int cmp = Updater::compare(r.version, Updater::thisVersion());
    icon = cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_CHECK;
    kick = cmp > 0 ? "Newer" : cmp < 0 ? "Earlier version" : "This version";
    title = "Gatefold NX " + r.version;
    sub = longDate(r.date) + " · " + megabytes(r.zipSize) + (r.manifest ? "" : " · without in-app updates");
}

void App::updateSheetRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    if (sheet.recipe < 0 || sheet.recipe >= int(updater.releases.size()) || updater.releases[size_t(sheet.recipe)].version != sheet.name) {
        closeSheet();   // the list was read again meanwhile
        return;
    }
    const int i = sheet.recipe;
    const UpdRelease& r = updater.releases[size_t(i)];
    const int cmp = Updater::compare(r.version, Updater::thisVersion());
    const float textW = 600 - 48 - 48;   // as view_smart.cpp draws a Text row
    auto text = [&](const char* icon, const std::string& s, bool strong = false) {
        Row t{ Row::Text, icon, "", s };
        t.on = strong;   // a warning: in the text colour, not the quieter one
        t.h = g.wrap(s, { css::Sm }, int(textW), 12).size() * 19.f + 14;
        rows.push_back(t);
    };
    // ---- what it does: one action, or what is going on
    if (updater.busy()) {
        rows.push_back({ Row::Action, ICON_HOURGLASS, updater.working() == r.version ? updateStatus() : "Busy with " + updater.working(),
                         "Settings › Updates shows how far", "", nullptr });
    } else {
        const std::string label = cmp > 0 ? "Install " + r.version : cmp < 0 ? "Go back to " + r.version : "Install " + r.version + " again";
        const std::string sub = cmp > 0   ? "The app, the background player and the overlay, together"
                              : cmp < 0   ? (r.manifest ? "Your settings are backed up first" : "Your settings are backed up first; back from it by hand")
                                          : "Puts this version's files back as they were released";
        rows.push_back({ Row::Action, cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_REFRESH, label, sub, megabytes(r.zipSize),
                         [this, i] { installRelease(i); } });
    }
    rows.back().h = 62;
    if (!r.manifest)
        text(ICON_INFO, r.version + " cannot update itself (it has no Updates page). To come back later, copy a newer release from "
                            "github.com/AndryTheBeast/GatefoldNX/releases to the SD card by hand; your settings are backed up first.", true);
    // ---- the changelog
    if (r.log.empty()) {
        rows.push_back({ Row::Group, nullptr, "What's new" });
        rows.back().h = 38;
        text(nullptr, "This release has no notes.");
    }
    for (const UpdLog& sec : r.log) {
        rows.push_back({ Row::Group, changelogIcon(sec.icon), sec.title });
        rows.back().h = 40;
        for (const UpdItem& it : sec.items) text(changelogIcon(it.icon), it.text);
    }
}

void App::installRelease(int i) {
    if (!store.updates) { allowUpdates(); return; }
    if (updater.busy()) { toast("Wait for " + updater.working() + " first"); return; }
    if (i < 0 || i >= int(updater.releases.size())) return;
    const UpdRelease& r = updater.releases[size_t(i)];
    const std::string me = Updater::thisVersion();
    const int cmp = Updater::compare(r.version, me);
    std::string text = "Your settings are backed up first. Then the app, the background player and the overlay are replaced; "
                       "Restart now finishes it (the music stops for a moment).";
    if (cmp < 0) {
        text += " Your playlists, Liked and edits stay. Start fresh clears the settings when Gatefold closes; the backup keeps them.";
    }

    confirm(cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_REFRESH, cmp > 0 ? "Update" : cmp < 0 ? "Go back" : "Install again",
            "Gatefold NX " + r.version, longDate(r.date) + " · " + megabytes(r.zipSize),
            cmp > 0 ? "Install " + r.version + "?" : cmp < 0 ? "Go back to " + r.version + "?" : "Install " + r.version + " again?", text,
            cmp < 0 ? "Keep my settings" : "Install", [this, i] { updater.install(i, false, backupRoot()); });
    sheet.askSafe = true;
    sheet.askYesIcon = cmp < 0 ? ICON_HISTORY : ICON_DOWNLOAD;
    if (!r.manifest) sheet.askWarn = r.version + " cannot update itself. Coming back to a newer version is by hand.";
    if (cmp < 0) {
        sheet.askAlt = "Start fresh";
        sheet.askAltYes = [this, i] { updater.install(i, true, backupRoot()); };
    }
}

void App::restoreBackup(const UpdBackup& b) {
    confirm(ICON_HISTORY, "Backups", b.name, b.note, "Put these settings back?",
            "Your settings now are backed up first. Then Gatefold restarts with these: settings, playlists, Liked, edits and statistics "
            "as they were then.",
            "Restore and restart", [this, path = b.path] {
                persistNow();
                Store::flush();
                // The backup first, on the worker (a big settings folder froze the app), then the restart
                auto made = std::make_shared<std::string>();
                if (!startFileWork("Backing up the settings", backupRoot(), false, [this, made, root = backupRoot()](FileWork& w) {
                        w.total = 0;
                        *made = updater.backupNow(root, "before a restore");
                        w.result = made->empty() ? "Could not back up the settings now: nothing was changed" : "Backed up: restarting";
                    }, [this, made, path] {
                        if (made->empty()) return;
                        updater.restoreAtExit(path);
                        relaunch = true;
                        running = false;
                    }))
                    return;
                fileWork.cat = SetReset;
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_HISTORY;
}

/**
 * Settings → Reset → Reset settings: every setting as Gatefold came, after a question; only settings (the user,
 * 2026-10-28: everything else is the factory reset's). Before, it reset only the store, so the equalizer kept playing
 * as it was (both players hold their own copy), the background player kept its game actions and fades, the sidebar
 * stayed collapsed until the next launch, and your tag edits, covers, lyric timings and saved presets vanished until
 * the next launch (dropped in memory, not on the card).
 */
void App::resetSettings() {
    confirm(ICON_REFRESH, "Reset", "Reset settings", "Every setting, as Gatefold came", "Reset every setting?",
            "Colours, the sidebar, playback, the equalizer, lyrics, the Canvas, the visualizer, controls, where browsers open, "
            "updates, and the background player's game actions and fades go back to how they came. Only settings: your music "
            "folder, playlists, Liked, tag edits, covers, statistics and saved presets stay (Factory reset, below, erases those).",
            "Reset", [this] {
                defaultSettings();
                persistNow();
                toast("Every setting is as it came");
            });
}

void App::defaultSettings() {
    Store fresh;
    // Yours, not settings: kept
    fresh.musicRoot = store.musicRoot;
    fresh.playlists = store.playlists;
    fresh.liked = store.liked;
    fresh.edits = store.edits;
    fresh.covers = store.covers;
    fresh.lyricNudge = store.lyricNudge;
    fresh.eqUser = store.eqUser;
    const bool bgWasOff = !player.remote();
    if (store.updates) updater.cancel();
    store = std::move(fresh);
    sideW = store.sidebarCollapsed ? css::SideWCollapsed : css::SideW;
    volumeShown = store.volume;
    player.setVolume(volumeShown);
    setEq(store.eq);   // Gatefold's player, and the background player's copy (sent within 0.1 s)
    if (player.remote()) {   // its own settings: game actions, fades, action playlists
        bgs.cfg = BgConfig{};
        bg.setConfig(bgs.cfg);
    }
    applyTheme();
    coversHeld = false;
    applyJobStops();   // stopped work starts again
    if (bgWasOff) setBackground(true);   // on, as it came (when it is installed)
}

namespace {
struct FactoryPartInfo { const char* label; const char* sub; };
// In App::FactoryPart's order (and Updater::factoryFiles')
const FactoryPartInfo kFactoryParts[] = {
    { "Settings", "Every setting, the background player's game actions and fades too" },
    { "Music folder", "Gatefold asks where your music is again, as at its first start" },
    { "Playlists", "Your playlists, smart and folder ones, and their icons" },
    { "Liked", "Every track you liked" },
    { "Tag edits and covers", "Tags you edited and covers you chose (your music files were never changed)" },
    { "Lyric timings", "How far you moved each track's synced lyrics" },
    { "Saved equalizer presets", "The presets you saved under your own names" },
    { "Play counts and statistics", "Plays, last played and dates added (smart playlists use them)" },
    { "The queue", "What was playing and where you left off, the background player's too" },
    { "Caches", "Covers, overlay covers, tags and loudness: made again from your music" },
};
}  // namespace

/** Settings → Reset → Factory reset: the sheet of what Gatefold keeps, each with a switch (all off at first; what you
 *  ticked stays ticked when you come back after a Cancel on the question). */
void App::openFactory() {
    const Id back = ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Factory;
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::factoryRows(std::vector<SheetRow>& rows) {
    rows.push_back({ SheetRow::Group, nullptr, "Erase what you tick" });
    rows.back().h = 34;
    for (int k = 0; k < FacParts; k++) {
        rows.push_back({ SheetRow::Toggle, nullptr, kFactoryParts[k].label, kFactoryParts[k].sub, "", [this, k] { factoryPick ^= 1u << k; } });
        rows.back().h = 60;
        rows.back().on = factoryPick & (1u << k);
    }
    rows.push_back({ SheetRow::Group, nullptr, "" });
    rows.back().h = 14;
    int n = 0;
    for (int k = 0; k < FacParts; k++) n += (factoryPick >> k) & 1;
    rows.push_back({ SheetRow::Action, ICON_X, "Erase what is ticked",
                     n ? std::to_string(n) + " of " + std::to_string(int(FacParts)) + " ticked: they go when Gatefold closes and starts again"
                       : "Tick what to erase above first",
                     "", n ? std::function<void()>([this] { factoryErase(factoryPick); }) : nullptr });
    rows.back().h = 60;
    rows.push_back({ SheetRow::Action, ICON_X, "Erase everything", "As if Gatefold were newly installed: every part above, and the rest", "",
                     [this] { factoryErase(~0u); } });
    rows.back().h = 60;
}

/** Asks, then closes Gatefold: shutdown moves the ticked parts aside (Updater::factoryReset) and it starts again.
 *  Everything asks twice. The backups (another folder) and the music files always stay. */
void App::factoryErase(unsigned mask) {
    const unsigned allParts = (1u << FacParts) - 1;
    const bool all = (mask & allParts) == allParts;
    auto go = [this, mask, all] {
        // Settings and the music folder live together in settings.txt, saved as Gatefold closes: they are reset here
        if (!all && (mask & (1u << FacSettings))) defaultSettings();
        if (!all && (mask & (1u << FacMusic))) store.musicRoot.clear();
        factoryMask_ = all ? ~0u : mask;
        relaunch = true;
        running = false;
    };
    if (!all) {
        std::string list;
        for (int k = 0; k < FacParts; k++)
            if (mask & (1u << k)) list += (list.empty() ? "" : ", ") + std::string(kFactoryParts[k].label);
        confirm(ICON_X, "Factory reset", "Erase what is ticked", list, "Erase these for good?",
                "Erased: " + list + ". Gatefold closes now and starts again without them. Only a backup made before can bring them back; "
                "your music files and backups in " + backupRoot() + " stay.",
                "Erase", go);
        sheet.askWarn = "THIS CANNOT BE UNDONE";
        return;
    }
    confirm(ICON_X, "Factory reset", "Erase everything", "Gatefold as if it were newly installed", "Erase everything Gatefold keeps?",
            "Every setting, playlist and Liked track; your tag edits, chosen covers, lyric timings and saved equalizer presets; play "
            "counts and statistics; the queue and the music folder; the background player's queue and game actions; every cache. "
            "Gatefold then closes and starts again as new. Your music files are not touched, and backups in " + backupRoot() + " stay.",
            "Erase everything", [this, go] {
                confirm(ICON_X, "Factory reset", "Are you absolutely sure?", "There is no way back", "Erase everything now?",
                        "Only a backup made before can bring any of it back. If you might want your playlists, Liked or settings "
                        "again, choose Cancel and use Back up the settings now first.",
                        "Yes, erase everything", go);
                sheet.askWarn = "EVERYTHING WILL BE ERASED FOR GOOD";
            });
    sheet.askWarn = "THIS CANNOT BE UNDONE: SETTINGS, PLAYLISTS, LIKED AND STATISTICS ARE LOST";
}

/** The background player from its new files (pm:shell), then Gatefold: it closes, the new .nro is swapped in, and it starts again. */
void App::finishUpdate() {
    if (updater.installed.bgChanged) {
        bg.disconnect();
        std::string err;
        if (!Updater::restartBackground(err)) {   // the console's restart does it (and Gatefold's .nro waits for the next close or launch)
            toast(err);
            return;
        }
        updater.markBgRestarted();
    }
    relaunch = true;
    running = false;
}
