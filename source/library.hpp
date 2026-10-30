/*
 * Gatefold NX · library.hpp
 * Folder scan of the chosen music folder. Until tags arrive from the metadata
 * cache, names come from the file name ("NN Artist - Title.ext").
 *
 * Model:
 *   tracks   every playable file, sorted by path (so a folder's tracks are contiguous)
 *   folders  the directory tree under the music folder, for the Folders view
 *   albums   grouped from tags by the app (album + album artist), not by folder
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Track {
    std::string path;
    std::string subsonicCoverId;
    std::string subsonicId;   // empty for SD-card tracks
    std::string folder;
    std::string title, artist, album, date;   // from the file name until tags arrive
    std::string albumArtist, genre;   // genre: one or more, "; "-separated
    int trackNo = 0;
    double duration = 0;
    int albumIdx = -1;
    bool tagged = false;
};

struct Album {
    std::string key;   // lower-cased album + album artist
    std::string title, artist, year;
    std::vector<int> tracks;
};

struct FolderNode {
    std::string path, name;
    int parent = -1;
    std::vector<int> children;   // sub-folders, sorted by name
    std::vector<int> tracks;     // tracks directly inside
    int first = 0, last = 0;     // [first, last) range of every track below, in `tracks` order
};

struct Library {
    std::vector<std::string> roots;
    std::vector<Track> tracks;
    std::vector<Album> albums;   // filled by App::rebuildIndex
    std::vector<FolderNode> folders;
    std::vector<int> rootFolders;

    void scan(const std::vector<std::string>& candidateRoots);
    /** The folders that exist (a FAT card's spelling counts once), and every audio file below them: the folder
     *  walk, slow on an SD card, so it can run on a thread (it touches no member). */
    static std::vector<std::string> validRoots(const std::vector<std::string>& candidateRoots);
    static std::vector<std::string> listFiles(const std::vector<std::string>& roots);
    /** The library from a list of files (a walk's, or the one remembered from the last run). */
    void build(const std::vector<std::string>& roots, const std::vector<std::string>& files);
    int folderByPath(const std::string& path) const;
    std::vector<int> tracksUnder(int folder) const;
};

/** cover.jpg / folder.jpg / front.png … in a folder, read into memory. */
std::vector<uint8_t> findFolderCover(const std::string& folder);

std::string baseName(const std::string& path);

/** Sub-folders of a directory (hidden ones skipped), sorted by name; for the folder picker. */
std::vector<std::string> listDirs(const std::string& path);
/** The .m3u / .m3u8 files in a directory (names only), sorted; for + → Import a playlist file. */
std::vector<std::string> listPlaylistFiles(const std::string& path);
