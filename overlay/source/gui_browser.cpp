#include "gui_browser.hpp"
#include "gui_main.hpp"

#include "config/config.hpp"
#include "play_context.hpp"
#include "tag_reader.hpp"
#include "symbol.hpp"
#include "tune.h"
#include "get_funcs.hpp"    // ult::isDirectory, ult::DirCloser
#include "pl_symbols.hpp"   // Songate's playlist icons
#include "tune_gfbg.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unordered_map>

namespace {

    bool ListItemTextCompare(const tsl::elm::ListItem *a, const tsl::elm::ListItem *b) {
        return strcasecmp(a->getText().c_str(), b->getText().c_str()) < 0;
    }

    bool StringTextCompare(const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    }

    ALWAYS_INLINE bool EndsWith(const char *name, const char *ext) {
        const size_t nlen = std::strlen(name);
        const size_t elen = std::strlen(ext);
        if (nlen < elen) return false;
        return strcasecmp(name + nlen - elen, ext) == 0;
    }

    // What Gatefold's background player decodes (sysmodule/source/decoder.cpp).
    constexpr const std::array SupportedTypes = { ".m4a", ".mp4", ".m4b", ".mp3", ".flac", ".wav", ".wave" };

    bool SupportsType(const char *name) {
        for (auto &ext : SupportedTypes)
            if (EndsWith(name, ext))
                return true;
        return false;
    }

    constexpr const char *const base_path = "/music/";

    /** musicRoot from Gatefold's settings, without the "sdmc:" and with a trailing slash; empty when unset. */
    std::string gatefoldMusicRoot() {
        FILE *f = fopen("/config/gatefold-nx/settings.txt", "r");
        if (!f) return {};
        char line[FS_MAX_PATH + 16];
        std::string root;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "musicRoot=", 10) != 0) continue;
            root = line + 10;
            while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) root.pop_back();
        }
        fclose(f);
        if (root.rfind("sdmc:", 0) == 0) root.erase(0, 5);
        if (!root.empty() && root.back() != '/') root += '/';
        return root;
    }

    /* Builds the display label used for a file entry. */
    static std::string buildFileLabel(u32 num,
                                      const std::string &title,
                                      const std::string &artist) {
        // Reserve upfront to avoid reallocations from 4 concatenations.
        std::string s;
        s.reserve(8 + ult::DIVIDER_SYMBOL.size() + title.size() + 4 + artist.size());
        s += std::to_string(num);
        s += ult::DIVIDER_SYMBOL;
        s += title;
        s += " by ";
        s += artist;
        return s;
    }

    struct FileEntry {
        std::string filename;
        std::string full_path;
        std::string title;
        std::string artist;
    };

    bool FileEntryCompare(const FileEntry &a, const FileEntry &b) {
        return strcasecmp(a.filename.c_str(), b.filename.c_str()) < 0;
    }

// =============================================================================
// BrowserFileItem
//
// A ListItem that draws a play/pause indicator exactly like ButtonListItem in
// gui_playlist.cpp, but conditioned on Folder context + matching folder path.
// =============================================================================

    constexpr float kBFI_Scale     = 0.65f;
    constexpr s32   kBFI_SymMargin = 19;

    class BrowserFileItem final : public tsl::elm::ListItem {
        std::string m_full_path;
        std::string m_folder_path;  // the directory this file lives in
        bool        m_was_current = false;

        static constexpr const char *kPlaceholder = "\uE098";

    public:
        BrowserFileItem(const std::string &label,
                        const std::string &full_path,
                        const std::string &folder_path)
            : ListItem(label, "", /*isMini=*/true)
            , m_full_path(full_path)
            , m_folder_path(folder_path) {}

        const std::string &getFullPath() const { return m_full_path; }

        /* True only when Folder context is active for THIS folder and this
           file is the currently playing track. */
        bool isCurrent() const {
            if (play_ctx::source() != play_ctx::Source::Folder) return false;
            if (play_ctx::folderPath() != m_folder_path)        return false;
            const char *cp = play_ctx::currentPath();
            return cp[0] != '\0' &&
                   strcasecmp(m_full_path.c_str(), cp) == 0;
        }

        void draw(tsl::gfx::Renderer *renderer) override {
            const bool cur = isCurrent();

            if (cur != m_was_current) {
                m_was_current = cur;
                setValue(cur ? kPlaceholder : "");
                m_maxWidth = 0;   // force layout recalc on next draw
            }

            if (!cur) {
                ListItem::draw(renderer);
                return;
            }

            // ---- Draw base item text (suppress the placeholder value so we
            //      can draw the symbol ourselves at the right scale) ----------
            const s32 scaledW = static_cast<s32>(26.0f * kBFI_Scale);

            if (!m_maxWidth) {
                const u16 textMaxW = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kBFI_SymMargin - scaledW - kBFI_SymMargin - 19);
                m_maxWidth = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kBFI_SymMargin - scaledW - kBFI_SymMargin - 55);
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
                // Temporarily suppress the placeholder so the base draw doesn't
                // render it — we draw the symbol ourselves at the right scale.
                // Use swap instead of copy to avoid a heap allocation every frame.
                std::string saved_value;
                saved_value.swap(m_value);
                ListItem::draw(renderer);
                m_value.swap(saved_value);
            }

            // ---- Draw play/pause symbol ------------------------------------
            const s32 cx = getX() + static_cast<s32>(getWidth()) - kBFI_SymMargin - scaledW / 2;
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

            sym.draw(play_ctx::isPlaying() ? cx - 1 : cx, cy, renderer, symColor, kBFI_Scale);
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

// =============================================================================
// BrowserFolderItem
//
// A ListItem for directory entries that shows the same play/pause indicator
// as BrowserFileItem whenever the currently playing file lives inside this
// folder (or any subfolder of it).  Clicking still just navigates into the
// folder — the indicator is purely visual.
// =============================================================================

    class BrowserFolderItem final : public tsl::elm::ListItem {
        std::string m_sub_path;      // absolute path of this folder, trailing slash included
        bool        m_was_ancestor = false;

    public:
        BrowserFolderItem(const std::string &name, const std::string &sub_path)
            : ListItem(name, "", /*isMini=*/true)
            , m_sub_path(sub_path) {}

        /* True when Folder context is active and the playing file is inside
           this folder or any of its subdirectories. */
        bool isAncestor() const {
            if (play_ctx::source() != play_ctx::Source::Folder) return false;
            const std::string &fp = play_ctx::folderPath();
            // folderPath must start with m_sub_path (which ends with '/').
            return fp.size() >= m_sub_path.size() &&
                   fp.compare(0, m_sub_path.size(), m_sub_path) == 0;
        }

        void draw(tsl::gfx::Renderer *renderer) override {
            const bool anc = isAncestor();

            if (anc != m_was_ancestor) {
                m_was_ancestor = anc;
                setValue(anc ? ult::INPROGRESS_SYMBOL : "");
                m_maxWidth = 0;   // force layout recalc
            }

            ListItem::draw(renderer);
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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BrowserGui::BrowserGui(std::string path, std::string focus_name, std::string root,
                       std::function<void(u32)> on_count_changed, int depth)
    : m_frame(nullptr), m_list(nullptr),
      m_cwd(std::move(path)), m_root(std::move(root)), m_focus_name(std::move(focus_name)),
      m_on_count_changed(std::move(on_count_changed)), m_depth(depth) {

    if (m_cwd.empty()) {
        // Gatefold's music folder (settings.txt musicRoot), so Browse starts where the library is.
        const std::string music = gatefoldMusicRoot();
        if (!music.empty() && ult::isDirectory(music)) {
            m_cwd  = music;
            m_root = music;
        } else if (ult::isDirectory(base_path)) {
            m_cwd  = base_path;
            m_root = base_path;
        } else {
            m_cwd  = "/";
            m_root = "/";
        }
    }
}

BrowserGui::~BrowserGui() {
    delete m_list;
}

// ---------------------------------------------------------------------------

void BrowserGui::notifyCountChanged() const {
    if (!m_on_count_changed) return;
    // Always report the size of the user's saved playlist, regardless of
    // whether we're currently in Folder or Playlist context.
    m_on_count_changed(play_ctx::savedPlaylistSize());
}

// ---------------------------------------------------------------------------

tsl::elm::Element *BrowserGui::createUI() {
    m_frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    m_list  = new tsl::elm::List();

    buildList();

    m_frame->setContent(m_list);
    return m_frame;
}

// ---------------------------------------------------------------------------

void BrowserGui::update() {
    static u8 tick = 0;
    if ((++tick % 15) == 0)
        play_ctx::poll();
}

// ---------------------------------------------------------------------------

bool BrowserGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) {
    /* Left footer tap OR KEY_LEFT — store where we are, mark dest as Browse,
       then swap [SettingsGui, BrowseGui, BrowserGui] with a fresh MainGui (m_depth: the pages above Settings). */
    const bool goLeft = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)
                     || ((keysDown & KEY_LEFT) && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK));
    if (goLeft) {
        setBrowserReturnPath(m_cwd, m_root);
        setPlayerRightDest(PlayerRightDest::Browse);
        tsl::swapTo<MainGui>(SwapDepth{u32(m_depth + 1)});
        triggerNavigationFeedback();
        return true;
    }

    SysTuneGui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight);

    if (keysDown & HidNpadButton_B) {
        /* At root: return false so the base class calls goBack(), popping
           BrowserGui and returning to Browse. */
        if (m_cwd == m_root)
            return false;
        /* Inside a subdir: swap this BrowserGui with one at the parent directory.
           focus_name = current dir name so the cursor lands on the right item.
           Stack stays constant: [SettingsGui, BrowserGui]. */
        tsl::swapTo<BrowserGui>(parentPath(), currentDirName(), m_root, m_on_count_changed, m_depth);
        triggerExitFeedback();
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// buildList
// ---------------------------------------------------------------------------

void BrowserGui::buildList() {
    // Use POSIX opendir/readdir — no FsFileSystem handle needed.
    std::unique_ptr<DIR, ult::DirCloser> d(opendir(m_cwd.c_str()));
    if (!d) {
        m_list->addItem(new tsl::elm::ListItem("Couldn't open: " + m_cwd));
        return;
    }

    std::vector<tsl::elm::ListItem *> folders;
    std::vector<FileEntry>            file_entries;

    folders.reserve(32);
    file_entries.reserve(32);

    constexpr u32 kScanMax = 2048;
    bool hit_max = false;

    struct dirent *ent;
    while ((ent = readdir(d.get())) != nullptr) {
        if (folders.size() + file_entries.size() >= kScanMax) {
            hit_max = true;
            break;
        }
        if (ent->d_name[0] == '.') continue;  // skip hidden and . / ..

        if (ent->d_type == DT_DIR) {
            const std::string sub_path  = m_cwd + ent->d_name + "/";
            const std::string root_copy = m_root;

            auto *item = new BrowserFolderItem(ent->d_name, sub_path);
            item->setClickListener([this, item, sub_path, root_copy](u64 down) -> bool {
                if (down & HidNpadButton_A) {
                    //tsl::shiftItemFocus(item);
                    // swapTo keeps the stack at constant depth [SettingsGui, BrowserGui].
                    // focus_name="" — we're entering, not returning, so no item to focus.
                    tsl::swapTo<BrowserGui>(sub_path, "", root_copy, m_on_count_changed, m_depth);
                    return true;
                }
                if (down & HidNpadButton_Y) {
                    addAllToPlaylist(sub_path);
                    return true;
                }
                if (down & KEY_MINUS) {
                    const std::string no_slash = sub_path.substr(0, sub_path.size() - 1);
                    config::set_load_path(no_slash.c_str());
                    if (tsl::notification)
                        tsl::notification->showNow(item->getText(), 26, "Startup Folder Set", 2500, false);
                    return true;
                }
                return false;
            });
            folders.push_back(item);

        } else if (ent->d_type == DT_REG && SupportsType(ent->d_name)) {
            TitleArtist ta = readTitleArtist((m_cwd + ent->d_name).c_str());
            file_entries.push_back({
                std::string(ent->d_name),
                m_cwd + ent->d_name,
                std::move(ta.title),
                std::move(ta.artist)
            });
        }
    }

    if (hit_max) {
        if (tsl::notification)
            tsl::notification->showNow(
                "Maximum of " + std::to_string(kScanMax) + " hit!", 26,
                "Stopped Scanning Folder", 2500, false);
    }

    if (folders.empty() && file_entries.empty()) {
        m_list->addItem(new tsl::elm::CategoryHeader("Empty..."));
        return;
    }

    tsl::elm::ListItem *focus_elm              = nullptr;  // default focus (first item / nav-back)
    tsl::elm::ListItem *playing_focus_elm      = nullptr;  // overrides when this folder is active
    tsl::elm::ListItem *playing_folder_focus_elm = nullptr; // subfolder that contains the playing file

    // Compute which direct child folder (if any) is an ancestor of the
    // currently playing file's folder.  Used to auto-focus the right subfolder
    // when the user navigates back up from a deeply-nested playing context.
    // Uses pointer arithmetic to avoid temporary string allocations.
    std::string playing_subfolder_name;
    {
        if (play_ctx::source() == play_ctx::Source::Folder) {
            const std::string &fp = play_ctx::folderPath();
            if (fp.size() > m_cwd.size() &&
                fp.compare(0, m_cwd.size(), m_cwd) == 0)
            {
                const char *rest  = fp.c_str() + m_cwd.size();
                const char *slash = std::strchr(rest, '/');
                if (slash && slash > rest)
                    playing_subfolder_name.assign(rest, slash - rest);
            }
        }
    }

    // ---- Folders -----------------------------------------------------------
    if (!folders.empty()) {
        m_list->addItem(new tsl::elm::CategoryHeader(
            m_cwd + " " + ult::DIVIDER_SYMBOL + " \uE0E3 Add To Playlist " +
            ult::DIVIDER_SYMBOL + " \uE0B6 Set As Startup", true));

        std::sort(folders.begin(), folders.end(), ListItemTextCompare);
        for (auto *el : folders) {
            m_list->addItem(el);
            if (!m_focus_name.empty() && el->getText() == m_focus_name)
                focus_elm = el;
            // Track which subfolder leads toward the currently playing file.
            if (!playing_subfolder_name.empty() && el->getText() == playing_subfolder_name)
                playing_folder_focus_elm = el;
        }
        if (!focus_elm)
            focus_elm = folders[0];
    }

    // ---- Files -------------------------------------------------------------
    if (!file_entries.empty()) {
        m_list->addItem(new tsl::elm::CategoryHeader(
            "Tracks " + ult::DIVIDER_SYMBOL +
            " \uE0E3 Add To Playlist " + ult::DIVIDER_SYMBOL +
            " \uE0E2 Add All "         + ult::DIVIDER_SYMBOL +
            " \uE0B6 Set As Startup"));

        std::sort(file_entries.begin(), file_entries.end(), FileEntryCompare);

        // Always populate m_folder_songs from the SORTED file_entries BEFORE
        // any shuffle reorder.  This guarantees it matches the service's
        // m_playlist (enqueue order = sorted), so sorted_idx lookups at click
        // time are always correct regardless of what display order is used.
        m_folder_songs.clear();
        m_folder_songs.reserve(file_entries.size());
        for (const auto &fe : file_entries)
            m_folder_songs.push_back(fe.full_path);

        // If this folder is the active playing context and shuffle is on,
        // reorder file_entries (display only) to match the IPC queue order.
        // m_folder_songs is intentionally NOT rebuilt here — it stays sorted.
        {
            TuneShuffleMode shuffleMode = TuneShuffleMode_Off;
            tuneGetShuffleMode(&shuffleMode);

            const bool thisFolderIsActive =
                play_ctx::source() == play_ctx::Source::Folder &&
                play_ctx::folderPath() == m_cwd;

            if (shuffleMode != TuneShuffleMode_Off && thisFolderIsActive) {
                u32 ipc_count = 0;
                if (R_SUCCEEDED(tuneGetPlaylistSize(&ipc_count)) && ipc_count > 0) {
                    std::unordered_map<std::string, FileEntry> lookup;
                    lookup.reserve(file_entries.size());
                    for (auto &fe : file_entries)
                        lookup.emplace(fe.full_path, std::move(fe));

                    std::vector<FileEntry> reordered;
                    reordered.reserve(ipc_count);

                    char ipc_path[FS_MAX_PATH];
                    for (u32 i = 0; i < ipc_count; ++i) {
                        if (R_SUCCEEDED(tuneGetPlaylistItem(i, ipc_path, sizeof(ipc_path)))) {
                            auto it = lookup.find(ipc_path);
                            if (it != lookup.end()) {
                                reordered.push_back(std::move(it->second));
                                lookup.erase(it);
                            }
                        }
                    }
                    // Append any filesystem entries not found in the IPC queue.
                    for (auto &[_, fe] : lookup)
                        reordered.push_back(std::move(fe));

                    file_entries = std::move(reordered);
                }
            }
        }

        // Determine whether this folder is currently the active playing context
        // so we can pre-select the currently playing item for focus.
        const bool thisFolderActive =
            play_ctx::source() == play_ctx::Source::Folder &&
            play_ctx::folderPath() == m_cwd;
        const char *cp = play_ctx::currentPath();

        for (u32 idx = 0; idx < static_cast<u32>(file_entries.size()); ++idx) {
            const FileEntry &fe = file_entries[idx];

            const std::string label     = buildFileLabel(idx + 1, fe.title, fe.artist);
            const std::string full_path = fe.full_path;

            auto *item = new BrowserFileItem(label, full_path, m_cwd);

            // Capture only 'this' and 'item'. The correct sorted index is
            // resolved at click time via std::find on m_folder_songs (always
            // sorted), so it is never stale from a shuffle reorder.
            item->setClickListener([this, item](u64 down) -> bool {
                const std::string &full_path = item->getFullPath();

                // ---- A: play from this folder ------------------------------
                if (down & HidNpadButton_A) {
                    // Toggle play/pause if tapping the already-playing track.
                    if (play_ctx::source() == play_ctx::Source::Folder &&
                        play_ctx::folderPath() == m_cwd &&
                        play_ctx::currentPath()[0] != '\0' &&
                        strcasecmp(full_path.c_str(), play_ctx::currentPath()) == 0)
                    {
                        if (play_ctx::isPlaying()) { tunePause(); }
                        else                       { tunePlay();  }
                        play_ctx::poll();
                        return true;
                    }

                    // m_folder_songs is sorted — binary search for the sorted index.
                    // All paths share the same m_cwd prefix so comparing full
                    // paths with the same case-insensitive order is correct.
                    const auto it = std::lower_bound(
                        m_folder_songs.begin(), m_folder_songs.end(), full_path,
                        [](const std::string &a, const std::string &b) {
                            return strcasecmp(a.c_str(), b.c_str()) < 0;
                        });
                    const u32 sorted_idx =
                        (it != m_folder_songs.end() &&
                         strcasecmp(it->c_str(), full_path.c_str()) == 0)
                        ? static_cast<u32>(it - m_folder_songs.begin()) : 0u;

                    const bool samefolder =
                        play_ctx::source() == play_ctx::Source::Folder &&
                        play_ctx::folderPath() == m_cwd;

                    TuneShuffleMode shuffleMode = TuneShuffleMode_Off;
                    tuneGetShuffleMode(&shuffleMode);

                    if (samefolder && shuffleMode != TuneShuffleMode_Off) {
                        // Same folder, shuffle on: IPC queue is already loaded
                        // in the correct shuffle order. Scan it to find the
                        // exact shuffled position — no queue reload needed.
                        play_ctx::startByPath(full_path);
                    } else {
                        // Different folder, or shuffle off. Shuffle stays as it is: with it on, the
                        // new queue is shuffled with this track first (tune_gfbg.cpp, shuffleFresh),
                        // and the list above follows the queue's order when the folder is shown again.
                        if (!play_ctx::switchToFolder(m_cwd, m_folder_songs, sorted_idx)) {
                            if (tsl::notification)
                                tsl::notification->showNow("Failed to switch to folder.");
                        }
                    }

                    play_ctx::poll();
                    notifyCountChanged();
                    return true;
                }

                // ---- Y: add to playlist (saved[], and IPC when in Playlist ctx)
                if (down & HidNpadButton_Y) {
                    //tsl::shiftItemFocus(item);
                    if (play_ctx::source() == play_ctx::Source::Playlist) {
                        // Playlist context — add to IPC and saved[] together.
                        Result r = tuneEnqueue(full_path.c_str(), TuneEnqueueType_Back);
                        if (R_SUCCEEDED(r)) {
                            play_ctx::savedAppend(full_path);
                            if (tsl::notification)
                                tsl::notification->showNow("Added 1 track to Playlist.");
                            notifyCountChanged();
                        } else {
                            if (tsl::notification)
                                tsl::notification->showNow("Failed to add track.");
                        }
                    } else {
                        // Folder context — IPC has folder songs; only update saved[].
                        play_ctx::savedAppend(full_path);
                        if (tsl::notification)
                            tsl::notification->showNow("Added 1 track to Playlist.");
                        notifyCountChanged();
                    }
                    return true;
                }

                // ---- X: add all songs in this directory --------------------
                if (down & HidNpadButton_X) {
                    addAllToPlaylist(m_cwd);
                    return true;
                }

                // ---- Minus: set as startup file ----------------------------
                if (down & KEY_MINUS) {
                    config::set_load_path(full_path.c_str());
                    if (tsl::notification)
                        tsl::notification->showNow(item->getText(), 26, "Startup File Set", 2500, false);
                    return true;
                }
                return false;
            });

            // Pre-set focus on the currently playing item when re-entering
            // this folder while it is the active folder context.
            if (thisFolderActive && cp[0] != '\0' &&
                strcasecmp(full_path.c_str(), cp) == 0)
            {
                playing_focus_elm = item;
            }

            if (!focus_elm)
                focus_elm = item;

            m_list->addItem(item);
        }
    }

    // ---- Focus resolution --------------------------------------------------
    // Priority: playing file (exact folder match)
    //         > nav-back name (explicit user navigation, always respected)
    //         > playing subfolder (breadcrumb toward playing file, passive)
    //         > first item
    if (playing_focus_elm) {
        m_list->jumpToItem(playing_focus_elm->getText());
    } else if (!m_focus_name.empty()) {
        // User just pressed B out of this child — return cursor there.
        m_list->jumpToItem(m_focus_name);
    } else if (playing_folder_focus_elm) {
        // No explicit nav target — guide cursor toward the playing file.
        m_list->jumpToItem(playing_folder_focus_elm->getText());
    } else if (focus_elm) {
        tsl::Gui::requestFocus(focus_elm, tsl::FocusDirection::None);
    }
}

// ---------------------------------------------------------------------------

std::string BrowserGui::parentPath() const {
    if (m_cwd.size() <= 1) return m_cwd;
    const size_t pos = m_cwd.rfind('/', m_cwd.size() - 2);
    if (pos == std::string::npos) return "/";
    return m_cwd.substr(0, pos + 1);
}

std::string BrowserGui::currentDirName() const {
    if (m_cwd.size() <= 1) return "";
    const size_t end   = m_cwd.size() - 1;
    const size_t start = m_cwd.rfind('/', end - 1);
    if (start == std::string::npos) return m_cwd.substr(0, end);
    return m_cwd.substr(start + 1, end - start - 1);
}

// ---------------------------------------------------------------------------
// addAllToPlaylist
//
// Adds every supported file in 'path' to the user's playlist.
// When in Playlist context: adds to IPC AND saved[].
// When in Folder context:   adds to saved[] only (IPC has folder songs).
// ---------------------------------------------------------------------------

void BrowserGui::addAllToPlaylist(const std::string &path) {
    std::unique_ptr<DIR, ult::DirCloser> d(opendir(path.c_str()));
    if (!d) {
        if (tsl::notification) tsl::notification->show("something went wrong :/");
        return;
    }

    std::vector<std::string> file_list;
    file_list.reserve(64);
    constexpr u32 kAddAllMax = 300;

    struct dirent *ent;
    while ((ent = readdir(d.get())) != nullptr && file_list.size() < kAddAllMax) {
        if (ent->d_type == DT_REG && SupportsType(ent->d_name))
            file_list.emplace_back(ent->d_name);
    }

    std::sort(file_list.begin(), file_list.end(), StringTextCompare);

    s64 songs_added = 0;
    const bool inPlaylist = (play_ctx::source() == play_ctx::Source::Playlist);
    Result rc = 0;

    for (const auto &file : file_list) {
        const std::string full = path + file;

        if (inPlaylist) {
            rc = tuneEnqueue(full.c_str(), TuneEnqueueType_Back);
            if (R_SUCCEEDED(rc)) {
                play_ctx::savedAppend(full);
                songs_added++;
            }
        } else {
            play_ctx::savedAppend(full);
            songs_added++;
        }
    }

    char msg[64];
    std::snprintf(msg, sizeof(msg), "Added %ld tracks to Playlist.", songs_added);
    if (tsl::notification) tsl::notification->showNow(msg);
    if (songs_added > 0)
        notifyCountChanged();
}

// =============================================================================
// Browse › Folders / Playlists (Songate, 2026-10-30)
// =============================================================================

namespace {

    /* A list item with one of Songate's icons before its text (pl_symbols.hpp: the app's own, rasterised): the item is
       drawn 30 px to the right, the icon in the room it leaves; its value stays at the right edge. */
    class IconListItem final : public tsl::elm::ListItem {
        const AlphaSymbol *m_icon;
      public:
        IconListItem(const std::string &text, const std::string &value, const AlphaSymbol &icon)
            : ListItem(text, value), m_icon(&icon) {}

        void draw(tsl::gfx::Renderer *renderer) override {
            const s32 x = getX(), y = getY(), w = s32(getWidth()), h = s32(getHeight());
            setBoundaries(x + 30, y, w - 30, h);
            ListItem::draw(renderer);
            setBoundaries(x, y, w, h);
            const tsl::Color c = (m_focused && ult::useSelectionText) ? tsl::selectedTextColor : tsl::defaultTextColor;
            m_icon->draw(x + 31, y + h / 2, renderer, c);
        }

        bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
            if (event == tsl::elm::TouchEvent::Touch) this->m_touched = this->inBounds(currX, currY);
            if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
                this->m_touched = false;
                if (Element::getInputMode() == tsl::InputMode::Touch) {
                    m_clickAnimationProgress = 0;
                    return onClick(HidNpadButton_A);
                }
            }
            return false;
        }
    };

    /* Left (the D-pad or the footer's tap): back to the player, which comes back to this page. depth: the pages above
       Settings, this one included. */
    bool leftToPlayer(u64 keysDown, u64 keysHeld, const std::string &target, const std::string &root, int depth) {
        const bool goLeft = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)
                         || ((keysDown & KEY_LEFT) && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK));
        if (!goLeft) return false;
        setBrowserReturnPath(target, root);
        setPlayerRightDest(PlayerRightDest::Browse);
        tsl::swapTo<MainGui>(SwapDepth{u32(depth + 1)});
        triggerNavigationFeedback();
        return true;
    }

    /* The playing source's own page: a playlist plays as the folder "#<its name>". */
    bool playingPlaylist() {
        return play_ctx::source() == play_ctx::Source::Folder && !play_ctx::folderPath().empty() && play_ctx::folderPath()[0] == '#' &&
               play_ctx::currentPath()[0] != '\0';
    }
    bool playingFolder() {
        return play_ctx::source() == play_ctx::Source::Folder && !playingPlaylist() && play_ctx::currentPath()[0] != '\0';
    }

    /* The four kinds of Browse › Playlists, as Songate's playlists are: Normal (Liked, yours, a folder's), Smart, Mood,
       Automix; the icon of each kind as the app's "New playlist" shows it. */
    char groupOf(char kind) {
        switch (kind) {
            case GFBG_KIND_SMART:   return 'S';
            case GFBG_KIND_MOOD:    return 'M';
            case GFBG_KIND_AUTOMIX: return 'A';
            default:                return 'N';
        }
    }
    const char *groupName(char g) { return g == 'S' ? "Smart" : g == 'M' ? "Mood" : g == 'A' ? "Automix" : "Normal"; }
    const AlphaSymbol &groupIcon(char g) {
        return g == 'S' ? plsym::smart : g == 'M' ? plsym::mood : g == 'A' ? plsym::sparkle : plsym::list;
    }
    /* The kicker Songate gives a playlist (App::playlistKind). */
    const char *kindLabel(char kind) {
        switch (kind) {
            case GFBG_KIND_AUTOMIX: return "Automix Playlist";
            case GFBG_KIND_MOOD:    return "Mood Playlist";
            case GFBG_KIND_SMART:   return "Smart Playlist";
            case GFBG_KIND_FOLDER:  return "Folder Playlist";
            default:                return "Playlist";
        }
    }
    std::string tracksText(u32 n) { return n == 1 ? "1 track" : std::to_string(n) + " tracks"; }

} // namespace

// ---------------------------------------------------------------------------
// Browse: Folders and Playlists

BrowseGui::BrowseGui(std::function<void(u32)> on_count_changed) : m_on_count_changed(std::move(on_count_changed)) {}
BrowseGui::~BrowseGui() { delete m_list; }

tsl::elm::Element *BrowseGui::createUI() {
    auto *frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    m_list = new tsl::elm::List();
    m_list->addItem(new tsl::elm::CategoryHeader("Browse"));
    auto *folders = new IconListItem("Folders", playingFolder() ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL, plsym::folder);
    folders->setClickListener([this](u64 keys) -> bool {
        if (!(keys & HidNpadButton_A)) return false;
        tsl::changeTo<BrowserGui>("", "", "", m_on_count_changed, 2);
        return true;
    });
    m_list->addItem(folders);
    auto *lists = new IconListItem("Playlists", playingPlaylist() ? ult::INPROGRESS_SYMBOL : ult::DROPDOWN_SYMBOL, plsym::list);
    lists->setClickListener([this](u64 keys) -> bool {
        if (!(keys & HidNpadButton_A)) return false;
        tsl::changeTo<PlaylistKindsGui>(m_on_count_changed);
        return true;
    });
    m_list->addItem(lists);
    if (playingPlaylist()) m_list->jumpToItem("Playlists");
    frame->setContent(m_list);
    return frame;
}

void BrowseGui::update() {
    static u8 tick = 0;
    if ((++tick % 15) == 0) play_ctx::poll();
}

bool BrowseGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState l, HidAnalogStickState r) {
    if (leftToPlayer(keysDown, keysHeld, "@browse", "", 1)) return true;
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos, l, r);
}

// ---------------------------------------------------------------------------
// Playlists: Normal, Smart, Mood, Automix

PlaylistKindsGui::PlaylistKindsGui(std::function<void(u32)> on_count_changed) : m_on_count_changed(std::move(on_count_changed)) {}
PlaylistKindsGui::~PlaylistKindsGui() { delete m_list; }

tsl::elm::Element *PlaylistKindsGui::createUI() {
    auto *frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    m_list = new tsl::elm::List();
    m_list->addItem(new tsl::elm::CategoryHeader("Playlists"));
    const std::vector<GfPlaylist> all = gfPlaylists();
    if (all.empty()) {
        m_list->addItem(new tsl::elm::ListItem("Open Songate once to list its playlists"));
        frame->setContent(m_list);
        return frame;
    }
    // which kind plays now: the playlist the queue was made from
    char playing = 0;
    if (playingPlaylist())
        for (const auto &pl : all)
            if (play_ctx::folderPath().size() > 1 && play_ctx::folderPath().compare(1, std::string::npos, pl.name) == 0) playing = groupOf(pl.kind);
    for (char g : { 'N', 'S', 'M', 'A' }) {
        u32 n = 0;
        for (const auto &pl : all) n += groupOf(pl.kind) == g;
        const std::string value = playing == g ? ult::INPROGRESS_SYMBOL : n == 1 ? std::string("1 playlist") : std::to_string(n) + " playlists";
        auto *item = new IconListItem(groupName(g), value, groupIcon(g));
        item->setClickListener([this, g](u64 keys) -> bool {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<PlaylistListGui>(g, m_on_count_changed);
            return true;
        });
        m_list->addItem(item);
    }
    if (playing) m_list->jumpToItem(groupName(playing));
    frame->setContent(m_list);
    return frame;
}

void PlaylistKindsGui::update() {
    static u8 tick = 0;
    if ((++tick % 15) == 0) play_ctx::poll();
}

bool PlaylistKindsGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState l, HidAnalogStickState r) {
    if (leftToPlayer(keysDown, keysHeld, "@playlists", "", 2)) return true;
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos, l, r);
}

// ---------------------------------------------------------------------------
// One kind's playlists

PlaylistListGui::PlaylistListGui(char group, std::function<void(u32)> on_count_changed)
    : m_group(group), m_on_count_changed(std::move(on_count_changed)) {}
PlaylistListGui::~PlaylistListGui() { delete m_list; }

tsl::elm::Element *PlaylistListGui::createUI() {
    auto *frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    m_list = new tsl::elm::List();
    m_list->addItem(new tsl::elm::CategoryHeader(std::string(groupName(m_group)) + " Playlists"));
    std::string focus;
    u32 shown = 0;
    for (const GfPlaylist &pl : gfPlaylists()) {
        if (groupOf(pl.kind) != m_group) continue;
        const bool now = playingPlaylist() && play_ctx::folderPath().size() > 1 && play_ctx::folderPath().compare(1, std::string::npos, pl.name) == 0;
        if (now) focus = pl.name;
        auto *item = new IconListItem(pl.name, now ? ult::INPROGRESS_SYMBOL : tracksText(pl.count), plsym::of(pl.icon.c_str()));
        const char g = m_group, kind = pl.kind;
        const std::string name = pl.name;
        item->setClickListener([this, g, kind, name](u64 keys) -> bool {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<PlaylistTracksGui>(g, name, kind, m_on_count_changed);
            return true;
        });
        m_list->addItem(item);
        shown++;
    }
    if (!shown) m_list->addItem(new tsl::elm::ListItem(std::string("No ") + groupName(m_group) + " playlists yet"));
    if (!focus.empty()) m_list->jumpToItem(focus);
    frame->setContent(m_list);
    return frame;
}

void PlaylistListGui::update() {
    static u8 tick = 0;
    if ((++tick % 15) == 0) play_ctx::poll();
}

bool PlaylistListGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState l, HidAnalogStickState r) {
    if (leftToPlayer(keysDown, keysHeld, std::string("@kind:") + m_group, "", 3)) return true;
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos, l, r);
}

// ---------------------------------------------------------------------------
// A playlist's tracks: a folder's actions and words

PlaylistTracksGui::PlaylistTracksGui(char group, std::string name, char kind, std::function<void(u32)> on_count_changed)
    : m_group(group), m_kind(kind), m_name(std::move(name)), m_on_count_changed(std::move(on_count_changed)) {}
PlaylistTracksGui::~PlaylistTracksGui() { delete m_list; }

void PlaylistTracksGui::notifyCountChanged() const {
    if (m_on_count_changed) m_on_count_changed(play_ctx::savedPlaylistSize());
}

tsl::elm::Element *PlaylistTracksGui::createUI() {
    auto *frame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    m_list = new tsl::elm::List();
    const std::string folder = "#" + m_name;   // the playlist's "folder", as play_ctx knows it
    const bool thisPlays = play_ctx::source() == play_ctx::Source::Folder && play_ctx::folderPath() == folder;
    const char *cp = play_ctx::currentPath();
    // the tracks, and the titles of a window of them (Gatefold's whole library can be a playlist: an item each would
    // run the overlay's memory out, as the queue page's; Songate Shows Them All)
    constexpr u32 kWindow = 200;
    u32 from = 0;
    std::vector<std::pair<std::string, std::string>> tags;
    gfPlaylistTracks(m_name, thisPlays ? cp : nullptr, kWindow, m_songs, from, tags);
    m_list->addItem(new tsl::elm::CategoryHeader(m_name + " " + ult::DIVIDER_SYMBOL + " " + kindLabel(m_kind)));
    const u32 count = u32(m_songs.size());
    if (!count) {
        m_list->addItem(new tsl::elm::CategoryHeader("Empty..."));
        frame->setContent(m_list);
        return frame;
    }
    if (count > kWindow) {
        const std::string note = "Tracks " + std::to_string(from + 1) + "-" + std::to_string(std::min(count, from + kWindow)) + " of " +
                                 std::to_string(count) + " " + ult::DIVIDER_SYMBOL + " Songate Shows Them All";
        m_list->addItem(new tsl::elm::CustomDrawer([note](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            const auto tw = r->getTextDimensions(note, false, 15).first;
            r->drawString(note, false, x + (w - s32(tw)) / 2, y + 20, 15, tsl::offTextColor);
        }), 32);
    }
    m_list->addItem(new tsl::elm::CategoryHeader(
        "Tracks " + ult::DIVIDER_SYMBOL + "  Add To Playlist " + ult::DIVIDER_SYMBOL + "  Add All " + ult::DIVIDER_SYMBOL +
        "  Set As Startup"));
    std::string focus;
    const bool automix = m_kind == GFBG_KIND_AUTOMIX;
    for (u32 k = 0; k < tags.size() && from + k < count; ++k) {
        const u32 idx = from + k;
        const std::string &full = m_songs[idx];
        std::string title = tags[k].first, artist = tags[k].second;
        if (title.empty()) {   // (not in Songate's library: its file name)
            const size_t s = full.rfind('/'), d = full.rfind('.');
            title = full.substr(s == std::string::npos ? 0 : s + 1, d != std::string::npos && d > s ? d - s - 1 : std::string::npos);
        }
        const std::string label = buildFileLabel(idx + 1, title, artist);
        auto *item = new BrowserFileItem(label, full, folder);
        item->setClickListener([this, item, idx, folder, automix](u64 down) -> bool {
            const std::string &path = item->getFullPath();
            // ---- A: play from here (the playlist as the queue, named after it; an Automix one in its order, joined)
            if (down & HidNpadButton_A) {
                if (play_ctx::source() == play_ctx::Source::Folder && play_ctx::folderPath() == folder &&
                    play_ctx::currentPath()[0] != '\0' && strcasecmp(path.c_str(), play_ctx::currentPath()) == 0) {
                    if (play_ctx::isPlaying()) tunePause();
                    else                       tunePlay();
                    play_ctx::poll();
                    return true;
                }
                TuneShuffleMode shuffleMode = TuneShuffleMode_Off;
                tuneGetShuffleMode(&shuffleMode);
                const bool same = play_ctx::source() == play_ctx::Source::Folder && play_ctx::folderPath() == folder;
                if (same && shuffleMode != TuneShuffleMode_Off && !automix) {
                    play_ctx::startByPath(path);
                } else if (!play_ctx::switchToFolder(folder, m_songs, idx, m_name, automix)) {
                    if (tsl::notification) tsl::notification->showNow("Failed to switch to folder.");
                }
                play_ctx::poll();
                notifyCountChanged();
                return true;
            }
            // ---- Y: add to the Playlist (as a folder's track)
            if (down & HidNpadButton_Y) {
                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    if (R_SUCCEEDED(tuneEnqueue(path.c_str(), TuneEnqueueType_Back))) {
                        play_ctx::savedAppend(path);
                        if (tsl::notification) tsl::notification->showNow("Added 1 track to Playlist.");
                        notifyCountChanged();
                    } else if (tsl::notification) {
                        tsl::notification->showNow("Failed to add track.");
                    }
                } else {
                    play_ctx::savedAppend(path);
                    if (tsl::notification) tsl::notification->showNow("Added 1 track to Playlist.");
                    notifyCountChanged();
                }
                return true;
            }
            // ---- X: add all its tracks (as a folder's Add All: at most 300)
            if (down & HidNpadButton_X) {
                constexpr u32 kAddAllMax = 300;
                const bool inPlaylist = play_ctx::source() == play_ctx::Source::Playlist;
                long added = 0;
                for (u32 i = 0; i < m_songs.size() && i < kAddAllMax; ++i) {
                    if (inPlaylist && R_FAILED(tuneEnqueue(m_songs[i].c_str(), TuneEnqueueType_Back))) continue;
                    play_ctx::savedAppend(m_songs[i]);
                    added++;
                }
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Added %ld tracks to Playlist.", added);
                if (tsl::notification) tsl::notification->showNow(msg);
                if (added > 0) notifyCountChanged();
                return true;
            }
            // ---- Minus: set as startup file
            if (down & KEY_MINUS) {
                config::set_load_path(path.c_str());
                if (tsl::notification) tsl::notification->showNow(item->getText(), 26, "Startup File Set", 2500, false);
                return true;
            }
            return false;
        });
        if (thisPlays && cp[0] != '\0' && strcasecmp(full.c_str(), cp) == 0) focus = label;
        m_list->addItem(item);
    }
    if (!focus.empty()) m_list->jumpToItem(focus);
    frame->setContent(m_list);
    return frame;
}

void PlaylistTracksGui::update() {
    static u8 tick = 0;
    if ((++tick % 15) == 0) play_ctx::poll();
}

bool PlaylistTracksGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState l, HidAnalogStickState r) {
    if (leftToPlayer(keysDown, keysHeld, std::string("#") + m_group + m_kind + m_name, "", 4)) return true;
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos, l, r);
}

// ---------------------------------------------------------------------------

void pushBrowseStack(const std::string &target, const std::string &root) {
    tsl::changeTo<BrowseGui>(nullptr);
    if (target == "@browse") return;
    if (target.empty() || (target[0] != '@' && target[0] != '#')) {   // a folder of Folders (or none yet: the music folder)
        tsl::changeTo<BrowserGui>(target, "", root, nullptr, 2);
        return;
    }
    tsl::changeTo<PlaylistKindsGui>(nullptr);
    if (target == "@playlists") return;
    if (target.rfind("@kind:", 0) == 0 && target.size() > 6) {
        tsl::changeTo<PlaylistListGui>(target[6], nullptr);
        return;
    }
    if (target[0] == '#' && target.size() > 3) {
        tsl::changeTo<PlaylistListGui>(target[1], nullptr);
        tsl::changeTo<PlaylistTracksGui>(target[1], target.substr(3), target[2], nullptr);
    }
}
