/*
 * Songate · mood.hpp
 * Mood playlists: the library sorted into moods by how each track sounds (mixan::Mood, measured with Automix's analysis).
 * Our own, from features we measure (the PC's Gatefold uses a neural network, Essentia's, licensed non-commercial; its radio
 * uses features like these, vibe.js): Thayer's two axes, as most feature-based mood work does, each feature a rank within
 * the library (0..1) so the library's own spread decides, not one loud master:
 *   energy: busy (onsets), drums, bright, changing, loud, little quiet music, a faster beat
 *   positivity: major rather than minor, bright, a steady danceable beat
 * then five moods (the PC's): Happy, Party, Relaxed, Sad, Aggressive. Each mood's score is compared with the library's
 * (a z-score), so no mood takes every track; each track belongs to its best one, ordered by how strongly.
 * Tried on the user's library (3,074 songs) with tools/preview's mixtrain mood and a script before it went in.
 */
#pragma once
#include <string>
#include <vector>

#include "mixan.hpp"

namespace mood {

enum Mood { Happy, Party, Relaxed, Sad, Aggressive, Count };

const char* name(int m);   // "Happy"
const char* desc(int m);   // a line for the New playlist sheet

/** The library's moods: built from every track's features (nullptr: not measured yet), on the main thread when they change
 *  (a few ms for thousands of tracks: ranks and sums, nothing read from the card). */
struct Index {
    std::vector<signed char> best;   // per track: its mood, -1 not known yet
    std::vector<float> score;        // … and how strongly (the mood's z-score)
    int known = 0;
    void build(const std::vector<const mixan::Mood*>& tracks);
};

}  // namespace mood
