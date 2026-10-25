/*
 * Gatefold NX · metadata.hpp
 * Tags, duration, stream format and embedded cover art, read straight from the
 * codec libraries (SDL_mixer 2.0 exposes none of this).
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TrackInfo {
    std::string title, artist, album, date;
    std::string albumArtist, genre;
    int trackNo = 0;
    std::string codec;          // "FLAC", "MP3", "Opus", "Vorbis", "WAV"
    double duration = 0;        // seconds, 0 if unknown
    int sampleRate = 0;
    int bitsPerSample = 0;      // lossless only
    int bitrateKbps = 0;        // lossy only
    std::vector<uint8_t> cover; // encoded image (JPEG/PNG), empty if none
    std::string lyrics;         // LRC or plain text from tags
};

/** Call once on the main thread before any reader thread starts. */
void metadataInit();

/** Fills what the file provides; missing fields stay empty. Thread-safe.
 *  wantCover / wantLyrics skip the heavier parts when they are not needed. */
bool readTrackInfo(const std::string& path, TrackInfo& out, bool wantCover = true, bool wantLyrics = false);

/** Lyrics from an .lrc (or .txt) file beside the track; empty if none. */
std::string readSidecarLyrics(const std::string& path);

/** ID3 genres come as "(17)", "(17)Rock" or "17": the name, from the ID3v1 list when only a number is given. */
std::string cleanGenre(const std::string& g);
