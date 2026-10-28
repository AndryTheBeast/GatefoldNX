/*
 * Gatefold NX · library.cpp
 */
#include "library.hpp"
#include "metadata.hpp"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unordered_map>

namespace {

const char* const kAudioExt[] = { "flac", "mp3", "ogg", "opus", "wav", "m4a", "m4b", "mp4", "aac", "alac" };
bool isPlaylistExt(const std::string& ext) { return ext == "m3u" || ext == "m3u8"; }
const char* const kCoverNames[] = { "cover", "folder", "front", "album", "albumart" };
const char* const kImageExt[] = { "jpg", "jpeg", "png" };

std::string extOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = char(tolower(c));
    return e;
}

std::string parentOf(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string() : path.substr(0, s);
}

/** "01 - Title", "01. Title", "1-03 Title" → "Title". */
std::string stripTrackNumber(std::string s) {
    size_t i = 0;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-') && i < 6) i++;
    if (i == 0 || i == s.size()) return s;
    size_t j = i;
    while (j < s.size() && (s[j] == ' ' || s[j] == '.' || s[j] == '-' || s[j] == '_')) j++;
    return (j > i || s[i - 1] == '-') && j < s.size() ? s.substr(j) : s;
}

/** Every audio file below dir, as full paths (the folder walk: the slow part on an SD card). */
void walk(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 10) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        const std::string full = dir + "/" + name;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            isDir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir) { walk(full, out, depth + 1); continue; }
        const std::string ext = extOf(name);
        if (std::none_of(std::begin(kAudioExt), std::end(kAudioExt), [&](const char* x) { return ext == x; })) continue;
        out.push_back(full);
    }
    closedir(d);
}

/** A track as the file name and folders tell it, until its tags arrive. */
Track makeTrack(const std::string& full, const std::string& root) {
    const std::string dir = parentOf(full);
    const std::string name = full.substr(dir.size() + 1);
    const std::string ext = extOf(name);
    Track t;
    t.path = full;
    t.folder = dir;
    t.title = stripTrackNumber(name.substr(0, name.size() - ext.size() - 1));
    // "Artist - Title" is the usual name for files in playlist folders.
    const size_t dash = t.title.find(" - ");
    if (dash != std::string::npos && dash > 0 && dash + 3 < t.title.size()) {
        t.artist = t.title.substr(0, dash);
        t.title = t.title.substr(dash + 3);
    }
    // Until tags are read, a two-level layout (Artist/Album/file) is the best guess at the album; any other layout gives none.
    const std::string parent = parentOf(dir);
    if (dir != root && parent != root && parent.size() > root.size()) {
        t.album = baseName(dir);
        if (t.artist.empty()) t.artist = baseName(parent);
    }
    return t;
}

bool pathLess(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

}  // namespace

std::string baseName(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::vector<std::string> Library::validRoots(const std::vector<std::string>& candidateRoots) {
    std::vector<std::string> out;
    for (const auto& r : candidateRoots) {
        struct stat st;
        if (stat(r.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        // FAT is case-insensitive: sdmc:/music and sdmc:/Music are the same folder.
        if (std::any_of(out.begin(), out.end(), [&](const std::string& x) { return strcasecmp(x.c_str(), r.c_str()) == 0; })) continue;
        out.push_back(r);
    }
    return out;
}

std::vector<std::string> Library::listFiles(const std::vector<std::string>& roots) {
    std::vector<std::string> files;
    for (const auto& r : roots) walk(r, files, 0);
    return files;
}

void Library::scan(const std::vector<std::string>& candidateRoots) {
    const std::vector<std::string> r = validRoots(candidateRoots);
    build(r, listFiles(r));
}

void Library::build(const std::vector<std::string>& rootList, const std::vector<std::string>& files) {
    roots = rootList;
    tracks.clear();
    albums.clear();
    folders.clear();
    rootFolders.clear();
    tracks.reserve(files.size());
    for (const auto& f : files)
        if (!isPlaylistExt(extOf(f)))   // a file list remembered by a build that listed them (1.2.0 dev)
        for (const auto& r : roots)
            if (f.size() > r.size() && f.compare(0, r.size(), r) == 0 && f[r.size()] == '/') { tracks.push_back(makeTrack(f, r)); break; }
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return pathLess(a.path, b.path); });

    // Folder tree: every directory between a root and a track.
    std::unordered_map<std::string, int> byPath;
    auto node = [&](const std::string& path, auto&& self) -> int {
        auto it = byPath.find(path);
        if (it != byPath.end()) return it->second;
        FolderNode n;
        n.path = path;
        n.name = baseName(path);
        const bool isRoot = std::find(roots.begin(), roots.end(), path) != roots.end();
        const int parent = isRoot ? -1 : self(parentOf(path), self);
        n.parent = parent;
        const int idx = int(folders.size());
        folders.push_back(std::move(n));
        byPath[path] = idx;
        if (parent >= 0) folders[parent].children.push_back(idx);
        else rootFolders.push_back(idx);
        return idx;
    };
    for (const auto& r : roots) node(r, node);

    for (int i = 0; i < int(tracks.size()); i++) folders[node(tracks[i].folder, node)].tracks.push_back(i);

    // Track ranges: a folder's tracks are contiguous because tracks are sorted by path.
    for (auto& f : folders) { f.first = int(tracks.size()); f.last = 0; }
    for (int i = 0; i < int(tracks.size()); i++)
        for (int f = byPath[tracks[i].folder]; f >= 0; f = folders[f].parent) {
            folders[f].first = std::min(folders[f].first, i);
            folders[f].last = std::max(folders[f].last, i + 1);
        }
    for (auto& f : folders) {
        if (f.last == 0) f.first = 0;
        std::sort(f.children.begin(), f.children.end(), [&](int a, int b) { return pathLess(folders[a].name, folders[b].name); });
    }
}

std::vector<std::string> listDirs(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            isDir = stat((path + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir) out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), pathLess);
    return out;
}

std::vector<std::string> listPlaylistFiles(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || e->d_type == DT_DIR || !isPlaylistExt(extOf(name))) continue;
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), pathLess);
    return out;
}

int Library::folderByPath(const std::string& path) const {
    for (int i = 0; i < int(folders.size()); i++)
        if (strcasecmp(folders[i].path.c_str(), path.c_str()) == 0) return i;
    return -1;
}

std::vector<int> Library::tracksUnder(int folder) const {
    std::vector<int> out;
    if (folder < 0 || folder >= int(folders.size())) return out;
    for (int i = folders[folder].first; i < folders[folder].last; i++) out.push_back(i);
    return out;
}

std::vector<uint8_t> findFolderCover(const std::string& folder) {
    DIR* d = opendir(folder.c_str());
    if (!d) return {};
    std::string best;
    int bestRank = 1 << 30;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        const std::string ext = extOf(name);
        if (std::none_of(std::begin(kImageExt), std::end(kImageExt), [&](const char* x) { return ext == x; })) continue;
        const std::string stem = name.substr(0, name.size() - ext.size() - 1);
        // Known names rank by list order; any other image is a last resort.
        int rank = 100;
        for (int i = 0; i < int(std::size(kCoverNames)); i++)
            if (strcasecmp(stem.c_str(), kCoverNames[i]) == 0) { rank = i; break; }
        if (rank < bestRank) { bestRank = rank; best = name; }
    }
    closedir(d);
    if (best.empty()) return {};

    std::vector<uint8_t> buf;
    if (FILE* f = fopen((folder + "/" + best).c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 32 * 1024 * 1024) {
            buf.resize(size_t(n));
            if (fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
        }
        fclose(f);
    }
    return buf;
}

std::string cleanGenre(const std::string& g) {
    static const char* v1[] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop",
        "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
        "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip",
        "Gospel", "Noise", "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic",
        "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
        "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer",
        "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock" };
    std::string s = g;
    while (!s.empty() && s[0] == '(') {   // "(17)Rock" → "Rock"; "(17)" → the list's name
        const size_t close = s.find(')');
        if (close == std::string::npos) break;
        const std::string num = s.substr(1, close - 1);
        s.erase(0, close + 1);
        if (s.empty() && !num.empty() && isdigit((unsigned char)num[0])) s = num;
    }
    if (!s.empty() && s.size() <= 3 && std::all_of(s.begin(), s.end(), [](char c) { return isdigit((unsigned char)c); })) {
        const int n = atoi(s.c_str());
        return n < int(sizeof v1 / sizeof *v1) ? v1[n] : std::string();
    }
    return s;
}
