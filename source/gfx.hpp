/*
 * Gatefold NX · gfx.hpp
 * Drawing over SDL_Renderer, aiming at what Chromium draws for Gatefold:
 * antialiased rounded shapes and gradients, Inter at four weights (with the
 * console's fonts as fallback for CJK), Gatefold's SVG icons, cached text.
 */
#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
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
    /** Every alpha drawn is multiplied by f (0..1) until set back to 1: a whole block faded at once (Now playing's info while
     *  Automix glides the cover over it). */
    void setFade(float f);
    /** A wipe (Automix's Simple joins: the user, 2026-10-29): while set, what is drawn fades through a soft edge `soft` wide
     *  centred on x = `edge`, kept to its left (keepLeft: the new song's) or to its right (the old one's). Text exactly,
     *  across its letters; shapes, images and icons by their middle. */
    void setWipe(float edge, float soft, bool keepLeft) { wipeOn_ = true; wipeBump_ = false; wipeEdge_ = edge; wipeSoft_ = std::max(1.f, soft); wipeLeft_ = keepLeft; }
    /** … or only round the edge: 1 on it, nothing `soft` away on either side (the wipe's blur) */
    void setWipeBump(float edge, float soft) { wipeOn_ = true; wipeBump_ = true; wipeEdge_ = edge; wipeSoft_ = std::max(1.f, soft); }
    void clearWipe() { wipeOn_ = false; }
    float wipeAt(float x) const {
        if (!wipeOn_) return 1;
        if (wipeBump_) return std::clamp(1 - std::fabs(x - wipeEdge_) / wipeSoft_, 0.f, 1.f);
        const float v = std::clamp((wipeEdge_ - x) / wipeSoft_ + 0.5f, 0.f, 1.f);   // 1 left of the edge
        return wipeLeft_ ? v : 1 - v;
    }
    // where wipeAt bends (for text drawn in columns): up to 3
    int wipeBends(float* out) const {
        if (!wipeOn_) return 0;
        if (wipeBump_) { out[0] = wipeEdge_ - wipeSoft_; out[1] = wipeEdge_; out[2] = wipeEdge_ + wipeSoft_; return 3; }
        out[0] = wipeEdge_ - wipeSoft_ / 2; out[1] = wipeEdge_ + wipeSoft_ / 2; return 2;
    }
    // The wipe's blur: text moved sideways by dx, and text alone (shapes, images and icons not drawn)
    void setTextNudge(float dx, float dy, bool textOnly) { nudgeX_ = dx; nudgeY_ = dy; textOnly_ = textOnly; }
    /** Text drawn from its blurred copy (blurTexture: made once and cached, as the text is; a few a frame), text alone (the
     *  Automix wipe's blur, between the two songs' letters: App::wiped) */
    void setTextBlur(bool on) { blurText_ = on; textOnly_ = on; }
    /** Shapes, images and icons alone, no text (App::wiped: a block's shapes cross the edge as one, its letters apart) */
    void setShapesOnly(bool on) { shapesOnly_ = on; }
    // the blurred copy's box blur radius for a font size (a title's blur as soft for its size as small text's), and its
    // margin round the text (three passes reach 3 radii)
    static int blurRadius(int size) { return std::clamp(int(std::lround(size * 0.11f)), 3, 7); }
    static int blurPad(int size) { return 3 * blurRadius(size); }
    float fade() const;
    void fill(const Rgb& c, float alpha = 1);
    void rect(float x, float y, float w, float h, const Rgb& c, float alpha = 1);
    void rectV(float x, float y, float w, float h, const Rgb& top, float aTop, const Rgb& bottom, float aBottom);
    void roundRect(float x, float y, float w, float h, float rad, const Rgb& c, float alpha = 1);
    void roundRectH(float x, float y, float w, float h, float rad, const Rgb& left, const Rgb& right, float alpha = 1);
    void roundRectV(float x, float y, float w, float h, float rad, const Rgb& top, const Rgb& bottom, float alpha = 1);
    void roundStroke(float x, float y, float w, float h, float rad, float thick, const Rgb& c, float alpha = 1);
    void circle(float cx, float cy, float rad, const Rgb& c, float alpha = 1);
    /** An antialiased line through the points, `thick` wide (its edges feathered by a pixel): curves, as the join editor's. */
    void polyline(const std::vector<SDL_FPoint>& pts, float thick, const Rgb& c, float alpha = 1);
    /** Image in a rounded box, cropped to fill it (CSS object-fit: cover); zoom > 1 crops further (Ken Burns). */
    /** image() and roundRect() with a radius per corner (top-right, bottom-right, bottom-left, top-left): mosaics. */
    void imageCorners(SDL_Texture* t, float x, float y, float w, float h, const float* rads, float alpha = 1);
    void roundRectCorners(float x, float y, float w, float h, const float* rads, const Rgb& c, float alpha = 1);
    void image(SDL_Texture* t, float x, float y, float w, float h, float rad, float alpha = 1, float zoom = 1, float panX = 0, float panY = 0);
    /** The whole texture stretched over the box, not cropped to its shape: gradients (the colour picker's). */
    void imageStretched(SDL_Texture* t, float x, float y, float w, float h, float rad, float alpha = 1);
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
    /**
     * One line of text in a box boxW wide, moved left by offset and cut at the box, its ends fading over
     * fadeL / fadeR px (Gatefold's mask-image fade, app.css .album-facts) instead of an ellipsis: the player
     * bar's long titles, which scroll. Returns the text's whole width.
     */
    int textScroll(const std::string& s, Font f, float x, float y, float boxW, float offset, float fadeL, float fadeR, const Rgb& c,
                   float alpha = 1);
    /** Letter-spaced text (the uppercase kickers), spacing in em. Centred on cy. */
    int textSpaced(const std::string& s, Font f, float x, float cy, float spacingEm, const Rgb& c, float alpha = 1, Align a = Align::Left);
    /** Songate's wordmark (wordmark.inc: Fraunces Black, made once by tools/brand/wordmark.py): its letters in a gradient
     *  across, a → b → c, over their own blurred glow (glowC, glowA), with a shimmer of white crossing them while shine is in
     *  0..1 (where the light is, across the word). x is its left, cy the letters' middle. Its width and height as made. */
    void wordmark(float x, float cy, const Rgb& a, const Rgb& b, const Rgb& c, const Rgb& glowC, float glowA, float shine, float alpha = 1);
    static int wordmarkW();
    static int wordmarkH();
    static float wordmarkMid();
    /** Over a textSpaced (left-aligned, drawn first): the same letters again in `c`, lit through a soft band bw wide at
     *  bx (its profile as band()'s), so a light passes through the letters only (the Automix playlist's kicker). */
    int textSpacedShine(const std::string& s, Font f, float x, float cy, float spacingEm, float bx, float bw, const Rgb& c, float alpha = 1);
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
    /** An SVG icon `size` px, centred; `scale` draws that one texture larger or smaller, smoothly (a twinkle: each size
     *  was an icon of its own, so it grew in whole-pixel steps) */
    void icon(const char* svg, float cx, float cy, float size, const Rgb& c, float alpha = 1, float scale = 1);
    /** A soft band of light w wide (white in its middle, nothing at its sides), at fractional positions: it slides
     *  smoothly (Automix's sweep; strips a pixel wide moved in whole pixels). */
    void band(float x, float y, float w, float h, const Rgb& c, float alpha);
    /** The same band (bx, bw wide) inside a pill (x, y, w, h: round ends of h/2), shaped to it, its round ends included
     *  (Automix's sweep goes end to end: clipped to the straight middle, it began late and stopped early). */
    void bandInPill(float x, float y, float w, float h, float bx, float bw, const Rgb& c, float alpha);
    /** A soft glow round a rounded box: `alpha` at its edge, nothing `spread` beyond it (`spreadX` at its sides), in one
     *  piece (Automix's halo: ten faint layers had only a few levels of alpha each, and breathed in steps). */
    void glowAround(float x, float y, float w, float h, float rad, float spread, float spreadX, const Rgb& c, float alpha);
    /** While set, unused text and icons stay cached (Automix hides the interface for longer than the 10 s they would last:
     *  on its return they were all made again, and the frames stuttered). */
    void holdCache(bool hold) { holdCache_ = hold; }

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
    Cached* blurTexture(const std::string& s, Font f);   // the text's blurred copy (blurPad margin), or nullptr not made yet
    bool blurText_ = false;
    int blurBudget_ = 0;

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
    bool wipeOn_ = false, wipeLeft_ = true, wipeBump_ = false, textOnly_ = false;
    bool pixelWipe_ = false, shapesOnly_ = false;   // shape(): through the wipe per pixel, not at its centre (images)
    float nudgeX_ = 0, nudgeY_ = 0;
    float wipeEdge_ = 0, wipeSoft_ = 1;
    std::vector<std::string> stale_[2];   // text and icon keys found unused, destroyed a few a frame
    std::unordered_map<std::string, Cached> iconCache_;
    SDL_Texture* glowTex_ = nullptr;
    SDL_Texture* bandTex_ = nullptr;
    SDL_Texture* wordTex_ = nullptr;       // the wordmark's letters, white (alpha from wordmark.inc)
    SDL_Texture* wordGlowTex_ = nullptr;   // … and their blur
    bool holdCache_ = false;
    SDL_Texture* veilTex_ = nullptr;
    SDL_Texture* vignetteTex_ = nullptr;
    void edgeBands(const SDL_FRect& dst, SDL_Color c);
    unsigned frame_ = 0;
    std::vector<SDL_Vertex> verts_;
    std::vector<int> idx_;
};
