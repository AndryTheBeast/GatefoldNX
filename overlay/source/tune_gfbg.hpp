/*
 * Gatefold NX overlay · tune_gfbg.hpp
 * What the overlay needs beyond sys-tune's API, over the same connection (tune_gfbg.cpp).
 */
#pragma once
#include <vector>

#include "gfbg.h"

bool gfTrackInfo(GfbgTrackInfo* out);
bool gfConfig(GfbgConfig* out);
void gfSetConfig(const GfbgConfig& c);
bool gfGameVolume(GfbgGameVolume* out);
bool gfState(GfbgState* out);
void gfSetStopAfter(bool on);
void gfFlush();

/** Gatefold's copy of a track's cover for the overlay (GFBG_THUMBS): GFBG_ART² RGB; false when there is none. */
bool gfLoadThumb(const char* trackPath, std::vector<u8>& rgb);
