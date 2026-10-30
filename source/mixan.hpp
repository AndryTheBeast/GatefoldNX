/*
 * Gatefold NX · mixan.hpp
 * Automix's analysis of a track, made while the levelling worker decodes it (loudness.cpp's measureFile):
 * the tempo and a beat grid at each end, where the music really starts and ends, whether it fades out,
 * and how much bass sits at each end. Gatefold's own player plans each transition from two of these
 * (automix.hpp); the background player keeps its plain crossfade.
 *
 * How: the mono signal is cut down to about 11 kHz; every 128 samples (~11 ms) a 1024-point spectrum gives
 * an onset strength (spectral flux of the log magnitudes) and the frame's loudness and bass share. At the
 * end the tempo comes from the onset strength's autocorrelation (with harmonics, and a prior around
 * 120 BPM), the beats from dynamic programming (Ellis, "Beat tracking by dynamic programming", 2007), and
 * a grid is fitted to the beats of the first and of the last minute. Our own code: the good open beat
 * trackers are GPL.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mixan {

constexpr int Version = 4;   // mix.tsv: results of another version are made again (2: downbeats, bars, phrases; 3: keys; 4: bass per bar)

/** How a track sounds, for Mood playlists (mood.hpp sorts the library by these): measured in the same pass as the rest,
 *  kept apart (mood.tsv, its own version) so Automix's analysis never changes for it. After Gatefold's vibe.js (PC), whose
 *  features it follows, plus the key's mode and the beat. */
struct Mood {
    static constexpr int Version = 1;   // mood.tsv: another version's are measured again
    bool valid = false;
    float loud = -99;     // the body's level, dB (Info::level)
    float dyn = 0;        // how much the loudness moves: the 1-s level's spread (90th minus 10th percentile), dB
    float calm = 0;       // the share of the music 6 dB or more under the body
    float bright = 0;     // the spectral centroid, loudness-weighted, Hz (to 5.5 kHz): dark and warm, or bright and harsh
    float hf = 0;         // the share of energy above 3 kHz (cymbals, distortion, air)
    float onsets = 0;     // onsets a second (the spectral flux's peaks): how busy it is
    float perc = 0;       // the kick's and the snare's share of the onsets: drums against pads and voices
    float flux = 0;       // how much the spectrum changes, on average (log magnitudes)
    float minor = 0;      // the whole track's chroma: its best minor key's correlation over its best major key's (-1..1)
    float bass = 0;       // the share of energy below 150 Hz
    float bpm = 0, beat = 0;   // the tempo and how steady the beat is (Info::bpm, confidence; 0 without a steady one)
    // how strongly the music pulses: the onsets' autocorrelation at its best period between 60 and 240 BPM, against its
    // power (0..1); of all onsets, and of the kick alone (a four-on-the-floor's is high). Danceability, without the grid
    // Automix needs to join on the beat (its confidence is low for much danceable pop)
    float pulse = 0, kickPulse = 0;
    std::string pack() const;
    static bool unpack(const std::string& s, Mood& out);
};

struct Info {
    float bpm = 0;          // 0: no steady beat found (ambient, classical, speech): Automix fades without matching
    float confidence = 0;   // 0..1: how sure the beat grid is
    // The grids: beat k at beatIn + k * periodIn near the start, beatOut - k * period near the end (seconds);
    // each end has its own period (live music's tempo wanders), bpm is the whole track's
    double beatIn = 0, beatOut = 0;
    float period = 0, periodIn = 0;
    // Where the music is (seconds): the first and the last moment above the quiet floor, where a fade-out
    // starts (== end when the track ends abruptly), and where the intro reaches the track's body level
    float start = 0, end = 0, fadeStart = 0, introEnd = 0;
    float level = -99;      // the body's level, dB (RMS of the 11 kHz mono), for comparing the two ends
    float bassIn = 0, bassOut = 0;   // the share of energy below 150 Hz in the first and last 15 s of music, 0..1
    float seconds = 0;
    bool valid = false;
    // Bars (v2): a downbeat (beat 1) near the start and near the end, on the grids above (-1: none found), and how sure
    // each is (0..1); each bar's loudness from barIn on, a digit 0-9 against the body's level ('0' 12 dB under it, '8'
    // at it); the bar (0-7) where the 8-bar phrases start, counted from barIn
    double barIn = -1, barOut = -1;
    float barSureIn = 0, barSureOut = 0;
    int phraseAt = 0;
    std::string bars;
    // v4 (Automix v3: Spotify's joins hand the bass over hard, on the incoming track's drop): each bar's bass (below
    // 150 Hz) from barIn on, a digit 0-9 against the body's bass ('9' at it or above, each step 2 dB, '0' 18 dB under it)
    std::string bass;
    double dropAt = -1;     // the drop: the beat where the bass comes in, in full, after bars without it (s; -1: none)
    float dropJump = 0;     // … by how much (dB over the two beats before): 6 and more is a real drop
    // The key (v3): 0-11 major (C = 0), 12-23 minor (A minor = 21); -1: unknown. Of the whole track, and of its first and
    // last minute (songs change key); how sure (0..1: the best key's lead over the next)
    int key = -1, keyIn = -1, keyOut = -1;
    float keySure = 0;
    Mood mood;              // (not in pack(): mood.tsv)

    /** "v,bpm,conf,…" for mix.tsv, and back (false: another version or damaged). */
    std::string pack() const;
    static bool unpack(const std::string& s, Info& out);
};

/** A key's name ("F♯ minor"), and its place on the Camelot wheel ("11A"); the distance between two keys there (0: the
 *  same, 1: a neighbour or its relative major/minor: compatible; more: they clash). */
std::string keyName(int key);
std::string camelot(int key);
int keyDistance(int a, int b);

class Analyzer {
public:
    Analyzer();
    void begin(int rate, int channels);
    /** Interleaved float samples, as decoded. Nothing is allocated per push but the per-frame results. */
    void push(const float* x, int frames);
    Info finish();   // Automix's analysis (finishMix, unchanged for Mood) and the mood's features (finishMood)

private:
    void frame();
    Info finishMix();
    Mood finishMood(const Info& mix) const;

    int rate_ = 44100, ch_ = 2, decim_ = 4;
    double ar_ = 11025;     // the analysis rate
    float acc_ = 0;
    int accN_ = 0;
    std::vector<float> buf_;   // the last N analysis samples (a ring)
    int bufAt_ = 0, fill_ = 0, sinceHop_ = 0;
    std::vector<float> win_, re_, im_, prevLog_;
    std::vector<int> rev_;
    std::vector<float> flux_, db_, bass_, low_, snare_;   // per hop (low_: onsets below 150 Hz, the kick; snare_: 1.5-5 kHz)
    std::vector<float> centroid_, hf_;                    // per hop, for Mood: the spectral centroid (Hz), the share above 3 kHz
    std::vector<float> chroma_;                  // 12 per 4 hops (~46 ms): the pitch classes' energy, G#3-G#6
    float chromaAcc_[12] = {};
    int chromaN_ = 0;
    std::vector<int8_t> pcOf_;                   // each spectrum bin's lower pitch class (-1: outside G#3-G#6)
    std::vector<float> binW_;                    // each bin's weight, 1/f: every semitone the same
    std::vector<float> pcW_;                     // … and its share there (the rest goes to the next one up)
    float pcNorm_[12] = {};                      // each pitch class's total weight: the chroma is divided by it (v4)
    double hopPow_ = 0;
    // the bass's pitch classes (v4): a 4096-point spectrum (2.7 Hz bins) of the last 4096 samples every 8 hops (~93 ms),
    // G#1 to G#3 (52-208 Hz), as the chroma above; the root of a power chord is its lowest note
    void bassFrame();
    std::vector<float> buf4_, win4_, re4_, im4_, bassChroma_;   // bassChroma_: 12 per 8 hops
    std::vector<int> rev4_;
    std::vector<int8_t> bpcOf_;
    std::vector<float> bpcW_, bbinW_;
    float bpcNorm_[12] = {};
    int buf4At_ = 0, hops8_ = 0;
    double bassKey_ = 0;   // its weight in the key (0: off, the spectrum not made)
};

}  // namespace mixan
