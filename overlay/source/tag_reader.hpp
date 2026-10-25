#pragma once

#include <string>
#include <cstdio>

/**
 * Lightweight tag reader — title + artist only, no art.
 * Shared between gui_browser.cpp and gui_playlist.cpp to avoid
 * duplicate compiled code in the binary.
 */

struct TitleArtist {
    std::string title;
    std::string artist;
};

/**
 * Read title and artist from embedded tags in an audio file.
 * Supports ID3v2 (MP3), Vorbis comment (FLAC), and ID3-in-WAV.
 * Falls back to: title → filename without extension,
 *               artist → "Unknown Artist".
 */
TitleArtist readTitleArtist(const char *path);