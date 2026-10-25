#pragma once

#include "tune.h"
#include "symbol.hpp"

#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <tesla.hpp>

/* Global readable by the overlay frame to dim the OK button while R is held. */
inline std::atomic<bool> g_player_r_held{false};

class StatusBar final : public tsl::elm::Element {
  private:
    bool m_playing;
    TuneRepeatMode m_repeat;
    TuneShuffleMode m_shuffle;
    TuneCurrentStats m_stats;

    float m_percentage;

    /* -----------------------------------------------------------------------
     * Title display
     *
     * m_song_title_str  — display title: metadata TIT2/TT2 tag if found,
     *                     otherwise the bare filename without extension.
     * m_artist_str      — display artist: TPE1/TP1/ARTIST tag if found,
     *                     otherwise "Unknown Artist".
     * m_scroll_text     — doubled + padded title used during scrolling.
     * m_text_width      — pixel width of scroll text (or static title). 0 = remeasure.
     * m_artist_width    — pixel width of m_artist_str. 0 = remeasure.
     * m_scroll_offset   — current horizontal scroll offset for the title.
     * m_truncated       — true when title is wider than the available span.
     * m_counter         — pause timer before scrolling starts.
     * ----------------------------------------------------------------------- */
    std::string  m_song_title_str;
    std::string  m_artist_str;
    std::string  m_scroll_text;
    u32          m_text_width    = 0;
    u32          m_artist_width  = 0;
    u32          m_scroll_offset = 0;
    bool         m_truncated = false;
    u8           m_counter   = 0;

    /* Artist scroll state — mirrors the title scroll fields above. */
    std::string  m_artist_scroll_text;
    u32          m_artist_scroll_offset = 0;
    bool         m_artist_truncated = false;
    u8           m_artist_counter   = 0;

    bool m_touched = false;
    bool m_r_held  = false;  /* true while KEY_R is held */
    bool m_seeking = false;
    bool m_seek_engaged = false;    /* Gatefold: A picked the seek bar up, so left/right scrub */
    bool m_ctrl_scrubbing = false;  /* true while controller hold is previewing a seek */
    float m_seek_feedback_last_pct = -1.f; /* last seek % at which haptic fired; -1 = not yet fired this scrub */
    float m_seek_percentage = 0.0f;

    /* Hold-repeat state for Left/Right navigation and seek scrubbing. */
    u64  m_hold_start_ns  = 0;
    u64  m_last_repeat_ns = 0;

    /* Per-button click saturation animation (index 0-4, -1 = none active). */
    u64  m_btn_click_start_ns = 0;
    int  m_btn_click_idx      = -1;

    /* Pending seek — set by NudgeSeek/scrub commit, drained by update()
       on the main thread.  UINT32_MAX = no seek pending.
       libnx IPC sessions are thread-local so tuneSeek must always be
       called from the thread that opened the session. */
    std::atomic<u32> m_pending_seek_frame{UINT32_MAX};

    /* 0=Shuffle  1=Prev  2=Play  3=Next  4=Repeat */
    int m_active_btn = 2;

    std::function<void()> m_on_page_right;

    /* -----------------------------------------------------------------------
     * Album art
     *
     * m_art_rgba4444    — the ONLY art buffer. Pre-sized with resize() before
     *                     every load; written directly — no temp vector.
     * m_art_scaled_size — side length (px) at which m_art_rgba4444 was built,
     *                     or 0 if not yet loaded.
     * m_art_valid       — true when real image data is present (not placeholder).
     * m_art_path        — full path last passed to loadArt().
     * m_last_full_path  — pre-committed by the constructor so layout() can
     *                     trigger the initial load before the first draw().
     * --------------------------------------------------------------------- */
    std::string     m_art_path;
    std::vector<u8> m_art_rgba4444;
    s32             m_art_scaled_size = 0;
    bool            m_art_valid = false;

    std::string     m_last_full_path;
    u32             m_art_retry = 0;   /* update() ticks since the cover was last missing */

  public:
    StatusBar();

    tsl::elm::Element *requestFocus(tsl::elm::Element *oldFocus, tsl::FocusDirection direction) override;
    bool onClick(u64 keys) override;
    void draw(tsl::gfx::Renderer *renderer) override;
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override;
    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override;

    void update();
    void onHeld(u64 keysHeld);

    void setPageRightCallback(std::function<void()> cb) {
        m_on_page_right = std::move(cb);
    }

    static s32 PreferredHeight(s32 contentWidth) {
        s32 art = (contentWidth - 30) * 9 / 10;   /* matches ArtSize() */
        return art + 54 + tsl::style::ListItemDefaultHeight * 3;
    }

  private:
    ALWAYS_INLINE s32 ArtSize()   { return (this->getWidth() - 30) * 9 / 10; }
    ALWAYS_INLINE s32 ArtOffset() { return ArtSize() + 14; }

    ALWAYS_INLINE constexpr s32 CenterOfLine(u8 line) {
        return (tsl::style::ListItemDefaultHeight * line) + (tsl::style::ListItemDefaultHeight / 2);
    }

    ALWAYS_INLINE s32 GetRepeatX()    { return this->getX() + this->getWidth() - 30; }
    ALWAYS_INLINE s32 GetRepeatY()    { return this->getY() + ArtOffset() + CenterOfLine(2) - 5; }
    ALWAYS_INLINE s32 GetShuffleX()   { return this->getX() + 30; }
    ALWAYS_INLINE s32 GetShuffleY()   { return this->getY() + ArtOffset() + CenterOfLine(2) - 5; }
    ALWAYS_INLINE s32 GetPlayStateX() { return this->getX() + (this->getWidth() / 2); }
    ALWAYS_INLINE s32 GetPlayStateY() { return this->getY() + ArtOffset() + CenterOfLine(2) - 5; }
    ALWAYS_INLINE s32 GetPrevX()      { return this->getX() + ((this->getWidth() / 4) * 1); }
    ALWAYS_INLINE s32 GetPrevY()      { return this->getY() + ArtOffset() + CenterOfLine(2) - 5; }
    ALWAYS_INLINE s32 GetNextX()      { return this->getX() + ((this->getWidth() / 4) * 3); }
    ALWAYS_INLINE s32 GetNextY()      { return this->getY() + ArtOffset() + CenterOfLine(2) - 5; }

    std::pair<s32, s32> ButtonCenter(int i) {
        switch (i) {
            case 0: return {GetShuffleX(),   GetShuffleY()};
            case 1: return {GetPrevX(),      GetPrevY()};
            case 2: return {GetPlayStateX(), GetPlayStateY()};
            case 3: return {GetNextX(),      GetNextY()};
            case 4: return {GetRepeatX(),    GetRepeatY()};
        }
        return {GetPlayStateX(), GetPlayStateY()};
    }

    bool loadArt(const char *fullPath);
    void ensureArtScaled(s32 size);

    void ActivateButton(int i);
    void CycleRepeat();
    void CyclePlay();
    void CycleShuffle();
    void Prev();
    void Next();
    void NudgeSeek(int direction, u32 seconds = 5);  /* +1 = forward, -1 = back */
    const AlphaSymbol &GetPlaybackSymbol();
};