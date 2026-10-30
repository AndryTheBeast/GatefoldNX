/*
 * Gatefold NX · updater.hpp
 * Settings → Updates: Gatefold's GitHub releases (AndryTheBeast/GatefoldNX), their changelogs, and installing any
 * of them, newer or older. Nothing goes online until Settings allows it (store.updates), and then only when asked
 * (or at launch, if "Check when Gatefold starts" is on).
 *
 * A release from 1.2.0 on carries a small manifest asset (gatefold-manifest.json, made by the release tools): its
 * version and background-player protocol, the zip's name, size and SHA-256, each file's path, size and SHA-256, and
 * the changelog written for the app (sections with an icon name and items). 1.0.0 and 1.1.0 have none: their zips
 * have a known layout, their SHA-256 is in a table here (and GitHub's own digest), and their changelog is the
 * release's notes. All three parts always come from the same release: the app, the background player and the overlay.
 *
 * Installing (on a worker, cores::Net): the zip is downloaded to config/gatefold-nx/updates/, its size and SHA-256
 * checked, each file unpacked (its CRC, and its SHA-256 when the manifest has one) next to where it goes as
 * "<file>.gfnew"; then config/gatefold-nx is copied to a dated backup folder; then the background player's files and
 * the overlay are swapped in (each renamed over the old one; one that cannot be, as an overlay Ultrahand holds, waits
 * as .gfnew). The app's own .nro is swapped when Gatefold closes (after romfsExit) or, if it was closed some other
 * way, at its next launch before anything reads it (Updater::finishPending, from main). Only the paths in
 * kAllowed can be written, whatever a manifest says.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "net.hpp"

struct UpdItem { std::string icon, text; };                                    // a changelog line (icon: its emoji as an icon name)
struct UpdLog { std::string icon, title; std::vector<UpdItem> items; };       // a changelog section
struct UpdFile { std::string path; int64_t size = 0; std::string sha256; };     // path from the SD card's root

// Where bugs are reported (Settings › Updates: the betas' warning). The repository's name until its rename to Songate
#define ISSUES_URL "github.com/AndryTheBeast/GatefoldNX/issues"

struct UpdRelease {
    std::string tag, version, name, date, body;   // date: "2026-10-27"; body: the GitHub notes (Markdown)
    std::string zipName, zipUrl, zipSha;          // zipSha: the manifest's, else GitHub's digest, else the table's
    int64_t zipSize = 0;
    bool manifest = false;                        // from 1.2.0 on: it can update itself (and roll back) from inside Gatefold
    bool beta = false;                            // a beta (GitHub's pre-release, or "2.0.0 BETA"): listed apart, only with Beta versions on
    int protocol = 0;                             // the background player's protocol (GFBG_PROTOCOL), 0 unknown
    std::vector<UpdFile> files;                   // the manifest's, else the known layout
    std::vector<UpdLog> log;                      // the app changelog, else sections read from the notes
};

/**
 * An Automix model sent over the air (the user, 2026-10-30): the model repository's latest release carries
 * songate-automix-models.json (each model's name, file, size, SHA-256, format, the lowest Songate that reads it, the analysis
 * it was trained on, notes) and the model files themselves (automix::ModelFile's text). Downloaded under the same permission
 * as releases, checked by size and checksum, kept in config/gatefold-nx/models/.
 */
struct UpdModel {
    std::string name, file, sha256, date, app = "2.0.0-beta";
    int64_t size = 0;
    int format = 1, mixan = 0;
    std::vector<std::string> notes;
};

/** What Settings → Updates shows about the last install (updates/installed.txt), and what is waiting for a restart. */
struct UpdInstalled {
    std::string version, from, backup;
    bool appPending = false;          // the .nro waits for Gatefold to close
    bool bgChanged = false;           // the background player's file changed: it must restart
    bool overlayPending = false;      // the overlay could not be swapped yet
    bool fresh = false;               // going back with fresh settings: config is cleared when Gatefold closes
};

/** A config backup (Settings → Updates → Backups). */
struct UpdBackup { std::string path, name, note; int64_t bytes = 0; int files = 0; };

class Updater {
public:
    enum Phase { Idle, Checking, Downloading, Verifying, Unpacking, BackingUp, Installing };

    /** configDir: sdmc:/config/gatefold-nx; self: the running .nro. Reads the last check's cache (no network). */
    void start(const std::string& configDir, const std::string& self);
    void stop();   // waits for the worker (a download is stopped first)

    void check();                                                     // the release list and their manifests
    /** Downloads and installs releases[i]; fresh: the settings are cleared when Gatefold closes (after the backup). */
    void install(int i, bool fresh, const std::string& backupRoot);
    /** Downloads models[i] into modelsDir (its checksum checked); justModel says so when it is in. */
    void installModel(int i, const std::string& modelsDir);
    /** Whether this Songate can play a listed model; why not, when it cannot (a newer one is needed). */
    static bool modelUsable(const UpdModel& m, std::string* why = nullptr);
    void cancel() { prog_.cancel = true; }
    /** Copies a backup back over config/gatefold-nx when Gatefold closes. */
    void restoreAtExit(const std::string& backup);
    /** Copies config/gatefold-nx into a new dated folder under root (caches left out); its path, or "" when it failed. */
    std::string backupNow(const std::string& root, const std::string& note);
    void cancelRestore();
    std::string restorePending() const;

    /** Main thread, every frame: takes over what the worker finished. True when something changed. */
    bool poll();
    bool busy() const { return phase_ != Idle; }
    Phase phase() const { return Phase(phase_.load()); }
    int64_t done() const { return prog_.done; }
    int64_t total() const { return prog_.total; }
    const std::string& working() const { return working_; }   // the version being installed

    std::vector<UpdRelease> releases;   // newest first, as GitHub lists them
    std::vector<UpdModel> models;       // the Automix models listed by the model repository's latest release
    bool justModel = false;             // poll(): a model download finished well (working() names it)
    std::string error;                  // the last task's failure, "" when it went well
    int64_t checkedAt = 0;              // time() of the last good check, 0 never
    bool justChecked = false;           // poll(): a check finished (with or without error)
    bool justInstalled = false;         // poll(): an install finished well
    UpdInstalled installed;
    std::vector<UpdBackup> backups(const std::string& backupRoot) const;

    /** The newest release above this version, or −1. */
    int newer(bool betas) const;   // the newest release above this one (betas too only when they are on), or -1
    static int compare(const std::string& a, const std::string& b);   // versions "1.2.0": <0, 0, >0
    static bool isBeta(const std::string& v);   // something after its numbers ("2.0.0 BETA")
    static std::string pretty(const std::string& v);   // the app's spelling: "2.0.0-beta.1" → "2.0.0 BETA 1"
    /** A release as it was named: "Gatefold NX 1.1.0" before the rebranding, "Songate 2.0.0" from it on (the releases
     *  before stay as they are: the user, 2026-10-29). */
    static std::string releaseName(const std::string& v) { return (atoi(v.c_str()) < 2 ? "Gatefold NX " : "Songate ") + v; }   // (by its first number: 2.0.0 BETA is Songate's)
    static std::string thisVersion();
    static int thisProtocol();   // GFBG_PROTOCOL: the background player this app talks to

    /** Before anything reads the .nro (main) and at the very end of Gatefold (shutdown): the files waiting as .gfnew are
     *  swapped in, a fresh start or a restore is done. Returns the .nro to start next: this one, replaced; the one a release
     *  under the other name put in its own place (going back to 1.x, Gatefold NX: this one was removed); or empty. */
    static std::string finishPending(const std::string& configDir, const std::string& self);
    /** The first launch after the rebranding (1.x was Gatefold NX): its .nro, and its overlay once Songate's is in, removed
     *  (the HOME menu's homebrew and Ultrahand would list both); the settings folder stays config/gatefold-nx. Not while an
     *  update waits (going back to 1.x puts them there on purpose). */
    static void cleanLegacy(const std::string& configDir, const std::string& self);
    /** The background player again, from its (new) files: pm:shell ends it and starts it. False (why in err) when not allowed. */
    static bool restartBackground(std::string& err);
    void markBgRestarted();   // after restartBackground: the new background player runs
    /** Settings → Reset → Factory reset, at the very end of Gatefold: the background player is ended; what the mask ticks
     *  (App::FactoryPart bits) is moved into "<folder>.erased" (instant), or with every bit the whole folder; then the
     *  player starts again with nothing of it to read. */
    static void factoryReset(const std::string& configDir, unsigned mask);
    /** The files of each factory reset part, bit by bit (App::FactoryPart). */
    static const std::vector<std::vector<const char*>>& factoryFiles();
    /** At the next launch, on a worker: what factoryReset moved aside is deleted (thousands of covers: never at exit). */
    static void eraseLeftovers(const std::string& configDir);

private:
    void run(std::function<void()> work);
    void doCheck();
    void doInstall(UpdRelease r, bool fresh, std::string backupRoot);
    void doInstallModel(UpdModel m, std::string modelsDir);
    void readCache();
    void readInstalled();

    std::string dir_, self_;
    std::string restore_;   // a backup waiting to be put back when Gatefold closes (updates/restore.txt)
    std::thread worker_;
    std::atomic<int> phase_{ Idle };
    net::Progress prog_;
    std::string working_;
    std::mutex m_;
    // handed over by the worker (under m_)
    bool doneFlag_ = false, checked_ = false, installedOk_ = false, modelOk_ = false;
    std::string err_;
    std::vector<UpdRelease> got_;
    std::vector<UpdModel> gotModels_;
    bool gotModelsSet_ = false;
};
