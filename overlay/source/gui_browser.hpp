#pragma once

#include <tesla.hpp>
#include <string>
#include <functional>
#include <vector>

#include "gui_base.hpp"
#include "elm_overlayframe.hpp"

class BrowserGui final : public SysTuneGui {
  private:
    SysTuneOverlayFrame     *m_frame;
    tsl::elm::List          *m_list;
    std::string              m_cwd;
    std::string              m_root;        // topmost browsable directory (never go above this)
    std::string              m_focus_name;  // list item name to restore focus to on nav-back
    std::vector<std::string> m_folder_songs; // paths of all files in m_cwd, shared across file item lambdas
    std::function<void(u32)> m_on_count_changed;
    int                      m_depth = 2;
    // Stack is always [SettingsGui, BrowserGui] — depth is constant, no member needed.

  public:
    // Default construction (entry point from SettingsGui) auto-detects /music/ vs /
    // Subsequent constructions via swapTo pass explicit path, focus_name, and root.
    // depth: the pages above Settings, this one included ([Settings, Browse, Folders]: 2; a left to the player replaces them)
    BrowserGui(std::string path = "", std::string focus_name = "", std::string root = "",
               std::function<void(u32)> on_count_changed = nullptr, int depth = 2);
    ~BrowserGui();

    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;

  private:
    void buildList();
    void addAllToPlaylist(const std::string &path);

    // Fire m_on_count_changed with play_ctx::savedPlaylistSize().
    void notifyCountChanged() const;

    std::string parentPath()     const;
    std::string currentDirName() const;
};

// =============================================================================
// Browse (Songate, 2026-10-30): Folders, as Browse always was, and Playlists: Songate's own, by kind (Normal: Liked, yours
// and folder playlists; Smart; Mood; Automix), each listing its tracks with a folder's actions. Read from the one file
// Songate writes for the background player (GFBG_PLAYLISTS): its header for the lists, one pass for a playlist's tracks.
// The stack: [Settings, Browse, Playlists, a kind, a playlist]; left goes to the player, which comes back to the same page.
// =============================================================================

class BrowseGui final : public SysTuneGui {
  public:
    explicit BrowseGui(std::function<void(u32)> on_count_changed = nullptr);
    ~BrowseGui();
    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;
  private:
    tsl::elm::List *m_list = nullptr;
    std::function<void(u32)> m_on_count_changed;
};

/** Browse › Playlists: Normal, Smart, Mood, Automix. */
class PlaylistKindsGui final : public SysTuneGui {
  public:
    explicit PlaylistKindsGui(std::function<void(u32)> on_count_changed = nullptr);
    ~PlaylistKindsGui();
    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;
  private:
    tsl::elm::List *m_list = nullptr;
    std::function<void(u32)> m_on_count_changed;
};

/** One kind's playlists (group: 'N' Normal, 'S' Smart, 'M' Mood, 'A' Automix). */
class PlaylistListGui final : public SysTuneGui {
  public:
    explicit PlaylistListGui(char group, std::function<void(u32)> on_count_changed = nullptr);
    ~PlaylistListGui();
    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;
  private:
    char m_group;
    tsl::elm::List *m_list = nullptr;
    std::function<void(u32)> m_on_count_changed;
};

/** A playlist's tracks, with a folder's actions: A plays from here, Y adds to the Playlist, X adds all, − sets the startup file. */
class PlaylistTracksGui final : public SysTuneGui {
  public:
    PlaylistTracksGui(char group, std::string name, char kind, std::function<void(u32)> on_count_changed = nullptr);
    ~PlaylistTracksGui();
    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;
  private:
    void notifyCountChanged() const;
    char m_group, m_kind;
    std::string m_name;
    tsl::elm::List *m_list = nullptr;
    std::vector<std::string> m_songs;   // every track, as the overlay spells paths (the queue a play makes)
    std::function<void(u32)> m_on_count_changed;
};

/** Right from the player, back to the Browse page it was left on (setBrowserReturnPath's target): Folders' folder, the
 *  Browse page ("@browse"), Playlists ("@playlists"), a kind ("@kind:<group>") or a playlist ("#<group><kind><name>"). */
void pushBrowseStack(const std::string &target, const std::string &root);
