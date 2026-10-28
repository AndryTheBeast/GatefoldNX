/*
 * Gatefold NX · view_files.cpp
 * Files: a browser of the whole SD card (the sidebar, below Library; hidden until Settings → Appearance shows it).
 * Folders first, then files, each with what it is, its size and date. A opens a folder, or a file's menu; + opens
 * any row's menu; B goes to the folder above. The menu offers what fits the file (an audio file plays, an image
 * becomes a cover, an .lrc or .txt goes with a track, a playlist file plays or is imported, a folder becomes a
 * playlist or the music folder) and what every file browser does: rename, copy, move, delete (asked first) and
 * details. Copying, and deleting a folder, run on a worker and show in the progress stack.
 */
#include <dirent.h>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <unistd.h>   // rmdir
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>

#include <SDL2/SDL_image.h>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {

enum class Kind { Folder, Audio, Image, Lyrics, Playlist, Other };

std::string extOf(const std::string& name) {
    const size_t d = name.find_last_of('.');
    if (d == std::string::npos || d == 0) return "";
    std::string e = name.substr(d + 1);
    for (char& c : e) c = char(tolower((unsigned char)c));
    return e;
}

Kind kindOf(const std::string& name, bool dir) {
    if (dir) return Kind::Folder;
    const std::string e = extOf(name);
    for (const char* a : { "flac", "mp3", "ogg", "opus", "wav", "m4a", "m4b", "mp4", "aac", "alac" }) if (e == a) return Kind::Audio;
    for (const char* a : { "jpg", "jpeg", "png", "webp", "bmp" }) if (e == a) return Kind::Image;
    if (e == "lrc" || e == "txt") return Kind::Lyrics;
    if (e == "m3u" || e == "m3u8") return Kind::Playlist;
    return Kind::Other;
}

const char* kindIcon(Kind k) {
    switch (k) {
    case Kind::Folder: return ICON_FOLDER;
    case Kind::Audio: return ICON_MUSIC;
    case Kind::Image: return ICON_IMAGE;
    case Kind::Lyrics: return ICON_QUOTE;
    case Kind::Playlist: return ICON_LIST;
    default: return PLI_FILE;
    }
}

std::string kindName(Kind k, const std::string& name) {
    std::string e = extOf(name);
    for (char& c : e) c = char(toupper((unsigned char)c));
    switch (k) {
    case Kind::Folder: return "Folder";
    case Kind::Audio: return "Audio file · " + e;
    case Kind::Image: return "Image · " + e;
    case Kind::Lyrics: return "Lyrics · " + e;
    case Kind::Playlist: return "Playlist file · " + e;
    default: return e.empty() ? std::string("File") : "File · " + e;
    }
}

std::string join(const std::string& a, const std::string& b) { return a.back() == '/' ? a + b : a + "/" + b; }
std::string parentOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : p.substr(0, s);
}
bool isRoot(const std::string& p) { return p.empty() || p == "sdmc:/" || p == "sdmc:"; }
/** p is a or below it. */
bool under(const std::string& p, const std::string& a) {
    if (a.empty()) return false;
    return strcasecmp(p.c_str(), a.c_str()) == 0 ||
           (p.size() > a.size() && strncasecmp(p.c_str(), a.c_str(), a.size()) == 0 && (p[a.size()] == '/' || a.back() == '/'));
}

std::string fmtSize(uint64_t b) {
    char s[32];
    if (b < 1024) snprintf(s, sizeof s, "%u bytes", unsigned(b));
    else if (b < 1024 * 1024) snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    else snprintf(s, sizeof s, "%.2f GB", b / 1073741824.0);
    return s;
}

std::string fmtDate(int64_t t) {
    if (t <= 0) return "";
    const time_t tt = time_t(t);
    const std::tm* lt = std::localtime(&tt);
    if (!lt) return "";
    char s[32];
    std::strftime(s, sizeof s, "%d %b %Y", lt);
    return s[0] == '0' ? s + 1 : s;
}

/** A name for a file or folder: not empty, no dot first, none of what FAT refuses. */
const char* badName(const std::string& n) {
    if (n.empty()) return "";
    if (n[0] == '.') return "A name cannot start with a dot";
    if (n.find_first_of("/\\:*?\"<>|") != std::string::npos) return "A name cannot hold / \\ : * ? \" < > |";
    return nullptr;
}

std::string trimmed(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    return s;
}

bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::vector<FileEntry> listFolder(const std::string& path, bool hidden) {
    std::vector<FileEntry> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (!hidden && n[0] == '.') continue;
        FileEntry f;
        f.name = n;
        struct stat st;
        if (stat(join(path, n).c_str(), &st) == 0) {
            f.dir = S_ISDIR(st.st_mode);
            f.size = uint64_t(st.st_size);
            f.mtime = int64_t(st.st_mtime);
        } else {
            f.dir = e->d_type == DT_DIR;
        }
        out.push_back(std::move(f));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

#ifdef __SWITCH__
/** The console's file system read directly: up to 64 entries with their sizes a request, where stat() asked once a file
 *  (a folder of thousands held the app for many seconds: Details and Delete measure before they show). */
void measureFs(FsFileSystem* fs, const std::string& path, int& files, uint64_t& bytes, int cap) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) return;
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (files >= cap || R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += uint64_t(e.file_size); files++; }
        }
    }
    fsDirClose(&d);
    for (const auto& n : dirs) {
        if (files >= cap) break;
        measureFs(fs, (path == "/" ? "/" : path + "/") + n, files, bytes, cap);
    }
}
#endif

/** Files and bytes below p (p itself when a file), counting at most `cap` entries. */
void measureTree(const std::string& p, int& files, uint64_t& bytes, int cap = 50000) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { files++; bytes += uint64_t(st.st_size); return; }
#ifdef __SWITCH__
    if (p.rfind("sdmc:/", 0) == 0)
        if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) { measureFs(fs, p.substr(5), files, bytes, cap); return; }
#endif
    DIR* d = opendir(p.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (files >= cap) break;
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        measureTree(join(p, n), files, bytes, cap);
    }
    closedir(d);
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (size_t n; (n = fread(buf.data(), 1, buf.size(), in)) > 0;)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to.c_str());
    return ok;
}

bool copyTree(const std::string& from, const std::string& to, std::atomic<int>& count, std::string& failed) {
    if (!isDir(from)) {
        const bool ok = copyFile(from, to);
        if (!ok && failed.empty()) failed = baseName(from);
        count++;
        return ok;
    }
    mkdir(to.c_str(), 0777);
    DIR* d = opendir(from.c_str());
    if (!d) { if (failed.empty()) failed = baseName(from); return false; }
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    bool ok = true;
    for (const auto& n : names) ok = copyTree(join(from, n), join(to, n), count, failed) && ok;
    return ok;
}

bool removeTree(const std::string& p, std::atomic<int>& count) {
    if (!isDir(p)) { count++; return remove(p.c_str()) == 0; }
    DIR* d = opendir(p.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    bool ok = true;
    for (const auto& n : names) ok = removeTree(join(p, n), count) && ok;
    return rmdir(p.c_str()) == 0 && ok;
}

/** A free name in dir for a copy of `name`: "name", then "name (copy)", "name (copy 2)" … (before the extension). */
std::string freeName(const std::string& dir, const std::string& name, bool folder) {
    if (!exists(join(dir, name))) return name;
    const size_t dot = folder ? std::string::npos : name.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot), ext = stem == name ? "" : name.substr(dot);
    for (int i = 1; i < 1000; i++) {
        const std::string n = stem + (i == 1 ? " (copy)" : " (copy " + std::to_string(i) + ")") + ext;
        if (!exists(join(dir, n))) return n;
    }
    return name;
}

/** Folders the console needs, asked about more firmly. */
bool systemPath(const std::string& p) {
    for (const char* s : { "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/switch", "sdmc:/Nintendo", "sdmc:/config", "sdmc:/emuMMC" })
        if (under(s, p) || under(p, s)) return true;
    return false;
}

}  // namespace

// ------------------------------------------------------------------ helpers kept by App

bool App::inMusicFolder(const std::string& path) const {
    return !store.musicRoot.empty() && (under(path, store.musicRoot) || under(store.musicRoot, path));
}

std::string App::filesStart() const {
    if (!store.filesDir.empty() && isDir(store.filesDir)) return store.filesDir;
    return "sdmc:/";
}

void App::openFiles(const std::string& dir) {
    navigate(View::Files, -1, dir.empty() ? filesStart() : dir);
}

void App::remapPaths(const std::string& from, const std::string& to) {
    auto fix = [&](std::string& s) {
        if (!under(s, from)) return false;
        s = to + s.substr(from.size());
        return true;
    };
    for (auto& p : store.liked) fix(p);
    for (auto& pl : store.playlists) {
        if (pl.folder()) fix(pl.path);
        for (auto& t : pl.tracks) fix(t);
    }
    auto rekey = [&](auto& map) {
        std::vector<std::string> keys;
        for (const auto& kv : map) if (under(kv.first, from)) keys.push_back(kv.first);
        for (const auto& k : keys) {
            std::string nk = k;
            fix(nk);
            auto node = map.extract(k);
            node.key() = nk;
            map.insert(std::move(node));
        }
        return !keys.empty();
    };
    const bool e = rekey(store.edits), c = rekey(store.covers), n = rekey(store.lyricNudge);
    fix(store.musicRoot);
    for (std::string* d : { &store.filesDir, &store.importDir, &store.exportDir, &store.coverDir, &store.backupDir }) fix(*d);
    // The library's paths too: the rescan that follows keeps the play queue by path, and dropped what was renamed
    for (Track& t : lib.tracks) { fix(t.path); fix(t.folder); }
    store.save();
    if (e || c) store.saveEdits();
    if (n) store.saveLyricNudges();
}

void App::reloadNowTrack() {
    if (now.track < 0) return;
    const Track& t = lib.tracks[now.track];
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    r.readInfo = true;
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));
}

// ------------------------------------------------------------------ work on a worker (the progress stack's slot 3)

bool App::startFileWork(const std::string& label, const std::string& dest, bool library, std::function<void(FileWork&)> work,
                        std::function<void()> after) {
    if (fileWork.thread.joinable()) { toast("Wait until “" + fileWork.label + "” has finished"); return false; }
    fileWork.after = std::move(after);
    fileWork.cat = -1;
    fileWork.done = false;
    fileWork.count = 0;
    fileWork.total = 0;
    fileWork.label = label;
    fileWork.dest = dest;
    fileWork.result.clear();
    fileWork.failed.clear();
    fileWork.library = library;
    fileWork.thread = std::thread([this, work] {
        cores::moveHere(cores::FileOps);
        work(fileWork);
        fileWork.done = true;
    });
    return true;
}

void App::updateFileWork() {
    if (!fileWork.thread.joinable() || !fileWork.done) return;
    fileWork.thread.join();
    files.gen++;   // list the folder again
    toast(fileWork.failed.empty() ? fileWork.result : fileWork.result + ". Could not: " + fileWork.failed);
    if (fileWork.library) rescan();
    if (auto after = std::move(fileWork.after)) { fileWork.after = nullptr; after(); }
}

// ------------------------------------------------------------------ actions

void App::fileNewFolder(const std::string& dir) {
    std::string name;
    if (!keyboard("New folder", "", name)) return;
    name = trimmed(name);
    if (const char* bad = badName(name)) { if (*bad) toast(bad); return; }
    const std::string made = join(dir, name);
    if (exists(made)) { toast("“" + name + "” is already here"); return; }
    if (mkdir(made.c_str(), 0777) != 0) { toast("Could not make the folder “" + name + "”"); return; }
    files.gen++;
    files.focusAfter = made;
    toast("Made the folder “" + name + "”");
}

void App::fileRename(const std::string& path) {
    const std::string old = baseName(path);
    std::string name = old;
    if (!keyboard("Rename", old, name)) return;
    name = trimmed(name);
    if (name == old) return;
    if (const char* bad = badName(name)) { if (*bad) toast(bad); return; }
    const std::string to = join(parentOf(path), name);
    if (exists(to) && strcasecmp(name.c_str(), old.c_str()) != 0) { toast("“" + name + "” is already here"); return; }
    if (now.track >= 0 && under(lib.tracks[now.track].path, path)) { toast("It is playing: play something else first"); return; }
    if (rename(path.c_str(), to.c_str()) != 0) { toast("Could not rename “" + old + "”"); return; }
    files.gen++;
    files.focusAfter = to;
    if (inMusicFolder(path)) { remapPaths(path, to); rescan(); }
    toast("Renamed to “" + name + "”");
}

void App::fileDelete(const std::string& path) {
    const bool dir = isDir(path);
    const std::string name = baseName(path);
    if (now.track >= 0 && under(lib.tracks[now.track].path, path)) { toast("It is playing: play something else first"); return; }
    int n = 0;
    uint64_t bytes = 0;
    if (dir) measureTree(path, n, bytes);
    const std::string sub = dir ? "Folder · " + std::to_string(n) + (n == 1 ? " file · " : " files · ") + fmtSize(bytes)
                                : kindName(kindOf(name, false), name) + " · " + fmtSize(bytes = [&] { struct stat st; return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0; }());
    std::string text = dir ? "Everything in it is deleted from the SD card. This cannot be undone." : "It is deleted from the SD card. This cannot be undone.";
    if (inMusicFolder(path)) text += " Your library is read again after.";
    if (systemPath(path)) text = "This is part of your console's setup (or Gatefold's own settings): deleting it can stop homebrew or the console "
                                 "from working. " + text;
    confirm(dir ? ICON_FOLDER : kindIcon(kindOf(name, false)), "Delete", name, sub, dir ? "Delete this folder?" : "Delete this file?", text, "Delete",
            [this, path, name, dir, n] {
                const bool lib = inMusicFolder(path);
                if (!dir) {
                    if (remove(path.c_str()) != 0) { toast("Could not delete “" + name + "”"); return; }
                    files.gen++;
                    if (lib) rescan();
                    toast("Deleted “" + name + "”");
                    return;
                }
                startFileWork("Deleting “" + name + "”", parentOf(path), lib, [path, name, n](FileWork& w) {
                    w.total = n;
                    if (!removeTree(path, w.count)) w.failed = "delete all of it";
                    w.result = "Deleted “" + name + "”";
                });
            });
}

bool App::fileSelected(const std::string& p) const {
    return std::find(files.selected.begin(), files.selected.end(), p) != files.selected.end();
}

void App::fileSelect(const std::string& p, bool on) {
    auto it = std::find(files.selected.begin(), files.selected.end(), p);
    if (on && it == files.selected.end()) files.selected.push_back(p);
    else if (!on && it != files.selected.end()) files.selected.erase(it);
}

/** Several at once (Select): asked once, with how many and how big; deleted on the worker. */
void App::fileDeleteMany(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) { fileDelete(paths[0]); return; }
    int n = 0;
    uint64_t bytes = 0;
    bool inLib = false, sys = false;
    for (const auto& p : paths) {
        if (now.track >= 0 && under(lib.tracks[now.track].path, p)) { toast("One of them is playing: play something else first"); return; }
        measureTree(p, n, bytes);
        inLib = inLib || inMusicFolder(p);
        sys = sys || systemPath(p);
    }
    std::string text = "Everything selected is deleted from the SD card, folders with all that is in them. This cannot be undone.";
    if (inLib) text += " Your library is read again after.";
    if (sys) text = "Some of it is part of your console's setup (or Gatefold's own settings): deleting it can stop homebrew or the console "
                    "from working. " + text;
    const std::string what = std::to_string(paths.size()) + " items";
    confirm(ICON_X, "Delete", what, std::to_string(n) + (n == 1 ? " file · " : " files · ") + fmtSize(bytes), "Delete " + what + "?", text, "Delete",
            [this, paths, n, inLib, what] {
                files.selecting = false;
                files.selected.clear();
                startFileWork("Deleting " + what, parentOf(paths[0]), inLib, [paths, n, what](FileWork& w) {
                    w.total = n;
                    for (const auto& p : paths)
                        if (!removeTree(p, w.count) && w.failed.empty()) w.failed = "delete all of " + baseName(p);
                    w.result = "Deleted " + what;
                });
            });
}

void App::filePaste() {
    const std::string dir = route.query.empty() ? filesStart() : route.query;
    if (files.clip.empty()) return;
    if (files.clip.size() > 1) {
        // A selection: moves are renames, one after another; a copy is one piece of work on the worker
        std::vector<std::string> from;
        for (const auto& p : files.clip) if (exists(p)) from.push_back(p);
        if (from.empty()) { files.clip.clear(); toast("They are not there any more"); return; }
        for (const auto& p : from) if (isDir(p) && under(dir, p)) { toast(std::string("A folder cannot go into itself")); return; }
        const std::string what = std::to_string(from.size()) + " items";
        if (files.clipMove) {
            int moved = 0;
            bool library = false;
            std::string failed;
            for (const auto& p : from) {
                if (strcasecmp(parentOf(p).c_str(), dir.c_str()) == 0) continue;   // already here
                if (now.track >= 0 && under(lib.tracks[now.track].path, p)) { if (failed.empty()) failed = baseName(p) + " (playing)"; continue; }
                const std::string to = join(dir, baseName(p));
                if (exists(to) || rename(p.c_str(), to.c_str()) != 0) { if (failed.empty()) failed = baseName(p); continue; }
                if (inMusicFolder(p) || inMusicFolder(to)) { remapPaths(p, to); library = true; }
                moved++;
            }
            files.clip.clear();
            files.gen++;
            if (library) rescan();
            toast("Moved " + std::to_string(moved) + " here" + (failed.empty() ? std::string() : ". Could not: " + failed));
            return;
        }
        std::vector<std::pair<std::string, std::string>> jobs;
        bool library = false;
        for (const auto& p : from) {
            const std::string to = join(dir, freeName(dir, baseName(p), isDir(p)));
            jobs.push_back({ p, to });
            library = library || inMusicFolder(to);
        }
        startFileWork("Copying " + what, dir, library, [jobs, what](FileWork& w) {
            int n = 0;
            uint64_t bytes = 0;
            for (const auto& j : jobs) measureTree(j.first, n, bytes);
            w.total = n;
            for (const auto& j : jobs) copyTree(j.first, j.second, w.count, w.failed);
            w.result = "Copied " + what + " here";
        });
        return;
    }
    const std::string from = files.clip[0];
    if (!exists(from)) { files.clip.clear(); toast("It is not there any more"); return; }
    const bool dir0 = isDir(from), move = files.clipMove;
    const std::string name = baseName(from);
    if (dir0 && under(dir, from)) { toast(std::string("A folder cannot go into itself")); return; }
    if (move) {
        if (strcasecmp(parentOf(from).c_str(), dir.c_str()) == 0) { files.clip.clear(); return; }   // already here
        if (now.track >= 0 && under(lib.tracks[now.track].path, from)) { toast("It is playing: play something else first"); return; }
        const std::string to = join(dir, name);
        if (exists(to)) { toast("“" + name + "” is already here"); return; }
        // One SD card: a move is a rename, instant whatever its size
        if (rename(from.c_str(), to.c_str()) != 0) { toast("Could not move “" + name + "”"); return; }
        files.clip.clear();
        files.gen++;
        files.focusAfter = to;
        if (inMusicFolder(from) || inMusicFolder(to)) { remapPaths(from, to); rescan(); }
        toast("Moved “" + name + "” here");
        return;
    }
    const std::string to = join(dir, freeName(dir, name, dir0));
    files.focusAfter = to;
    startFileWork("Copying “" + name + "”", dir, inMusicFolder(to), [from, to, name](FileWork& w) {
        int n = 0;
        uint64_t bytes = 0;
        measureTree(from, n, bytes);
        w.total = n;
        copyTree(from, to, w.count, w.failed);
        w.result = "Copied “" + name + "” here";
    });
}

void App::linkLyrics(const std::string& file, int track) {
    if (track < 0 || track >= int(lib.tracks.size())) return;
    const std::string& tp = lib.tracks[size_t(track)].path;
    const size_t dot = tp.find_last_of('.');
    const std::string dest = (dot == std::string::npos ? tp : tp.substr(0, dot)) + ".lrc";
    const std::string title = lib.tracks[size_t(track)].title;
    auto doIt = [this, file, dest, track, title] {
        if (!copyFile(file, dest)) { toast("Could not write the lyrics next to the track"); return; }
        files.gen++;
        if (track == now.track) reloadNowTrack();
        toast("Lyrics linked to “" + title + "”" + (track == now.track ? "" : ": they show when it plays"));
    };
    if (strcasecmp(file.c_str(), dest.c_str()) == 0) { toast("These are already its lyrics"); return; }
    if (exists(dest)) {
        confirm(ICON_QUOTE, "Lyrics", title, baseName(dest), "Replace its lyrics file?",
                "The track already has an .lrc file next to it. It is replaced by this one.", "Replace", doIt);
        return;
    }
    doIt();
}

void App::setCoverFor(const std::string& image, const std::vector<int>& targets) {
    sheet.track = targets.empty() ? -1 : targets[0];
    sheet.wholeAlbum = false;
    if (targets.empty()) return;
    // setTrackCover copies the image into Gatefold's folder and sets it for sheet.track; the rest follow it
    setTrackCover(image);
    const auto it = store.covers.find(lib.tracks[size_t(targets[0])].path);
    if (it == store.covers.end()) return;
    for (int t : targets) {
        store.covers[lib.tracks[size_t(t)].path] = it->second;
        meta.setCoverFile(t, it->second);
    }
    store.saveEdits();
    if (std::find(targets.begin(), targets.end(), now.track) != targets.end()) loadTrackCoverAgain();
}

// ------------------------------------------------------------------ the page

void App::drawFiles(float x, float y, float w, float h) {
    if (route.query.empty()) route.query = filesStart();
    std::string path = route.query;
    const bool root = isRoot(path);
    if (files.listed != path || files.listedHidden != store.showHidden || files.listedGen != files.gen) {
        // Looked at only when listed (a stat every frame waited on the SD card each time)
        if (!isDir(path)) path = route.query = "sdmc:/";   // gone (deleted, the card changed)
        files.entries = listFolder(path, store.showHidden);
        files.listed = path;
        files.listedHidden = store.showHidden;
        files.listedGen = files.gen;
    }
    const std::vector<FileEntry>& es = files.entries;
    int nDirs = 0;
    for (const auto& e : es) nDirs += e.dir;
    const int nFiles = int(es.size()) - nDirs;

    ScrollArea& sa = area("files", int(uid(path) & 0x7fffffff));
    const float headH = 18 + 8 + 10 + 38 + 22 + 16 + 34 + 16, rowH = 52;
    sa.view = { x, y, w, h };
    sa.content = headH + es.size() * rowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    float cy = oy + 18 + 8;
    kicker("Files", x + 28, cy, pal.text3);
    cy += 10;
    g.text(root ? "SD card" : baseName(path), { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    std::string sub = path + "  ·  " + std::to_string(nDirs) + (nDirs == 1 ? " folder" : " folders") + "  ·  " + std::to_string(nFiles) +
                      (nFiles == 1 ? " file" : " files");
    if (store.showHidden) sub += "  ·  hidden files shown";
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    if (files.selectIn != path) { files.selecting = false; files.selected.clear(); files.selectIn = path; }   // a selection stays in its folder
    if (files.selecting) {
        // Select: Select all / none, then what to do with them all
        const bool all = !es.empty() && files.selected.size() == es.size();
        bx += button(uid("fl.selall"), ICON_CHECK, all ? "Select none" : "Select all", bx, cy, false, &clicked) + 8;
        if (clicked) {
            files.selected.clear();
            if (!all) for (const auto& e : es) files.selected.push_back(join(path, e.name));
        }
        const bool any = !files.selected.empty();
        auto take = [&](bool move) {
            files.clip = files.selected;
            files.clipMove = move;
            files.selecting = false;
            files.selected.clear();
            toast(std::string("Go to a folder and press ") + (move ? "Move here" : "Paste here"));
        };
        bx += button(uid("fl.selcopy"), ICON_DOWNLOAD, "Copy", bx, cy, false, &clicked) + 8;
        if (clicked) { if (any) take(false); else toast("Select something first"); }
        bx += button(uid("fl.selmove"), ICON_FOLDER, "Move", bx, cy, false, &clicked) + 8;
        if (clicked) { if (any) take(true); else toast("Select something first"); }
        bx += button(uid("fl.selno"), nullptr, "Cancel", bx, cy, false, &clicked) + 8;
        if (clicked) { files.selecting = false; files.selected.clear(); }
        bx += button(uid("fl.seldel"), ICON_X, "Delete", bx, cy, false, &clicked, false, true) + 8;   // Delete always last
        if (clicked) { if (any) fileDeleteMany(files.selected); else toast("Select something first"); }
        const size_t n = files.selected.size();
        g.textC(n == 0 ? std::string("A ticks what to take") : std::to_string(n) + " selected", { css::Sm }, x + w - 28, cy + 17,
                n ? pal.accentInk : pal.text3, 1, Align::Right, int(std::max(80.f, x + w - 28 - bx - 16)));
    } else if (!files.clip.empty()) {
        bx += button(uid("fl.paste"), files.clipMove ? ICON_FOLDER : ICON_DOWNLOAD, files.clipMove ? "Move here" : "Paste here", bx, cy, true, &clicked) + 8;
        if (clicked) filePaste();
        bx += button(uid("fl.unclip"), ICON_X, "Cancel", bx, cy, false, &clicked) + 8;
        if (clicked) { files.clip.clear(); toast(files.clipMove ? "Not moving it" : "Not copying it"); }
    }
    if (!root && !files.selecting) {
        bx += button(uid("fl.up"), ICON_BACK, "Up", bx, cy, false, &clicked, false, false, es.empty() ? DefaultFocus : 0) + 8;
        if (clicked) {
            files.focusAfter = path;
            navigate(View::Files, -1, parentOf(path), false);
        }
    }
    if (!files.selecting) {
        bx += button(uid("fl.mkdir"), ICON_PLUS, "New folder", bx, cy, false, &clicked, false, false, root && es.empty() ? DefaultFocus : 0) + 8;
        if (clicked) fileNewFolder(path);
        if (!es.empty()) {
            bx += button(uid("fl.select"), ICON_CHECK, "Select", bx, cy, false, &clicked) + 8;
            if (clicked) { files.selecting = true; files.selected.clear(); }
        }
    }
    // what is waiting to be pasted, at the right
    if (!files.clip.empty() && !files.selecting)
        g.textC((files.clipMove ? "Moving: " : "Copying: ") + (files.clip.size() == 1 ? baseName(files.clip[0]) : std::to_string(files.clip.size()) + " items"),
                { css::Sm }, x + w - 28, cy + 17, pal.text3, 1, Align::Right, int(std::max(80.f, x + w - 28 - bx - 16)));
    cy += 34 + 16;

    g.clip(x, y, w, h);
    files.hover.clear();
    for (int i = 0; i < int(es.size()); i++) {
        const FileEntry& e = es[size_t(i)];
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        const std::string full = join(path, e.name);
        const Id id = uid("fl.row", int(uid(full) & 0x7fffffff));
        if (!files.focusAfter.empty() && strcasecmp(files.focusAfter.c_str(), full.c_str()) == 0) {
            ui.focus(id);
            sa.target = ry - oy - (h - rowH) / 2;
            sa.clamp();
            files.focusAfter.clear();
        }
        const bool hit = ui.item(id, { rx, ry, rw, rowH - 4 }, 10, i == 0 ? DefaultFocus : 0);
        if (ui.focused(id)) files.hover = full;
        const bool picked = files.selecting && fileSelected(full);
        if (hit) {
            if (files.selecting) fileSelect(full, !picked);   // Select: A ticks it (a folder too, without opening it)
            else if (e.dir) { files.focusAfter.clear(); navigate(View::Files, -1, full, false); }
            else openFileMenu(full);
        }
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        const Kind k = kindOf(e.name, e.dir);
        const bool known = k != Kind::Other && k != Kind::Folder;
        const bool clipped = std::any_of(files.clip.begin(), files.clip.end(), [&](const std::string& c) { return strcasecmp(c.c_str(), full.c_str()) == 0; });
        const bool hidden = e.name[0] == '.';
        if (picked) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.accent, 0.12f);
        g.roundRect(rx + 16, mid - 18, 36, 36, 8, known || picked ? pal.accent : pal.surface2, picked ? 0.9f : known ? 0.16f : 1.f);
        g.icon(picked ? ICON_CHECK : kindIcon(k), rx + 34, mid, 18, picked ? pal.onAccent : known ? pal.accentInk : pal.text2, hidden ? 0.5f : 1.f);
        std::string info = e.dir ? fmtDate(e.mtime) : fmtSize(e.size) + (e.mtime > 0 ? "  ·  " + fmtDate(e.mtime) : "");
        if (clipped) info = (files.clipMove ? "Moving" : "Copying") + std::string("  ·  ") + info;
        const int iw = g.measure(info, { css::Sm });
        g.textC(e.name, { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, hidden ? pal.text3 : pal.text, 1, Align::Left,
                int(rw - 16 - 36 - 14 - iw - 70));
        g.textC(info, { css::Sm }, rx + rw - 44, mid, clipped ? pal.accentInk : pal.text3, 1, Align::Right);
        if (files.selecting && picked) g.circle(rx + rw - 24, mid, 9, pal.accent);
        else if (files.selecting) g.roundStroke(rx + rw - 33, mid - 9, 18, 18, 9, 1.5f, pal.text3, 0.6f);
        else if (e.dir) g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    if (es.empty()) g.textC("This folder is empty", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}

// ------------------------------------------------------------------ the menu (the sheet's card)

void App::openFileMenu(const std::string& path) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::FileMenu;
    sheet.dir = path;
    sheet.returnFocus = ui.focusedId();
    {   // read once: the sheet's rows and header are built every frame
        struct stat st;
        const bool ok = stat(path.c_str(), &st) == 0;
        sheet.memo["exists"] = ok ? "1" : "";
        sheet.memo["dir"] = ok && S_ISDIR(st.st_mode) ? "1" : "";
        sheet.memo["sub"] = !ok || S_ISDIR(st.st_mode) ? "" : fmtSize(uint64_t(st.st_size)) + (st.st_mtime > 0 ? "  ·  " + fmtDate(int64_t(st.st_mtime)) : "");
    }
    // an image shows itself in the header's tile
    if (kindOf(baseName(path), !sheet.memo["dir"].empty()) == Kind::Image) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size < 16 * 1024 * 1024)
            if (SDL_Surface* s = [&]() -> SDL_Surface* {
                    std::vector<uint8_t> bytes;
                    if (FILE* fp = fopen(path.c_str(), "rb")) {
                        bytes.resize(size_t(st.st_size));
                        bytes.resize(fread(bytes.data(), 1, bytes.size(), fp));
                        fclose(fp);
                    }
                    return bytes.empty() ? nullptr : IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1);
                }()) {
                if (SDL_Surface* small = Gfx::shrink(s, 128)) {
                    sheet.preview = SDL_CreateTextureFromSurface(renderer_, small);
                    if (sheet.preview) SDL_SetTextureScaleMode(sheet.preview, SDL_ScaleModeLinear);
                    SDL_FreeSurface(small);
                }
                SDL_FreeSurface(s);
            }
    }
    ui.focusDefault();
}

void App::fileSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const std::string& p = sheet.dir;
    const bool dir = !sheet.memo["dir"].empty();
    const Kind k = kindOf(baseName(p), dir);
    icon = kindIcon(k);
    kick = sheet.page == Sheet::FileLink ? "Lyrics for a track" : sheet.page == Sheet::FileInfo ? "Details" : kindName(k, baseName(p));
    title = isRoot(p) ? "SD card" : baseName(p);
    if (sheet.page == Sheet::FileLink) { sub = sheet.arg.empty() ? "Every track" : "Tracks matching “" + sheet.arg + "”"; return; }
    sub = dir ? parentOf(p) : sheet.memo["sub"];
}

void App::fileSheetRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    const std::string p = sheet.dir;
    if (sheet.memo["exists"].empty()) { closeSheet(); return; }
    const bool dir = !sheet.memo["dir"].empty();
    const std::string name = baseName(p);
    const Kind k = kindOf(name, dir);

    if (sheet.page == Sheet::FileInfo) {
        // What it is, how big, when, where: worked out once when the page opens (a big folder takes a moment)
        if (sheet.entries.empty()) {
            int n = 0;
            uint64_t bytes = 0;
            measureTree(p, n, bytes);
            struct stat st;
            const int64_t mt = stat(p.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
            sheet.entries = { kindName(k, name), dir ? fmtSize(bytes) + " in " + std::to_string(n) + (n == 1 ? " file" : " files") + (n >= 50000 ? " or more" : "")
                                                     : fmtSize(bytes) + (bytes >= 1024 ? " (" + std::to_string(bytes) + " bytes)" : ""),
                              mt > 0 ? fmtDate(mt) : "Unknown", parentOf(p) };
        }
        const char* labels[4] = { "Kind", "Size", "Modified", "Where" };
        for (int i = 0; i < 4; i++) {
            rows.push_back({ Row::Name, nullptr, labels[i], "", sheet.entries[size_t(i)], nullptr });
            rows.back().h = 46;
        }
        const int t = pathIndex.count(p) ? pathIndex.at(p) : -1;
        if (t >= 0) {
            const Track& tr = lib.tracks[size_t(t)];
            rows.push_back({ Row::Name, nullptr, "In your library", "", tr.title + (tr.artist.empty() ? "" : " · " + tr.artist), nullptr });
            rows.back().h = 46;
        }
        return;
    }

    if (sheet.page == Sheet::FileLink) {
        // The tracks to link the lyrics with: the search's, or every track (the playing one first)
        std::vector<int> found;
        if (now.track >= 0) found.push_back(now.track);
        std::string q = sheet.arg;
        for (char& c : q) c = char(tolower((unsigned char)c));
        for (int i = 0; i < int(lib.tracks.size()) && found.size() < 200; i++) {
            if (i == now.track) continue;
            const Track& t = lib.tracks[size_t(i)];
            std::string hay = t.title + " " + t.artist + " " + t.album + " " + baseName(t.path);
            for (char& c : hay) c = char(tolower((unsigned char)c));
            if (q.empty() || hay.find(q) != std::string::npos) found.push_back(i);
        }
        rows.push_back({ Row::Action, ICON_SEARCH, sheet.arg.empty() ? "Find a track…" : "Find another…", "By title, artist, album or file name", "", [this] {
                             std::string q2 = sheet.arg;
                             if (keyboard("Find the track", q2, q2)) sheet.arg = trimmed(q2);
                         } });
        for (int t : found) {
            const Track& tr = lib.tracks[size_t(t)];
            rows.push_back({ Row::Action, t == now.track ? ICON_DISC : ICON_MUSIC, tr.title, tr.artist + (tr.album.empty() ? "" : " · " + tr.album),
                             t == now.track ? "Playing" : "", [this, p, t] {
                                 closeSheet();
                                 linkLyrics(p, t);
                             } });
        }
        if (found.empty()) { rows.push_back({ Row::Group, nullptr, "No track matches" }); rows.back().h = 40; }
        return;
    }

    // ---- FileMenu: what fits this kind first
    const int f = dir ? lib.folderByPath(p) : -1;
    const int t = !dir && pathIndex.count(p) ? pathIndex.at(p) : -1;
    if (dir) {
        rows.push_back({ Row::Action, ICON_CHEV, "Open", "", "", [this, p] { closeSheet(); navigate(View::Files, -1, p, false); } });
        if (f >= 0) {
            const std::vector<int> all = lib.tracksUnder(f);
            const std::string fname = baseName(p);
            rows.push_back({ Row::Action, ICON_PLAY, "Play", std::to_string(all.size()) + " tracks from your library", "", [this, all, fname] {
                                 closeSheet();
                                 if (!all.empty()) playTracks(all, 0, fname);
                             } });
            rows.push_back({ Row::Action, ICON_SHUFFLE, "Shuffle", "", "", [this, all, fname] {
                                 closeSheet();
                                 if (!all.empty()) playTracks(all, 0, fname, true);
                             } });
            if (store.playlistByPath(p) < 0)
                rows.push_back({ Row::Action, ICON_PLUS, "Add as a playlist", "Kept in step with the folder, in the sidebar", "", [this, p, fname] {
                                     const std::string as = store.freePlaylistName(fname);
                                     store.playlists.push_back({ as, p, {} });
                                     store.save();
                                     closeSheet();
                                     toast("Added “" + as + "” to playlists");
                                 } });
            rows.push_back({ Row::Action, ICON_FOLDER, "Show in Folders", "", "", [this, f] { closeSheet(); if (tabFor(Store::TabFolders, "folder")) navigate(View::Folders, f); } });
        }
        if (strcasecmp(p.c_str(), store.musicRoot.c_str()) != 0)
            rows.push_back({ Row::Action, ICON_GRID, "Use as the music folder", "Gatefold reads your library from here (Settings · Library)", "", [this, p] {
                                 closeSheet();
                                 useMusicFolder(p);
                             } });
    } else if (k == Kind::Audio) {
        if (t >= 0) {
            const Track& tr = lib.tracks[size_t(t)];
            rows.push_back({ Row::Action, ICON_PLAY, "Play", "With the rest of its folder", "", [this, t] {
                                 closeSheet();
                                 const int fo = lib.folderByPath(lib.tracks[size_t(t)].folder);
                                 const std::vector<int> all = fo >= 0 ? lib.folders[size_t(fo)].tracks : std::vector<int>{ t };
                                 const int at = int(std::find(all.begin(), all.end(), t) - all.begin());
                                 playTracks(all, at < int(all.size()) ? at : 0, baseName(lib.tracks[size_t(t)].folder));
                             } });
            // As the track menu words and draws them
            rows.push_back({ Row::Action, ICON_NEXT, "Play next", "", "", [this, t] { closeSheet(); queueAdd(t, true); toast("Plays next"); } });
            rows.push_back({ Row::Action, ICON_QUEUE, "Add to the end of the playlist", "", "", [this, t] {
                                 closeSheet();
                                 queueAdd(t, false);
                                 toast("Added to the playlist");
                             } });
            rows.push_back({ Row::Action, ICON_LIST, "Add to a playlist…", "", "", [this, t] {
                                 openTrackMenu(t);
                                 sheet.page = Sheet::Lists;
                                 ui.focusDefault();
                             } });
            if (tr.albumIdx >= 0)
                rows.push_back({ Row::Action, ICON_V_GRID, "Show its album", tr.album, "", [this, a = tr.albumIdx] { closeSheet(); goToAlbum(a); } });
            rows.push_back({ Row::Action, ICON_MORE, "Track menu…", "Tags, cover, details", "", [this, t] { openTrackMenu(t); } });
        } else {
            rows.push_back({ Row::Action, ICON_INFO, "Not in your library", "Gatefold plays what is in its music folder (Settings · Library)", "", nullptr });
        }
    } else if (k == Kind::Image) {
        const std::string img = p;
        if (now.track >= 0) {
            const Track& tr = lib.tracks[size_t(now.track)];
            if (tr.albumIdx >= 0)
                rows.push_back({ Row::Action, ICON_V_GRID, "Cover of the album playing", tr.album, "", [this, img] {
                                     closeSheet();
                                     setCoverFor(img, lib.albums[size_t(lib.tracks[size_t(now.track)].albumIdx)].tracks);
                                 } });
            rows.push_back({ Row::Action, ICON_DISC, "Cover of the track playing", tr.title, "", [this, img] {
                                 closeSheet();
                                 setCoverFor(img, { now.track });
                             } });
        }
        const int fo = lib.folderByPath(parentOf(p));
        if (fo >= 0 && !lib.folders[size_t(fo)].tracks.empty())
            rows.push_back({ Row::Action, ICON_FOLDER, "Cover of the music in this folder",
                             std::to_string(lib.folders[size_t(fo)].tracks.size()) + " tracks", "", [this, img, fo] {
                                 closeSheet();
                                 setCoverFor(img, lib.folders[size_t(fo)].tracks);
                             } });
        rows.push_back({ Row::Action, ICON_INFO, "Kept by Gatefold", "A copy goes in Gatefold's folder; your music files are not changed", "", nullptr });
    } else if (k == Kind::Lyrics) {
        if (now.track >= 0)
            rows.push_back({ Row::Action, ICON_DISC, "Lyrics of the track playing", lib.tracks[size_t(now.track)].title, "", [this, p] {
                                 closeSheet();
                                 linkLyrics(p, now.track);
                             } });
        rows.push_back({ Row::Action, ICON_QUOTE, "Lyrics of a track…", "Copied next to it as an .lrc file with its name", "", [this] {
                             sheet.page = Sheet::FileLink;
                             sheet.arg.clear();
                             ui.focusDefault();
                         } });
    } else if (k == Kind::Playlist) {
        rows.push_back({ Row::Action, ICON_PLAY, "Play or import…", "Its tracks found in your library", "", [this, p] { openPlaylistFile(p); } });
    }

    // ---- what every file browser does
    rows.push_back({ Row::Group, nullptr, dir ? "Folder" : "File" });
    rows.back().h = 34;
    rows.push_back({ Row::Action, ICON_TAG, "Rename", "", "", [this, p] { closeSheet(); fileRename(p); } });
    rows.push_back({ Row::Action, ICON_CHECK, "Select", "Tick more, then copy, move or delete them together", "", [this, p] {
                         closeSheet();
                         files.selecting = true;
                         files.selected = { p };
                     } });
    rows.push_back({ Row::Action, ICON_DOWNLOAD, "Copy", "Then Paste here in another folder", "", [this, p, name] {
                         files.clip = { p };
                         files.clipMove = false;
                         closeSheet();
                         toast("Go to a folder and press Paste here");
                     } });
    rows.push_back({ Row::Action, ICON_FOLDER, "Move", "Then Move here in another folder", "", [this, p] {
                         files.clip = { p };
                         files.clipMove = true;
                         closeSheet();
                         toast("Go to a folder and press Move here");
                     } });
    rows.push_back({ Row::Action, ICON_INFO, "Details", "Size, date and where it is", "", [this] {
                         sheet.page = Sheet::FileInfo;
                         sheet.entries.clear();
                         ui.focusDefault();
                     } });
    rows.push_back({ Row::Action, ICON_X, "Delete", dir ? "The folder and everything in it" : "From the SD card", "", [this, p] {
                         closeSheet();
                         fileDelete(p);
                     } });
}
