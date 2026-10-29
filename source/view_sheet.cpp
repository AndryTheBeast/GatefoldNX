/*
 * Gatefold NX · view_sheet.cpp
 * The track menu: + on any track (or ⋯ on Now playing) opens a sheet over the
 * view with what can be done to that track — queue it, like it, edit its tags,
 * give it a cover from the SD card, jump to its album, artist, genre or folder,
 * see its details, or delete the file.
 *
 * Edits and covers are kept by Gatefold (sdmc:/config/gatefold-nx/edits.tsv,
 * covers.tsv, covers/) and layered over the file's own tags everywhere in the
 * app: the console's FFmpeg can read but not write files, and the music stays
 * untouched — Gatefold 1.4's default for covers ("only in Gatefold") as well.
 */
#include <SDL2/SDL_image.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <strings.h>

#include "app.hpp"
#include "icons.inc"

namespace {

const std::pair<const char*, const char*> kFields[] = { { "title", "Title" },       { "artist", "Artist" }, { "album", "Album" },
                                                        { "albumArtist", "Album artist" }, { "genre", "Genre" }, { "date", "Year" },
                                                        { "trackNo", "Track number" } };

std::string fieldOf(const Track& t, const std::string& k) {
    if (k == "title") return t.title;
    if (k == "artist") return t.artist;
    if (k == "album") return t.album;
    if (k == "albumArtist") return t.albumArtist;
    if (k == "genre") return t.genre;
    if (k == "date") return t.date;
    if (k == "trackNo") return t.trackNo ? std::to_string(t.trackNo) : std::string();
    return {};
}

std::string joinPath(const std::string& dir, const std::string& name) { return dir.back() == '/' ? dir + name : dir + "/" + name; }

bool isImage(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string e = name.substr(dot + 1);
    return !strcasecmp(e.c_str(), "jpg") || !strcasecmp(e.c_str(), "jpeg") || !strcasecmp(e.c_str(), "png") || !strcasecmp(e.c_str(), "webp") ||
           !strcasecmp(e.c_str(), "bmp");
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

std::string sizeText(double bytes) {
    char b[32];
    if (bytes >= 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", bytes / (1024 * 1024));
    else snprintf(b, sizeof b, "%.0f KB", bytes / 1024);
    return b;
}

}  // namespace

// ------------------------------------------------------------------ edits

void App::applyEdits(Track& t) {
    auto it = store.edits.find(t.path);
    if (it == store.edits.end()) return;
    for (const auto& [k, v] : it->second) {
        if (k == "title") t.title = v;
        else if (k == "artist") t.artist = v;
        else if (k == "album") t.album = v;
        else if (k == "albumArtist") t.albumArtist = v;
        else if (k == "genre") t.genre = v;
        else if (k == "date") t.date = v;
        else if (k == "trackNo") t.trackNo = atoi(v.c_str());
    }
}

bool App::keyboard(const char* header, const std::string& initial, std::string& out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        toast("The keyboard is not available in applet mode");
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, header);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    char buf[512] = {};
    const Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;   // cancelled: keep what was there
    out = buf;
    return true;
}

void App::editTag(const char* field, const char* label) {
    std::string v;
    if (keyboard(label, sheet.draft[field], v)) sheet.draft[field] = v;
}

void App::saveTagEdits() {
    Track& t = lib.tracks[sheet.track];
    auto& e = store.edits[t.path];
    for (const auto& [k, v] : sheet.draft)
        if (v != fieldOf(t, k)) e[k] = v;
    if (e.empty()) store.edits.erase(t.path);
    store.saveEdits();
    applyEdits(t);
    rebuildIndex();
    if (now.track == sheet.track) {
        now.info.title = t.title; now.info.artist = t.artist; now.info.album = t.album;
        now.info.albumArtist = t.albumArtist; now.info.genre = t.genre; now.info.date = t.date; now.info.trackNo = t.trackNo;
    }
    toast("Tags saved");
}

void App::undoTagEdits(int track) {
    store.edits.erase(lib.tracks[track].path);
    store.saveEdits();
    meta.retag(track);   // the file's own tags come back as it is read again
    toast("Back to the tags in the file");
}

// ------------------------------------------------------------------ covers

void App::listCoverDir(const std::string& dir) {
    sheet.dir = dir;
    sheet.entries.clear();
    sheet.memo.clear();
    for (const auto& d : listDirs(dir)) sheet.entries.push_back(d + "/");
    std::vector<std::string> images;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && isImage(e->d_name)) images.push_back(e->d_name);
        closedir(d);
    }
    std::sort(images.begin(), images.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    sheet.entries.insert(sheet.entries.end(), images.begin(), images.end());
    area("sheet", int(Sheet::Cover)).target = area("sheet", int(Sheet::Cover)).y = 0;
    ui.focusDefault();
}

void App::setTrackCover(const std::string& image) {
    const std::vector<uint8_t> bytes = readAll(image);
    if (bytes.empty()) { toast("Couldn't read that image"); return; }
    // Copied into Gatefold's folder, so the cover stays when the image is moved; named by its content.
    uint32_t h = 2166136261u;
    for (uint8_t b : bytes) h = (h ^ b) * 16777619u;
    char name[32];
    snprintf(name, sizeof name, "%08x", h);
    const std::string dir = store.path("covers");
    mkdir(dir.c_str(), 0777);
    const std::string dest = dir + "/" + name + image.substr(image.find_last_of('.'));
    if (FILE* f = fopen(dest.c_str(), "wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }

    const Track& t = lib.tracks[sheet.track];
    std::vector<int> targets{ sheet.track };
    if (sheet.wholeAlbum && t.albumIdx >= 0) targets = lib.albums[t.albumIdx].tracks;
    for (int i : targets) {
        store.covers[lib.tracks[i].path] = dest;
        meta.setCoverFile(i, dest);
    }
    store.saveEdits();
    if (std::find(targets.begin(), targets.end(), now.track) != targets.end()) loadTrackCoverAgain();   // now yours (store.covers)
    toast(targets.size() > 1 ? "Cover set for the whole album" : "Cover set");
}

// ------------------------------------------------------------------ delete

void App::deleteTrack(int track) {
    const Track t = lib.tracks[track];
    if (now.track == track) {
        if (q.order.size() > 1) next(false);   // move on first: the file is open while it plays
        else player.stop();
    }
    if (now.track == track) player.stop();
    if (remove(t.path.c_str()) != 0) {
        toast("Couldn't delete “" + baseName(t.path) + "”");
        return;
    }
    std::erase(store.liked, t.path);
    stats.forget(t.path);
    store.edits.erase(t.path);
    store.covers.erase(t.path);
    store.saveEdits();
    store.save();
    closeSheet();
    rescan();
    toast("Deleted “" + t.title + "”");
}

// ------------------------------------------------------------------ the sheet

void App::openTrackMenu(int track, int queuePos, bool upNext) {
    if (track < 0 || track >= int(lib.tracks.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Menu;
    sheet.track = track;
    sheet.queuePos = queuePos;
    sheet.upNext = upNext && queuePos >= 0;
    sheet.returnFocus = ui.focusedId();
    area("sheet", int(Sheet::Menu)).target = area("sheet", int(Sheet::Menu)).y = 0;
    ui.focusDefault();
}

void App::closeSheet() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet.preview = nullptr;
    sheet.page = Sheet::Closed;
    ui.setModal(false);
    if (sheet.returnFocus) ui.focus(sheet.returnFocus);
}

void App::sheetBack() {
    if (sheet.page == Sheet::Colour) {   // B puts down a square or bar first (Ui); then it is Cancel
        closeColourPicker(false);
        return;
    }
    if (sheet.page == Sheet::Job && sheet.fromList) {   // back to the list, on the job it came from
        int row = 0;
        for (int i = 0; i < sheet.recipe && i < JobSlots; i++) row += jobCards[i].shown > 0.01f;
        openJobList();
        sheet.queuePos = row;   // drawSmartSheet focuses it
        return;
    }
    if (sheet.page == Sheet::Menu || sheet.page == Sheet::NewList || sheet.page == Sheet::Sleep || sheet.page == Sheet::Job || sheet.page == Sheet::Jobs ||
        sheet.page == Sheet::PlFile || sheet.page == Sheet::Confirm || sheet.page == Sheet::EqPreset ||
        sheet.page == Sheet::ActList || sheet.page == Sheet::ListActions || sheet.page == Sheet::PlMenu || sheet.page == Sheet::FileMenu ||
        sheet.page == Sheet::Factory) {
        closeSheet();
        return;
    }
    if (sheet.page == Sheet::FileInfo || sheet.page == Sheet::FileLink) {   // Files: back to the file's menu
        sheet.page = Sheet::FileMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::PlIcon && !sheet.fromList) {   // the icons → the playlist's menu (opened from the page's icon button: closes)
        sheet.page = Sheet::PlMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::Smart || sheet.page == Sheet::SmartForm) {   // the recipe → the recipes → the choice
        sheet.page = sheet.page == Sheet::SmartForm ? Sheet::Smart : Sheet::NewList;
        ui.focusDefault();
        return;
    }
    sheet.page = Sheet::Menu;   // every page goes back to the menu ("Up one folder" climbs in the picker)
    ui.focusDefault();
}

void App::drawSheet() {
    if (sheet.page == Sheet::Closed) return;
    if (sheet.page >= Sheet::NewList) { drawSmartSheet(); return; }   // no track: view_smart.cpp
    if (sheet.track < 0 || sheet.track >= int(lib.tracks.size())) { closeSheet(); return; }
    ui.region(Region::Modal);
    const int t = sheet.track;
    const Track& tr = lib.tracks[t];
    const Sheet::Page page = sheet.page;

    // backdrop and card
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.55f);
    const float w = 600, x = (css::W - w) / 2;
    const bool footer = page == Sheet::Tags || page == Sheet::Delete;
    const float headH = 24 + 64 + 18, footH = footer ? 64 : 0, maxBody = css::H - 60 - headH - footH - 16;

    // ---- the rows of this page, laid out first so the card can be sized to them
    struct Item { const char* icon; std::string label, detail; std::function<void()> act; bool danger = false; float h = 46; bool text = false; };
    std::vector<Item> items;
    const bool liked = store.isLiked(tr.path);
    // Opened on a row of the queue (the Playlist view, Up next): still that row, unless the queue changed meanwhile.
    const int qp = sheet.queuePos >= 0 && sheet.queuePos < int(q.order.size()) && q.tracks[q.order[sheet.queuePos]] == t ? sheet.queuePos : -1;
    if (page == Sheet::Menu && qp >= 0) {
        if (qp != q.pos)
            items.push_back({ ICON_NEXT, "Play next", "", [this, qp] {
                                  queuePlayNext(qp);
                                  toast("Plays next");
                                  closeSheet();
                              } });
        // In Up next, a row wrapped round from the start (Repeat) cannot move: it lies before the track playing.
        if (q.order.size() > 1 && (!sheet.upNext || qp > q.pos))
            items.push_back({ ICON_SORT_DOWN, "Move", "", [this, qp, upNext = sheet.upNext] {
                                  closeSheet();
                                  grab = Grab{ Grab::Queue, qp, upNext, 1 };
                                  ui.focus(upNext ? uid("np.next", qp - q.pos - 1) : uid("pl.r", qp));
                                  ui.setPadMode(true);
                                  toast("Move it with the D-pad, then press A");
                              } });
        if (qp != q.pos)
            items.push_back({ ICON_X, "Remove from the playlist", "", [this, qp] {
                                  queueRemove(qp);
                                  toast("Removed from the playlist");
                                  closeSheet();
                              } });
    } else if (page == Sheet::Menu) {
        items.push_back({ ICON_NEXT, "Play next", "", [this, t] {
                              queueAdd(t, true);
                              toast("Plays next");
                              closeSheet();
                          } });
        items.push_back({ ICON_QUEUE, "Add to the end of the playlist", "", [this, t] {
                              queueAdd(t, false);
                              toast("Added to the playlist");
                              closeSheet();
                          } });
    }
    if (page == Sheet::Menu) {
        items.push_back({ ICON_LIST, "Add to a playlist…", "", [this] { sheet.page = Sheet::Lists; ui.focusDefault(); } });
        // On one of your own playlists, the track can leave it.
        if (route.view == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].own()) {
            auto& pl = store.playlists[route.ref];
            // Move: picked up as the queue's rows are (an Automix playlist then keeps this order until Order again)
            {
                const std::vector<int> rows = playlistTracks(route.ref);
                const int row = int(std::find(rows.begin(), rows.end(), t) - rows.begin());
                if (row < int(rows.size()) && rows.size() > 1)
                    items.push_back({ ICON_SORT_DOWN, "Move", pl.automix ? "Your order stays" : "", [this, row, ref = route.ref] {
                                          closeSheet();
                                          grab = Grab{ Grab::List, row, false, 1, ref };
                                          ui.focus(uid("pl.r", row));
                                          ui.setPadMode(true);
                                          toast("Move it with the D-pad, then press A");
                                      } });
            }
            if (pl.automix) {   // an Automix playlist: this track first (pinned), or Gatefold's choice again
                const bool pinned = pl.pin == tr.path;
                items.push_back({ ICON_LINK, pinned ? "Let Gatefold choose the first track" : "Play this first", "", [this, ref = route.ref, pinned, path = tr.path] {
                                      store.playlists[ref].pin = pinned ? "" : path;
                                      orderAutomixList(ref, false);
                                      toast(pinned ? "Gatefold chooses the first track again" : "Plays first; the rest ordered after it");
                                      closeSheet();
                                  } });
            }
            if (std::find(pl.tracks.begin(), pl.tracks.end(), tr.path) != pl.tracks.end())
                items.push_back({ ICON_X, "Remove from “" + pl.name + "”", "", [this, ref = route.ref, path = tr.path] {
                                      auto& tl = store.playlists[ref].tracks;
                                      if (auto it = std::find(tl.begin(), tl.end(), path); it != tl.end()) tl.erase(it);
                                      store.save();
                                      toast("Removed from “" + store.playlists[ref].name + "”");
                                      closeSheet();
                                  } });
        }
        items.push_back({ liked ? ICON_HEART_F : ICON_HEART, liked ? "Remove from Liked" : "Add to Liked", "", [this, liked, path = tr.path] {
                              store.toggleLiked(path);
                              store.save();
                              toast(liked ? "Removed from Liked" : "Added to Liked");
                          } });
        items.push_back({ ICON_TAG, "Edit tags…", store.edits.count(tr.path) ? "Edited" : "", [this, t] {
                              sheet.draft.clear();
                              for (const auto& [k, l] : kFields) sheet.draft[k] = fieldOf(lib.tracks[t], k);
                              sheet.page = Sheet::Tags;
                              ui.focusDefault();
                          } });
        items.push_back({ ICON_IMAGE, "Change the cover…", store.covers.count(tr.path) ? "Yours" : "", [this, t] {
                              sheet.page = Sheet::Cover;
                              sheet.wholeAlbum = lib.tracks[t].albumIdx >= 0;
                              // Settings → Storage and reset may name a folder of covers; else the track's own
                              struct stat st;
                              const bool own = !store.coverDir.empty() && stat(store.coverDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                              listCoverDir(own ? store.coverDir : lib.tracks[t].folder);
                          } });
        if (store.covers.count(tr.path))
            items.push_back({ ICON_REFRESH, "Use the original cover", "", [this, t] {
                                  // for every track of the album that shares this cover
                                  const std::string mine = store.covers[lib.tracks[t].path];
                                  std::vector<int> group{ t };
                                  if (lib.tracks[t].albumIdx >= 0) group = lib.albums[lib.tracks[t].albumIdx].tracks;
                                  for (int i : group)
                                      if (auto it = store.covers.find(lib.tracks[i].path); it != store.covers.end() && it->second == mine) {
                                          store.covers.erase(it);
                                          meta.setCoverFile(i, "");
                                      }
                                  store.saveEdits();
                                  if (now.track >= 0 && std::find(group.begin(), group.end(), now.track) != group.end()) loadTrackCoverAgain();
                                  toast("Original cover back");
                              } });
        if (store.edits.count(tr.path)) items.push_back({ ICON_REFRESH, "Undo my tag edits", "", [this, t] { undoTagEdits(t); } });
        if (tr.albumIdx >= 0)
            items.push_back({ ICON_GRID, "Go to the album", lib.albums[tr.albumIdx].title, [this, a = tr.albumIdx] { closeSheet(); goToAlbum(a); } });
        items.push_back({ ICON_MIC, "Go to the artist", tr.artist, [this, a = tr.artist] { closeSheet(); openArtist(a); } });
        if (!tr.genre.empty()) {
            const std::string g1 = tr.genre.substr(0, tr.genre.find(';'));
            items.push_back({ ICON_TAG, "Go to the genre", g1, [this, g1] { closeSheet(); openGenre(g1); } });
        }
        items.push_back({ ICON_FOLDER, "Show in Folders", "", [this, f = tr.folder] { closeSheet(); if (tabFor(Store::TabFolders, "folder")) navigate(View::Folders, lib.folderByPath(f)); } });
        items.push_back({ ICON_INFO, "Details", "", [this, t] {
                              sheet.info = TrackInfo{};
                              readTrackInfo(lib.tracks[t].path, sheet.info, true, true);
                              sheet.page = Sheet::Info;
                              ui.focusDefault();
                          } });
        items.push_back({ ICON_X, "Delete from the SD card…", "", [this] { sheet.page = Sheet::Delete; ui.focusDefault(); }, true });
    } else if (page == Sheet::Lists) {
        // Your own playlists (folder playlists follow their folder, so tracks cannot be added to them).
        items.push_back({ ICON_PLUS, "New playlist…", "", [this, t] { closeSheet(); newPlaylist(t); } });
        for (int i = 0; i < int(store.playlists.size()); i++) {
            const FolderPlaylist& p = store.playlists[i];
            if (!p.own()) continue;
            const bool in = std::find(p.tracks.begin(), p.tracks.end(), tr.path) != p.tracks.end();
            items.push_back({ ICON_LIST, p.name, in ? "Already in it" : std::to_string(p.tracks.size()) + " tracks", [this, i, t] {
                                  addToPlaylist(i, t);
                                  closeSheet();
                              } });
        }
    } else if (page == Sheet::Tags) {
        for (const auto& [k, l] : kFields)
            items.push_back({ nullptr, l, sheet.draft[k], [this, k = k, l = l] { editTag(k, l); } });
    } else if (page == Sheet::Cover) {
        if (sheet.dir != "sdmc:/")
            items.push_back({ ICON_BACK, "Up one folder", "", [this] {
                                  const size_t s = sheet.dir.find_last_of('/');
                                  listCoverDir(s <= 5 ? std::string("sdmc:/") : sheet.dir.substr(0, s));
                              } });
        for (const auto& e : sheet.entries) {
            if (e.back() == '/') {
                const std::string name = e.substr(0, e.size() - 1);
                items.push_back({ ICON_FOLDER, name, "", [this, name] { listCoverDir(joinPath(sheet.dir, name)); } });
            } else {
                const std::string full = joinPath(sheet.dir, e);
                auto m = sheet.memo.find(full);
                if (m == sheet.memo.end()) {
                    struct stat st {};
                    m = sheet.memo.emplace(full, stat(full.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "").first;
                }
                const std::string size = m->second;
                items.push_back({ ICON_IMAGE, e, size, [this, full] { setTrackCover(full); sheet.page = Sheet::Menu; ui.focusDefault(); } });
            }
        }
        if (sheet.entries.empty()) items.push_back({ nullptr, "No images or folders here", "", nullptr, false, 46, true });
    } else if (page == Sheet::Info) {
        const TrackInfo& in = sheet.info;
        auto info = [&](const char* label, const std::string& v) {
            if (!v.empty()) items.push_back({ nullptr, label, v, [] {}, false, 40, true });
        };
        info("Title", tr.title);
        info("Artist", tr.artist);
        info("Album", tr.album);
        info("Album artist", tr.albumArtist);
        info("Genre", tr.genre);
        info("Year", tr.date);
        info("Track", tr.trackNo ? std::to_string(tr.trackNo) : "");
        std::string fmt = in.codec;
        if (in.sampleRate) fmt += " · " + std::to_string(in.sampleRate / 1000) + "." + std::to_string(in.sampleRate % 1000 / 100) + " kHz";
        if (in.bitsPerSample && in.codec != "MP3") fmt += " · " + std::to_string(in.bitsPerSample) + "-bit";
        if (in.bitrateKbps) fmt += " · " + std::to_string(in.bitrateKbps) + " kbps";
        info("Format", fmt);
        info("Length", fmtTime(tr.duration > 0 ? tr.duration : in.duration));
        if (!sheet.memo.count("size")) {   // once, not every frame
            struct stat st {};
            sheet.memo["size"] = stat(tr.path.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "";
            sheet.memo["lrc"] = readSidecarLyrics(tr.path).empty() ? "" : "1";
        }
        info("Size", sheet.memo["size"]);
        info("Cover", store.covers.count(tr.path) ? "Yours, kept by Gatefold" : !in.cover.empty() ? "In the file" : "From the folder, or none");
        info("Lyrics", !sheet.memo["lrc"].empty() ? ".lrc file" : !in.lyrics.empty() ? "In the tags" : "None");
        if (const auto it = store.lyricNudge.find(tr.path); it != store.lyricNudge.end()) {   // the timing control (Now playing, the Canvas)
            char b[48];
            snprintf(b, sizeof b, "%.1f s %s, kept by Gatefold", double(std::fabs(it->second)), it->second > 0 ? "later" : "earlier");
            info("Lyrics timing", b);
        }
        info("Your edits", store.edits.count(tr.path) ? "Yes, kept by Gatefold; the file is unchanged" : "");
        // Volume levelling (levels.hpp)
        if (const Level* lv = levels.find(tr.path); lv && !std::isnan(lv->lufs)) {
            char b[64];
            snprintf(b, sizeof b, "%.1f LUFS, peak %.1f dBTP", double(lv->lufs), 20 * std::log10(std::max(1e-6, double(lv->peak))));
            std::string v = b;
            if (store.levelMode) {
                snprintf(b, sizeof b, " · %+.1f dB here", levelDb(t));
                v += b;
            }
            info("Loudness", v);
        } else {
            info("Loudness", lv ? "Could not be measured" : "Not measured yet");
        }
        // Automix's analysis (mixan.hpp), made with the loudness
        if (const Level* lv = levels.find(tr.path); lv && lv->mix.seconds != 0) {
            char b[64];
            if (lv->mix.bpm > 0 && lv->mix.confidence >= 0.5f) snprintf(b, sizeof b, "%.0f", double(lv->mix.bpm));
            else if (lv->mix.bpm > 0) snprintf(b, sizeof b, "About %.0f (no steady beat for Automix)", double(lv->mix.bpm));
            else snprintf(b, sizeof b, "No steady beat");
            info("BPM", b);
            if (lv->mix.key >= 0)   // the key, and its place on the Camelot wheel (Automix v2)
                info("Key", (lv->mix.keySure >= 0.5f ? "" : "Perhaps ") + mixan::keyName(lv->mix.key) + " (" + mixan::camelot(lv->mix.key) + ")");
        }
        // Play statistics (stats.hpp)
        auto when = [](int64_t t) {
            char buf[32];
            const std::time_t tt = std::time_t(t);
            std::strftime(buf, sizeof buf, "%d %b %Y", std::localtime(&tt));
            return std::string(buf);
        };
        const PlayStat* ps = stats.find(tr.path);
        info("Plays", ps && ps->plays ? std::to_string(ps->plays) : "Not played yet");
        if (ps && ps->last) info("Last played", when(ps->last));
        if (ps && ps->added) info("Added", when(ps->added));
        info("File", tr.path);
    }

    // ---- card
    float bodyH = 8;
    for (const auto& it : items) bodyH += it.h;
    if (page == Sheet::Cover && tr.albumIdx >= 0) bodyH += 22 + 44;   // the folder line and Use for (below)
    if (page == Sheet::Delete) bodyH = 110;
    if (page == Sheet::Tags) bodyH += 34;
    bodyH = std::min(bodyH + 8, maxBody);
    const float h = headH + bodyH + footH + 16, y = (css::H - h) / 2;
    g.glow(x + w / 2, y + h / 2 + 30, w * 0.75f, { 0, 0, 0 }, 0.5f);
    g.roundRect(x, y, w, h, 20, pal.surface2);
    g.roundRect(x, y, w, h, 20, pal.text, 0.03f);
    g.roundStroke(x, y, w, h, 20, 1, pal.text, 0.12f);

    // ---- header: cover (or the image being considered), kicker, title, artist · album, close
    const float hx = x + 24, hy = y + 24;
    if (page == Sheet::Cover && sheet.preview) g.image(sheet.preview, hx, hy, 64, 64, 10);
    else coverBox(t, hx, hy, 64, 10);
    g.roundStroke(hx, hy, 64, 64, 10, 1, pal.text, 0.09f);
    static const char* kickers[] = { "", "Track", "Edit tags", "Choose a cover", "Delete", "Details", "Add to a playlist" };
    kicker(kickers[int(page)], hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    g.text(tr.title, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, textW);
    g.text(trackSub(t), { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    if (iconButton(uid("sh.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeSheet(); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- body
    const float by = y + headH;
    ScrollArea& sa = area("sheet", int(page));
    sa.view = { x, by, w, bodyH };
    ui.pushArea(&sa);
    g.clip(x, by, w, bodyH);
    float ry = by + 8 - sa.y;
    if (page == Sheet::Cover) {
        // where the browser is, and which tracks get the cover
        g.textC(sheet.dir, { css::Xs }, x + 24, ry + 10, pal.text3, 1, Align::Left, int(w - 48));
        ry += 22;
        if (tr.albumIdx >= 0) {
            int pick = -1;
            segmented(uid("sh.scope"), x + w - 24, ry + 14, { "This track", "The whole album" }, sheet.wholeAlbum ? 1 : 0, &pick);
            g.textC("Use for", { css::Base, Weight::SemiBold }, x + 24, ry + 14, pal.text2);
            if (pick >= 0) sheet.wholeAlbum = pick == 1;
            ry += 44;   // the control is 36 tall round ry + 14: 30 put the first row's focus ring against it
        }
    }
    if (page == Sheet::Delete) {
        g.text("Delete this track from the SD card?", { css::Lg, Weight::Bold }, x + 24, ry + 8, pal.text, 1, Align::Left, int(w - 48));
        float ty = ry + 40;
        for (const auto& l : g.wrap("The file is removed for good, and it leaves the library, the playlist and Liked. " + tr.path, { css::Sm },
                                    int(w - 48), 3)) {
            g.text(l, { css::Sm }, x + 24, ty, pal.text3);
            ty += 19;
        }
    }
    // Focus starts on the first row, or in the picker on the first image (most folders hold just the one cover).
    size_t focusRow = 0;
    if (page == Sheet::Cover)
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].icon == ICON_IMAGE) { focusRow = i; break; }
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        const Id id = uid("sh.row", int64_t(i) + 1000 * int(page));
        const bool hit = it.act && ui.item(id, { x + 12, ry, w - 24, it.h }, 10, i == focusRow ? DefaultFocus : 0);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (hot && !it.text) g.roundRect(x + 12, ry, w - 24, it.h, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
        const Rgb fg = it.danger ? dangerInk() : pal.text;
        float lx = x + 24;
        if (it.icon) { g.icon(it.icon, lx + 9, ry + it.h / 2, 18, it.danger ? fg : pal.text2); lx += 18 + 14; }
        if (page == Sheet::Info) {
            // label column, value wrapped beside it
            g.textC(it.label, { css::Sm }, lx, ry + 20, pal.text3);
            g.textC(it.detail, { css::Base }, lx + 120, ry + 20, pal.text, 1, Align::Left, int(w - 48 - 120));
        } else if (page == Sheet::Tags) {
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, pal.text2);
            const std::string v = it.detail.empty() ? "—" : it.detail;
            g.textC(v, { css::Base }, x + w - 24, ry + it.h / 2, it.detail.empty() ? pal.text3 : pal.text, 1, Align::Right, int(w - 48 - 150));
        } else {
            const int dw = it.detail.empty() ? 0 : std::min(200, g.measure(it.detail, { css::Sm }));
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, it.text ? pal.text3 : fg, 1, Align::Left,
                    int(x + w - 24 - lx - dw - 16));
            if (dw) g.textC(it.detail, { css::Sm }, x + w - 24, ry + it.h / 2, pal.text3, 1, Align::Right, 200);
        }
        if (hit && it.act) it.act();
        if (page == Sheet::Cover && ui.focused(id) && it.icon == ICON_IMAGE) {
            // the header shows the image under focus, so you see it before choosing
            const std::string full = joinPath(sheet.dir, it.label);
            if (sheet.previewPath != full) {
                sheet.previewPath = full;
                if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                sheet.preview = nullptr;
                const std::vector<uint8_t> bytes = readAll(full);
                if (SDL_Surface* s = bytes.empty() ? nullptr : IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1)) {
                    if (SDL_Surface* small = Gfx::shrink(s, 128)) {
                        sheet.preview = SDL_CreateTextureFromSurface(renderer_, small);
                        if (sheet.preview) SDL_SetTextureScaleMode(sheet.preview, SDL_ScaleModeLinear);
                        SDL_FreeSurface(small);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }
        if (sheet.page != page) break;   // the action moved to another page (or closed): its rows are gone
        ry += it.h;
    }
    if (page == Sheet::Tags && sheet.page == page) {
        g.text("Kept by Gatefold and shown everywhere in the app. The file itself is not changed.", { css::Sm }, x + 24, ry + 10, pal.text3, 1,
               Align::Left, int(w - 48));
        ry += 34;
    }
    sa.content = ry + sa.y - by + 8;
    g.unclip();
    ui.popArea();
    if (sheet.page != page) return;

    // ---- footer buttons
    if (footer) {
        const float fy = y + h - 16 - 48;
        g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
        bool c = false;
        const Font bf{ css::Sm, Weight::SemiBold };
        if (page == Sheet::Tags) {
            const float sw = g.measure("Save", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.save"), ICON_CHECK, "Save", x + w - 24 - sw, fy + 7, true, &c);
            if (c) { saveTagEdits(); sheet.page = Sheet::Menu; ui.focusDefault(); return; }
            button(uid("sh.cancel"), nullptr, "Cancel", x + w - 24 - sw - 8 - cw, fy + 7, false, &c);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        } else {
            // Cancel is where focus lands: a stray A press must not delete anything
            const float dw = g.measure("Delete", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.del"), ICON_X, "Delete", x + w - 24 - dw, fy + 7, false, &c, false, true);
            if (c) { deleteTrack(t); return; }
            button(uid("sh.keep"), nullptr, "Cancel", x + w - 24 - dw - 8 - cw, fy + 7, false, &c, false, false, DefaultFocus);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        }
    }
}
