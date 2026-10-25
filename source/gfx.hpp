/*
 * Gatefold NX · gfx.hpp
 * Drawing over SDL_Renderer, aiming at what Chromium draws for Gatefold:
 * antialiased rounded shapes and gradients, Inter at four weights (with the
 * console's fonts as fallback for CJK), Gatefold's SVG icons, cached text.
 */
#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "palette.hpp"

enum class Align { Left, Center, Right };
enum class Weight : uint8_t { Regular, SemiBold, Bold, ExtraBold };

struct Font {
    int size;
    Weight weight = Weight::Regular;
};

class Gfx {
public:
    bool init(SDL_Renderer* r);
    void shutdown();
    void endFrame();   // evicts text and icons that were not drawn recently

    SDL_Renderer* renderer() const { return r_; }

    // ---- shapes (all antialiased) ----
    void fill(const Rgb& c, float alpha = 1);
    void rect(float x, float y, float w, float h, const Rgb& c, float alpha = 1);
    void rectV(float x, float y, float w, float h, const Rgb& top, float aTop, const Rgb& bottom, float aBottom);
    void roundRect(float x, float y, float w, float h, float rad, const Rgb& c, float alpha = 1);
    void roundRectH(float x, float y, float w, float h, float rad, const Rgb& left, const Rgb& right, float alpha = 1);
    void roundRectV(float x, float y, float w, float h, float rad, const Rgb& top, const Rgb& bottom, float alpha = 1);
    void roundStroke(float x, float y, float w, float h, float rad, float thick, const Rgb& c, float alpha = 1);
    void circle(float cx, float cy, float rad, const Rgb& c, float alpha = 1);
    /** Image in a rounded box, cropped to fill it (CSS object-fit: cover); zoom > 1 crops further (Ken Burns). */
    /** image() and roundRect() with a radius per corner (top-right, bottom-right, bottom-left, top-left): mosaics. */
    void imageCorners(SDL_Texture* t, float x, float y, float w, float h, const float* rads, float alpha = 1);
    void roundRectCorners(float x, float y, float w, float h, const float* rads, const Rgb& c, float alpha = 1);
    void image(SDL_Texture* t, float x, float y, float w, float h, float rad, float alpha = 1, float zoom = 1, float panX = 0, float panY = 0);
    /** Soft radial glow: full colour in the centre fading to nothing at `rad`. */
    void glow(float cx, float cy, float rad, const Rgb& c, float alpha);
    /** Radial veil: transparent at (fx, fy), rising to `alpha` at 70% of an rx×ry ellipse (Gatefold's #bg .veil). */
    void veil(float fx, float fy, float rx, float ry, const Rgb& c, float alpha);
    /** Radial vignette: transparent to 40% of an rx×ry ellipse, `alpha` at its edge and beyond (the Canvas's .cv-vignette). */
    void vignette(float cx, float cy, float rx, float ry, const Rgb& c, float alpha);

    // ---- text ----
    int text(const std::string& s, Font f, float x, float y, const Rgb& c, float alpha = 1, Align a = Align::Left, int maxW = 0);
    /** Same, vertically centred on cy. */
    int textC(const std::string& s, Font f, float x, float cy, const Rgb& c, float alpha = 1, Align a = Align::Left, int maxW = 0);
    /** Letter-spaced text (the uppercase kickers), spacing in em. Centred on cy. */
    int textSpaced(const std::string& s, Font f, float x, float cy, float spacingEm, const Rgb& c, float alpha = 1, Align a = Align::Left);
    /**
     * Ghost mode: nothing is drawn, but text() and icon() render and keep what they would draw, at
     * most a couple of new textures a frame. The Canvas runs its hidden layouts and faded controls
     * through it (and the whole Canvas while it is closed), so nothing that appears has to render a
     * screenful of text in its first frame, and nothing about to appear is evicted.
     */
    void setGhost(bool on) { ghost_ = on; }
    bool ghost() const { return ghost_; }
    /** Ghost-renders one text (Gfx::setGhost) without drawing anything: warms what is about to show. */
    void warm(const std::string& s, Font f, int maxW = 0) { const bool g = ghost_; ghost_ = true; text(s, f, 0, 0, {}, 1, Align::Left, maxW); ghost_ = g; }
    /**
     * Text and icons snap to whole pixels so they stay crisp. Moving ones would then step a pixel at a
     * time, which at the slow ends of an eased animation (or a scroll) looks like stutter. A shift is
     * the moving part of the position: the rest is snapped, the shift is added on top, sub-pixel.
     */
    void setShift(float dx, float dy) { shiftX_ = dx; shiftY_ = dy; }
    float shiftY() const { return shiftY_; }
    int measure(const std::string& s, Font f);
    /**
     * A count that changes: every character placed by its own advance, digits in equal cells (the widest
     * digit's), so nothing moves as it rises and the spacing is the font's. Inter has tabular figures, but
     * SDL_ttf cannot turn OpenType features on. Latin text only (Inter's own glyphs).
     */
    int textNum(const std::string& s, Font f, float x, float y, const Rgb& c, float alpha = 1, Align a = Align::Left);
    int measureNum(const std::string& s, Font f);
    int advance(Uint32 codepoint, Font f);
    /** A space's advance: FreeType leaves a trailing space out of a measured width, so " " measures nothing. */
    int spaceWidth(Font f) { return measure("x x", f) - measure("xx", f); }
    int measureSpaced(const std::string& s, Font f, float spacingEm);
    int lineHeight(Font f);
    /** Greedy word wrap; the last line is ellipsised when text remains. */
    std::vector<std::string> wrap(const std::string& s, Font f, int maxW, int maxLines);

    // ---- icons (Gatefold's 24×24 SVG symbols, see icons.inc) ----
    void icon(const char* svg, float cx, float cy, float size, const Rgb& c, float alpha = 1);

    // ---- misc ----
    void clip(float x, float y, float w, float h);
    void unclip();
    SDL_Texture* loadTexture(const void* data, size_t size, SDL_Surface** keepSurface = nullptr);
    /** An RGBA32 copy whose longer side is at most maxSide: 2×2 box halvings, then one linear step. The GPU
     *  path has no mipmaps, so a 1280 px cover drawn at 450 px aliases and shimmers when it moves. */
    static SDL_Surface* shrink(SDL_Surface* src, int maxSide);

private:
    struct Cached { SDL_Texture* tex; int w, h; unsigned lastFrame; };
    struct Pt { float x, y, nx, ny; };

    TTF_Font* font(Font f, int face);
    int faceFor(const std::string& s);
    std::string ellipsize(const std::string& s, Font f, int maxW);
    Cached* textTexture(const std::string& s, Font f);

    void outline(float x, float y, float w, float h, float rad, std::vector<Pt>& pts);
    void outline(float x, float y, float w, float h, const float* rads, std::vector<Pt>& pts);
    template <class ColorFn>
    void shape(const std::vector<Pt>& pts, float cx, float cy, ColorFn col, SDL_Texture* tex = nullptr, const SDL_FRect* uvBox = nullptr,
               const SDL_FRect* uvSrc = nullptr);

    SDL_Renderer* r_ = nullptr;
    std::vector<std::pair<void*, size_t>> sysFonts_;   // console fonts for fallback
    std::unordered_map<int, TTF_Font*> fonts_;
    std::vector<uint8_t> interFiles_[4];   // the Inter weights, read once (fonts open from memory)
    std::unordered_map<std::string, Cached> textCache_;
    std::unordered_map<std::string, int> faceCache_;
    std::unordered_map<std::string, std::string> ellipsized_;
    std::unordered_map<std::string, std::vector<std::string>> wrapped_;
    std::unordered_map<std::string, int> measured_;   // font + text → width: measuring is FreeType work, every text asks each frame
    bool ghost_ = false;
    int ghostBudget_ = 0;
    int newBudget_ = 0;
    int wrapBudget_ = 0;  // new wraps a ghost may do this frame   // new visible text textures left this frame: a page or a track change spreads over a few frames
    float shiftX_ = 0, shiftY_ = 0;
    std::vector<std::string> stale_[2];   // text and icon keys found unused, destroyed a few a frame
    std::unordered_map<std::string, Cached> iconCache_;
    SDL_Texture* glowTex_ = nullptr;
    SDL_Texture* veilTex_ = nullptr;
    SDL_Texture* vignetteTex_ = nullptr;
    void edgeBands(const SDL_FRect& dst, SDL_Color c);
    unsigned frame_ = 0;
    std::vector<SDL_Vertex> verts_;
    std::vector<int> idx_;
};
