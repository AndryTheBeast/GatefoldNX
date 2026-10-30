/*
 * Gatefold NX · metacache.cpp
 */
#include "metacache.hpp"
#include "coverio.hpp"

#include <SDL2/SDL_image.h>
#ifdef __SWITCH__
#include <turbojpeg.h>
#endif

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "cores.hpp"
#include "metadata.hpp"

namespace {

/** Centre-crops to a square and scales to size×size RGBA (object-fit: cover). */
SDL_Surface* squareThumb(SDL_Surface* src, int size) {
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    if (!rgba) return nullptr;
    const int side = std::min(rgba->w, rgba->h);
    SDL_Rect crop{ (rgba->w - side) / 2, (rgba->h - side) / 2, side, side };
    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (out && SDL_SoftStretchLinear(rgba, &crop, out, nullptr) != 0) { SDL_FreeSurface(out); out = nullptr; }
    SDL_FreeSurface(rgba);
    return out;
}

Palette paletteOf(SDL_Surface* thumb) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
    Palette p = paletteDefault();
    if (s && SDL_SoftStretchLinear(thumb, nullptr, s, nullptr) == 0) {
        std::vector<uint8_t> px(64 * 64 * 4);
        for (int y = 0; y < 64; y++) memcpy(&px[y * 256], static_cast<uint8_t*>(s->pixels) + y * s->pitch, 256);
        p = paletteFromPixels(px.data(), 64, 64);
    }
    if (s) SDL_FreeSurface(s);
    return p;
}

unsigned long long fnv64(const void* data, size_t n) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= static_cast<const unsigned char*>(data)[i]; h *= 0x100000001b3ULL; }
    return h;
}

/**
 * A JPEG of an RGBA32 surface, in memory, written to the card with plain stdio. On the console it is
 * TurboJPEG: the console's SDL_image 2.0 saves through libjpeg with an error handler that jumps to a
 * point it never set, so any complaint from libjpeg crashed the app (0.7.3 at launch). The preview's
 * SDL_image 2.8 is sound.
 */
std::vector<uint8_t> encodeJpeg(SDL_Surface* s, int quality) {
    std::vector<uint8_t> out;
#ifdef __SWITCH__
    tjhandle h = tjInitCompress();
    if (!h) return out;
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    if (tjCompress2(h, static_cast<const unsigned char*>(s->pixels), s->w, s->pitch, s->h, TJPF_RGBA, &buf, &size, TJSAMP_420, quality,
                    TJFLAG_FASTDCT) == 0 && buf)
        out.assign(buf, buf + size);
    if (buf) tjFree(buf);
    tjDestroy(h);
#else
    SDL_RWops* rw = SDL_AllocRW();
    if (!rw) return out;
    rw->type = SDL_RWOPS_UNKNOWN;   // SDL_AllocRW sets nothing else: every callback is filled in
    rw->hidden.unknown.data1 = &out;
    rw->size = [](SDL_RWops* c) -> Sint64 { return Sint64(static_cast<std::vector<uint8_t>*>(c->hidden.unknown.data1)->size()); };
    rw->read = [](SDL_RWops*, void*, size_t, size_t) -> size_t { return 0; };
    rw->write = [](SDL_RWops* c, const void* p, size_t size, size_t n) -> size_t {
        auto* v = static_cast<std::vector<uint8_t>*>(c->hidden.unknown.data1);
        v->insert(v->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size * n);
        return n;
    };
    rw->seek = [](SDL_RWops* c, Sint64, int) -> Sint64 { return Sint64(static_cast<std::vector<uint8_t>*>(c->hidden.unknown.data1)->size()); };
    rw->close = [](SDL_RWops* c) -> int { SDL_FreeRW(c); return 0; };
    if (IMG_SaveJPG_RW(s, rw, 1, quality) != 0) out.clear();
#endif
    return out;
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

std::string hex16(unsigned long long v) {
    char b[20];
    snprintf(b, sizeof b, "%016llx", v);
    return b;
}

std::string field(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

}  // namespace

void MetaCache::start(Library* lib, const std::string& cacheFile) {
    lib_ = lib;
    cacheFile_ = cacheFile;
    trackSlot_.assign(lib->tracks.size(), -1);
    tagQueued_.assign(lib->tracks.size(), 0);
    paths_.clear();
    folders_.clear();
    for (const auto& t : lib->tracks) { paths_.push_back(t.path); folders_.push_back(t.folder); }
    sweep_ = 0;
    tagged_ = 0;
    done_.assign(paths_.size(), 0);
    coverFile_.assign(paths_.size(), std::string());
    load();
    for (int i = 0; i < int(lib->tracks.size()); ++i)
        if (!lib->tracks[i].subsonicId.empty()) { done_[i] = 1; tagged_++; }
    artDir_ = cacheFile_.substr(0, cacheFile_.find_last_of('/')) + "/art";
    mkdir(artDir_.c_str(), 0777);
    loadArtMap();
    artSweep_ = 0;
    artSweepPos_ = 0;
    artBegin_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    quit_ = false;
    thread_ = std::thread(&MetaCache::run, this);
}

void MetaCache::stop() {
    if (thread_.joinable()) {
        { std::lock_guard<std::mutex> g(m_); quit_ = true; }
        cv_.notify_all();
        thread_.join();
        if (artNew_ > 0) saveArtMap();
    }
    for (auto& r : artDone_) if (r.thumb) SDL_FreeSurface(r.thumb);
    artDone_.clear();
    tagDone_.clear();
    for (auto& s : slots_) if (s.tex) SDL_DestroyTexture(s.tex);
    slots_.clear();
    slotByKey_.clear();
    slotKey_.clear();
    folderDecoded_.clear();
    urgent_.clear();
    if (dirty_) save();
}

void MetaCache::queue(Job j) {
    { std::lock_guard<std::mutex> g(m_); urgent_.push_front(j); }
    cv_.notify_one();
}

AlbumArt& MetaCache::art(int t) {
    static AlbumArt pending{ AlbumArt::Pending };
    if (t < 0 || t >= int(trackSlot_.size())) { static AlbumArt none{ AlbumArt::Missing }; return none; }
    int& s = trackSlot_[t];
    if (s >= 0 && slots_[s].state == AlbumArt::None) s = -1;   // evicted: load again
    if (s == -1) { s = -2; queue({ true, t }); }
    if (s < 0) return pending;
    slots_[s].lastUsed = frame_;
    return slots_[s];
}

void MetaCache::wantTags(int t) {
    if (t < 0 || t >= int(tagQueued_.size()) || tagQueued_[t]) return;
    tagQueued_[t] = 1;
    queue({ false, t });
}

void MetaCache::setCoverFile(int t, const std::string& file) {
    if (t < 0 || t >= int(trackSlot_.size())) return;
    { std::lock_guard<std::mutex> g(m_); coverFile_[t] = file; }
    trackSlot_[t] = -1;   // drawn again from the new source
}

void MetaCache::retag(int t) {
    if (t < 0 || t >= int(tagQueued_.size())) return;
    { std::lock_guard<std::mutex> g(m_); done_[t] = 0; }
    tagQueued_[t] = 0;
    wantTags(t);
}

int MetaCache::slotFor(const std::string& key) {
    auto it = slotByKey_.find(key);
    if (it != slotByKey_.end()) return it->second;
    slots_.push_back({});
    slotKey_.push_back(key);
    return slotByKey_[key] = int(slots_.size()) - 1;
}

bool MetaCache::pump(SDL_Renderer* r) {
    frame_++;
    std::vector<TagResult> tags;
    std::vector<ArtResult> art;
    {
        std::lock_guard<std::mutex> g(m_);
        tags.swap(tagDone_);
        // A few uploads per frame keep scrolling smooth.
        const size_t n = std::min<size_t>(artDone_.size(), 6);
        art.assign(artDone_.begin(), artDone_.begin() + n);
        artDone_.erase(artDone_.begin(), artDone_.begin() + n);
    }
    for (auto& t : tags) {
        Track& tr = lib_->tracks[t.idx];
        if (!t.ok) { tagged_++; continue; }
        if (!t.title.empty()) tr.title = t.title;
        if (!t.artist.empty()) tr.artist = t.artist;
        if (!t.album.empty()) tr.album = t.album;
        tr.date = t.date;
        tr.albumArtist = t.albumArtist;
        tr.genre = t.genre;
        tr.trackNo = t.trackNo;
        if (t.duration > 0) tr.duration = t.duration;
        if (overlay) overlay(tr);
        if (!tr.tagged) tagged_++;
        tr.tagged = true;
        dirty_ = true;
    }
    for (auto& a : art) {
        const int s = slotFor(a.key);
        AlbumArt& dst = slots_[s];
        if (a.thumb) {
            if (dst.tex) SDL_DestroyTexture(dst.tex);
            dst.tex = SDL_CreateTextureFromSurface(r, a.thumb);
            if (dst.tex) SDL_SetTextureScaleMode(dst.tex, SDL_ScaleModeLinear);
            SDL_FreeSurface(a.thumb);
            dst.pal = a.pal;
            dst.state = dst.tex ? AlbumArt::Ready : AlbumArt::Missing;
        } else if (a.missing) {
            dst.state = AlbumArt::Missing;
        } else if (a.shared && dst.state == AlbumArt::None) {
            // The shared folder image was evicted before this track got to it.
            { std::lock_guard<std::mutex> g(m_); folderDecoded_.erase(a.key); }
            trackSlot_[a.idx] = -1;
            continue;
        }
        dst.lastUsed = frame_;
        trackSlot_[a.idx] = s;
    }

    // Keep texture memory bounded: drop the covers drawn least recently.
    int live = 0;
    for (const auto& s : slots_) live += s.tex != nullptr;
    if (live > MaxTextures) {
        std::vector<int> order;
        for (int i = 0; i < int(slots_.size()); i++) if (slots_[i].tex && frame_ - slots_[i].lastUsed > 2) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return slots_[a].lastUsed < slots_[b].lastUsed; });
        std::lock_guard<std::mutex> g(m_);
        for (int i = 0; i < int(order.size()) && live > MaxTextures * 3 / 4; i++, live--) {
            AlbumArt& s = slots_[order[i]];
            SDL_DestroyTexture(s.tex);
            s.tex = nullptr;
            s.state = AlbumArt::None;
            folderDecoded_.erase(slotKey_[order[i]]);
        }
    }
    return !tags.empty();
}

void MetaCache::run() {
    cores::moveHere(cores::Meta);
    for (;;) {
        Job job{ false, -1 };
        {
            std::unique_lock<std::mutex> lk(m_);
            const int n = int(paths_.size());
            const bool artDue = artSweep_ < n && !artPaused_ && std::chrono::steady_clock::now() >= artBegin_;
            const bool sweeping = sweep_ < n || artDue;
            cv_.wait_for(lk, std::chrono::milliseconds(urgent_.empty() && sweeping ? 2 : 500), [&] { return quit_ || !urgent_.empty(); });
            if (quit_) return;
            if (!urgent_.empty()) { job = urgent_.front(); urgent_.pop_front(); }
            else if (sweep_ < n) job = { false, sweep_++ };
            else if (artDue) { job = { true, artSweep_++, true }; artSweepPos_ = artSweep_; }
            else continue;
        }

        bool known;
        { std::lock_guard<std::mutex> g(m_); known = done_[job.idx]; }
        if (!job.art && known) continue;

        std::string mine;
        { std::lock_guard<std::mutex> g(m_); mine = coverFile_[job.idx]; }
        if (job.cacheOnly) {
            cacheArtOnly(job.idx, mine);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));   // the SD card goes to what is on screen first
            continue;
        }
        // A cover seen on an earlier run comes from the disk cache: a small JPEG instead of opening the
        // track (FFmpeg, for M4A) and decoding its full-size picture, which made covers arrive one by one.
        if (job.art && known) {
            ArtResult res{ job.idx, {}, nullptr, paletteDefault(), false, false };
            if (cachedArt(job.idx, mine, res)) {
                std::lock_guard<std::mutex> g(m_);
                artDone_.push_back(res);
                continue;
            }
        }
        TrackInfo info;
        const bool ok = readTrackInfo(paths_[job.idx], info, job.art && mine.empty(), false);
        if (!known) {
            std::lock_guard<std::mutex> g(m_);
            done_[job.idx] = 1;
            tagDone_.push_back({ job.idx, info.title, info.artist, info.album, info.date, info.albumArtist, info.genre, info.trackNo, info.duration, ok });
        }
        if (!job.art) continue;

        ArtResult res{ job.idx, {}, nullptr, paletteDefault(), false, false };
        std::vector<uint8_t> bytes = std::move(info.cover);
        if (!mine.empty()) {
            res.key = "o:" + mine;   // your cover, shared by every track it was chosen for
            if (FILE* f = fopen(mine.c_str(), "rb")) {
                char buf[16384];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
                fclose(f);
            }
        } else if (!bytes.empty()) {
            res.key = "t:" + paths_[job.idx];
        } else {
            res.key = "f:" + folders_[job.idx];
            bool decoded;
            {
                std::lock_guard<std::mutex> g(m_);
                decoded = !folderDecoded_.insert(res.key).second;
            }
            if (decoded) {
                res.shared = true;
                storeArt(job.idx, {}, res);
                std::lock_guard<std::mutex> g(m_);
                artDone_.push_back(res);
                continue;
            }
            bytes = findFolderCover(folders_[job.idx]);
        }
        if (!bytes.empty())
            if (SDL_Surface* s = loadImage(bytes, paths_[job.idx].c_str())) {
                res.thumb = squareThumb(s, ThumbSize);
                SDL_FreeSurface(s);
                if (res.thumb) res.pal = paletteOf(res.thumb);
            }
        res.missing = !res.thumb;
        storeArt(job.idx, bytes, res);
        std::lock_guard<std::mutex> g(m_);
        artDone_.push_back(res);
    }
}

/**
 * The cover sweep: a track's cover into the cache without showing it. An album's tracks share one
 * picture, so after the first of them it is only an entry in the map: no decoding.
 */
void MetaCache::cacheArtOnly(int idx, const std::string& mine) {
    const unsigned long long ph = fnv64(paths_[idx].data(), paths_[idx].size());
    if (artMap_.count(ph)) return;
    ArtResult res{ idx, {}, nullptr, paletteDefault(), false, false };
    auto known = [&](const std::string& file) {
        artMap_[ph] = { file, res.key };
        artCount_ = int(artMap_.size());
        if (++artNew_ >= 20) saveArtMap();
    };
    std::vector<uint8_t> bytes;
    if (!mine.empty()) {
        res.key = "o:" + mine;
        if (auto k = artByKey_.find(res.key); k != artByKey_.end()) { known(k->second); return; }
        bytes = readAll(mine);
    } else {
        TrackInfo info;
        readTrackInfo(paths_[idx], info, true, false);
        bytes = std::move(info.cover);
        if (!bytes.empty()) {
            res.key = "t:" + paths_[idx];
        } else {
            res.key = "f:" + folders_[idx];
            if (auto k = artByKey_.find(res.key); k != artByKey_.end()) { known(k->second); return; }
            bytes = findFolderCover(folders_[idx]);
        }
    }
    if (!bytes.empty()) {
        const std::string file = hex16(fnv64(bytes.data(), bytes.size())) + ".jpg";
        struct stat st;
        if (stat((artDir_ + "/" + file).c_str(), &st) == 0) {   // the same picture as another track's
            artByKey_[res.key] = file;
            known(file);
            return;
        }
        if (SDL_Surface* s = loadImage(bytes)) {
            res.thumb = squareThumb(s, ThumbSize);
            SDL_FreeSurface(s);
        }
    }
    res.missing = !res.thumb;
    storeArt(idx, bytes, res);
    if (res.thumb) SDL_FreeSurface(res.thumb);
}

/** The cover from the disk cache, when this track's is there and still the one it uses. */
bool MetaCache::cachedArt(int idx, const std::string& mine, ArtResult& res) {
    auto it = artMap_.find(fnv64(paths_[idx].data(), paths_[idx].size()));
    if (it == artMap_.end()) return false;
    const ArtEntry& e = it->second;
    // Your cover chosen or forgotten since: the entry is for another picture.
    if (mine.empty() ? e.key.rfind("o:", 0) == 0 : e.key != "o:" + mine) return false;
    res.key = e.key;
    if (e.file == "-") { res.missing = true; return true; }
    if (e.key.rfind("f:", 0) == 0) {   // a folder image another track already shows
        std::lock_guard<std::mutex> g(m_);
        if (!folderDecoded_.insert(e.key).second) { res.shared = true; return true; }
    }
    const std::vector<uint8_t> jpg = readAll(artDir_ + "/" + e.file);
    SDL_Surface* s = loadImage(jpg);
    if (s) {
        res.thumb = squareThumb(s, ThumbSize);
        SDL_FreeSurface(s);
    }
    if (!res.thumb) {   // gone from the card: read it the long way, and forget the folder claim made above
        if (e.key.rfind("f:", 0) == 0) { std::lock_guard<std::mutex> g(m_); folderDecoded_.erase(e.key); }
        artMap_.erase(it);
        return false;
    }
    res.pal = paletteOf(res.thumb);
    return true;
}

/** Remembers a track's cover for next time: the thumbnail as a JPEG named by its source's bytes, so an album's tracks share one. */
void MetaCache::storeArt(int idx, const std::vector<uint8_t>& source, const ArtResult& res) {
    ArtEntry e{ "-", res.key };
    if (res.shared) {
        auto k = artByKey_.find(res.key);
        if (k == artByKey_.end()) return;   // its file is not known yet: next run
        e.file = k->second;
    } else if (res.thumb) {
        e.file = hex16(fnv64(source.data(), source.size())) + ".jpg";
        const std::string path = artDir_ + "/" + e.file;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            const std::vector<uint8_t> jpg = encodeJpeg(res.thumb, 90);
            FILE* f = jpg.empty() ? nullptr : fopen(path.c_str(), "wb");
            if (!f) return;
            const bool ok = fwrite(jpg.data(), 1, jpg.size(), f) == jpg.size();
            fclose(f);
            if (!ok) { remove(path.c_str()); return; }
        }
        artByKey_[res.key] = e.file;
    }
    artMap_[fnv64(paths_[idx].data(), paths_[idx].size())] = e;
    artCount_ = int(artMap_.size());
    if (++artNew_ >= 20) saveArtMap();
}

/**
 * The cover reader's version, the map's first line ("#covers <n>"): a track remembered without a cover ("-") by an older
 * reader is tried again, its good covers kept. 2: FLAC pictures read by our own reader and images decoded by stb_image
 * when SDL_image cannot (coverio.cpp; the user's FLACs had none on the console, 2026-10-30). Raise it when the reader
 * finds covers it could not before. (Older Songates skip the line: it has no tab.)
 */
constexpr int kCoverReader = 2;

void MetaCache::loadArtMap() {
    artMap_.clear();
    artByKey_.clear();
    artNew_ = 0;
    std::vector<uint8_t> raw = readAll(artDir_ + "/map.tsv");   // in one go, split in place (see load)
    const char* p = reinterpret_cast<const char*>(raw.data());
    const char* const end = p + raw.size();
    int reader = 1;   // (a map from before the line: the first reader)
    if (raw.size() > 8 && !memcmp(p, "#covers ", 8)) {
        reader = atoi(p + 8);
        const char* eol = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        p = eol ? eol + 1 : end;
    }
    const bool retryMissing = reader < kCoverReader;
    bool dropped = false;
    artMap_.reserve(paths_.size());
    while (p < end) {
        const char* eol = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        if (!eol) eol = end;
        const char* lineEnd = eol > p && eol[-1] == '\r' ? eol - 1 : eol;
        const char* a = static_cast<const char*>(memchr(p, '\t', size_t(lineEnd - p)));
        const char* b = a ? static_cast<const char*>(memchr(a + 1, '\t', size_t(lineEnd - a - 1))) : nullptr;
        if (b) {
            std::string file(a + 1, b), key(b + 1, lineEnd);
            if (file != "-") artByKey_[key] = file;
            if (file == "-" && retryMissing) dropped = true;   // found nothing then: asked again by this reader
            else artMap_[strtoull(std::string(p, a).c_str(), nullptr, 16)] = { std::move(file), std::move(key) };
        }
        p = eol + 1;
    }
    artCount_ = int(artMap_.size());
    if (dropped || (retryMissing && !raw.empty())) saveArtMap();   // (with the new line: they are asked once)
}

void MetaCache::saveArtMap() {
    artNew_ = 0;
    const std::string tmp = artDir_ + "/map.tmp", dst = artDir_ + "/map.tsv";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "#covers %d\n", kCoverReader);
    for (const auto& [h, e] : artMap_) fprintf(f, "%s\t%s\t%s\n", hex16(h).c_str(), e.file.c_str(), e.key.c_str());
    const bool ok = !ferror(f);
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return; }   // a full SD card: the old map stays whole
    remove(dst.c_str());
    rename(tmp.c_str(), dst.c_str());
}

void MetaCache::load() {
    // Read in one go and split in place: line by line, with a string for every column, this took a third of a
    // second of the console's launch for a few thousand tracks.
    std::string data;
    if (FILE* f = fopen(cacheFile_.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(std::max(0L, ftell(f))));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    if (data.empty()) return;
    std::unordered_map<std::string_view, int> byPath;
    byPath.reserve(paths_.size());
    for (int i = 0; i < int(paths_.size()); i++) byPath.emplace(paths_[i], i);
    const char* p = data.data();
    const char* const end = p + data.size();
    while (p < end) {
        const char* eol = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        if (!eol) eol = end;
        const char* lineEnd = eol > p && eol[-1] == '\r' ? eol - 1 : eol;
        // path, title, artist, album, date, duration, album artist, track number, genre
        std::string_view cols[9];
        int n = 0;
        for (const char* c = p; n < 9;) {
            const char* tab = static_cast<const char*>(memchr(c, '\t', size_t(lineEnd - c)));
            const char* stop = tab ? tab : lineEnd;
            cols[n++] = std::string_view(c, size_t(stop - c));
            if (!tab) break;
            c = tab + 1;
        }
        p = eol + 1;
        if (n < 9) continue;   // older caches lack these: read those files again
        auto it = byPath.find(cols[0]);
        if (it == byPath.end()) continue;
        Track& t = lib_->tracks[it->second];
        if (!t.subsonicId.empty()) continue;
        if (!cols[1].empty()) t.title = cols[1];
        if (!cols[2].empty()) t.artist = cols[2];
        if (!cols[3].empty()) t.album = cols[3];
        t.date = cols[4];
        t.duration = strtod(std::string(cols[5]).c_str(), nullptr);
        t.albumArtist = cols[6];
        t.trackNo = atoi(std::string(cols[7]).c_str());
        t.genre = cols[8];
        if (overlay) overlay(t);
        if (!t.tagged) tagged_++;
        t.tagged = true;
        done_[it->second] = 1;
    }
}

void MetaCache::save() {
    // Next to the old one and renamed over it: a cut write (power, a full SD card) left half a cache
    const std::string tmp = cacheFile_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    for (const auto& t : lib_->tracks)
        if (t.tagged)
            fprintf(f, "%s\t%s\t%s\t%s\t%s\t%.3f\t%s\t%d\t%s\n", field(t.path).c_str(), field(t.title).c_str(), field(t.artist).c_str(),
                    field(t.album).c_str(), field(t.date).c_str(), t.duration, field(t.albumArtist).c_str(), t.trackNo, field(t.genre).c_str());
    const bool ok = !ferror(f);
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return; }
    remove(cacheFile_.c_str());
    rename(tmp.c_str(), cacheFile_.c_str());
    dirty_ = false;
}

void MetaCache::clearCovers() {
    artMap_.clear();
    artByKey_.clear();
    artCount_ = 0;
    remove((artDir_ + "/map.tsv").c_str());
}

void MetaCache::clearDiskCache() {
    remove(cacheFile_.c_str());
    dirty_ = false;
    // The covers too: "Read all tags again" is also how a changed picture is picked up.
    artMap_.clear();
    artByKey_.clear();
    artCount_ = 0;
    remove((artDir_ + "/map.tsv").c_str());
}
