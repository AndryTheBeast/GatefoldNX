/*
 * Gatefold NX · loader.cpp
 */
#include "loader.hpp"

#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>

#include "cores.hpp"
#include "gfx.hpp"
#include "library.hpp"

void TrackLoader::Result::release() {
    if (cover) SDL_FreeSurface(cover);
    if (blur) SDL_FreeSurface(blur);
    cover = blur = nullptr;
}

void TrackLoader::start() {
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void TrackLoader::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (hasDone_) done_.release();
    hasDone_ = hasReq_ = false;
}

void TrackLoader::request(Request r) {
    {
        std::lock_guard<std::mutex> g(m_);
        req_ = std::move(r);
        hasReq_ = true;
    }
    cv_.notify_one();
}

bool TrackLoader::take(Result& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!hasDone_) return false;
    out = std::move(done_);
    done_ = Result{};
    hasDone_ = false;
    return true;
}

void TrackLoader::run() {
    cores::moveHere(cores::Loader);
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> g(m_);
            cv_.wait(g, [&] { return quit_ || hasReq_; });
            if (quit_) return;
            r = std::move(req_);
            hasReq_ = false;
        }
        Result res;
        work(r, res);
        std::lock_guard<std::mutex> g(m_);
        if (hasDone_) done_.release();   // not taken yet, and already old
        done_ = std::move(res);
        hasDone_ = true;
    }
}

void TrackLoader::work(const Request& r, Result& out) {
    out.gen = r.gen;
    out.readInfo = r.readInfo;
    std::vector<uint8_t> bytes;
    if (!r.myCover.empty())
        if (FILE* f = fopen(r.myCover.c_str(), "rb")) {
            char buf[16384];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
            fclose(f);
        }
    if (r.readInfo) {
        readTrackInfo(r.path, out.info, bytes.empty(), true);
        if (bytes.empty()) bytes = std::move(out.info.cover);
        out.info.cover.clear();
        out.info.cover.shrink_to_fit();
        out.lyrics = readSidecarLyrics(r.path);
        out.sidecar = !out.lyrics.empty();
        if (out.lyrics.empty()) out.lyrics = std::move(out.info.lyrics);
        out.info.lyrics.clear();
    } else if (bytes.empty()) {
        TrackInfo info;
        readTrackInfo(r.path, info, true, false);
        bytes = std::move(info.cover);
    }
    if (bytes.empty()) bytes = findFolderCover(r.folder);
    if (bytes.empty()) return;

    SDL_Surface* rgba = nullptr;
    if (SDL_Surface* src = IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1)) {
        rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(src);
    }
    if (!rgba) return;
    // The cover is drawn at most ~450 px (the Canvas); 512 keeps it sharp without aliasing while it floats and zooms.
    out.cover = Gfx::shrink(rgba, 512);
    // 64×64 for the palette (as Gatefold samples it), 12×12 stretched with linear filtering for the backdrop blur.
    SDL_Surface* thumb = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Surface* tiny = SDL_CreateRGBSurfaceWithFormat(0, 12, 12, 32, SDL_PIXELFORMAT_RGBA32);
    if (thumb && tiny && SDL_SoftStretchLinear(rgba, nullptr, thumb, nullptr) == 0) {
        out.px.resize(64 * 64 * 4);
        for (int y = 0; y < 64; y++) memcpy(&out.px[y * 256], static_cast<uint8_t*>(thumb->pixels) + y * thumb->pitch, 256);
        out.pal = paletteFromPixels(out.px.data(), 64, 64, r.opts);
        if (SDL_SoftStretchLinear(thumb, nullptr, tiny, nullptr) == 0) { out.blur = tiny; tiny = nullptr; }
    }
    if (thumb) SDL_FreeSurface(thumb);
    if (tiny) SDL_FreeSurface(tiny);
    SDL_FreeSurface(rgba);
}
