#pragma once

#include "tune.h"
#include "elm_status_bar.hpp"
#include "gui_base.hpp"

#include <tesla.hpp>

class SysTuneOverlayFrame;
#include "elm_volume.hpp"

// ---------------------------------------------------------------------------
// PlayerRightDest — updated by Playlist/Browser when leaving to the player,
// and reset to Settings when Settings itself navigates back to the player.
enum class PlayerRightDest { Settings, Playlist, Browse };
void setPlayerRightDest(PlayerRightDest dest);
// Store the exact browser path the user was at, so right-from-player
// returns there (not the currently-playing folder).
void setBrowserReturnPath(const std::string& cwd, const std::string& root);

// ---------------------------------------------------------------------------
// Page 0 — Player
// ---------------------------------------------------------------------------
class MainGui final : public SysTuneGui {
public:
    MainGui();

    /**
     * Deletes m_list, which cascade-deletes all its children including
     * m_status_bar.  m_frame is owned by Tesla — do NOT delete here.
     */
    ~MainGui();

    tsl::elm::Element *createUI() final;
    void update() final;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;

private:
    // m_list owns m_status_bar (deleted with it).
    // m_frame is owned by Tesla, do NOT delete.
    StatusBar           *m_status_bar  = nullptr;
    tsl::elm::List      *m_list        = nullptr;
    SysTuneOverlayFrame *m_frame       = nullptr;
    std::string          m_right_label;
};

// ---------------------------------------------------------------------------
// Page 1 — Settings
// ---------------------------------------------------------------------------
class SettingsGui final : public SysTuneGui {
public:
    /**
     * jumpTo — if non-empty, createUI() will call m_list->jumpToItem(jumpTo)
     * at the end of construction.  Used by the "Default Focus" toggle to
     * swapTo a fresh SettingsGui and land focus on that item after the list
     * has been rebuilt with or without the Custom Focus row.
     */
    explicit SettingsGui(std::string jumpTo = {}) : m_jump_to(std::move(jumpTo)) {}

    /**
     * Deletes m_list and all its children.
     * m_frame is owned by Tesla — do NOT delete here.
     */
    ~SettingsGui();

    tsl::elm::Element *createUI() final;
    void update() final;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;

private:
    // m_frame is owned by Tesla, do NOT delete.
    tsl::elm::List      *m_list         = nullptr;
    SysTuneOverlayFrame *m_frame        = nullptr;

    // If set, createUI() calls m_list->jumpToItem(m_jump_to) at the end.
    std::string          m_jump_to;

    // Pointers to the Playlist and Browse buttons so update() can set their
    // value labels live.  Both owned by m_list — do NOT delete here.
    tsl::elm::ListItem  *m_queue_button   = nullptr;
    tsl::elm::ListItem  *m_browser_button = nullptr;
    u32                  m_last_count     = UINT32_MAX; /* sentinel — forces first refresh */

    /* Called directly by the PlaylistGui callback and by update() for the
       BrowserGui case.  Updates the label only when the count has changed. */
    void refreshPlaylistCount(u32 count);

    // ---- Volume mute-toggle state ----------------------------------------
    // Slider pointers (owned by m_list — do NOT delete here).
    VolumeTrackBar      *m_music_slider        = nullptr;
    VolumeTrackBar      *m_game_slider         = nullptr;  // null if no game running
    VolumeTrackBar      *m_game_default_slider = nullptr;

    // Current slider values (0-100), kept in sync by the value-changed listeners.
    u8  m_music_vol        = 100;
    u8  m_game_vol         = 100;
    u8  m_game_default_vol = 100;

    // Pre-mute backup values — persisted to disk so they survive overlay close.
    u8  m_music_vol_backup        = 100;
    u8  m_game_vol_backup         = 100;
    u8  m_game_default_vol_backup = 100;

    // Title ID needed by handleInput to call config::set_title_volume().
    u64 m_tid = 0;

    // Last tid we built the title-id labels for — lets update() detect changes.
    u64 m_last_tid = UINT64_MAX; /* sentinel — forces first refresh */
};