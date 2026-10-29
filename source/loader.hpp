/*
 * Gatefold NX · loader.hpp
 * What a track change needs from the SD card and the CPU, done on a worker core: the tags and
 * format, the lyrics, the cover (your own, the embedded one or the folder's), decoded and shrunk,
 * its palette and its backdrop blur. The main thread only uploads the textures, so a track change
 * no longer stops the frame it happens in.
 */
#pragma once
#include <SDL2/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "metadata.hpp"
#include "palette.hpp"

class TrackLoader {
public:
    struct Request {
        unsigned gen = 0;
        std::string path, folder;
        std::string myCover;    // your cover for it (Store::covers), else empty
        bool readInfo = true;   // false: only the cover again (one set or forgotten in the track menu)
        PaletteOpts opts;
    };
    struct Result {
        unsigned gen = 0;
        bool readInfo = true;
        TrackInfo info;                  // without the cover bytes
        std::string lyrics;              // the .lrc file's, else the tag's
        bool sidecar = false;            // the lyrics came from an .lrc file
        SDL_Surface* cover = nullptr;    // RGBA32, at most 512 px: the GPU path has no mipmaps (Gfx::shrink)
        SDL_Surface* blur = nullptr;     // 12×12, stretched with linear filtering for the backdrop
        SDL_Surface* frost = nullptr;    // 64×64, blurred (three box passes): the cover frosted while Automix joins
        std::vector<uint8_t> px;         // the 64×64 sample the palette came from (Brightness and Saturation rebuild it)
        Palette pal;
        void release();
    };

    void start();
    void stop();
    /** Replaces whatever is waiting: only the newest track matters. */
    void request(Request r);
    /** The newest finished result, once. The caller frees its surfaces (Result::release). */
    bool take(Result& out);

private:
    void run();
    static void work(const Request& r, Result& out);

    std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false, hasReq_ = false, hasDone_ = false;
    Request req_;
    Result done_;
};
