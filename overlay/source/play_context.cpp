#include "tune_gfbg.hpp"
#include "play_context.hpp"

#include "tune.h"

#include <tesla.hpp>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <strings.h>

// =============================================================================
// play_ctx implementation
// =============================================================================

namespace play_ctx {

namespace {

    // ---- In-memory state -----------------------------------------------

    Source                   g_source      = Source::Playlist;
    std::string              g_folder_path;
    std::vector<std::string> g_saved;

    // g_current_path  — last path confirmed by IPC (tuneGetCurrentQueueItem).
    // g_pending_path  — path we intend to be current after a context switch.
    //                   Set immediately on every switch before any IPC call
    //                   so draw() never sees index-0 during the service's
    //                   async commit window.  Cleared by poll() once IPC
    //                   returns this exact path, meaning the service has
    //                   caught up.  currentPath() returns g_pending_path
    //                   whenever it is non-empty, so the UI is always
    //                   consistent with our intent.
    char g_current_path[FS_MAX_PATH] = "";
    char g_pending_path[FS_MAX_PATH] = "";
    bool g_is_playing = false;

    // How many poll() calls have fired while g_pending_path is set.
    // If the service never confirms the pending path (e.g. it skipped the
    // track due to a read error), we give up after kPendingTimeout polls so
    // the indicator doesn't stay stuck for the entire session.
    // At the default ~4 Hz poll rate (every 15 ticks at 60 fps) this is ~7 s.
    u8 g_pending_polls = 0;
    static constexpr u8 kPendingTimeout = 30;

    // ---- Persistence paths ---------------------------------------------

    constexpr const char* kSavedFile = "/config/gatefold-nx/overlay-playlist.txt";
    constexpr const char* kStateFile = "/config/gatefold-nx/overlay-source.txt";

    // ---- Disk helpers --------------------------------------------------

    void writeState() {
        FILE* f = fopen(kStateFile, "w");
        if (!f) return;
        if (g_source == Source::Folder)
            fprintf(f, "Folder:%s\n", g_folder_path.c_str());
        else
            fprintf(f, "Playlist\n");
        fclose(f);
    }

    void writeSaved() {
        FILE* f = fopen(kSavedFile, "w");
        if (!f) return;
        for (const auto& p : g_saved)
            fprintf(f, "%s\n", p.c_str());
        fclose(f);
    }

    void readState() {
        FILE* f = fopen(kStateFile, "r");
        if (!f) {
            g_source = Source::Playlist;
            g_folder_path.clear();
            return;
        }
        char buf[FS_MAX_PATH + 16] = {};
        if (fgets(buf, (int)sizeof(buf), f)) {
            const size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            if (strncmp(buf, "Folder:", 7) == 0) {
                g_source      = Source::Folder;
                g_folder_path = buf + 7;
            } else {
                g_source = Source::Playlist;
                g_folder_path.clear();
            }
        }
        fclose(f);
    }

    void readSaved() {
        g_saved.clear();
        FILE* f = fopen(kSavedFile, "r");
        if (!f) return;
        char buf[FS_MAX_PATH + 2] = {};
        while (fgets(buf, (int)sizeof(buf), f)) {
            const size_t len = strlen(buf);
            if (len > 1) {
                if (buf[len - 1] == '\n')
                    buf[len - 1] = '\0';
                if (buf[0] != '\0')
                    g_saved.emplace_back(buf);
            }
        }
        fclose(f);
    }

    // Set g_pending_path to 'path' so that currentPath() returns it
    // immediately, before IPC has reflected the change.
    void setPending(const char* path) {
        std::strncpy(g_pending_path, path, FS_MAX_PATH - 1);
        g_pending_path[FS_MAX_PATH - 1] = '\0';
        g_pending_polls = 0;
    }

    // Snapshot current IPC playlist to g_saved (overwrites), then flush disk.
    void snapshotIPC() {
        g_saved.clear();
        u32 count = 0;
        if (R_FAILED(tuneGetPlaylistSize(&count))) {
            writeSaved();
            return;
        }
        char path[FS_MAX_PATH];
        g_saved.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            if (R_SUCCEEDED(tuneGetPlaylistItem(i, path, sizeof(path))))
                g_saved.emplace_back(path);
        }
        writeSaved();
    }

    // Replace the IPC queue with 'songs', select play_idx, and start playback.
    //
    // Order:
    //   1. Pause  — stops the service so it won't auto-play index 0 the
    //               instant the first enqueue lands on a freshly cleared queue.
    //   2. Clear + enqueue all songs while paused.
    //   3. tuneSelect(idx) — marks the right track before play.
    //   4. tunePlay        — begin playback exactly on the intended track.
    //
    // Callers must call setPending() BEFORE this function so that
    // currentPath() is already correct for any draw that fires between calls.
    bool loadQueueAndPlay(const std::vector<std::string>& songs, u32 play_idx, const std::string& name = "", bool automix = false) {
        tunePause();
        if (R_FAILED(tuneClearQueue()))
            return false;
        if (!name.empty() || automix) gfQueueMeta(name, automix);   // a playlist of Songate's: its name, and Automix's rules
        for (const auto& s : songs)
            tuneEnqueue(s.c_str(), TuneEnqueueType_Back);
        if (!songs.empty()) {
            const u32 idx = std::min(play_idx, static_cast<u32>(songs.size() - 1));
            tuneSelect(idx);
            tunePlay();
        }
        return true;
    }

} // anonymous namespace

// ---- Queries ---------------------------------------------------------------

Source             source()      { return g_source; }
const std::string& folderPath()  { return g_folder_path; }
const std::vector<std::string>& savedPlaylist() { return g_saved; }
u32  savedPlaylistSize()         { return static_cast<u32>(g_saved.size()); }

// Return the pending (intended) path while IPC is still catching up.
// Once poll() sees the service report this path, pending is cleared and
// we fall through to the IPC-confirmed g_current_path as normal.
const char* currentPath() {
    return g_pending_path[0] != '\0' ? g_pending_path : g_current_path;
}

bool isPlaying() { return g_is_playing; }

// ---- Saved-playlist mutations ----------------------------------------------

void savedAppend(const std::string& path) {
    g_saved.push_back(path);
    writeSaved();
}

void savedRemove(u32 idx) {
    if (idx >= static_cast<u32>(g_saved.size())) return;
    g_saved.erase(g_saved.begin() + idx);
    writeSaved();
}

void savedClear() {
    g_saved.clear();
    writeSaved();
}

// ---- Context switches ------------------------------------------------------

bool switchToFolder(const std::string& folder_path,
                    const std::vector<std::string>& songs,
                    u32 play_idx,
                    const std::string& queueName,
                    bool automix) {
    const u32 idx = songs.empty() ? 0u
                  : std::min(play_idx, static_cast<u32>(songs.size() - 1));

    // Lock in the intended path immediately — BEFORE any IPC call — so that
    // currentPath() already returns the right song for the very next draw().
    // g_is_playing is also set optimistically; poll() will correct it if the
    // service reports otherwise within a tick or two.
    if (!songs.empty())
        setPending(songs[idx].c_str());
    g_is_playing = true;

    // Fast path: already playing from this exact folder — the IPC queue is
    // already loaded correctly.  Just seek to the new index and resume.
    // Guard with a count check: after a sysmodule restart the queue may only
    // hold a single startup file rather than the full folder, so a mismatch
    // means we must fall through to a full reload instead of seeking within
    // a truncated queue.
    if (g_source == Source::Folder && g_folder_path == folder_path) {
        u32 ipc_count = 0;
        tuneGetPlaylistSize(&ipc_count);
        if (ipc_count == static_cast<u32>(songs.size())) {
            tuneSelect(idx);
            tunePlay();
            return true;
        }
        // Count mismatch — fall through to full reload below.
    }

    // Transitioning FROM Playlist context: snapshot IPC to saved[] first.
    // FROM a different Folder context: saved[] is already correct, skip.
    if (g_source == Source::Playlist)
        snapshotIPC();

    if (!loadQueueAndPlay(songs, idx, queueName, automix))
        return false;

    g_source      = Source::Folder;
    g_folder_path = folder_path;
    writeState();
    return true;
}

bool switchToPlaylist(u32 play_idx) {
    const u32 idx = g_saved.empty() ? 0u
                  : std::min(play_idx, static_cast<u32>(g_saved.size() - 1));

    if (!g_saved.empty())
        setPending(g_saved[idx].c_str());
    g_is_playing = true;

    if (!loadQueueAndPlay(g_saved, idx))
        return false;

    g_source = Source::Playlist;
    g_folder_path.clear();
    writeState();
    return true;
}

// ---- Playlist-context IPC pass-through ------------------------------------

bool playlistTuneRemove(u32 idx) {
    if (R_FAILED(tuneRemove(idx)))
        return false;
    savedRemove(idx);
    return true;
}

bool playlistTuneClearQueue() {
    if (R_FAILED(tuneClearQueue()))
        return false;
    savedClear();
    return true;
}

// ---- Lifecycle -------------------------------------------------------------

void init() {
    // Detect expanded memory early so wallpaper and other memory-dependent
    // features are correctly gated from the very first overlay frame.
    ult::currentHeapSize = ult::getCurrentHeapSize();
    ult::expandedMemory  = ult::currentHeapSize >= ult::OverlayHeapSize::Size_6MB;

    readState();

    // Gatefold: its queue is the playlist. The player holds it, Gatefold changes it, so it is read
    // from the player each time the overlay opens rather than from a file of our own. Only while a
    // folder from Browse plays does the file keep the queue it replaced, to go back to.
    if (g_source == Source::Folder) readSaved();
    else snapshotIPC();

    // After tuneQuit + restart the service IPC queue is empty.
    // Check the count and silently reload saved[] back into IPC so that
    // tuneSelect() / tuneRemove() work the moment the user taps a song.
    // We do NOT call tuneSelect or tunePlay here — the user triggers that.
    // In Folder context we can't restore the folder queue from disk, so we
    // fall back to Playlist context so the saved playlist is usable immediately.
    u32 ipc_count = 0;
    tuneGetPlaylistSize(&ipc_count);

    // If we think we're in Folder context, verify IPC was actually populated
    // with that folder's songs and not a single startup file.  After a
    // sysmodule restart load_path may be one file rather than the full folder,
    // leaving IPC with just that one track while play_source.txt still says
    // Folder context.  Check the first IPC path: if it doesn't begin with
    // g_folder_path the persisted context is stale — reset to Playlist.
    if (g_source == Source::Folder && ipc_count > 0 && !g_folder_path.empty()) {
        char probe[FS_MAX_PATH] = "";
        tuneGetPlaylistItem(0, probe, sizeof(probe));
        if (strncmp(probe, g_folder_path.c_str(), g_folder_path.size()) != 0) {
            g_source = Source::Playlist;
            g_folder_path.clear();
            writeState();
        }
    }

    if (ipc_count == 0 && !g_saved.empty()) {
        if (g_source == Source::Folder) {
            // Can't restore folder queue — reset to Playlist context.
            g_source = Source::Playlist;
            g_folder_path.clear();
            writeState();
        }
        for (const auto& s : g_saved)
            tuneEnqueue(s.c_str(), TuneEnqueueType_Back);
    }

    poll();
}

void poll() {
    bool playing = false;
    tuneGetStatus(&playing);
    g_is_playing = playing;

    char ipc_path[FS_MAX_PATH] = "";
    TuneCurrentStats stats;
    if (R_SUCCEEDED(tuneGetCurrentQueueItem(ipc_path, FS_MAX_PATH, &stats))) {
        if (g_pending_path[0] != '\0') {
            // A context switch is in flight.  Only accept the IPC result once
            // it matches the path we intended — that is the signal that the
            // background service has processed our tuneSelect.
            // Until then, discard whatever IPC reports (it could be index 0
            // or the previous track) so the indicator never flashes wrong.
            if (strcasecmp(ipc_path, g_pending_path) == 0) {
                std::strncpy(g_current_path, ipc_path, FS_MAX_PATH - 1);
                g_current_path[FS_MAX_PATH - 1] = '\0';
                g_pending_path[0] = '\0';
                g_pending_polls   = 0;
            } else if (++g_pending_polls >= kPendingTimeout) {
                // Service never played the intended track (skipped / error).
                // Accept whatever IPC is actually reporting as ground truth so
                // the indicator doesn't stay wrong for the whole session.
                std::strncpy(g_current_path, ipc_path, FS_MAX_PATH - 1);
                g_current_path[FS_MAX_PATH - 1] = '\0';
                g_pending_path[0] = '\0';
                g_pending_polls   = 0;
            }
            // else: still waiting within the timeout window
        } else {
            std::strncpy(g_current_path, ipc_path, FS_MAX_PATH - 1);
            g_current_path[FS_MAX_PATH - 1] = '\0';
        }
    } else {
        // IPC returned no track.  Only blank current if no switch is pending.
        if (g_pending_path[0] == '\0')
            g_current_path[0] = '\0';
    }

    // Per-folder wallpaper — runs in every poll() so it fires regardless of
    // which page (player, browser, playlist) is currently visible.
    // Uses the pending path when a switch is in flight so the wallpaper
    // updates at the same moment the UI shows the new track, not after IPC
    // confirms it.  Gated by s_last_wallpaper_folder so reloadWallpaper()
    // is only called when the folder actually changes.
    if (ult::expandedMemory) {
        const char *cur = g_pending_path[0] != '\0' ? g_pending_path : g_current_path;
        checkWallpaper(cur);
    }
}

void checkWallpaper(const char *path) {
    if (!ult::expandedMemory) return;

    static std::string s_default_wallpaper_path;
    static bool        s_default_captured = false;
    static std::string s_last_wallpaper_folder;
    // Debounce: number of consecutive no-song calls before restoring default.
    // checkWallpaper is called every tick from StatusBar::update(), so 90 ticks
    // is ~1.5 s at 60 fps — long enough to survive any between-track gap but
    // short enough to restore promptly when playback actually stops.
    static u8          s_no_song_ticks    = 0;
    static constexpr u8 kNoSongDebounce   = 90;

    if (!s_default_captured) {
        s_default_wallpaper_path = ult::WALLPAPER_PATH;
        s_default_captured       = true;
    }

    if (path && path[0] != '\0') {
        s_no_song_ticks = 0;  // song is present — reset debounce

        std::string song_folder;
        const char *slash = std::strrchr(path, '/');
        if (slash)
            song_folder.assign(path, static_cast<size_t>(slash + 1 - path));

        if (song_folder != s_last_wallpaper_folder) {
            s_last_wallpaper_folder = song_folder;
            const std::string candidate = song_folder + "wallpaper.rgba";
            ult::WALLPAPER_PATH = ult::isFile(candidate)
                ? candidate
                : s_default_wallpaper_path;
            ult::reloadWallpaper();
        }
    } else if (!s_last_wallpaper_folder.empty()) {
        // No song — only restore the default once the absence has persisted
        // long enough to distinguish "between tracks" from "stopped".
        if (++s_no_song_ticks >= kNoSongDebounce) {
            s_no_song_ticks = 0;
            s_last_wallpaper_folder.clear();
            ult::WALLPAPER_PATH = s_default_wallpaper_path;
            ult::reloadWallpaper();
        }
    }
}

void resyncFromIPC() {
    // Only meaningful in Playlist context — in Folder context the list order
    // is determined by the filesystem and doesn't change with shuffle.
    if (g_source != Source::Playlist) return;

    // Re-snapshot the IPC queue (now reordered by the service after shuffle)
    // into g_saved so that PlaylistGui builds its list in the correct order.
    snapshotIPC();

    // Poll immediately so g_current_path reflects the new index-0 track that
    // the service may have selected after reshuffling.
    poll();
}

bool startByPath(const std::string& path) {
    // Scan the IPC queue in its current order (shuffle or sorted depending on
    // the service's g_shuffle state).  tuneGetPlaylistItem already applies
    // the active shuffle mode, so the index we find here is exactly the one
    // tuneSelect expects — no shuffle toggling required.
    u32 count = 0;
    if (R_FAILED(tuneGetPlaylistSize(&count)) || count == 0)
        return false;

    char ipc_path[FS_MAX_PATH];
    for (u32 i = 0; i < count; ++i) {
        if (R_SUCCEEDED(tuneGetPlaylistItem(i, ipc_path, sizeof(ipc_path))) &&
            strcasecmp(path.c_str(), ipc_path) == 0)
        {
            setPending(path.c_str());
            g_is_playing = true;
            tuneSelect(i);
            tunePlay();
            return true;
        }
    }
    return false;
}

} // namespace play_ctx