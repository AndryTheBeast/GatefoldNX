/*
 * Gatefold NX overlay · tune_gfbg.hpp
 * What the overlay needs beyond sys-tune's API, over the same connection (tune_gfbg.cpp).
 */
#pragma once
#include <string>
#include <vector>

#include "gfbg.h"

bool gfTrackInfo(GfbgTrackInfo* out);
bool gfConfig(GfbgConfig* out);
void gfSetConfig(const GfbgConfig& c);
bool gfGameVolume(GfbgGameVolume* out);
bool gfState(GfbgState* out);
void gfSetStopAfter(bool on);
/** The sleep timer: the music fades out and pauses after this many seconds; 0 off. */
void gfSetSleep(float seconds);
bool gfEq(GfbgEq* out);
void gfSetEq(const GfbgEq& e);
/** The names of Gatefold's playlists (GFBG_PLAYLISTS), in its order. */
std::vector<std::string> gfPlaylistNames();
/** The name of the playlist whose name hashes to `hash`, "" when there is none. */
std::string gfPlaylistName(u64 hash);

/** Gatefold's playlists as Browse › Playlists lists them (protocol 6: GFBG_PLAYLISTS' "!list" lines, the file's header
 *  only): each one's kind (GFBG_KIND_*), the icon Songate's sidebar shows (plicons.inc's key), its tracks and name. */
struct GfPlaylist { char kind = 0; std::string icon, name; u32 count = 0; };
std::vector<GfPlaylist> gfPlaylists();
/** A playlist's tracks (paths without "sdmc:", as the overlay's), and the titles and artists of `window` of them from
 *  `from` (the "!t" lines; empty where unknown): one pass over the file. `current` (a path) moves the window to start a
 *  quarter of it before that track, as the queue page's. False when there is no such playlist. */
bool gfPlaylistTracks(const std::string& name, const char* current, u32 window, std::vector<std::string>& paths, u32& from,
                      std::vector<std::pair<std::string, std::string>>& tags);
/** The queue is an Automix playlist (GfbgQueue_Automix): joined by Automix, in its own order, never shuffled. */
bool gfQueueAutomix();
/** Right after tuneClearQueue: the queue about to be filled is this playlist (its name, and whether it is an Automix one). */
void gfQueueMeta(const std::string& name, bool automix);
void gfFlush();

/** Gatefold's copy of a track's cover for the overlay (GFBG_THUMBS): GFBG_ART² RGB; false when there is none. */
bool gfLoadThumb(const char* trackPath, std::vector<u8>& rgb);
