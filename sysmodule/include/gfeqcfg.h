/*
 * Gatefold NX · gfeqcfg.h
 * The equalizer's settings and presets, shared by Gatefold, the background player (GfbgCmd_SetEq)
 * and the overlay. Plain C and fixed-size, as everything that crosses gfbg.h's IPC.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define GFEQ_BANDS 10

/** The ten bands' centres, Hz. */
static const float kGfEqFreq[GFEQ_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

typedef struct {
    uint8_t on;          // 0 off: the sound goes through untouched
    uint8_t advanced;    // 0 Simple (Preset, Bass, Treble shown), 1 Advanced (the bands and the preamp too)
    int8_t preset;       // index into kGfEqPresets, GFEQ_PRESETS + k your saved preset k (GFEQ_USER), −1 Custom (a band moved by hand)
    uint8_t edited;      // your saved preset, changed since: it keeps its name ("Warm (edited)"), and Save to … stores the change
    float preamp;        // dB, −12..+12 (Advanced)
    float bass, treble;  // dB, −12..+12: a low shelf at 100 Hz and a high shelf at 10 kHz, on top of the bands
    float bands[GFEQ_BANDS];   // dB, −12..+12; the presets set these, in either mode
    float custom[GFEQ_BANDS];  // your own curve, kept while a preset is chosen, so Custom brings it back
    uint8_t hasCustom;         // a band was ever moved by hand
    uint8_t reserved1[3];
    uint32_t reserved[4];
} GfbgEq;

typedef struct {
    const char* name;
    float bands[GFEQ_BANDS];
} GfEqPreset;

#define GFEQ_PRESETS 12
static const GfEqPreset kGfEqPresets[GFEQ_PRESETS] = {
    { "Flat",         { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "Bass boost",   { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 } },
    { "Bass reduce",  { -6, -5, -4, -2, 0, 0, 0, 0, 0, 0 } },
    { "Treble boost", { 0, 0, 0, 0, 0, 1, 2, 4, 5, 6 } },
    { "Vocal",        { -2, -2, -1, 0, 2, 4, 4, 3, 1, 0 } },
    { "Rock",         { 4, 3, 2, 0, -1, -1, 1, 3, 4, 4 } },
    { "Pop",          { -1, 0, 2, 3, 4, 3, 1, 0, -1, -1 } },
    { "Electronic",   { 5, 4, 1, 0, -2, 1, 0, 2, 4, 5 } },
    { "Hip-hop",      { 5, 4, 2, 3, -1, -1, 1, -1, 2, 3 } },
    { "Jazz",         { 3, 2, 1, 2, -2, -2, 0, 1, 2, 3 } },
    { "Classical",    { 4, 3, 2, 1, -1, -1, 0, 2, 3, 4 } },
    { "Acoustic",     { 4, 3, 2, 1, 1, 1, 2, 2, 3, 2 } },
};

/**
 * Your saved presets (Gatefold's Settings → Equalizer → Save as a preset), in GFEQ_USER: one a line,
 * "name<TAB>preamp<TAB>bass<TAB>treble<TAB>the ten bands". Unlike Gatefold's, one keeps the whole sound:
 * the bands, bass, treble and the preamp. Gatefold writes it; the overlay reads it to step through them.
 */
#define GFEQ_USER "sdmc:/config/gatefold-nx/eq-presets.txt"
#define GFEQ_USER_MAX 100   /* preset indices stay within int8_t */

typedef struct {
    char name[64];
    float preamp, bass, treble;
    float bands[GFEQ_BANDS];
} GfEqUser;

/** Off, Flat, nothing moved: what a fresh install has. */
static inline GfbgEq gfEqDefault(void) {
    GfbgEq e = { 0, 0, 0, 0, 0, 0, 0, { 0 }, { 0 }, 0, { 0 }, { 0 } };
    return e;
}

/** Sets the bands to a preset (the bass, treble and preamp are left as they are). */
static inline void gfEqApplyPreset(GfbgEq* e, int preset) {
    if (preset < 0 || preset >= GFEQ_PRESETS) return;
    e->preset = (int8_t)preset;
    e->edited = 0;
    for (int i = 0; i < GFEQ_BANDS; i++) e->bands[i] = kGfEqPresets[preset].bands[i];
}

/** The curve as it is now becomes Custom, and is kept there (choosing Custom again brings it back). */
static inline void gfEqMakeCustom(GfbgEq* e) {
    e->preset = -1;
    e->edited = 0;
    e->hasCustom = 1;
    for (int i = 0; i < GFEQ_BANDS; i++) e->custom[i] = e->bands[i];
}

/**
 * A change by hand. On your saved preset it stays that preset, marked edited (Custom's own curve is left alone);
 * on Custom or one of Gatefold's, the curve is Custom.
 */
static inline void gfEqKeepCustom(GfbgEq* e) {
    if (e->preset >= GFEQ_PRESETS) { e->edited = 1; return; }
    gfEqMakeCustom(e);
}

/** Custom chosen again: your own curve back on the bands (nothing when there is none). */
static inline void gfEqApplyCustom(GfbgEq* e) {
    if (!e->hasCustom) return;
    e->preset = -1;
    for (int i = 0; i < GFEQ_BANDS; i++) e->bands[i] = e->custom[i];
}

/** "1.0 -2.5 …" as written in the config files: up to GFEQ_BANDS values into out. */
static inline void gfEqParseBands(const char* v, float* out) {
    for (int b = 0; b < GFEQ_BANDS; b++) {
        char* end = NULL;
        const float x = strtof(v, &end);
        if (end == v) break;
        out[b] = x;
        v = end;
    }
}

/** One line of GFEQ_USER (the newline may be there or not); false when it holds no name. */
static inline int gfEqParseUser(const char* line, GfEqUser* u) {
    int n = 0;
    while (line[n] && line[n] != '\t' && line[n] != '\n' && line[n] != '\r' && n < (int)sizeof u->name - 1) { u->name[n] = line[n]; n++; }
    u->name[n] = 0;
    if (!n) return 0;
    const char* p = line + n;
    while (*p && *p != '\t') p++;
    char* end = NULL;
    float* f[3] = { &u->preamp, &u->bass, &u->treble };
    for (int i = 0; i < 3; i++) { *f[i] = *p ? strtof(p, &end) : 0.f; if (*p) p = end; }
    for (int i = 0; i < GFEQ_BANDS; i++) u->bands[i] = 0;
    gfEqParseBands(p, u->bands);
    return 1;
}

/** Your saved preset k: the whole sound it keeps. */
static inline void gfEqApplyUser(GfbgEq* e, const GfEqUser* u, int k) {
    e->preset = (int8_t)(GFEQ_PRESETS + k);
    e->edited = 0;
    e->preamp = u->preamp;
    e->bass = u->bass;
    e->treble = u->treble;
    for (int i = 0; i < GFEQ_BANDS; i++) e->bands[i] = u->bands[i];
}

/** −12..+12 dB everywhere, a preset index that exists; for anything read from a file or the other side. */
static inline void gfEqClamp(GfbgEq* e) {
    #define GFEQ_CL(v) ((v) < -12.f ? -12.f : (v) > 12.f ? 12.f : (v) != (v) ? 0.f : (v))
    e->on = e->on ? 1 : 0;
    e->advanced = e->advanced ? 1 : 0;
    if (e->preset < -1 || e->preset >= GFEQ_PRESETS + GFEQ_USER_MAX) e->preset = -1;
    e->edited = e->edited && e->preset >= GFEQ_PRESETS ? 1 : 0;
    e->preamp = GFEQ_CL(e->preamp);
    e->bass = GFEQ_CL(e->bass);
    e->treble = GFEQ_CL(e->treble);
    for (int i = 0; i < GFEQ_BANDS; i++) e->bands[i] = GFEQ_CL(e->bands[i]);
    for (int i = 0; i < GFEQ_BANDS; i++) e->custom[i] = GFEQ_CL(e->custom[i]);
    e->hasCustom = e->hasCustom ? 1 : 0;
    if (e->preset == -1 && !e->hasCustom) gfEqMakeCustom(e);   // a Custom from before this was kept
    #undef GFEQ_CL
}
