#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "elm_volume.hpp"
#include "gui_browser.hpp"
#include "gui_playlist.hpp"
#include "play_context.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"
#include "tune_gfbg.hpp"

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// =============================================================================
// PlayerRightDest + browser return path globals
// =============================================================================

static PlayerRightDest g_player_right_dest   = PlayerRightDest::Settings;
static std::string     g_browser_return_cwd;
static std::string     g_browser_return_root;

// ---------------------------------------------------------------------------
// Volume mute-backup persistence
// Three integers (0-100), one per line: Music, Game, Game (default).
// ---------------------------------------------------------------------------
static constexpr const char* kVolBackupFile = "/config/gatefold-nx/overlay-volume.txt";

static void readVolBackups(u8 &music, u8 &game, u8 &game_def) {
    music = game = game_def = 100;
    FILE* f = fopen(kVolBackupFile, "r");
    if (!f) return;
    int m = 100, g = 100, gd = 100;
    fscanf(f, "%d\n%d\n%d", &m, &g, &gd);
    fclose(f);
    music    = static_cast<u8>(std::clamp(m,  0, 100));
    game     = static_cast<u8>(std::clamp(g,  0, 100));
    game_def = static_cast<u8>(std::clamp(gd, 0, 100));
}

static void writeVolBackups(u8 music, u8 game, u8 game_def) {
    FILE* f = fopen(kVolBackupFile, "w");
    if (!f) return;
    fprintf(f, "%d\n%d\n%d\n", static_cast<int>(music),
                                static_cast<int>(game),
                                static_cast<int>(game_def));
    fclose(f);
}

void setPlayerRightDest(PlayerRightDest dest) { g_player_right_dest = dest; }

void setBrowserReturnPath(const std::string& cwd, const std::string& root) {
    g_browser_return_cwd   = cwd;
    g_browser_return_root  = root;
}

// ---------------------------------------------------------------------------
// Tri-state focus action used by the "Title Focus" and "Custom Focus"
// cycling list items in SettingsGui.
//
//   Pass  — policy engine takes no action at title focus transitions
//           (music simply keeps doing whatever it was doing).
//   Play  — force music to play.
//   Pause — force music to pause.
//
// A KEY_A press cycles: Pass -> Play -> Pause -> Pass -> ...
//
// Storage is unchanged from the previous pair-of-toggles UI — each list
// item maps to two mutually-exclusive bool keys in config.ini — so
// existing user configs are fully backward compatible.
// ---------------------------------------------------------------------------
namespace {

/**
 * Gatefold: the running game's own name (its control data, in the console's language), for the
 * Settings page's game volume. Empty when it cannot be read; long names are shortened to fit.
 */
std::string gameName(u64 programId) {
    std::string out;
    if (R_FAILED(nsInitialize())) return out;
    // About 130 KB with the icon: borrowed for the call only, an overlay's heap is small.
    auto* data = static_cast<NsApplicationControlData*>(std::malloc(sizeof(NsApplicationControlData)));
    // A game made of several programs runs one of them (the id's low bits); the application is the base id.
    for (u64 id : { programId, u64(programId & ~u64(0xFFF)) }) {
        u64 size = 0;
        NacpLanguageEntry* e = nullptr;
        if (data && R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, id, data, sizeof *data, &size)) &&
            R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &e)) && e && e->name[0]) {
            const void* nul = std::memchr(e->name, 0, sizeof e->name);
            out.assign(e->name, nul ? static_cast<const char*>(nul) - e->name : sizeof e->name);
            break;
        }
    }
    std::free(data);
    nsExit();
    constexpr size_t kMax = 26;   // what fits beside the percentage
    if (out.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;   // not inside a UTF-8 character
        out = out.substr(0, cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "…";
    }
    return out;
}
    enum class FocusMode { Pass, Play, Pause };

    constexpr const char* focusLabel(FocusMode m) {
        switch (m) {
            case FocusMode::Play:  return "Play";
            case FocusMode::Pause: return "Pause";
            case FocusMode::Pass:
            default:               return "Pass";
        }
    }

    constexpr FocusMode focusNext(FocusMode m) {
        switch (m) {
            case FocusMode::Pass:  return FocusMode::Play;
            case FocusMode::Play:  return FocusMode::Pause;
            case FocusMode::Pause:
            default:               return FocusMode::Pass;
        }
    }
}

// ---------------------------------------------------------------------------
// Deferred SettingsGui rebuild — set by the "Default Focus" toggle listener
// and consumed at the top of SettingsGui::update() on the next frame.
//
// We CANNOT call tsl::swapTo from inside a setStateChangedListener because
// swapTo immediately destroys the current GUI (and therefore the element
// whose onClick is still on the call stack), causing a use-after-free on the
// triggerClickAnimation() and Element::onClick() calls that follow the
// listener invocation.  Deferring to update() lets the full input chain
// unwind safely before the rebuild fires.
// ---------------------------------------------------------------------------
static bool        s_settings_rebuild_pending = false;
static std::string s_settings_rebuild_jump;

// ---------------------------------------------------------------------------
// Return to the exact browser directory the user was in.
// The stack is always [SettingsGui, BrowserGui] — one changeTo is enough.
// Pressing B inside BrowserGui swaps to the parent directory, so the full
// tree is navigable without ever growing the stack beyond depth 2.
// ---------------------------------------------------------------------------
static void pushBrowserStack() {
    tsl::changeTo<BrowserGui>(
        g_browser_return_cwd, /*focus_name=*/"",
        g_browser_return_root, /*on_count_changed=*/nullptr);
}

// =============================================================================
// MainGui  (Page 0 — Player)
// =============================================================================

MainGui::MainGui() {
    // Initialise play context once on overlay open.
    // Loads persisted state from /config/sys-tune/ and snapshots IPC on first run.
    play_ctx::init();

    m_status_bar = new StatusBar();
}

MainGui::~MainGui() {
    // m_list cascade-deletes all its children, including m_status_bar.
    // m_frame is owned by Tesla — do NOT delete here.
    delete m_list;
}

// ---------------------------------------------------------------------------
tsl::elm::Element* MainGui::createUI() {
    m_right_label = (g_player_right_dest == PlayerRightDest::Playlist) ? "Playlist"
                  : (g_player_right_dest == PlayerRightDest::Browse)   ? "Browse"
                  : "Settings";
    m_frame = new SysTuneOverlayFrame(/*pageLeft=*/"", m_right_label);

    m_list = new tsl::elm::List();
    m_list->addItem(m_status_bar, StatusBar::PreferredHeight(tsl::cfg::FramebufferWidth - 85));

    m_status_bar->setPageRightCallback([] {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            /* swapTo replaces MainGui with SettingsGui, then changeTo stacks
               PlaylistGui on top: [SettingsGui, PlaylistGui].
               B on Playlist → [SettingsGui] → B closes. */
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            /* Return to the exact directory the user was browsing, not the
               currently-playing folder. */
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
    });

    // Pre-warm before the first draw so m_playing and m_percentage are already
    // correct on frame 0 — prevents the 0:00 flicker when swapping back from Settings.
    m_status_bar->update();

    m_frame->setContent(m_list);
    return m_frame;
}

// ---------------------------------------------------------------------------
void MainGui::update() {
    //static u8 tick = 0;
    //if ((tick % 15) == 0)
    m_status_bar->update();
    //if ((tick % 15) == 8)   // stagger from status_bar: fires midway between its updates
    play_ctx::poll();
    //++tick;

    // Detect shuffle mode changes and immediately resync g_saved to the
    // service's new queue order (the IPC call is synchronous so by the time
    // CycleShuffle() returns, the service has already reshuffled the queue).
    // Without this, g_saved stays in the pre-shuffle order until the user
    // opens PlaylistGui, causing index-based operations to use wrong positions.
    if (play_ctx::source() == play_ctx::Source::Playlist) {
        static TuneShuffleMode s_last_shuffle = TuneShuffleMode_Off;
        TuneShuffleMode cur = TuneShuffleMode_Off;
        //if ((tick % 15) == 1) {  // stagger slightly from status_bar update
        tuneGetShuffleMode(&cur);
        if (cur != s_last_shuffle) {
            s_last_shuffle = cur;
            play_ctx::resyncFromIPC();
        }
        //}
    }

    const std::string newLabel =
        (g_player_right_dest == PlayerRightDest::Playlist) ? "Playlist" :
        (g_player_right_dest == PlayerRightDest::Browse)   ? "Browse"   : "Settings";
    if (newLabel != m_right_label) {
        m_right_label = newLabel;
        if (m_frame) m_frame->setPageNames("", m_right_label);
    }
}

// ---------------------------------------------------------------------------
bool MainGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                          HidAnalogStickState joyStickPosLeft,
                          HidAnalogStickState joyStickPosRight) {

    if (m_status_bar->hasFocus())
        m_status_bar->onHeld(keysHeld);
    else
        g_player_r_held.store(false, std::memory_order_release);

    if (ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)) {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
        return true;
    }

    if (SysTuneGui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight))
        return true;

    /* KEY_R + RIGHT/LEFT while status bar is focused: bypass cursor movement
       and navigate pages directly — same destinations as normal page nav. */
    if (m_status_bar->hasFocus() && (keysHeld & KEY_R)) {
        if ((keysDown & KEY_RIGHT) && !(keysHeld & ~KEY_RIGHT & ~KEY_R & ALL_KEYS_MASK)) {
            g_player_r_held.store(false, std::memory_order_release);
            if (g_player_right_dest == PlayerRightDest::Playlist) {
                play_ctx::poll();
                tsl::swapTo<SettingsGui>();
                tsl::changeTo<PlaylistGui>(nullptr);
            } else if (g_player_right_dest == PlayerRightDest::Browse) {
                play_ctx::poll();
                tsl::swapTo<SettingsGui>();
                pushBrowserStack();
            } else {
                tsl::swapTo<SettingsGui>();
            }
            triggerNavigationFeedback();
            return true;
        }
    }

    if ((keysDown & KEY_RIGHT)
        && !(keysHeld & ~KEY_RIGHT & ALL_KEYS_MASK)
        && !m_status_bar->hasFocus()
        && !(ult::onTrackBar.load(std::memory_order_acquire)
             && (ult::unlockedSlide.load(std::memory_order_acquire) || ult::allowSlide.load(std::memory_order_acquire))
             && !(keysHeld & KEY_R))) {
        g_player_r_held.store(false, std::memory_order_release);
        if (g_player_right_dest == PlayerRightDest::Playlist) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            tsl::changeTo<PlaylistGui>(nullptr);
        } else if (g_player_right_dest == PlayerRightDest::Browse) {
            play_ctx::poll();
            tsl::swapTo<SettingsGui>();
            pushBrowserStack();
        } else {
            tsl::swapTo<SettingsGui>();
        }
        triggerNavigationFeedback();
        return true;
    }

    return false;
}

// =============================================================================
// SettingsGui  (Page 1 — Settings)
// =============================================================================

SettingsGui::~SettingsGui() {
    delete m_list;
}

// ---------------------------------------------------------------------------
void SettingsGui::refreshPlaylistCount(u32 count) {
    if (!m_queue_button) return;
    m_last_count = count;
    /* Don't clobber the INPROGRESS_SYMBOL that update() manages. */
    const bool playlistActive = (play_ctx::source() == play_ctx::Source::Playlist)
                              && (play_ctx::currentPath()[0] != '\0');
    if (!playlistActive)
        m_queue_button->setValue(count == 1 ? "1 track" : std::to_string(count) + " tracks");
}

// ---------------------------------------------------------------------------
tsl::elm::Element* SettingsGui::createUI() {
    m_frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");

    u64 pid{}, tid{};
    pm::getCurrentPidTid(&pid, &tid);
    m_tid      = tid;
    m_last_tid = tid; // in sync from the start so update() only fires on changes

    // Format a title ID as a hex string for use as a label.
    auto tidLabel = [](u64 t) -> std::string {
        char buf[19];
        std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(t));
        return buf;
    };

    m_list = new tsl::elm::List();

    // ---- Music Selection ----
    m_list->addItem(new tsl::elm::CategoryHeader("Music Library"));

    /* Snapshot play_ctx state once so both buttons get the right initial value
       on frame 0, before any update() tick fires. */
    const bool init_inPlaylist = (play_ctx::source() == play_ctx::Source::Playlist);
    const bool init_inFolder   = (play_ctx::source() == play_ctx::Source::Folder);
    const bool init_hasTrack   = (play_ctx::currentPath()[0] != '\0');

    {
        const u32 count = play_ctx::savedPlaylistSize();
        m_last_count    = count;
        const std::string queueVal = (init_inPlaylist && init_hasTrack)
            ? ult::INPROGRESS_SYMBOL
            : (count == 1 ? "1 track" : std::to_string(count) + " tracks");
        m_queue_button = new tsl::elm::ListItem("Playlist", queueVal);
    }

    m_queue_button->setClickListener([this](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(m_queue_button);
            tsl::changeTo<PlaylistGui>([this](u32 count) {
                refreshPlaylistCount(count);
            });
            return true;
        }
        return false;
    });
    m_list->addItem(m_queue_button);

    const std::string browseVal = (init_inFolder && init_hasTrack)
        ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL;
    auto browser_button = new tsl::elm::ListItem("Browse", browseVal);
    m_browser_button = browser_button;
    browser_button->setClickListener([this, browser_button](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            //tsl::shiftItemFocus(browser_button);
            tsl::changeTo<BrowserGui>("", "", "", [this](u32 count) {
                refreshPlaylistCount(count);
            });
            return true;
        }
        return false;
    });
    m_list->addItem(browser_button);

    // ---- Volume ----
    m_list->addItem(new tsl::elm::CategoryHeader("Volume "+ult::DIVIDER_SYMBOL+"  Toggle Mute"));

    float tune_volume = 1.f, title_volume = 1.f, default_title_volume = 1.f;
    tuneGetVolume(&tune_volume);
    tuneGetTitleVolume(&title_volume);
    tuneGetDefaultTitleVolume(&default_title_volume);

    // Load persisted pre-mute backups so Y-toggle survives overlay reopen.
    readVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);

    // Initialise current-vol tracking from actual IPC values.
    m_music_vol        = static_cast<u8>(std::clamp(static_cast<int>(tune_volume          * 100.f + 0.5f), 0, 100));
    m_game_vol         = static_cast<u8>(std::clamp(static_cast<int>(title_volume         * 100.f + 0.5f), 0, 100));
    m_game_default_vol = static_cast<u8>(std::clamp(static_cast<int>(default_title_volume * 100.f + 0.5f), 0, 100));

    // Helper: same mute-toggle logic as the Y-key handler, usable from icon taps.
    // Captures SettingsGui members by pointer so the lambda stays copyable.
    auto makeMuteTap = [this](VolumeTrackBar** sliderPtr, u8* vol, u8* backup,
                               std::function<void(u8)> applyFn) -> std::function<void()> {
        return [this, sliderPtr, vol, backup, applyFn]() {
            auto* slider = *sliderPtr;
            if (!slider) return;
            if (*vol > 0) {
                *backup = *vol;
                *vol    = 0;
            } else {
                *vol = (*backup > 0) ? *backup : static_cast<u8>(100);
            }
            slider->setProgress(*vol);
            applyFn(*vol);
            writeVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);
        };
    };

    auto tune_volume_slider = new VolumeTrackBar("\uE13C", false, false, true, "Music", "%", false);
    tune_volume_slider->setProgress(tune_volume * 100);
    tune_volume_slider->setValueChangedListener([this](u8 value) {
        m_music_vol = value;
        tuneSetVolume(float(value) / 100.f);
    });
    m_music_slider = tune_volume_slider;
    tune_volume_slider->setIconTapCallback(makeMuteTap(
        &m_music_slider, &m_music_vol, &m_music_vol_backup,
        [](u8 v) { tuneSetVolume(float(v) / 100.f); }));
    m_list->addItem(tune_volume_slider);
    
    // Gatefold: the game in front's own volume, remembered per game, and the one every other
    // game gets. Neither shows for the HOME menu or for Gatefold itself.
    GfbgGameVolume game{};
    gfGameVolume(&game);
    if (game.programId) {
        const std::string name = gameName(game.programId);
        auto title_volume_slider = new VolumeTrackBar("", false, false, true, name.empty() ? "This Game" : name, "%", false);
        title_volume_slider->setProgress(title_volume * 100);
        title_volume_slider->setValueChangedListener([this](u8 value) {
            m_game_vol = value;
            tuneSetTitleVolume(float(value) / 100.f);
        });
        m_game_slider = title_volume_slider;
        title_volume_slider->setIconTapCallback(makeMuteTap(
            &m_game_slider, &m_game_vol, &m_game_vol_backup,
            [](u8 v) { tuneSetTitleVolume(float(v) / 100.f); }));
        m_list->addItem(title_volume_slider);
    }

    auto default_title_volume_slider = new VolumeTrackBar("", false, false, true, "Games", "%", false);
    default_title_volume_slider->setProgress(static_cast<u8>(std::clamp(default_title_volume * 100.f + 0.5f, 0.f, 100.f)));
    m_game_default_vol = default_title_volume_slider->getProgress();
    default_title_volume_slider->setValueChangedListener([this](u8 value) {
        m_game_default_vol = value;
        tuneSetDefaultTitleVolume(float(value) / 100.f);
    });
    m_game_default_slider = default_title_volume_slider;
    default_title_volume_slider->setIconTapCallback(makeMuteTap(
        &m_game_default_slider, &m_game_default_vol, &m_game_default_vol_backup,
        [](u8 v) { tuneSetDefaultTitleVolume(float(v) / 100.f); }));
    m_list->addItem(default_title_volume_slider);
    // What "Games" means, centred in a line under it: a game you have set a volume for (the slider above,
    // named after it) keeps its own.
    m_list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32) {
        static constexpr const char* kHint = "For games without a volume of their own";
        const s32 tw = static_cast<s32>(r->getTextDimensions(kHint, false, 15).first);
        r->drawString(kHint, false, x + (w - tw) / 2, y + 20, 15, tsl::offTextColor);
    }), 32);

    // ---- Games: what the music does at each game event (Gatefold's background player) ----
    // Each item cycles with A, like sys-tune's Title Focus: Nothing, Play (a random folder,
    // shuffled), Resume, Pause.
    m_list->addItem(new tsl::elm::CategoryHeader("Games"));
    static GfbgConfig s_cfg{};
    gfConfig(&s_cfg);
    auto actionItem = [this](const char* label, u8 GfbgConfig::*field) {
        static constexpr const char* names[] = { "Nothing", "Play", "Resume", "Pause" };
        auto* item = new tsl::elm::ListItem(label, names[std::min<u8>(s_cfg.*field, 3)]);
        item->setValue(names[std::min<u8>(s_cfg.*field, 3)], s_cfg.*field == GfbgAct_Nothing);
        item->setClickListener([item, field](u64 keys) -> bool {
            if (!(keys & HidNpadButton_A)) return false;
            gfConfig(&s_cfg);
            s_cfg.*field = u8((s_cfg.*field + 1) % 4);
            gfSetConfig(s_cfg);
            item->setValue(names[s_cfg.*field], s_cfg.*field == GfbgAct_Nothing);
            return true;
        });
        m_list->addItem(item);
    };
    actionItem("Game Opens", &GfbgConfig::onOpen);
    actionItem("HOME Menu", &GfbgConfig::onHome);
    actionItem("Back to the Game", &GfbgConfig::onReturn);
    actionItem("Game Closes", &GfbgConfig::onClose);
    // Fades step through a few lengths, A by A.
    auto fadeItem = [this](const char* label, float GfbgConfig::*field) {
        static constexpr float steps[] = { 0.f, 0.5f, 1.f, 1.5f, 2.f, 3.f, 5.f };
        auto text = [](float s) {
            char b[16];
            if (s <= 0.f) std::snprintf(b, sizeof(b), "None");
            else std::snprintf(b, sizeof(b), "%.1f s", static_cast<double>(s));
            return std::string(b);
        };
        auto* item = new tsl::elm::ListItem(label, text(s_cfg.*field));
        item->setClickListener([item, field, text](u64 keys) -> bool {
            if (!(keys & HidNpadButton_A)) return false;
            gfConfig(&s_cfg);
            float next = steps[0];
            for (float s : steps) if (s > s_cfg.*field + 0.01f) { next = s; break; }
            s_cfg.*field = next;
            gfSetConfig(s_cfg);
            item->setValue(text(next));
            return true;
        });
        m_list->addItem(item);
    };
    fadeItem("Fade Out", &GfbgConfig::fadeOut);
    fadeItem("Fade In", &GfbgConfig::fadeIn);

    // ---- Misc ----
    m_list->addItem(new tsl::elm::CategoryHeader("Miscellaneous"));
    {
        GfbgState st{};
        gfState(&st);
        auto stop_after = new tsl::elm::ToggleListItem("Stop After This Track", st.stopAfter != 0, "On", "Off");
        stop_after->setStateChangedListener([](bool v) { gfSetStopAfter(v); });
        m_list->addItem(stop_after);
    }

    auto exit_button = new tsl::elm::SilentListItem("Stop Playback");
    exit_button->setValue("", true);
    exit_button->setClickListener([exit_button](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            tuneQuit();
            tsl::goBack();
            return true;
        }
        return false;
    });
    m_list->addItem(exit_button);

    m_frame->setContent(m_list);

    // Auto-jump to whichever music source is actively playing so the user
    // lands directly on the relevant button rather than the first item.
    if (!m_jump_to.empty()) {
        m_list->jumpToItem(m_jump_to);
    } else if (init_inPlaylist && init_hasTrack) {
        m_list->jumpToItem("Playlist");
    } else if (init_inFolder && init_hasTrack) {
        m_list->jumpToItem("Browse");
    }

    return m_frame;
}

// ---------------------------------------------------------------------------
void SettingsGui::update() {
    // Consume any pending rebuild request.  This is set by the "Default On
    // Start" listener and intentionally deferred here so the swapTo fires
    // after the full onClick/handleInput chain has returned — not while the
    // toggle element is still mid-callback.
    if (s_settings_rebuild_pending) {
        s_settings_rebuild_pending = false;
        tsl::swapTo<SettingsGui>(std::move(s_settings_rebuild_jump));
        return;
    }

    /* Poll IPC on a throttled schedule — it's a syscall.
       Button label updates run every tick so values snap back instantly
       the moment SettingsGui resumes after a child GUI is popped. */
    //static u8 tick = 0;
    //if ((++tick % 15) == 0)
    play_ctx::poll();

    // Re-check the running title every tick (cheap — just reads globals set by
    // pm::getCurrentPidTid which was already called recently by play_ctx::poll).
    {
        u64 pid{}, tid{};
        pm::getCurrentPidTid(&pid, &tid);
        if (tid != m_last_tid) {
            m_tid      = tid;
            m_last_tid = tid;

            // Gatefold: "Games" (the default) is one volume for all of them, nothing to reload per title.
        }
    }

    const bool inPlaylist = (play_ctx::source() == play_ctx::Source::Playlist);
    const bool inFolder   = (play_ctx::source() == play_ctx::Source::Folder);
    const bool hasTrack   = (play_ctx::currentPath()[0] != '\0');

    if (m_queue_button) {
        if (inPlaylist && hasTrack) {
            m_queue_button->setValue(ult::INPROGRESS_SYMBOL);
        } else {
            const u32 count = play_ctx::savedPlaylistSize();
            m_last_count = count;
            m_queue_button->setValue(count == 1 ? "1 track" : std::to_string(count) + " tracks");
        }
    }

    if (m_browser_button) {
        m_browser_button->setValue(
            (inFolder && hasTrack) ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL);
    }
}

// ---------------------------------------------------------------------------
bool SettingsGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                              HidAnalogStickState joyStickPosLeft,
                              HidAnalogStickState joyStickPosRight) {

    if (ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)) {
        setPlayerRightDest(PlayerRightDest::Settings);
        tsl::swapTo<MainGui>();
        triggerNavigationFeedback();
        return true;
    }

    // Y — mute/restore toggle on the focused volume slider.
    // If the slider is at any non-zero value, back it up and set to 0.
    // If it is already at 0, restore from the backup (or 100 if no backup).
    // The backup values are written to disk so they survive overlay reopen.
    if (keysDown & KEY_Y) {
        auto doMuteToggle = [&](VolumeTrackBar* slider, u8& vol, u8& backup,
                                std::function<void(u8)> applyFn) -> bool {
            if (!slider || !slider->hasFocus()) return false;
            if (vol > 0) {
                backup = vol;           // store current level
                vol    = 0;
            } else {
                vol = (backup > 0) ? backup : static_cast<u8>(100);  // restore
            }
            slider->setProgress(vol);
            applyFn(vol);
            writeVolBackups(m_music_vol_backup, m_game_vol_backup, m_game_default_vol_backup);
            triggerNavigationFeedback();
            return true;
        };

        if (doMuteToggle(m_music_slider, m_music_vol, m_music_vol_backup,
            [](u8 v) { tuneSetVolume(float(v) / 100.f); }))
            return true;

        if (doMuteToggle(m_game_slider, m_game_vol, m_game_vol_backup,
            [this](u8 v) {
                const float fv = float(v) / 100.f;
                tuneSetTitleVolume(fv);
                config::set_title_volume(m_tid, fv);
            }))
            return true;

        if (doMuteToggle(m_game_default_slider, m_game_default_vol, m_game_default_vol_backup,
            [](u8 v) { tuneSetDefaultTitleVolume(float(v) / 100.f); }))
            return true;
    }

    if (SysTuneGui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight))
        return true;

    if ((keysDown & KEY_LEFT)
        && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK)
        && !(ult::onTrackBar.load(std::memory_order_acquire)
             && (ult::unlockedSlide.load(std::memory_order_acquire) || ult::allowSlide.load(std::memory_order_acquire)))) {
        setPlayerRightDest(PlayerRightDest::Settings);
        tsl::swapTo<MainGui>();
        triggerNavigationFeedback();
        return true;
    }
    return false;
}