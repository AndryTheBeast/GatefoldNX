/*
 * Songate · coverio.cpp
 * See coverio.hpp.
 */
#include "coverio.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"   // (inside stb_image)
#include "stb_image.h"
#pragma GCC diagnostic pop

bool flacPicture(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    auto u32be = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
    uint8_t h[10];
    bool ok = fread(h, 1, 4, f) == 4;
    if (ok && !memcmp(h, "ID3", 3)) {   // an ID3v2 tag before the stream (some taggers add one): its synchsafe size
        ok = fread(h + 4, 1, 6, f) == 6;
        const long size = long(h[6] & 0x7f) << 21 | long(h[7] & 0x7f) << 14 | long(h[8] & 0x7f) << 7 | long(h[9] & 0x7f);
        ok = ok && fseek(f, 10 + size + ((h[5] & 0x10) ? 10 : 0), SEEK_SET) == 0 && fread(h, 1, 4, f) == 4;
    }
    if (!ok || memcmp(h, "fLaC", 4)) { fclose(f); return false; }
    std::vector<uint8_t> first;   // the first picture of any type, kept in case there is no front cover
    for (bool last = false; !last;) {
        uint8_t b[4];
        if (fread(b, 1, 4, f) != 4) break;
        last = (b[0] & 0x80) != 0;
        const uint32_t type = b[0] & 0x7f, len = uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        if (type != 6) {   // METADATA_BLOCK_PICTURE is 6
            if (fseek(f, long(len), SEEK_CUR) != 0) break;
            continue;
        }
        std::vector<uint8_t> d(len);
        if (fread(d.data(), 1, len, f) != len || len < 32) break;
        // picture type, MIME (length + bytes), description (length + bytes), width, height, depth, colours, data (length + bytes)
        size_t at = 0;
        const uint32_t ptype = u32be(&d[at]);
        at += 4;
        const uint32_t mimeLen = u32be(&d[at]);
        at += 4 + mimeLen;
        if (at + 4 > len) break;
        const uint32_t descLen = u32be(&d[at]);
        at += 4 + descLen + 16;
        if (at + 4 > len) break;
        const uint32_t dataLen = u32be(&d[at]);
        at += 4;
        if (dataLen == 0 || at + dataLen > len) break;
        if (ptype == 3) {   // the front cover
            out.assign(d.begin() + long(at), d.begin() + long(at + dataLen));
            break;
        }
        if (first.empty()) first.assign(d.begin() + long(at), d.begin() + long(at + dataLen));
    }
    fclose(f);
    if (out.empty()) out.swap(first);
    return !out.empty();
}

SDL_Surface* loadImage(const void* data, size_t size, const char* what) {
    if (!data || !size) return nullptr;
    if (SDL_Surface* s = IMG_Load_RW(SDL_RWFromConstMem(data, int(size)), 1)) return s;
    const std::string sdlWhy = IMG_GetError();
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(size), &w, &h, &n, 4);
    if (!px) {
        coverTrace(std::string("undecodable: ") + (what ? what : "(an image)") + " (" + std::to_string(size) + " bytes; SDL_image: " + sdlWhy +
                   "; stb_image: " + stbi_failure_reason() + ")");
        return nullptr;
    }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        for (int y = 0; y < h; y++) memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), px + size_t(y) * size_t(w) * 4, size_t(w) * 4);
        coverTrace(std::string("decoded by stb_image (SDL_image could not: ") + sdlWhy + "): " + (what ? what : "(an image)"));
    }
    stbi_image_free(px);
    return s;
}

void coverTrace(const std::string& line) {
#ifdef GF_DEV
    static std::mutex m;
    static int lines = 0;
    std::lock_guard<std::mutex> g(m);
    if (lines >= 200) return;
    FILE* f = fopen("sdmc:/config/gatefold-nx/cover-trace.txt", lines == 0 ? "wb" : "ab");
    if (!f) return;
    fprintf(f, "%s\n", line.c_str());
    fclose(f);
    lines++;
#else
    (void)line;
#endif
}
