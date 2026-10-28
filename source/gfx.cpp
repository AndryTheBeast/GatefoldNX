/*
 * Gatefold NX · gfx.cpp
 */
#include "gfx.hpp"

#include <SDL2/SDL_image.h>
#include <switch.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr float PI = 3.14159265358979f;

const char* const kInter[] = { "romfs:/fonts/Inter-Regular.ttf", "romfs:/fonts/Inter-SemiBold.ttf", "romfs:/fonts/Inter-Bold.ttf",
                               "romfs:/fonts/Inter-ExtraBold.ttf" };
const PlSharedFontType kSystemFonts[] = { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                                          PlSharedFontType_ChineseTraditional, PlSharedFontType_KO };

Uint8 byte(float v) { return Uint8(std::lround(std::clamp(v, 0.f, 1.f) * 255)); }
SDL_Color sdl(const Rgb& c, float a) { return { byte(c.r), byte(c.g), byte(c.b), byte(a) }; }

/** Decodes one UTF-8 code point and advances i. */
Uint32 nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + len > s.size()) len = 1;
    Uint32 cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3f);
    i += len;
    return cp;
}

}  // namespace

bool Gfx::init(SDL_Renderer* r) {
    r_ = r;
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    if (TTF_Init() != 0) return false;
    for (PlSharedFontType t : kSystemFonts) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t))) sysFonts_.push_back({ fd.address, fd.size });
    }

    // Glow: CSS radial-gradient(closest-side, colour, transparent).
    const int N = 128;
    std::vector<Uint32> px(N * N);
    glowTex_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const float d = std::hypot(x + 0.5f - N / 2.f, y + 0.5f - N / 2.f) / (N / 2.f);
            px[y * N + x] = (Uint32(byte(std::max(0.f, 1 - d))) << 24) | 0xffffff;
        }
    SDL_UpdateTexture(glowTex_, nullptr, px.data(), N * 4);

    // Veil: alpha rises linearly from the focus to 70% of the radius, then stays.
    veilTex_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const float d = std::hypot(x + 0.5f - N / 2.f, y + 0.5f - N / 2.f) / (N / 2.f);
            px[y * N + x] = (Uint32(byte(std::min(1.f, d / 0.7f))) << 24) | 0xffffff;
        }
    SDL_UpdateTexture(veilTex_, nullptr, px.data(), N * 4);

    // Vignette: transparent to 40% of the radius, then a smooth rise to full at the edge.
    vignetteTex_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const float d = std::hypot(x + 0.5f - N / 2.f, y + 0.5f - N / 2.f) / (N / 2.f);
            const float t = std::clamp((d - 0.4f) / 0.6f, 0.f, 1.f);
            px[y * N + x] = (Uint32(byte(t)) << 24) | 0xffffff;
        }
    SDL_UpdateTexture(vignetteTex_, nullptr, px.data(), N * 4);
    for (SDL_Texture* t : { glowTex_, veilTex_, vignetteTex_ }) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return true;
}

void Gfx::shutdown() {
    for (auto& [k, c] : textCache_) SDL_DestroyTexture(c.tex);
    for (auto& [k, c] : iconCache_) SDL_DestroyTexture(c.tex);
    textCache_.clear();
    iconCache_.clear();
    for (auto& [k, f] : fonts_) if (f) TTF_CloseFont(f);
    fonts_.clear();
    for (SDL_Texture* t : { glowTex_, veilTex_, vignetteTex_ }) if (t) SDL_DestroyTexture(t);
    glowTex_ = veilTex_ = vignetteTex_ = nullptr;
    TTF_Quit();
}

void Gfx::endFrame() {
    frame_++;
    ghostBudget_ = 2;
    newBudget_ = 40;
    wrapBudget_ = 3;
    // Eviction is spread out: destroying a hundred textures in one frame was a hitch of its own, and it
    // could land right as an animation began. Only a big cache is trimmed, and only what has been
    // unused for 10 s (the Canvas controls fade for longer than 3 s and come back).
    std::unordered_map<std::string, Cached>* caches[2] = { &textCache_, &iconCache_ };
    const size_t keep[2] = { 320, 96 };
    if (frame_ % 60 == 0)
        for (int c = 0; c < 2; c++) {
            if (caches[c]->size() <= keep[c] || !stale_[c].empty()) continue;
            for (const auto& [k, v] : *caches[c]) if (frame_ - v.lastFrame > 600) stale_[c].push_back(k);
        }
    for (int c = 0, n = 0; c < 2; c++)
        while (!stale_[c].empty() && n < 3) {
            auto it = caches[c]->find(stale_[c].back());
            stale_[c].pop_back();
            if (it == caches[c]->end() || frame_ - it->second.lastFrame <= 600) continue;   // drawn again since
            if (it->second.tex) SDL_DestroyTexture(it->second.tex);
            caches[c]->erase(it);
            n++;
        }
    if (ellipsized_.size() > 2048) ellipsized_.clear();
    if (wrapped_.size() > 2048) wrapped_.clear();
    if (faceCache_.size() > 4096) faceCache_.clear();
    if (measured_.size() > 8192) measured_.clear();
}

// ------------------------------------------------------------------ shapes

/** Rounded-rectangle outline, clockwise, with outward normals for the AA feather. */
void Gfx::outline(float x, float y, float w, float h, float rad, std::vector<Pt>& pts) {
    const float r4[4] = { rad, rad, rad, rad };
    outline(x, y, w, h, r4, pts);
}

/** Corners clockwise from the top right: top-right, bottom-right, bottom-left, top-left. */
void Gfx::outline(float x, float y, float w, float h, const float* rads, std::vector<Pt>& pts) {
    pts.clear();
    const float start[4] = { -PI / 2, 0, PI / 2, PI };
    for (int c = 0; c < 4; c++) {
        const float rad = std::clamp(rads[c], 0.f, std::min(w, h) / 2);
        const int seg = rad < 0.5f ? 0 : std::clamp(int(rad / 1.5f), 3, 20);
        const float cx = c < 2 ? x + w - rad : x + rad;
        const float cy = c == 0 || c == 3 ? y + rad : y + h - rad;
        if (seg == 0) {
            const float a = start[c] + PI / 4;
            pts.push_back({ cx, cy, std::cos(a) * 1.4142f, std::sin(a) * 1.4142f });
            continue;
        }
        // A quarter circle in seg steps, worked out once per step count: every rounded shape asked for its
        // sines and cosines each frame. Rotated per corner: 90° turns are swaps and sign changes.
        static float quarter[21][21][2];
        static bool made[21];
        if (!made[seg]) {
            for (int i = 0; i <= seg; i++) {
                quarter[seg][i][0] = std::cos((PI / 2) * i / seg);
                quarter[seg][i][1] = std::sin((PI / 2) * i / seg);
            }
            made[seg] = true;
        }
        for (int i = 0; i <= seg; i++) {
            const float co = quarter[seg][i][0], si = quarter[seg][i][1];
            const float nx = c == 0 ? si : c == 1 ? co : c == 2 ? -si : -co;
            const float ny = c == 0 ? -co : c == 1 ? si : c == 2 ? co : -si;
            pts.push_back({ cx + rad * nx, cy + rad * ny, nx, ny });
        }
    }
}

/** Fan from the centre to an inset outline, then a 1 px feather strip to transparent. */
template <class ColorFn>
void Gfx::shape(const std::vector<Pt>& pts, float cx, float cy, ColorFn col, SDL_Texture* tex, const SDL_FRect* box, const SDL_FRect* src) {
    const int n = int(pts.size());
    if (n < 3) return;
    auto uv = [&](float px, float py) -> SDL_FPoint {
        if (!box) return { 0, 0 };
        // The AA feather reaches half a pixel outside the box; SDL rejects the whole draw for UVs outside 0..1.
        const float u = std::clamp((px - box->x) / box->w, 0.f, 1.f), v = std::clamp((py - box->y) / box->h, 0.f, 1.f);
        return { src->x + u * src->w, src->y + v * src->h };
    };
    auto vert = [&](float px, float py, float alphaMul) {
        SDL_Color c = col(px, py);
        c.a = Uint8(c.a * alphaMul);
        verts_.push_back({ { px, py }, c, uv(px, py) });
    };
    verts_.clear();
    idx_.clear();
    vert(cx, cy, 1);
    for (const Pt& p : pts) vert(p.x - p.nx * 0.5f, p.y - p.ny * 0.5f, 1);
    for (const Pt& p : pts) vert(p.x + p.nx * 0.5f, p.y + p.ny * 0.5f, 0);
    for (int i = 0; i < n; i++) {
        const int a = 1 + i, b = 1 + (i + 1) % n, oa = 1 + n + i, ob = 1 + n + (i + 1) % n;
        idx_.insert(idx_.end(), { 0, a, b, a, oa, ob, a, ob, b });
    }
    SDL_RenderGeometry(r_, tex, verts_.data(), int(verts_.size()), idx_.data(), int(idx_.size()));
}

void Gfx::fill(const Rgb& c, float alpha) {
    if (ghost_) return;   // a ghost draws nothing
    const SDL_Color s = sdl(c, alpha);
    SDL_SetRenderDrawColor(r_, s.r, s.g, s.b, s.a);
    SDL_RenderFillRect(r_, nullptr);
}

void Gfx::rect(float x, float y, float w, float h, const Rgb& c, float alpha) {
    if (ghost_) return;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetRenderDrawColor(r_, s.r, s.g, s.b, s.a);
    const SDL_FRect rc{ x, y, w, h };
    SDL_RenderFillRectF(r_, &rc);
}

void Gfx::rectV(float x, float y, float w, float h, const Rgb& top, float aTop, const Rgb& bottom, float aBottom) {
    if (ghost_) return;
    const SDL_Color t = sdl(top, aTop), b = sdl(bottom, aBottom);
    const SDL_Vertex v[4] = { { { x, y }, t, {} }, { { x + w, y }, t, {} }, { { x + w, y + h }, b, {} }, { { x, y + h }, b, {} } };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

void Gfx::roundRect(float x, float y, float w, float h, float rad, const Rgb& c, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rad, pts);
    const SDL_Color s = sdl(c, alpha);
    shape(pts, x + w / 2, y + h / 2, [&](float, float) { return s; });
}

void Gfx::roundRectH(float x, float y, float w, float h, float rad, const Rgb& l, const Rgb& r, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rad, pts);
    shape(pts, x + w / 2, y + h / 2, [&](float px, float) {
        const float t = std::clamp((px - x) / w, 0.f, 1.f);
        return sdl({ l.r + (r.r - l.r) * t, l.g + (r.g - l.g) * t, l.b + (r.b - l.b) * t }, alpha);
    });
}

void Gfx::roundRectV(float x, float y, float w, float h, float rad, const Rgb& top, const Rgb& bot, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rad, pts);
    shape(pts, x + w / 2, y + h / 2, [&](float, float py) {
        const float t = std::clamp((py - y) / h, 0.f, 1.f);
        return sdl({ top.r + (bot.r - top.r) * t, top.g + (bot.g - top.g) * t, top.b + (bot.b - top.b) * t }, alpha);
    });
}

void Gfx::roundStroke(float x, float y, float w, float h, float rad, float th, const Rgb& c, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    std::vector<Pt> outer;
    outline(x, y, w, h, rad, outer);
    const int n = int(outer.size());
    const SDL_Color full = sdl(c, alpha), none = sdl(c, 0);
    verts_.clear();
    idx_.clear();
    // Four loops from outside in: feather, solid outer edge, solid inner edge, feather.
    const float off[4] = { 0.5f, -0.5f, -th + 0.5f, -th - 0.5f };
    for (int k = 0; k < 4; k++)
        for (const Pt& p : outer) verts_.push_back({ { p.x + p.nx * off[k], p.y + p.ny * off[k] }, (k == 0 || k == 3) ? none : full, {} });
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < n; i++) {
            const int a = k * n + i, b = k * n + (i + 1) % n, c2 = (k + 1) * n + i, d = (k + 1) * n + (i + 1) % n;
            idx_.insert(idx_.end(), { a, b, c2, b, d, c2 });
        }
    SDL_RenderGeometry(r_, nullptr, verts_.data(), int(verts_.size()), idx_.data(), int(idx_.size()));
}

void Gfx::circle(float cx, float cy, float rad, const Rgb& c, float alpha) { roundRect(cx - rad, cy - rad, rad * 2, rad * 2, rad, c, alpha); }

void Gfx::image(SDL_Texture* t, float x, float y, float w, float h, float rad, float alpha, float zoom, float panX, float panY) {
    if (ghost_) return;
    if (!t || w <= 0 || h <= 0) return;
    int tw = 1, th = 1;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    // object-fit: cover — crop the texture to the box's aspect ratio, centred.
    const float boxAR = w / h, texAR = float(tw) / th;
    SDL_FRect src{ 0, 0, 1, 1 };
    if (texAR > boxAR) { src.w = boxAR / texAR; src.x = (1 - src.w) / 2; }
    else if (texAR < boxAR) { src.h = texAR / boxAR; src.y = (1 - src.h) / 2; }
    if (zoom > 1) {
        const float nw = src.w / zoom, nh = src.h / zoom;
        src.x += (src.w - nw) / 2 * (1 + std::clamp(panX, -1.f, 1.f));
        src.y += (src.h - nh) / 2 * (1 + std::clamp(panY, -1.f, 1.f));
        src.w = nw;
        src.h = nh;
    }
    std::vector<Pt> pts;
    outline(x, y, w, h, rad, pts);
    const SDL_FRect box{ x, y, w, h };
    const SDL_Color s = sdl({ 1, 1, 1 }, alpha);
    shape(pts, x + w / 2, y + h / 2, [&](float, float) { return s; }, t, &box, &src);
}

void Gfx::imageStretched(SDL_Texture* t, float x, float y, float w, float h, float rad, float alpha) {
    if (ghost_) return;
    if (!t || w <= 0 || h <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rad, pts);
    const SDL_FRect box{ x, y, w, h }, src{ 0, 0, 1, 1 };
    const SDL_Color s = sdl({ 1, 1, 1 }, alpha);
    shape(pts, x + w / 2, y + h / 2, [&](float, float) { return s; }, t, &box, &src);
}

void Gfx::imageCorners(SDL_Texture* t, float x, float y, float w, float h, const float* rads, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rads, pts);
    const SDL_FRect box{ x, y, w, h };
    if (!t) return;
    int tw = 1, th = 1;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    const float boxAR = w / h, texAR = float(tw) / th;
    SDL_FRect src{ 0, 0, 1, 1 };
    if (texAR > boxAR) { src.w = boxAR / texAR; src.x = (1 - src.w) / 2; }
    else if (texAR < boxAR) { src.h = texAR / boxAR; src.y = (1 - src.h) / 2; }
    const SDL_Color s = sdl({ 1, 1, 1 }, alpha);
    shape(pts, x + w / 2, y + h / 2, [&](float, float) { return s; }, t, &box, &src);
}

void Gfx::roundRectCorners(float x, float y, float w, float h, const float* rads, const Rgb& c, float alpha) {
    if (ghost_) return;
    if (w <= 0 || h <= 0) return;
    std::vector<Pt> pts;
    outline(x, y, w, h, rads, pts);
    const SDL_Color s = sdl(c, alpha);
    shape(pts, x + w / 2, y + h / 2, [&](float, float) { return s; });
}

void Gfx::glow(float cx, float cy, float rad, const Rgb& c, float alpha) {
    if (ghost_) return;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetTextureColorMod(glowTex_, s.r, s.g, s.b);
    SDL_SetTextureAlphaMod(glowTex_, s.a);
    const SDL_FRect dst{ cx - rad, cy - rad, rad * 2, rad * 2 };
    SDL_RenderCopyF(r_, glowTex_, nullptr, &dst);
}

void Gfx::veil(float fx, float fy, float rx, float ry, const Rgb& c, float alpha) {
    if (ghost_) return;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetTextureColorMod(veilTex_, s.r, s.g, s.b);
    SDL_SetTextureAlphaMod(veilTex_, s.a);
    const SDL_FRect dst{ fx - rx, fy - ry, rx * 2, ry * 2 };
    SDL_RenderCopyF(r_, veilTex_, nullptr, &dst);
    edgeBands(dst, s);
}

void Gfx::vignette(float cx, float cy, float rx, float ry, const Rgb& c, float alpha) {
    if (ghost_) return;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetTextureColorMod(vignetteTex_, s.r, s.g, s.b);
    SDL_SetTextureAlphaMod(vignetteTex_, s.a);
    const SDL_FRect dst{ cx - rx, cy - ry, rx * 2, ry * 2 };
    SDL_RenderCopyF(r_, vignetteTex_, nullptr, &dst);
    edgeBands(dst, s);
}

/** Beyond a radial texture's ellipse its full strength continues to the screen edges. */
void Gfx::edgeBands(const SDL_FRect& dst, SDL_Color s) {
    int W = 0, H = 0;
    SDL_GetRendererOutputSize(r_, &W, &H);
    SDL_SetRenderDrawColor(r_, s.r, s.g, s.b, s.a);
    const SDL_FRect bands[4] = { { 0, 0, float(W), std::max(0.f, dst.y) },
                                 { 0, dst.y + dst.h, float(W), std::max(0.f, H - dst.y - dst.h) },
                                 { 0, dst.y, std::max(0.f, dst.x), dst.h },
                                 { dst.x + dst.w, dst.y, std::max(0.f, W - dst.x - dst.w), dst.h } };
    SDL_RenderFillRectsF(r_, bands, 4);
}

// ------------------------------------------------------------------ text

TTF_Font* Gfx::font(Font f, int face) {
    const int key = (face * 4 + int(f.weight)) * 512 + f.size;
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    TTF_Font* t = nullptr;
    if (face == 0) {
        // Each weight's file is read into memory once and every size opens from there. Opened by name, each
        // size read the file from romfs again, and FreeType kept reading glyphs from romfs as text needed
        // them: the console's first frame took a second, most of it there.
        std::vector<uint8_t>& file = interFiles_[int(f.weight)];
        if (file.empty())
            if (FILE* fp = fopen(kInter[int(f.weight)], "rb")) {
                fseek(fp, 0, SEEK_END);
                file.resize(size_t(std::max(0L, ftell(fp))));
                fseek(fp, 0, SEEK_SET);
                if (fread(file.data(), 1, file.size(), fp) != file.size()) file.clear();
                fclose(fp);
            }
        if (!file.empty()) t = TTF_OpenFontRW(SDL_RWFromConstMem(file.data(), int(file.size())), 1, f.size);
    } else if (face - 1 < int(sysFonts_.size())) {
        const auto& sf = sysFonts_[face - 1];
        t = TTF_OpenFontRW(SDL_RWFromConstMem(sf.first, int(sf.second)), 1, f.size);
        if (t && f.weight >= Weight::SemiBold) TTF_SetFontStyle(t, TTF_STYLE_BOLD);   // no bold cut in the system fonts
    }
    if (t) TTF_SetFontHinting(t, TTF_HINTING_LIGHT);
    fonts_[key] = t;
    return t;
}

/** 0 = Inter; otherwise the first console font that has every glyph. */
int Gfx::faceFor(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return 0;
    auto it = faceCache_.find(s);
    if (it != faceCache_.end()) return it->second;
    int face = 1;
    for (int f = 0; f <= int(sysFonts_.size()); f++) {
        TTF_Font* t = font({ 16, Weight::Regular }, f);
        if (!t) continue;
        bool all = true;
        for (size_t i = 0; i < s.size() && all;) {
            const Uint32 cp = nextCodepoint(s, i);
            if (cp > 0x20 && cp != 0x2026 && !TTF_GlyphIsProvided32(t, cp)) all = false;
        }
        if (all) { face = f; break; }
    }
    faceCache_[s] = face;
    return face;
}

int Gfx::measure(const std::string& s, Font f) {
    if (s.empty()) return 0;
    // Remembered: every text with a width limit, every kicker letter and every wrap measures, every frame,
    // and on the console that was a large part of a frame.
    std::string key;
    key.reserve(s.size() + 2);
    key += char(f.size);
    key += char('0' + int(f.weight));
    key += s;
    if (auto it = measured_.find(key); it != measured_.end()) return it->second;
    int w = 0, h = 0;
    if (TTF_Font* t = font(f, faceFor(s))) TTF_SizeUTF8(t, s.c_str(), &w, &h);
    measured_.emplace(std::move(key), w);
    return w;
}

int Gfx::lineHeight(Font f) {
    TTF_Font* t = font(f, 0);
    return t ? TTF_FontHeight(t) : f.size;
}

std::string Gfx::ellipsize(const std::string& s, Font f, int maxW) {
    if (maxW <= 0 || measure(s, f) <= maxW) return s;
    const std::string key = std::to_string(int(f.weight) * 512 + f.size) + ":" + std::to_string(maxW) + ":" + s;
    if (auto it = ellipsized_.find(key); it != ellipsized_.end()) return it->second;
    std::vector<size_t> cuts;
    for (size_t i = 0; i < s.size();) { cuts.push_back(i); nextCodepoint(s, i); }
    int lo = 0, hi = int(cuts.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (measure(s.substr(0, cuts[mid]) + "…", f) <= maxW) lo = mid; else hi = mid - 1;
    }
    std::string out = s.substr(0, cuts.empty() ? 0 : cuts[lo]);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += "…";
    ellipsized_[key] = out;
    return out;
}

Gfx::Cached* Gfx::textTexture(const std::string& s, Font f) {
    const std::string key = std::to_string(int(f.weight) * 512 + f.size) + ":" + s;
    auto it = textCache_.find(key);
    if (it == textCache_.end()) {
        TTF_Font* t = font(f, faceFor(s));
        if (!t) return nullptr;
        // Rendered white once; colour comes from texture modulation so palette fades stay free.
        SDL_Surface* surf = TTF_RenderUTF8_Blended(t, s.c_str(), { 255, 255, 255, 255 });
        if (!surf) return nullptr;
        newBudget_--;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r_, surf);
        // Linear: at whole pixels it is the same as nearest, and a shifted (moving) text glides sub-pixel.
        if (tex) SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        it = textCache_.emplace(key, Cached{ tex, surf->w, surf->h, frame_ }).first;
        SDL_FreeSurface(surf);
    }
    it->second.lastFrame = frame_;
    return &it->second;
}

int Gfx::text(const std::string& raw, Font f, float x, float y, const Rgb& c, float alpha, Align a, int maxW) {
    if (ghost_) {
        if (raw.empty()) return 0;
        const std::string s = ellipsize(raw, f, maxW);
        const std::string key = std::to_string(int(f.weight) * 512 + f.size) + ":" + s;
        auto it = textCache_.find(key);
        if (it != textCache_.end()) { it->second.lastFrame = frame_; return it->second.w; }
        if (ghostBudget_ <= 0) return 0;   // the rest next frame
        ghostBudget_--;
        Cached* ct = textTexture(s, f);
        return ct ? ct->w : 0;
    }
    if (raw.empty() || alpha <= 0) return 0;
    const std::string str = ellipsize(raw, f, maxW);
    // Rendering a text is FreeType work and an upload: when a track change or a page brings dozens at once,
    // the rest wait for the next frame (drawn a frame late, measured now so the layout does not move).
    if (newBudget_ <= 0 && !textCache_.count(std::to_string(int(f.weight) * 512 + f.size) + ":" + str)) return measure(str, f);
    Cached* ct = textTexture(str, f);
    if (!ct) return 0;
    float dx = x;
    if (a == Align::Center) dx -= ct->w / 2.f;
    else if (a == Align::Right) dx -= ct->w;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetTextureColorMod(ct->tex, s.r, s.g, s.b);
    SDL_SetTextureAlphaMod(ct->tex, s.a);
    const SDL_FRect dst{ std::round(dx - shiftX_) + shiftX_, std::round(y - shiftY_) + shiftY_, float(ct->w), float(ct->h) };
    SDL_RenderCopyF(r_, ct->tex, nullptr, &dst);
    return ct->w;
}

int Gfx::textScroll(const std::string& s, Font f, float x, float y, float boxW, float offset, float fadeL, float fadeR, const Rgb& c, float alpha) {
    if (s.empty() || alpha <= 0 || ghost_) return 0;
    if (newBudget_ <= 0 && !textCache_.count(std::to_string(int(f.weight) * 512 + f.size) + ":" + s)) return measure(s, f);
    Cached* ct = textTexture(s, f);
    if (!ct || ct->w <= 0) return 0;
    // A ramp under half a pixel is none: its point would be dropped below as too close to the edge, and the alpha then
    // ran from 0 at the edge across the whole box (the player bar's title faded out for one frame as it began to move).
    if (fadeL < 0.5f) fadeL = 0;
    if (fadeR < 0.5f) fadeR = 0;
    const float tx0 = x - offset, tw = float(ct->w);
    const float v0 = std::max(x, tx0), v1 = std::min(x + boxW, tx0 + tw);
    if (v1 <= v0) return ct->w;
    // Where the alpha changes: the ends of what shows, and where each edge's ramp begins (mask-image: linear-gradient)
    float xs[4] = { v0, x + fadeL, x + boxW - fadeR, v1 };
    int n = 0;
    float pts[4];
    for (float p : xs) if (p >= v0 && p <= v1 && (n == 0 || p > pts[n - 1] + 0.01f)) pts[n++] = p;
    auto alphaAt = [&](float px) {
        float a = alpha;
        if (fadeL > 0) a *= std::clamp((px - x) / fadeL, 0.f, 1.f);
        if (fadeR > 0) a *= std::clamp((x + boxW - px) / fadeR, 0.f, 1.f);
        return a;
    };
    SDL_SetTextureColorMod(ct->tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(ct->tex, 255);
    const float top = std::round(y - shiftY_) + shiftY_, bottom = top + ct->h;
    verts_.clear();
    idx_.clear();
    for (int i = 0; i < n; i++) {
        const SDL_Color col = sdl(c, alphaAt(pts[i]));
        const float u = std::clamp((pts[i] - tx0) / tw, 0.f, 1.f);
        verts_.push_back({ { pts[i], top }, col, { u, 0 } });
        verts_.push_back({ { pts[i], bottom }, col, { u, 1 } });
        if (i > 0) {
            const int b = (i - 1) * 2;
            for (int k : { b, b + 1, b + 2, b + 1, b + 3, b + 2 }) idx_.push_back(k);
        }
    }
    if (!idx_.empty()) SDL_RenderGeometry(r_, ct->tex, verts_.data(), int(verts_.size()), idx_.data(), int(idx_.size()));
    return ct->w;
}

// Every character goes where its advance puts it (the width the font itself spaces it by, which is what
// measuring a piece of text gets wrong at its ends); digits in cells as wide as the widest digit's advance,
// each centred in its cell. A count rising then moves nothing, and the spaces are the font's own.
int Gfx::advance(Uint32 cp, Font f) {
    TTF_Font* t = font(f, 0);
    int minx = 0, maxx = 0, miny = 0, maxy = 0, adv = 0;
    if (!t || TTF_GlyphMetrics32(t, cp, &minx, &maxx, &miny, &maxy, &adv) != 0) return 0;
    return adv;
}

int Gfx::measureNum(const std::string& s, Font f) {
    int cell = 0;
    for (Uint32 d = '0'; d <= '9'; d++) cell = std::max(cell, advance(d, f));
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        const Uint32 cp = nextCodepoint(s, i);
        w += cp >= '0' && cp <= '9' ? cell : advance(cp, f);
    }
    return w;
}

int Gfx::textNum(const std::string& s, Font f, float x, float y, const Rgb& c, float alpha, Align a) {
    int cell = 0;
    for (Uint32 d = '0'; d <= '9'; d++) cell = std::max(cell, advance(d, f));
    const int total = measureNum(s, f);
    float pen = a == Align::Center ? x - total / 2.f : a == Align::Right ? x - total : x;
    for (size_t i = 0; i < s.size();) {
        const size_t st = i;
        const Uint32 cp = nextCodepoint(s, i);
        const int adv = advance(cp, f);
        const bool digit = cp >= '0' && cp <= '9';
        if (cp != ' ') text(s.substr(st, i - st), f, pen + (digit ? std::round((cell - adv) / 2.f) : 0), y, c, alpha);
        pen += digit ? cell : adv;
    }
    return total;
}

int Gfx::textC(const std::string& s, Font f, float x, float cy, const Rgb& c, float alpha, Align a, int maxW) {
    return text(s, f, x, cy - lineHeight(f) / 2.f, c, alpha, a, maxW);
}

int Gfx::measureSpaced(const std::string& s, Font f, float em) {
    int w = 0, n = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t st = i;
        nextCodepoint(s, i);
        w += measure(s.substr(st, i - st), f);
        n++;
    }
    return w + int(std::lround(em * f.size * std::max(0, n - 1)));
}

int Gfx::textSpaced(const std::string& s, Font f, float x, float cy, float em, const Rgb& c, float alpha, Align a) {
    const int total = measureSpaced(s, f, em);
    float px = a == Align::Center ? x - total / 2.f : a == Align::Right ? x - total : x;
    for (size_t i = 0; i < s.size();) {
        const size_t st = i;
        nextCodepoint(s, i);
        const std::string ch = s.substr(st, i - st);
        px += textC(ch, f, px, cy, c, alpha) + em * f.size;
    }
    return total;
}

std::vector<std::string> Gfx::wrap(const std::string& s, Font f, int maxW, int maxLines) {
    // Remembered: the Canvas wraps every lyric line every frame, and measuring is not free on the console.
    const std::string key = std::to_string(int(f.weight) * 512 + f.size) + ":" + std::to_string(maxW) + ":" + std::to_string(maxLines) + ":" + s;
    if (auto it = wrapped_.find(key); it != wrapped_.end()) return it->second;
    // A ghost (a hidden Canvas layout warming up) wraps a few new lines a frame: a new track's lyrics at
    // three sizes at once were the frame that stuttered. Until its turn a line counts as one line.
    if (ghost_) {
        if (wrapBudget_ <= 0) return { s };
        wrapBudget_--;
    }
    std::vector<std::string> words, lines;
    std::string cur;
    for (char ch : s) {
        if (ch == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    if (!cur.empty()) words.push_back(cur);
    // Each word measured on its own (remembered, and lyrics repeat their words), lines summed with the
    // space: what kerning across a space would change is under a pixel.
    const int space = spaceWidth(f);
    std::string line;
    int lineW = 0;
    std::vector<int> widths;
    bool over = false;   // the last line holds the rest and may need its ellipsis
    for (size_t i = 0; i < words.size(); i++) {
        const int ww = measure(words[i], f);
        bool fits = line.empty() || lineW + space + ww <= maxW;
        // The summed words run short of the drawn line (a part of a pixel each, kerning): Settings' hints ran 6 px past
        // their cards' padding. Near the edge the line itself is measured.
        int exact = -1;
        if (fits && !line.empty() && lineW + space + ww > maxW - 24) fits = (exact = measure(line + ' ' + words[i], f)) <= maxW;
        if (fits) {
            if (!line.empty()) { line += ' '; lineW += space; }
            line += words[i];
            lineW = exact >= 0 ? exact : lineW + ww;
            continue;
        }
        lines.push_back(line);
        widths.push_back(lineW);
        line = words[i];
        lineW = ww;
        if (int(lines.size()) == maxLines - 1) {
            for (size_t k = i + 1; k < words.size(); k++) line += " " + words[k];
            over = true;
            break;
        }
    }
    if (!line.empty()) { lines.push_back(line); widths.push_back(over ? maxW + 1 : lineW); }
    // Only a line that may be too wide is ellipsized: the last one holding the rest, or one long word.
    for (size_t k = 0; k < lines.size(); k++)
        if (widths[k] > maxW) lines[k] = ellipsize(lines[k], f, maxW);
    wrapped_[key] = lines;
    return lines;
}

// ------------------------------------------------------------------ icons

void Gfx::icon(const char* svg, float cx, float cy, float size, const Rgb& c, float alpha) {
    const int px = int(std::lround(size));
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(svg)) + ":" + std::to_string(px);
    auto it = iconCache_.find(key);
    if (it == iconCache_.end()) {
        if (ghost_) {   // the same budget as ghost text
            if (ghostBudget_ <= 0) return;
            ghostBudget_--;
        }
        std::string doc(strlen(svg) + 32, '\0');
        doc.resize(snprintf(doc.data(), doc.size(), svg, px, px));
        SDL_Surface* s = IMG_LoadSVG_RW(SDL_RWFromConstMem(doc.data(), int(doc.size())));
        SDL_Texture* t = s ? SDL_CreateTextureFromSurface(r_, s) : nullptr;
        if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        it = iconCache_.emplace(key, Cached{ t, s ? s->w : px, s ? s->h : px, frame_ }).first;
        if (s) SDL_FreeSurface(s);
    }
    Cached& ic = it->second;
    ic.lastFrame = frame_;
    if (!ic.tex || ghost_) return;
    const SDL_Color s = sdl(c, alpha);
    SDL_SetTextureColorMod(ic.tex, s.r, s.g, s.b);
    SDL_SetTextureAlphaMod(ic.tex, s.a);
    const SDL_FRect dst{ std::round(cx - ic.w / 2.f - shiftX_) + shiftX_, std::round(cy - ic.h / 2.f - shiftY_) + shiftY_, float(ic.w), float(ic.h) };
    SDL_RenderCopyF(r_, ic.tex, nullptr, &dst);
}

// ------------------------------------------------------------------ misc

void Gfx::clip(float x, float y, float w, float h) {
    const SDL_Rect r{ int(std::floor(x)), int(std::floor(y)), int(std::ceil(w)), int(std::ceil(h)) };
    SDL_RenderSetClipRect(r_, &r);
}

void Gfx::unclip() { SDL_RenderSetClipRect(r_, nullptr); }

SDL_Surface* Gfx::shrink(SDL_Surface* src, int maxSide) {
    SDL_Surface* cur = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    while (cur && std::max(cur->w, cur->h) >= maxSide * 2) {
        const int w = cur->w / 2, h = cur->h / 2;
        SDL_Surface* half = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!half) break;
        for (int y = 0; y < h; y++) {
            const uint8_t* a = static_cast<const uint8_t*>(cur->pixels) + (y * 2) * cur->pitch;
            const uint8_t* b = a + cur->pitch;
            uint8_t* o = static_cast<uint8_t*>(half->pixels) + y * half->pitch;
            for (int x = 0; x < w * 4; x++) {
                const int c = (x / 4) * 8 + x % 4;
                o[x] = uint8_t((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) / 4);
            }
        }
        SDL_FreeSurface(cur);
        cur = half;
    }
    if (cur && std::max(cur->w, cur->h) > maxSide) {
        const float k = float(maxSide) / std::max(cur->w, cur->h);
        SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, int(cur->w * k + 0.5f)), std::max(1, int(cur->h * k + 0.5f)), 32,
                                                          SDL_PIXELFORMAT_RGBA32);
        if (out && SDL_SoftStretchLinear(cur, nullptr, out, nullptr) == 0) { SDL_FreeSurface(cur); cur = out; }
        else if (out) SDL_FreeSurface(out);
    }
    return cur;
}

SDL_Texture* Gfx::loadTexture(const void* data, size_t size, SDL_Surface** keepSurface) {
    SDL_Surface* s = IMG_Load_RW(SDL_RWFromConstMem(data, int(size)), 1);
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r_, s);
    if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    if (keepSurface) *keepSurface = s;
    else SDL_FreeSurface(s);
    return t;
}
