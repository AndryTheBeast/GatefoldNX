/*
 * Gatefold NX · thumbs.cpp
 * See thumbs.hpp. The constants repeat gfbg.h's (GFBG_ART, gfbgHash), which the PC preview
 * cannot include.
 */
#include "thumbs.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#ifdef __SWITCH__
#include <dirent.h>
#include <unistd.h>
#endif
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cores.hpp"
#include "library.hpp"
#include "metadata.hpp"

namespace {

constexpr int Art = 307;   // GFBG_ART: the overlay's cover size, so it draws 1:1

unsigned long long fnv(const void* data, size_t n) {   // gfbgHash
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= static_cast<const unsigned char*>(data)[i]; h *= 0x100000001b3ULL; }
    return h;
}

std::string hex(unsigned long long h) {
    char b[20];
    snprintf(b, sizeof b, "%016llx", h);
    return b;
}

bool exists(const std::string& p) {
    if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); return true; }
    return false;
}

std::vector<uint8_t> readFile(const std::string& p) {
    std::vector<uint8_t> out;
    if (FILE* f = fopen(p.c_str(), "rb")) {
        uint8_t buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

/** Halves an RGBA32 surface with a 2×2 box average: big steps of bilinear skip pixels and alias. */
SDL_Surface* halve(SDL_Surface* src) {
    const int w = std::max(1, src->w / 2), h = std::max(1, src->h / 2);
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;
    for (int y = 0; y < h; y++) {
        const uint8_t* r0 = static_cast<const uint8_t*>(src->pixels) + (y * 2) * src->pitch;
        const uint8_t* r1 = static_cast<const uint8_t*>(src->pixels) + std::min(src->h - 1, y * 2 + 1) * src->pitch;
        uint8_t* d = static_cast<uint8_t*>(dst->pixels) + y * dst->pitch;
        for (int x = 0; x < w; x++) {
            const int x0 = x * 2 * 4, x1 = std::min(src->w - 1, x * 2 + 1) * 4;
            for (int c = 0; c < 4; c++) d[x * 4 + c] = uint8_t((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
        }
    }
    return dst;
}

/** The centre square of the image, Art² RGB; false when it does not decode. */
bool makeArt(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out) {
    SDL_Surface* src = IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1);
    if (!src) return false;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(src);
    if (!rgba) return false;
    // The centre square first, then halved while it is still more than twice the size, then the last step.
    const int side = std::min(rgba->w, rgba->h);
    SDL_Surface* sq = SDL_CreateRGBSurfaceWithFormat(0, side, side, 32, SDL_PIXELFORMAT_RGBA32);
    if (sq) {
        const SDL_Rect r{ (rgba->w - side) / 2, (rgba->h - side) / 2, side, side };
        SDL_BlitSurface(rgba, &r, sq, nullptr);
    }
    SDL_FreeSurface(rgba);
    if (!sq) return false;
    while (sq->w >= Art * 2)
        if (SDL_Surface* h = halve(sq)) { SDL_FreeSurface(sq); sq = h; } else break;
    bool ok = false;
    if (SDL_Surface* t = SDL_CreateRGBSurfaceWithFormat(0, Art, Art, 32, SDL_PIXELFORMAT_RGBA32)) {
        if (SDL_SoftStretchLinear(sq, nullptr, t, nullptr) == 0) {
            out.resize(size_t(Art) * Art * 3);
            for (int y = 0; y < Art; y++) {
                const uint8_t* row = static_cast<const uint8_t*>(t->pixels) + y * t->pitch;
                for (int x = 0; x < Art; x++) memcpy(&out[(size_t(y) * Art + x) * 3], row + x * 4, 3);
            }
            ok = true;
        }
        SDL_FreeSurface(t);
    }
    SDL_FreeSurface(sq);
    return ok;
}

}  // namespace

void ThumbWorker::start(const std::string& dir) {
    if (started_) return;
    dir_ = dir;
#ifdef __SWITCH__
    // The copies live in thumbs/ (GFBG_THUMBS). 0.6 and 0.7.0 kept them in thumbs2/, which is moved over;
    // 0.5.1 kept RGB565 copies in thumbs/ without a map.tsv, which go.
    const std::string old = dir_.substr(0, dir_.rfind('/') + 1) + "thumbs2";
    auto hasMap = [](const std::string& d) {
        FILE* f = fopen((d + "/map.tsv").c_str(), "rb");
        if (f) fclose(f);
        return f != nullptr;
    };
    auto clear = [](const std::string& d) {
        if (DIR* dd = opendir(d.c_str())) {
            while (dirent* e = readdir(dd))
                if (e->d_name[0] != '.') remove((d + "/" + e->d_name).c_str());
            closedir(dd);
            rmdir(d.c_str());
        }
    };
    if (!hasMap(dir_)) clear(dir_);
    if (hasMap(old) && !hasMap(dir_)) rename(old.c_str(), dir_.c_str());
    else clear(old);
#endif
    mkdir(dir_.c_str(), 0777);
    loadMap();
    quit_ = false;
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void ThumbWorker::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
        jobs_.clear();
        queued_.clear();
        fresh_ = 0;
    }
    cv_.notify_all();
    thread_.join();
    started_ = false;
}

void ThumbWorker::want(Job j, bool first) {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> l(m_);
        const unsigned long long key = fnv(j.path.data(), j.path.size());
        const bool mapped = map_.count(key) != 0;
        if (!j.redo && mapped) return;   // done on an earlier run
        if (!mapped) {
            if (queued_.count(key)) { if (!j.redo) return; }   // already on its way
            else { queued_.insert(key); j.fresh = true; fresh_++; }
        }
        if (first) jobs_.push_front(std::move(j));
        else jobs_.push_back(std::move(j));
    }
    cv_.notify_one();
}

void ThumbWorker::run() {
    cores::moveHere(cores::Thumbs);
    using Clock = std::chrono::steady_clock;
    // Launch belongs to the library, the covers and the track playing: the SD card is shared, so this
    // starts 8 s in, and rests a moment between covers so anything the screen waits for goes first.
    const auto begin = Clock::now() + std::chrono::seconds(8);
    auto savedAt = Clock::now();
    int sinceSave = 0;
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(m_);
            // The map is written when the queue runs dry, and every 25 covers or 10 s: written only at the end,
            // a library's worth was lost whenever Gatefold closed first, and made again from the start next time.
            if (mapDirty_ && (jobs_.empty() || sinceSave >= 25 || Clock::now() - savedAt > std::chrono::seconds(10))) {
                mapDirty_ = false;
                sinceSave = 0;
                savedAt = Clock::now();
                l.unlock();
                saveMap();
                l.lock();
            }
            cv_.wait(l, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            if (Clock::now() < begin) {
                cv_.wait_until(l, begin, [this] { return quit_; });
                if (quit_) return;
            }
            j = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        // The same cover Now playing shows: yours, the file's own, or the folder's.
        std::vector<uint8_t> bytes;
        if (!j.mine.empty()) bytes = readFile(j.mine);
        if (bytes.empty()) {
            TrackInfo info;
            readTrackInfo(j.path, info, true, false);
            bytes = std::move(info.cover);
        }
        if (bytes.empty()) bytes = findFolderCover(j.folder);
        const unsigned long long key = fnv(j.path.data(), j.path.size());
        std::string file;
        if (!bytes.empty()) {
            // Named by the cover's own bytes: tracks sharing a cover share the file.
            file = hex(fnv(bytes.data(), bytes.size())) + ".rgb";
            std::vector<uint8_t> art;
            if (!exists(dir_ + "/" + file)) {
                if (makeArt(bytes, art)) {
                    const std::string tmp = dir_ + "/" + file + ".tmp";
                    if (FILE* f = fopen(tmp.c_str(), "wb")) {
                        const bool ok = fwrite(art.data(), 1, art.size(), f) == art.size();
                        fclose(f);
                        if (ok) rename(tmp.c_str(), (dir_ + "/" + file).c_str());
                    }
                }
                if (!exists(dir_ + "/" + file)) file.clear();
            }
        }
        sinceSave++;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        std::lock_guard<std::mutex> l(m_);
        busy_ = false;
        done_++;
        if (j.fresh) { fresh_--; freshDone_++; queued_.erase(key); }
        auto it = map_.find(key);
        if (file.empty()) {
            if (it != map_.end()) { map_.erase(it); mapDirty_ = true; }
        } else if (it == map_.end() || it->second != file) {
            map_[key] = file;
            mapDirty_ = true;
        }
    }
}

int ThumbWorker::waiting() {
    std::lock_guard<std::mutex> l(m_);
    return int(jobs_.size()) + (busy_ ? 1 : 0);
}

void ThumbWorker::progress(int& made, int& left) {
    std::lock_guard<std::mutex> l(m_);
    made = freshDone_;
    left = fresh_;
}

int ThumbWorker::done() {
    std::lock_guard<std::mutex> l(m_);
    return done_;
}

bool ThumbWorker::has(const std::string& track) {
    std::lock_guard<std::mutex> l(m_);
    return map_.count(fnv(track.data(), track.size())) != 0;
}

void ThumbWorker::loadMap() {
    if (FILE* f = fopen((dir_ + "/map.tsv").c_str(), "rb")) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            std::string file = tab + 1;
            while (!file.empty() && (file.back() == '\n' || file.back() == '\r')) file.pop_back();
            map_[strtoull(line, nullptr, 16)] = file;
        }
        fclose(f);
    }
}

void ThumbWorker::saveMap() {
    std::string out;
    {
        std::lock_guard<std::mutex> l(m_);
        out.reserve(map_.size() * 40);
        for (const auto& [k, f] : map_) out += hex(k) + "\t" + f + "\n";
    }
    // Next to the old one and renamed over it, so the overlay never reads half a map.
    const std::string tmp = dir_ + "/map.tmp", dst = dir_ + "/map.tsv";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        if (ok) {
            remove(dst.c_str());
            rename(tmp.c_str(), dst.c_str());
        }
    }
}
