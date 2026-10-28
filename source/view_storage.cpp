/*
 * Gatefold NX · view_storage.cpp
 * Settings → Storage and reset (its rows are in view_settings.cpp): where each browser opens (Files, importing and
 * exporting playlists, the cover picker, the updater's backups), chosen with the music-folder picker; and what
 * Gatefold keeps on the SD card, measured on a worker, each with a Clear button that asks first.
 */
#include <dirent.h>
#include <sys/stat.h>

#include <chrono>
#include <thread>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {

std::string joinP(const std::string& a, const std::string& b) { return a.empty() || a.back() == '/' ? a + b : a + "/" + b; }
bool isDirP(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

/** Files and bytes below p (p itself when a file); only names `keep` accepts at its top. */
struct Use { int64_t bytes = 0; int files = 0; };

#ifdef __SWITCH__
/** The console's file system read directly: a directory gives up to 64 entries with their sizes in one request, where
 *  stat() asked once per file (thousands of requests for the covers cache, each one queued with everything else). */
void measureFs(FsFileSystem* fs, const std::string& path, Use& u, bool (*keep)(const std::string&), bool top) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) {
        FsFile f;   // a file, not a folder
        s64 size = 0;
        if (R_SUCCEEDED(fsFsOpenFile(fs, p, FsOpenMode_Read, &f))) {
            if (R_SUCCEEDED(fsFileGetSize(&f, &size))) { u.bytes += size; u.files++; }
            fsFileClose(&f);
        }
        return;
    }
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (top && keep && !keep(e.name)) continue;
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { u.bytes += e.file_size; u.files++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a breath between batches for the rest of Gatefold
    }
    fsDirClose(&d);
    for (const auto& n : dirs) measureFs(fs, (path == "/" ? "/" : path + "/") + n, u, keep, false);
}
#endif

void measure(const std::string& p, Use& u, bool (*keep)(const std::string&) = nullptr) {
#ifdef __SWITCH__
    if (p.rfind("sdmc:/", 0) == 0)
        if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) { measureFs(fs, p.substr(5), u, keep, true); return; }
#endif
    // The SD card serves one request at a time: a walk of thousands of files without pauses held up everything Gatefold
    // reads (covers, tags, the settings) and the app stalled. A short pause every 24 files lets the rest through.
    static thread_local int walked = 0;
    if (++walked % 24 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { u.bytes += int64_t(st.st_size); u.files++; return; }
    for (const auto& n : entries(p)) if (!keep || keep(n)) measure(joinP(p, n), u);
}

bool removeAll(const std::string& p) {
    if (!isDirP(p)) return remove(p.c_str()) == 0;
    bool ok = true;
    for (const auto& n : entries(p)) ok = removeAll(joinP(p, n)) && ok;
    return rmdir(p.c_str()) == 0 && ok;
}

bool isDownload(const std::string& n) {   // updates/: the zips (and a cut-off .part), not the release list
    return (n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0) || (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0);
}

std::string sizeText(int64_t b) {
    char s[32];
    if (b < 1024 * 1024) snprintf(s, sizeof s, "%d KB", int((b + 1023) / 1024));
    else snprintf(s, sizeof s, "%.1f MB", double(b) / (1024.0 * 1024.0));
    return s;
}

}  // namespace

std::string& App::folderPickField() {
    switch (folderFor) {
    case 0: return store.filesDir;
    case 1: return store.importDir;
    case 2: return store.exportDir;
    case 3: return store.coverDir;
    default: return store.backupDir;
    }
}

const char* App::folderPickName() const {
    static const char* names[5] = { "Files opens in", "Importing playlists opens in", "Exporting playlists opens in", "The cover picker opens in",
                                    "Backups go to" };
    return names[std::clamp(folderFor, 0, 4)];
}

void App::chooseFolder(int which) {
    folderFor = which;
    const std::string cur = folderPickField();
    navigate(View::PickFolder, -1, isDirP(cur) ? cur : which == 4 ? std::string("sdmc:/config") : std::string("sdmc:/"));
}

void App::scanStorage() {
    if (storage.thread.joinable()) {
        if (!storage.done) return;   // still measuring
        storage.thread.join();
    }
    storage.at = time;
    storage.done = false;
    const std::string dir = Store::Dir, backups = backupRoot();
    storage.thread = std::thread([this, dir, backups] {
        cores::moveHere(cores::FileOps);
        Use u[6];
        measure(dir + "/art", u[0]);
        measure(dir + "/thumbs", u[1]);
        measure(dir + "/loudness.tsv", u[2]);
        measure(dir + "/gains.bin", u[2]);
        measure(dir + "/tags.tsv", u[3]);
        for (const auto& n : entries(backups))   // only Gatefold's own backups, whatever else that folder holds
            if (struct stat st; stat(joinP(joinP(backups, n), "gatefold-backup.txt").c_str(), &st) == 0) measure(joinP(backups, n), u[4]);
        measure(dir + "/updates", u[5], isDownload);
        for (int i = 0; i < 6; i++) storage.found[i] = { u[i].bytes, u[i].files };
        storage.done = true;
    });
}

std::string App::storageValue(int which) {
    if (settingsCat == SetStorage && storage.at < -50) scanStorage();   // once each time the page opens, and after a Clear
    if (storage.done && storage.thread.joinable()) {
        storage.thread.join();
        for (int i = 0; i < 6; i++) storage.use[i] = storage.found[i];
        storage.seen = true;
    }
    if (!storage.seen) return "…";
    const StorageUse& u = storage.use[which];
    return u.files == 0 ? std::string("Empty") : sizeText(u.bytes);
}

void App::clearStorage(int which) {
    static const char* titles[6] = { "Covers cache", "Covers for the overlay", "Loudness measurements", "Tag cache", "Backups", "Downloads" };
    static const char* texts[6] = {
        "The small copies of every cover that lists show. Gatefold makes them again in the background, a few minutes for a large library.",
        "The covers the overlay shows. Made again for each queue sent to the background player, or all of them from Settings › Playback.",
        "Every track's measured loudness. With volume levelling on, the library is measured again in the background; until then tracks play unlevelled.",
        "The tags read from your files. Every file is read again now, as Library's \"Read all tags again\" does; your own tag edits stay.",
        "Every settings backup made before an install or a restore. Going back to them is no longer possible; your settings now stay.",
        "Release zips kept from an install that did not finish. The release list stays.",
    };
    confirm(which == 4 ? ICON_HISTORY : which == 5 ? ICON_DOWNLOAD : ICON_FOLDER, "Storage", titles[which],
            storage.seen ? sizeText(storage.use[which].bytes) + " on the SD card" : std::string("On the SD card"), "Clear it?", texts[which], "Clear",
            [this, which] {
                const std::string dir = Store::Dir;
                switch (which) {
                // Thousands of files: deleted on the worker (the progress stack shows it), not on the main thread, which
                // held the whole app until they were gone
                case 0:
                    if (fileWork.thread.joinable()) { toast("Wait until “" + fileWork.label + "” has finished"); return; }
                    meta.stop();
                    meta.clearCovers();
                    startFileWork("Clearing the covers cache", dir, true, [art = dir + "/art"](FileWork& w) {
                        w.total = 0;
                        removeAll(art);
                        w.result = "Covers cache cleared";
                    }, [this] { storage.at = -100; });   // library: a rescan follows, which makes them again
                    fileWork.cat = SetStorage;   // its card opens Storage
                    return;
                case 1: {
                    if (fileWork.thread.joinable()) { toast("Wait until “" + fileWork.label + "” has finished"); return; }
                    const bool was = thumbs.running();
                    thumbs.stop();
                    startFileWork("Clearing the overlay's covers", dir, false, [th = dir + "/thumbs"](FileWork& w) {
                        w.total = 0;
                        removeAll(th);
                        w.result = "Covers for the overlay cleared";
                    }, [this, was] {
                        if (was) thumbs.start(store.path("thumbs"));
                        storage.at = -100;
                    });
                    fileWork.cat = SetStorage;
                    return;
                }
                case 2:
                    levels.forget();
                    store.writeFile("loudness.tsv", levels.serialise());
                    levelEpoch = ~0u;
                    levelFirst.clear();
                    gainsDirty = true;
                    writeGains();
                    break;
                case 3:
                    meta.stop();
                    meta.clearDiskCache();
                    rescan();
                    break;
                case 4:
                    if (!startFileWork("Clearing the backups", backupRoot(), false, [root = backupRoot()](FileWork& w) {
                        w.total = 0;
                        for (const auto& n : entries(root))
                            if (struct stat st; stat(joinP(joinP(root, n), "gatefold-backup.txt").c_str(), &st) == 0) removeAll(joinP(root, n));
                        w.result = "Backups cleared";
                    }, [this] { backupsAt = -100; storage.at = -100; }))
                        return;
                    fileWork.cat = SetStorage;
                    return;
                case 5:
                    for (const auto& n : entries(dir + "/updates")) if (isDownload(n)) remove(joinP(dir + "/updates", n).c_str());
                    break;
                }
                storage.at = -100;   // measured again
                toast(std::string(titles[which]) + " cleared");
            });
}
