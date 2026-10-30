/*
 * Songate · coverio.hpp
 * Covers that never fail on a file the app can play (the user, 2026-10-30: "FLAC are having issues in the switch app:
 * covers are not loading", while playback and lyrics worked): a FLAC's picture read by our own reader (libFLAC only as a
 * fallback), and an image decoded by SDL_image, else by stb_image (baseline and progressive JPEG, PNG). Dev builds keep
 * config/gatefold-nx/cover-trace.txt: each cover that was not found or did not decode, and which way it failed.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Surface;

/** A FLAC's picture (its front cover, else the first one): its image bytes, read straight from the metadata blocks (an
 *  ID3v2 tag before "fLaC" is skipped). False when the file has none or is not a FLAC. */
bool flacPicture(const std::string& path, std::vector<uint8_t>& out);

/** An image (JPEG, PNG…) from memory: SDL_image, else stb_image. `what` names it in the dev trace when both fail. */
SDL_Surface* loadImage(const void* data, size_t size, const char* what = nullptr);
inline SDL_Surface* loadImage(const std::vector<uint8_t>& bytes, const char* what = nullptr) {
    return bytes.empty() ? nullptr : loadImage(bytes.data(), bytes.size(), what);
}

/** Dev builds: one line in cover-trace.txt (at most 200 a run); nothing in a release. */
void coverTrace(const std::string& line);
