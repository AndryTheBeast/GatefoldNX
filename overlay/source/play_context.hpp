#pragma once

#include <string>
#include <vector>
#include <switch.h>   // u32, FS_MAX_PATH

// =============================================================================
// play_ctx — single source of truth for playback context.
//
// Two mutually exclusive contexts:
//
//   Playlist — the IPC queue equals the user's saved playlist.
//              Mutations (add/remove/clear) keep IPC and saved[] in sync.
//
//   Folder   — the IPC queue holds the songs of one filesystem folder.
//              The user's saved playlist is held separately in saved[] and
//              on-disk, untouched by folder playback.
//
// Persistence:
//   /config/sys-tune/saved_playlist.txt   — one absolute path per line
//   /config/sys-tune/play_source.txt      — "Playlist" | "Folder:<path>"
//
//   These files are updated on every mutation so state survives overlay
//   close/reopen while the sys-tune background service keeps running.
//
// Thread safety: all callers run on the Tesla UI thread — no locking needed.
// =============================================================================

namespace play_ctx {

    enum class Source { Playlist, Folder };

    // ---- Queries -------------------------------------------------------

    Source             source();
    const std::string& folderPath();

    // The user's saved playlist (absolute paths, sorted order preserved).
    const std::vector<std::string>& savedPlaylist();
    u32                             savedPlaylistSize();

    // Currently playing path and play/pause state.
    // Refreshed from IPC by poll(); always safe to call from draw/update.
    const char* currentPath();
    bool        isPlaying();

    // ---- Saved-playlist mutations  ------------------------------------
    // These always update the in-memory vector AND flush to disk.

    void savedAppend(const std::string& path);
    void savedRemove(u32 idx);   // 0-based
    void savedClear();

    // ---- Context switches ---------------------------------------------

    // Switch to Folder context:
    //   • If source was Playlist, snapshot IPC → saved[] first.
    //   • tuneClearQueue, tuneEnqueue songs[], tuneSelect(play_idx), tunePlay.
    //   Returns false on any IPC failure (state is NOT changed on failure).
    bool switchToFolder(const std::string& folder_path,
                        const std::vector<std::string>& songs,
                        u32 play_idx);

    // Switch to Playlist context:
    //   • tuneClearQueue, tuneEnqueue saved[], tuneSelect(play_idx), tunePlay.
    //   Returns false on any IPC failure.
    bool switchToPlaylist(u32 play_idx);

    // ---- Playlist-context IPC pass-through ----------------------------
    // Use these (instead of raw tuneRemove / tuneClearQueue) when
    // source == Playlist so saved[] and IPC stay in sync.

    bool playlistTuneRemove(u32 idx);   // tuneRemove + savedRemove
    bool playlistTuneClearQueue();       // tuneClearQueue + savedClear

    // ---- Lifecycle ----------------------------------------------------

    // Call once when the overlay opens (MainGui constructor).
    // Loads persisted state; falls back to snapshotting IPC when no file exists.
    void init();

    // Refresh currentPath() / isPlaying() from IPC.
    // Call periodically from each Gui's update() (≈every 15 ticks).
    void poll();

    // Check whether the current song's folder has a wallpaper.rgba and reload
    // ult::WALLPAPER_PATH if it has changed.  Safe to call from any Gui's
    // update() — internally gated so reloadWallpaper() only fires on folder
    // changes.  path must be the full absolute path to the current audio file.
    void checkWallpaper(const char *path);

    // Re-snapshot the IPC queue order into saved[] and persist to disk.
    // Call after toggling shuffle so the Playlist/Browser lists reflect the
    // new order the service has applied before the user navigates there.
    void resyncFromIPC();

    // Scan the live IPC queue (in current shuffle order) for 'path' and
    // call tuneSelect on its position, then tunePlay.  Use this instead of
    // an index-based select whenever shuffle may be on, so the correct song
    // is always chosen regardless of queue reorder state.
    // Returns false if the path is not found in the current queue.
    bool startByPath(const std::string& path);

} // namespace play_ctx