/*
 * Gatefold NX · palette.hpp
 * Cover → UI palette. A C++ port of Gatefold's theme/js/palette-core.js:
 * OKLab samples → weighted k-means++ → merged clusters → role assignment
 * → WCAG contrast enforcement → gamut-mapped sRGB.
 */
#pragma once
#include <cstdint>

struct Rgb { float r = 0, g = 0, b = 0; };   // sRGB, 0..1

struct Palette {
    bool dark = true;
    bool monochrome = false;
    Rgb bg, surface, surface2;
    Rgb text, text2, text3;
    Rgb accent, accentInk, accent2, onAccent, glow;
    Rgb blobs[4];
    struct Swatch { Rgb c; float share; };
    Swatch swatches[8];
    int swatchCount = 0;
    float avgL = 0;
    int clusters = 0;
};

/** Gatefold's idle palette (the CSS @property initial values in app.css). */
Palette paletteDefault();

/** Gatefold's fixed themes (theme.js FIXED), for an interface that does not follow the cover. */
enum class FixedTheme { Dark, Gray, Light };
Palette paletteFixed(FixedTheme id);

/** palette-core.js buildPalette's opts: light or dark, saturation, and colours you chose. */
struct PaletteOpts {
    enum Mode { Dark, Light, Auto } mode = Dark;
    float intensity = 1;   // Settings → Appearance → Saturation, 0..1.5
    bool hasAccent = false, hasAccent2 = false, hasBg = false;
    Rgb accent, accent2, bg;   // taken as they are, only moved as far as readability needs
};

/** What paletteFromPixels uses when given no opts (the metadata thread's album palettes): Settings → Appearance. */
void paletteSetDefaults(const PaletteOpts& o);

/** rgba: tightly packed R,G,B,A bytes. Gatefold samples a 64×64 thumbnail. */
Palette paletteFromPixels(const uint8_t* rgba, int w, int h);
Palette paletteFromPixels(const uint8_t* rgba, int w, int h, const PaletteOpts& o);

/** theme.js customPalette: your background and accents, no cover (Settings → Appearance → Custom). */
Palette paletteCustom(const PaletteOpts& o);

/** Per-channel blend, used to animate between palettes. */
Palette paletteMix(const Palette& a, const Palette& b, float t);
