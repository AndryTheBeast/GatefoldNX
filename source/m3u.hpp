/*
 * Gatefold NX · m3u.hpp
 * Playlist files: .m3u and .m3u8, read into a list of paths and written from one. Reading keeps the
 * lines as the file has them (UTF-8, or Latin-1 for an .m3u that is not valid UTF-8); matching them
 * to the library is the app's (App::resolvePlaylistFile), which knows the tracks.
 */
#pragma once
#include <string>
#include <vector>

struct M3uLine {
    std::string path;    // as written: relative, absolute, a PC's "C:\Music\…" or a file:// URL, slashes made forward
    std::string title;   // #EXTINF's "Artist - Title", when there is one
};

/** The entries of a playlist file; false when it cannot be read. */
bool readM3u(const std::string& file, std::vector<M3uLine>& out);

struct M3uTrack {
    std::string path;    // sdmc:/…
    std::string artist, title;
    double seconds = 0;
};

/** Writes an extended .m3u8 (UTF-8), each path relative to the file's folder when it is below the same root, else
 *  as "/…" from the card's root: the file then works on the console and on a PC that copies the card. */
bool writeM3u(const std::string& file, const std::string& name, const std::vector<M3uTrack>& tracks);

/** "dir/./a/../b" → "dir/b"; keeps a leading "sdmc:/". */
std::string normalisePath(const std::string& path);
