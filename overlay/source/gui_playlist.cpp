#include "gui_playlist.hpp"
#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "config/config.hpp"
#include "play_context.hpp"
#include "tag_reader.hpp"
#include "symbol.hpp"
#include "tune.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// =============================================================================
// UI helpers
// =============================================================================

    constexpr float kIndicatorScale = 0.65f;
    constexpr s32   kSymMargin      = 19;

    /* Deferred focus item — set by click handlers, resolved in update(). */
    class ButtonListItem;
    ButtonListItem *g_focus_item = nullptr;

    /* Fire the count callback with the saved-playlist size. */
    void notifyCountChanged(const std::function<void(u32)> &cb) {
        if (!cb) return;
        cb(play_ctx::savedPlaylistSize());
    }

// =============================================================================
// ButtonListItem
//
// Label format: "<entry_num>  ⊘  <song> by <artist>"
//
// The play/pause indicator is shown ONLY when:
//   • play_ctx::source() == Playlist  (IPC queue == saved[])
//   • this item's full_path matches play_ctx::currentPath()
//
// When source == Folder the user is playing from a folder, so no indicator
// appears in the playlist view.
// =============================================================================

    class ButtonListItem final : public tsl::elm::ListItem {
        std::string m_full_path;
        std::string m_song_name;
        std::string m_artist_name;
        u32         m_entry_num;
        bool        m_was_current = false;

        static constexpr const char *kPlaceholder = "\uE098";

    public:
        static std::string buildLabel(u32 num,
                                      const std::string &song,
                                      const std::string &artist) {
            std::string s;
            s.reserve(8 + ult::DIVIDER_SYMBOL.size() + song.size() + 4 + artist.size());
            s += std::to_string(num);
            s += ult::DIVIDER_SYMBOL;
            s += song;
            s += " by ";
            s += artist;
            return s;
        }

        ButtonListItem(u32 entry_num,
                       std::string song_name,
                       std::string artist_name,
                       std::string full_path)
            : ListItem(buildLabel(entry_num, song_name, artist_name),
                       /*value=*/"", /*isMini=*/true)
            , m_full_path  (std::move(full_path))
            , m_song_name  (std::move(song_name))
            , m_artist_name(std::move(artist_name))
            , m_entry_num  (entry_num) {}

        const std::string &getFullPath() const { return m_full_path; }

        std::string getLabel() const {
            return buildLabel(m_entry_num, m_song_name, m_artist_name);
        }

        void setEntryNumber(u32 num) {
            if (m_entry_num == num) return;
            m_entry_num  = num;
            const std::string newLabel = buildLabel(num, m_song_name, m_artist_name);
            m_text       = newLabel;
            m_text_clean = newLabel;
            m_maxWidth          = 0;
            m_textWidth         = 0;
            m_scrollText.clear();
            m_ellipsisText.clear();
            m_flags.m_truncated = false;
        }

        /* True only when Playlist context is active AND this path is current. */
        bool isCurrent() const {
            if (play_ctx::source() != play_ctx::Source::Playlist) return false;
            const char *cp = play_ctx::currentPath();
            return !m_full_path.empty() && cp[0] != '\0' &&
                   strcasecmp(m_full_path.c_str(), cp) == 0;
        }

        void draw(tsl::gfx::Renderer *renderer) override {
            const bool cur = isCurrent();

            if (cur != m_was_current) {
                m_was_current = cur;
                setValue(cur ? kPlaceholder : "");
                m_maxWidth = 0;   // force layout recalc on state change
            }

            if (!cur) {
                ListItem::draw(renderer);
                return;
            }

            // ---- Indicator path --------------------------------------------
            const s32 scaledW = static_cast<s32>(26.0f * kIndicatorScale);

            if (!m_maxWidth) {
                const u16 textMaxW = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kSymMargin - scaledW - kSymMargin - 19);
                m_maxWidth = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kSymMargin - scaledW - kSymMargin - 55);
                const u16 textW = static_cast<u16>(
                    renderer->getTextDimensions(m_text_clean, false, 23).first);
                m_flags.m_truncated = (textW > textMaxW);
                if (m_flags.m_truncated) {
                    m_scrollText.clear();
                    m_scrollText.reserve(m_text_clean.size() * 2 + 8);
                    m_scrollText.append(m_text_clean).append("        ");
                    m_textWidth = static_cast<u16>(
                        renderer->getTextDimensions(m_scrollText, false, 23).first);
                    m_scrollText.append(m_text_clean);
                    m_ellipsisText = renderer->limitStringLength(
                        m_text_clean, false, 23, textMaxW);
                } else {
                    m_textWidth = static_cast<u16>(textW);
                }
            }

            {
                // Use swap to avoid a heap allocation on every frame.
                std::string saved_value;
                saved_value.swap(m_value);
                ListItem::draw(renderer);
                m_value.swap(saved_value);
            }

            const s32 cx = getX() + static_cast<s32>(getWidth()) - kSymMargin - scaledW / 2;
            const s32 cy = getY() + static_cast<s32>(m_listItemHeight) / 2;

            const bool useClickTextColor =
                m_touched &&
                Element::getInputMode() == tsl::InputMode::Touch &&
                ult::touchInBounds;

            const tsl::Color symColor = (m_focused && ult::useSelectionValue)
                ? (useClickTextColor ? tsl::clickTextColor : tsl::selectedValueTextColor)
                : tsl::onTextColor;

            const auto &sym = play_ctx::isPlaying()
                ? symbol::pause::symbol
                : symbol::play::symbol;

            sym.draw(play_ctx::isPlaying() ? cx - 1 : cx, cy, renderer, symColor, kIndicatorScale);
        }

        bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                     s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
            if (event == tsl::elm::TouchEvent::Touch)
                this->m_touched = this->inBounds(currX, currY);

            if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
                this->m_touched = false;
                if (Element::getInputMode() == tsl::InputMode::Touch) {
                    m_clickAnimationProgress = 0;
                    const bool handled = onClick(HidNpadButton_A);
                    //if (handled) tsl::shiftItemFocus(this);
                    return handled;
                }
            }
            return false;
        }
    };

} // namespace

// =============================================================================
// PlaylistGui constructor
//
// Builds the UI from play_ctx::savedPlaylist() — the user's persisted list —
// regardless of whether we are currently in Playlist or Folder context.
// =============================================================================

PlaylistGui::~PlaylistGui() {
    delete m_list;
}

PlaylistGui::PlaylistGui(std::function<void(u32)> on_count_changed)
    : m_on_count_changed(std::move(on_count_changed)) {

    m_list = new tsl::elm::List();

    // Re-snapshot the IPC queue order into g_saved so the list always reflects
    // the actual playback order (including any shuffle the service has applied).
    // This also calls poll() internally to refresh currentPath().
    play_ctx::resyncFromIPC();

    const auto &saved = play_ctx::savedPlaylist();
    const u32   count = static_cast<u32>(saved.size());

    if (count == 0) {
        m_list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            const size_t ix = x + (w - r->getTextDimensions("\uE150", false, 90).first) / 2;
            r->drawString("\uE150", false, ix, y + (h / 2) - 10+30, 90, tsl::defaultTextColor);
            const size_t tx = x + (w - r->getTextDimensions("Playlist is empty!", false, 25).first) / 2;
            r->drawString("Playlist is empty!", false, tx, y + (h / 2) + 90+30, 25, tsl::defaultTextColor);
        }), 380);
        return;
    }

    m_list->addItem(new tsl::elm::CategoryHeader(
        "Playlist " + ult::DIVIDER_SYMBOL +
        " \uE0E3 Remove "   + ult::DIVIDER_SYMBOL +
        " \uE0E2 Remove All " + ult::DIVIDER_SYMBOL +
        " \uE0B6 Set As Startup", true));

    m_items.reserve(count);

    // Jump target: only meaningful when source == Playlist (indicator would
    // be shown) — otherwise we leave the list at the top.
    const bool   inPlaylistCtx = (play_ctx::source() == play_ctx::Source::Playlist);
    const char  *cp            = play_ctx::currentPath();
    std::string  current_label;

    for (u32 i = 0; i < count; ++i) {
        const std::string &full_path = saved[i];

        TitleArtist ta = readTitleArtist(full_path.c_str());

        auto *item = new ButtonListItem(i + 1,
                                        std::move(ta.title),
                                        std::move(ta.artist),
                                        full_path);

        // Only record a jump target when the playlist is the active context.
        if (inPlaylistCtx && cp[0] != '\0' &&
            !strcasecmp(full_path.c_str(), cp) &&
            current_label.empty())
        {
            current_label = item->getLabel();
        }

        item->setClickListener([this, item](u64 keys) -> bool {
            const auto index      = this->m_list->getIndexInList(item);
            const auto tune_index = index - 1;  // subtract 1 for the header

            // ---- A: play / toggle / context-switch -------------------------
            if (keys & HidNpadButton_A) {

                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    // Already in playlist context — toggle or select.
                    const bool isCurrentTrack =
                        play_ctx::currentPath()[0] != '\0' &&
                        strcasecmp(item->getFullPath().c_str(),
                                   play_ctx::currentPath()) == 0;

                    if (isCurrentTrack) {
                        if (play_ctx::isPlaying()) { tunePause(); }
                        else                       { tunePlay();  }
                    } else {
                        tuneSelect(tune_index);
                    }
                    play_ctx::poll();

                } else {
                    // Folder context — g_saved is in original sorted order and
                    // tune_index is the sorted position.  If shuffle is on,
                    // tuneSelect(tune_index) would play m_shuffle_playlist[tune_index]
                    // which is a random song, not the one clicked.  Disable shuffle
                    // first so the sorted index maps correctly, then switch context.
                    {
                        TuneShuffleMode shuffleMode = TuneShuffleMode_Off;
                        tuneGetShuffleMode(&shuffleMode);
                        if (shuffleMode != TuneShuffleMode_Off) {
                            tuneSetShuffleMode(TuneShuffleMode_Off);
                            config::set_shuffle(TuneShuffleMode_Off);
                        }
                    }
                    play_ctx::switchToPlaylist(static_cast<u32>(tune_index));
                    play_ctx::poll();
                }
                return true;

            // ---- Y: remove this item from saved playlist -------------------
            } else if (keys & KEY_Y) {
                bool ok = false;
                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    // IPC and saved[] are in sync — remove from both.
                    ok = play_ctx::playlistTuneRemove(static_cast<u32>(tune_index));
                } else {
                    // Folder context — IPC has folder songs; only remove from saved[].
                    play_ctx::savedRemove(static_cast<u32>(tune_index));
                    ok = true;
                }

                if (ok) {
                    const s32 nextIndex = (index + 1 < this->m_list->getLastIndex())
                        ? index + 1 : index - 1;
                
                    this->removeFocus();
                    this->m_list->removeIndex(index);
                
                    if ((size_t)tune_index < this->m_items.size())
                        this->m_items.erase(this->m_items.begin() + tune_index);
                
                    // ---- NEW: handle empty playlist case -------------------------
                    if (this->m_items.empty()) {
                        this->m_list->clear();
                
                        m_list->addItem(new tsl::elm::CustomDrawer(
                            [](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                                const size_t ix = x + (w - r->getTextDimensions("\uE150", false, 90).first) / 2;
                                r->drawString("\uE150", false, ix, y + (h / 2) - 10 + 30, 90, tsl::defaultTextColor);
                
                                const size_t tx = x + (w - r->getTextDimensions("Playlist is empty!", false, 25).first) / 2;
                                r->drawString("Playlist is empty!", false, tx, y + (h / 2) + 90 + 30, 25, tsl::defaultTextColor);
                            }), 380);
                
                        notifyCountChanged(m_on_count_changed);
                        triggerMoveFeedback();
                        return true;
                    }
                    // --------------------------------------------------------------
                
                    // Re-number remaining items
                    for (size_t k = (size_t)tune_index; k < this->m_items.size(); ++k)
                        static_cast<ButtonListItem*>(this->m_items[k])->setEntryNumber(
                            static_cast<u32>(k + 1));
                
                    this->m_list->setFocusedIndex(nextIndex);
                
                    notifyCountChanged(m_on_count_changed);
                    triggerMoveFeedback();
                }
                return true;

            // ---- X: clear entire playlist ----------------------------------
            } else if (keys & KEY_X) {
                bool ok = false;
                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    ok = play_ctx::playlistTuneClearQueue();
                } else {
                    play_ctx::savedClear();
                    ok = true;
                }

                if (ok) {
                    this->removeFocus();
                    this->m_list->clear();
                    this->m_items.clear();
                    m_list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                        const size_t ix = x + (w - r->getTextDimensions("\uE150", false, 90).first) / 2;
                        r->drawString("\uE150", false, ix, y + (h / 2) - 10+30, 90, tsl::defaultTextColor);
                        const size_t tx = x + (w - r->getTextDimensions("Playlist is empty!", false, 25).first) / 2;
                        r->drawString("Playlist is empty!", false, tx, y + (h / 2) + 90+30, 25, tsl::defaultTextColor);
                    }), 380);
                    if (m_on_count_changed) {
                        triggerMoveFeedback();
                        m_on_count_changed(0);
                    }
                }
                return true;

            // ---- Minus: set as startup path --------------------------------
            } else if (keys & KEY_MINUS) {
                // Use saved[] path directly — it's the authoritative source.
                const auto &sp = play_ctx::savedPlaylist();
                if ((size_t)tune_index < sp.size()) {
                    config::set_load_path(sp[tune_index].c_str());
                    if (tsl::notification)
                        tsl::notification->showNow(item->getText(), 26, "Startup File Set", 2500, false);
                }
                return true;
            }
            return false;
        });

        m_list->addItem(item);
        m_items.push_back(item);
    }

    // Jump to currently playing item (only when in Playlist context and
    // the track is actually in this list).
    if (!current_label.empty())
        m_list->jumpToItem(current_label);
}

// =============================================================================

tsl::elm::Element *PlaylistGui::createUI() {
    auto *rootFrame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    rootFrame->setContent(this->m_list);
    return rootFrame;
}

// =============================================================================

void PlaylistGui::update() {
    // Resolve any deferred focus change set during a click handler.
    if (g_focus_item) {
        const auto index = m_list->getIndexInList(g_focus_item);
        if (index >= 0) {
            this->removeFocus();
            this->requestFocus(g_focus_item, tsl::FocusDirection::Down);
            m_list->setFocusedIndex(index);
            g_focus_item = nullptr;
        }
    }

    static u8 tick = 0;
    if ((++tick % 15) == 0)
        play_ctx::poll();
}

// ---------------------------------------------------------------------------
bool PlaylistGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                              HidAnalogStickState joyStickPosLeft,
                              HidAnalogStickState joyStickPosRight) {
    /* Left footer tap OR KEY_LEFT — swap back to player.
       Stack before: [SettingsGui, PlaylistGui]
       SwapDepth{2}: pop×2, push MainGui → [MainGui] → B exits cleanly. */
    const bool goLeft = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)
                     || ((keysDown & KEY_LEFT) && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK));
    if (goLeft) {
        setPlayerRightDest(PlayerRightDest::Playlist);
        tsl::swapTo<MainGui>(SwapDepth{2});
        triggerNavigationFeedback();
        return true;
    }
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos,
                                   joyStickPosLeft, joyStickPosRight);
}