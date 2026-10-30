/*
 * Gatefold NX · gfbg.h
 * The background player's IPC protocol, shared by the sysmodule (server), Gatefold and the
 * overlay (clients). The sysmodule does all playback while it runs, so closing Gatefold never
 * interrupts the song; Gatefold sends it the queue and transport, and reads the state back.
 *
 * Every struct here crosses a process boundary: only fixed-size fields, no pointers, and the
 * layout may only grow at the end (GFBG_PROTOCOL bumps when it does).
 */
#pragma once
#include <stdio.h>
#include <switch.h>

#include "gfeqcfg.h"

#define GFBG_SERVICE    "gatefold"           // sm name, 8 characters at most
#define GFBG_PROGRAM_ID 0x4200000047464E58ULL // "GFNX" in the homebrew range
#define GFBG_PROTOCOL   6   // 5: the equalizer (GetEq/SetEq, GfbgState.eqGen), crossfade and playlist actions (GfbgConfig grew);
                            // 6: Automix (GfbgQueue_Automix, GfbgConfig.automixModel, GfbgState's join), the playlists' kinds
#define GFBG_PCM        4096                  // mono samples GetPcm returns, at 48 kHz (Spectrum::N)
#define GFBG_RATE       48000

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GfbgCmd_Hello        = 0,   // in u32 client kind, send pid → out u32 protocol
    GfbgCmd_GetState     = 1,   // → GfbgState
    GfbgCmd_SetQueue     = 2,   // in buffer: GfbgQueueHeader, s32 order[count], count NUL-ended paths
    GfbgCmd_GetQueue     = 3,   // out buffer, same layout; its size is GfbgState.queueBytes
    GfbgCmd_PlayAt       = 4,   // in GfbgPlayAt
    GfbgCmd_Transport    = 5,   // in u32 GfbgOp
    GfbgCmd_Seek         = 6,   // in double seconds
    GfbgCmd_SetVolume    = 7,   // in float 0..1
    GfbgCmd_SetMode      = 8,   // in u32 GfbgMode: the sysmodule rebuilds the order itself (overlay)
    GfbgCmd_SetStopAfter = 9,   // in u32 0/1
    GfbgCmd_GetPcm       = 10,  // out buffer float[GFBG_PCM] → out u32 serial (bumps per audio block)
    GfbgCmd_GetConfig    = 11,  // → GfbgConfig
    GfbgCmd_SetConfig    = 12,  // in GfbgConfig
    GfbgCmd_GetTrackInfo = 13,  // out buffer GfbgTrackInfo
    GfbgCmd_GetGameVolume = 14, // → GfbgGameVolume
    GfbgCmd_SetGameVolume = 15, // in float: the game in front's volume, remembered for that game
    GfbgCmd_SetSleep     = 16,  // in float seconds until the music fades out and pauses; 0 turns the timer off
    GfbgCmd_GetEq        = 17,  // → GfbgEq (gfeqcfg.h)
    GfbgCmd_SetEq        = 18,  // in GfbgEq: the player keeps it (bg-config.txt) and bumps GfbgState.eqGen
} GfbgCmd;

typedef enum { GfbgClient_Gatefold = 1, GfbgClient_Overlay = 2 } GfbgClient;

/** The same values as Gatefold's Order (store.hpp). */
typedef enum { GfbgMode_Default = 0, GfbgMode_RepeatAll = 1, GfbgMode_RepeatOne = 2, GfbgMode_Shuffle = 3 } GfbgMode;

typedef enum {
    GfbgOp_Pause = 0, GfbgOp_Resume = 1, GfbgOp_Toggle = 2, GfbgOp_Stop = 3, GfbgOp_Next = 4, GfbgOp_Prev = 5,
} GfbgOp;

typedef enum { GfbgStatus_Stopped = 0, GfbgStatus_Playing = 1, GfbgStatus_Paused = 2 } GfbgStatus;

typedef enum { GfbgCodec_None = 0, GfbgCodec_Aac = 1, GfbgCodec_Mp3 = 2, GfbgCodec_Flac = 3, GfbgCodec_Wav = 4 } GfbgCodec;

/** What the music does when a game opens, goes to the HOME menu, or closes (GfbgConfig). */
typedef enum {
    GfbgAct_Nothing = 0,
    GfbgAct_Play = 1,     // a random folder of the music folder, shuffled
    GfbgAct_Resume = 2,   // the track it was on
    GfbgAct_Pause = 3,
    GfbgAct_Playlist = 4, // one of Gatefold's playlists (GfbgConfig.lists, GFBG_PLAYLISTS); none chosen: as Resume
} GfbgAct;

/** What pause-for-apps is doing. */
typedef enum { GfbgApp_None = 0, GfbgApp_Foreground = 1, GfbgApp_Suspended = 2 } GfbgAppState;

typedef struct {
    u32 protocol;
    u32 status;        // GfbgStatus
    s32 pos;           // index into the order, −1 with no queue
    s32 track;         // order[pos]: index into the queue's paths
    u32 trackGen;      // bumps on every track load, whoever asked for it
    u32 queueGen;      // bumps when the queue or its order changes (a reshuffle too)
    u32 queueCount;
    u32 queueBytes;    // GetQueue's buffer size
    u32 mode;          // GfbgMode
    u32 stopAfter;
    double position;   // seconds, what is heard now
    double duration;
    float volume;      // the music's own volume
    float gain;        // the fade of pause-for-apps and of pause/resume, 0..1
    u32 sampleRate;    // the file's
    u32 channels;
    u32 codec;         // GfbgCodec
    u32 bitrate;       // kbit/s, 0 when unknown
    u32 errorGen;      // bumps when a track could not be played
    u32 appState;      // GfbgAppState
    u32 autoPaused;    // paused by pause-for-apps (and resumed by it)
    u32 trackBy;       // who caused the last trackGen bump: 0 the player itself, else a GfbgClient
    u32 queueBy;       // the same for queueGen
    u32 heapFree;      // bytes, for the About page
    float sleepLeft;   // seconds until the sleep timer pauses the music, 0 when it is off (protocol 4)
    u32 sleepGen;      // bumps when the sleep timer runs out
    u32 eqGen;         // bumps on every SetEq, whoever sent it (protocol 5): Gatefold reads the EQ again
    // protocol 6: Automix's join (an Automix playlist's queue): joinGen bumps as each one starts; joining 0 none, 1 the next
    // one planned (its track ready or not), 2 the two tracks playing together; joinAt where what is heard is in it (seconds,
    // negative before it starts), joinLength its length
    u32 joinGen;
    u32 joining;
    float joinAt;
    float joinLength;
    float joinBpm;     // on the beat: the tempo it meets on; 0 off the beat
    u32 joinFlags;     // GfbgJoin_*
} GfbgState;
#define GfbgJoin_Beat 1u   // on the beat
#define GfbgJoin_Drop 2u   // on the next track's drop (with Beat), or a bridge into it between tempos

typedef struct {
    u8 onOpen;         // GfbgAct when a game opens (a fresh launch)
    u8 onHome;         // ... when it waits on the HOME menu
    u8 onClose;        // ... when it quits
    u8 onReturn;       // ... when it comes back from the HOME menu, never having closed (protocol 3; was padding)
    float fadeOut;     // seconds
    float fadeIn;      // seconds
    float gameVolume;  // the volume of games that have none of their own (SetGameVolume)
    u32 reserved[4];   // [0] volume levelling's mode (GfbgLevel)
    // protocol 5
    float crossfade;   // seconds, 0 off
    u32 xfadeFlags;    // GfbgXfade_*
    u32 listStart;     // how an action's playlist starts: GfbgListStart
    u32 reserved5;
    u64 lists[4];      // GfbgAct_Playlist: gfbgHash of the playlist's name, per event (onOpen, onHome, onClose, onReturn); 0 none
    // protocol 6
    u32 automixModel;  // the Automix model Gatefold plays (automix::Model: 0 V1.0.0, 1 V1.1.0…), for an Automix playlist's joins
    u32 reserved6[3];
    char automixFile[64];   // a downloaded model's file in config/gatefold-nx/models/ ("": the built-in automixModel)
} GfbgConfig;

/** Crossfade: consecutive tracks of one album keep their join (gapless, live albums). */
#define GfbgXfade_NotInAlbum 1u

typedef enum { GfbgListStart_Continue = 0, GfbgListStart_Shuffle = 1 } GfbgListStart;

/**
 * Gatefold's playlists for the playlist action (protocol 5), rewritten by Gatefold when they change:
 * for each one a line "#<name>" then its tracks' paths, one a line, in its order. The player reads only
 * the one an event asks for, the overlay only the names.
 * Protocol 6: a first line "!kinds <letters>", before every "#" line (where readers before it skip it), one letter
 * per playlist in the file's order (GFBG_KIND_*): L Liked, N your own, F a folder's, S smart, M mood, A Automix.
 */
#define GFBG_KIND_LIKED   'L'
#define GFBG_KIND_OWN     'N'
#define GFBG_KIND_FOLDER  'F'
#define GFBG_KIND_SMART   'S'
#define GFBG_KIND_MOOD    'M'
#define GFBG_KIND_AUTOMIX 'A'
#define GFBG_PLAYLISTS "sdmc:/config/gatefold-nx/bg-playlists.txt"

typedef struct {
    u64 programId;     // the game in front, 0 on the HOME menu (or when it is Gatefold)
    float volume;      // its volume now
    u32 own;           // it has a volume of its own, not the default
} GfbgGameVolume;

typedef struct {
    u32 count;
    s32 pos;           // the current position in the order
    u32 mode;          // GfbgMode
    u32 flags;         // GfbgQueue_*
    u32 pathBytes;     // the paths' size, NULs included
    u32 reserved[3];
    char name[64];     // the queue's name ("Library", an album…)
} GfbgQueueHeader;

/** SetQueue: when the track playing now is the one at the new pos, it carries on untouched. */
#define GfbgQueue_KeepCurrent 1
/** SetQueue (protocol 6): the queue is an Automix playlist (its name is the playlist's): its tracks are joined by Automix
 *  from Gatefold's analysis (mix.tsv), in their own order: never shuffled (SetMode's Shuffle plays it in order). */
#define GfbgQueue_Automix 2
#define GFBG_MIX "sdmc:/config/gatefold-nx/mix.tsv"
#define GFBG_MODELS "sdmc:/config/gatefold-nx/models"
/** SetQueue's answer when the queue does not fit in the player's heap (it keeps the queue it had; nothing else changes). */
#define GFBG_QUEUE_TOO_BIG MAKERESULT(Module_Libnx, LibnxError_OutOfMemory)

typedef struct {
    s32 pos;
    u32 paused;        // load it and wait paused
    double start;      // seconds
} GfbgPlayAt;

typedef struct {
    u32 trackGen;
    u32 coverKind;     // 0 none, 1 JPEG, 2 PNG (embedded at coverOffset in path), 3 the image file coverPath
    u64 coverOffset;
    u32 coverSize;
    u32 reserved;
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char coverPath[512];
} GfbgTrackInfo;

/**
 * Covers for the overlay, made by Gatefold for every track the player may play: an overlay's
 * few MB cannot decode the large progressive JPEGs music carries. Each distinct cover is one
 * file of GFBG_ART² RGB (8 bits a channel, no header), GFBG_ART being the overlay's cover size,
 * so it is drawn 1:1. GFBG_THUMBS/map.tsv lists one track per line as
 * "<gfbgHash of its path, 16 hex digits> TAB <file name>".
 */
/**
 * Volume levelling (protocol 4, no new command): Gatefold measures each track's loudness and writes
 * GFBG_GAINS, a GfbgGainsHeader then `count` GfbgGain sorted by hash (gfbgHash of the path). The
 * player reads the header every couple of seconds and the whole file when `gen` changes. The mode
 * is GfbgConfig.reserved[0]: GfbgLevel_*. Auto: the album gain, but the track gain while shuffling.
 */
#define GFBG_GAINS "sdmc:/config/gatefold-nx/gains.bin"
#define GFBG_GAINS_MAGIC 0x564C4647u   // "GFLV"
typedef enum { GfbgLevel_Off = 0, GfbgLevel_Track = 1, GfbgLevel_Album = 2, GfbgLevel_Auto = 3 } GfbgLevel;
typedef struct { u32 magic, count, gen, reserved; } GfbgGainsHeader;
typedef struct { u64 hash; float track, album; } GfbgGain;   // dB, the peak limit already in

#define GFBG_ART    307
#define GFBG_THUMBS "sdmc:/config/gatefold-nx/thumbs"

/** FNV-1a 64: thumbnail names and the map's keys. */
static inline u64 gfbgHash(const void* data, size_t n) {
    u64 h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= ((const unsigned char*)data)[i]; h *= 0x100000001b3ULL; }
    return h;
}

// ---------------------------------------------------------------- client (header-only)
#ifndef GFBG_NO_CLIENT   // (tools/bgtest/join: the player on the PC has no IPC)

/**
 * True when the sysmodule is running. Asked through Atmosphère's sm extension (AtmosphereHasService,
 * 65100) because a plain GetService for a name nobody registered waits for it to appear.
 */
static inline bool gfbgRunning(void) {
    u8 has = 0;
    const SmServiceName name = smEncodeName(GFBG_SERVICE);
    const Result rc = hosversionAtLeast(12, 0, 0) ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, has)
                                                  : serviceDispatchInOut(smGetServiceSession(), 65100, name, has);
    return R_SUCCEEDED(rc) && has;
}

static inline Result gfbgOpen(Service* s, u32 kind) {
    if (!gfbgRunning()) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    Result rc = smGetService(s, GFBG_SERVICE);
    if (R_FAILED(rc)) return rc;
    u32 protocol = 0;
    rc = serviceDispatchInOut(s, GfbgCmd_Hello, kind, protocol, .in_send_pid = true);
    if (R_SUCCEEDED(rc) && protocol != GFBG_PROTOCOL) rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    if (R_FAILED(rc)) serviceClose(s);
    return rc;
}

static inline Result gfbgGetState(Service* s, GfbgState* out) { return serviceDispatchOut(s, GfbgCmd_GetState, *out); }

static inline Result gfbgSetQueue(Service* s, const void* data, size_t size) {
    return serviceDispatch(s, GfbgCmd_SetQueue, .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In }, .buffers = { { data, size } });
}

static inline Result gfbgGetQueue(Service* s, void* data, size_t size) {
    return serviceDispatch(s, GfbgCmd_GetQueue, .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out }, .buffers = { { data, size } });
}

static inline Result gfbgPlayAt(Service* s, s32 pos, bool paused, double start) {
    const GfbgPlayAt in = { pos, paused ? 1u : 0u, start };
    return serviceDispatchIn(s, GfbgCmd_PlayAt, in);
}

static inline Result gfbgTransport(Service* s, u32 op) { return serviceDispatchIn(s, GfbgCmd_Transport, op); }
static inline Result gfbgSeek(Service* s, double t) { return serviceDispatchIn(s, GfbgCmd_Seek, t); }
static inline Result gfbgSetVolume(Service* s, float v) { return serviceDispatchIn(s, GfbgCmd_SetVolume, v); }
static inline Result gfbgSetMode(Service* s, u32 mode) { return serviceDispatchIn(s, GfbgCmd_SetMode, mode); }
static inline Result gfbgSetStopAfter(Service* s, bool on) { const u32 v = on; return serviceDispatchIn(s, GfbgCmd_SetStopAfter, v); }
static inline Result gfbgSetSleep(Service* s, float seconds) { return serviceDispatchIn(s, GfbgCmd_SetSleep, seconds); }

static inline Result gfbgGetPcm(Service* s, float* out, u32* serial) {
    return serviceDispatchOut(s, GfbgCmd_GetPcm, *serial, .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                              .buffers = { { out, GFBG_PCM * sizeof(float) } });
}

static inline Result gfbgGetGameVolume(Service* s, GfbgGameVolume* out) { return serviceDispatchOut(s, GfbgCmd_GetGameVolume, *out); }
static inline Result gfbgSetGameVolume(Service* s, float v) { return serviceDispatchIn(s, GfbgCmd_SetGameVolume, v); }

static inline Result gfbgGetConfig(Service* s, GfbgConfig* out) { return serviceDispatchOut(s, GfbgCmd_GetConfig, *out); }
static inline Result gfbgSetConfig(Service* s, const GfbgConfig* in) { return serviceDispatchIn(s, GfbgCmd_SetConfig, *in); }

static inline Result gfbgGetEq(Service* s, GfbgEq* out) { return serviceDispatchOut(s, GfbgCmd_GetEq, *out); }
static inline Result gfbgSetEq(Service* s, const GfbgEq* in) { return serviceDispatchIn(s, GfbgCmd_SetEq, *in); }

static inline Result gfbgGetTrackInfo(Service* s, GfbgTrackInfo* out) {
    return serviceDispatch(s, GfbgCmd_GetTrackInfo, .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                           .buffers = { { out, sizeof *out } });
}

#endif  // GFBG_NO_CLIENT
#ifdef __cplusplus
}
#endif
