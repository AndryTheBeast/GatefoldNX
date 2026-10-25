/*
 * Gatefold NX · palette.cpp
 * Line-for-line port of palette-core.js (MIT, Gatefold contributors).
 * Constants are kept identical so a cover gives the same colours as on the PC.
 */
#include "palette.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

constexpr float PI = 3.14159265358979f;

struct Lch { float L, C, h; };

struct Cluster {
    float lab[3];
    float share;
    float L, C, h;
};

float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

// ---------- colour space conversions ----------
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; }

void linearRgbToOklab(float r, float g, float b, float out[3]) {
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    out[0] = 0.2104542553f * l + 0.793617785f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.428592205f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.808675766f * s;
}

void oklabToLinearRgb(float L, float a, float b, float out[3]) {
    const float l = std::pow(L + 0.3963377774f * a + 0.2158037573f * b, 3.f);
    const float m = std::pow(L - 0.1055613458f * a - 0.0638541728f * b, 3.f);
    const float s = std::pow(L - 0.0894841775f * a - 1.291485548f * b, 3.f);
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.707614701f * s;
}

Lch toLch(float L, float a, float b) {
    float h = std::atan2(b, a) * 180 / PI;
    if (h < 0) h += 360;
    return { L, std::hypot(a, b), h };
}

void lchToLinear(float L, float C, float h, float out[3]) {
    const float r = h * PI / 180;
    oklabToLinearRgb(L, C * std::cos(r), C * std::sin(r), out);
}

bool inGamut(const float v[3]) {
    for (int i = 0; i < 3; i++) if (v[i] < -1e-4f || v[i] > 1 + 1e-4f) return false;
    return true;
}

/** OKLCH → sRGB with chroma reduction until inside the sRGB gamut
 *  (hue and lightness preserved — the CSS Color 4 recommended approach). */
Rgb lchToSrgb(float L, float C, float h) {
    L = clampf(L, 0, 1);
    float lin[3];
    lchToLinear(L, C, h, lin);
    if (!inGamut(lin)) {
        float lo = 0, hi = C;
        for (int i = 0; i < 22; i++) {
            const float mid = (lo + hi) / 2;
            lchToLinear(L, mid, h, lin);
            if (inGamut(lin)) lo = mid; else hi = mid;
        }
        lchToLinear(L, lo, h, lin);
    }
    auto ch = [](float v) { return clampf(linearToSrgb(clampf(v, 0, 1)), 0, 1); };
    return { ch(lin[0]), ch(lin[1]), ch(lin[2]) };
}
Rgb lchToSrgb(const Lch& c) { return lchToSrgb(c.L, c.C, c.h); }

float relLuminance(const Rgb& c) {
    return 0.2126f * srgbToLinear(c.r) + 0.7152f * srgbToLinear(c.g) + 0.0722f * srgbToLinear(c.b);
}
float contrast(const Rgb& a, const Rgb& b) {
    const float la = relLuminance(a), lb = relLuminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

float hueDist(float a, float b) {
    const float d = std::fmod(std::fabs(a - b), 360.f);
    return d > 180 ? 360 - d : d;
}

// ---------- deterministic RNG for reproducible k-means (mulberry32) ----------
struct Mulberry32 {
    uint32_t seed;
    double next() {
        seed += 0x6d2b79f5u;
        uint32_t t = (seed ^ (seed >> 15)) * (1u | seed);
        t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
        return (t ^ (t >> 14)) / 4294967296.0;
    }
};

struct Sample {
    std::vector<float> pts;   // OKLab triples
    std::vector<float> wts;
    int n = 0;
};

/** Pixels → weighted OKLab points: slight centre bias (subjects sit in the
 *  middle, borders are often frames/white margins), transparent pixels dropped. */
Sample samplePixels(const uint8_t* data, int w, int h) {
    Sample s;
    s.pts.reserve(size_t(w) * h * 3);
    s.wts.reserve(size_t(w) * h);
    const float cx = (w - 1) / 2.f, cy = (h - 1) / 2.f, rmax = std::hypot(cx, cy);
    float lut[256];
    for (int i = 0; i < 256; i++) lut[i] = srgbToLinear(i / 255.f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = data + (size_t(y) * w + x) * 4;
            if (p[3] < 128) continue;
            float lab[3];
            linearRgbToOklab(lut[p[0]], lut[p[1]], lut[p[2]], lab);
            const float d = rmax > 0 ? std::hypot(x - cx, y - cy) / rmax : 0;
            s.pts.insert(s.pts.end(), lab, lab + 3);
            s.wts.push_back(1.15f - 0.35f * d);
            s.n++;
        }
    }
    return s;
}

/** Weighted k-means++ in OKLab. Lightness differences are slightly
 *  down-weighted so shading of one surface does not split into clusters. */
std::vector<Cluster> kmeans(const Sample& s, int K, int iters) {
    const int n = s.n;
    if (n == 0) return {};
    const float* pts = s.pts.data();
    const float* wts = s.wts.data();
    Mulberry32 rnd{ 0x5eed };
    const float LW = 0.7f;
    auto dist2 = [&](int i, const float* c) {
        const float dL = (pts[i * 3] - c[0]) * LW, da = pts[i * 3 + 1] - c[1], db = pts[i * 3 + 2] - c[2];
        return dL * dL + da * da + db * db;
    };

    // k-means++ seeding
    std::vector<std::array<float, 3>> cents;
    const int first = int(rnd.next() * n);
    cents.push_back({ pts[first * 3], pts[first * 3 + 1], pts[first * 3 + 2] });
    std::vector<double> dmin(n, INFINITY);
    while (int(cents.size()) < K) {
        double total = 0;
        const auto c = cents.back();
        for (int i = 0; i < n; i++) {
            const double d = dist2(i, c.data());
            if (d < dmin[i]) dmin[i] = d;
            total += dmin[i] * wts[i];
        }
        if (total <= 1e-12) break;
        double r = rnd.next() * total;
        int pick = n - 1;
        for (int i = 0; i < n; i++) { r -= dmin[i] * wts[i]; if (r <= 0) { pick = i; break; } }
        cents.push_back({ pts[pick * 3], pts[pick * 3 + 1], pts[pick * 3 + 2] });
    }

    const int k = int(cents.size());
    std::vector<int> assign(n, 0);
    for (int it = 0; it < iters; it++) {
        std::vector<std::array<double, 4>> sum(k, { 0, 0, 0, 0 });
        int moved = 0;
        for (int i = 0; i < n; i++) {
            int best = 0;
            float bd = INFINITY;
            for (int j = 0; j < k; j++) { const float d = dist2(i, cents[j].data()); if (d < bd) { bd = d; best = j; } }
            if (assign[i] != best) moved++;
            assign[i] = best;
            auto& sm = sum[best];
            const float w = wts[i];
            sm[0] += pts[i * 3] * w; sm[1] += pts[i * 3 + 1] * w; sm[2] += pts[i * 3 + 2] * w; sm[3] += w;
        }
        for (int j = 0; j < k; j++)
            if (sum[j][3] > 0) cents[j] = { float(sum[j][0] / sum[j][3]), float(sum[j][1] / sum[j][3]), float(sum[j][2] / sum[j][3]) };
        if (it > 2 && moved == 0) break;
    }

    // final shares
    std::vector<double> wsum(k, 0);
    double total = 0;
    for (int i = 0; i < n; i++) { wsum[assign[i]] += wts[i]; total += wts[i]; }
    std::vector<Cluster> out;
    for (int j = 0; j < k; j++) {
        const float share = float(wsum[j] / total);
        if (share <= 0.004f) continue;
        const Lch l = toLch(cents[j][0], cents[j][1], cents[j][2]);
        out.push_back({ { cents[j][0], cents[j][1], cents[j][2] }, share, l.L, l.C, l.h });
    }
    return out;
}

/** Merge perceptually near-identical clusters (ΔE_ok < 0.045). */
std::vector<Cluster> mergeClusters(std::vector<Cluster> cl) {
    std::vector<Cluster> out;
    std::stable_sort(cl.begin(), cl.end(), [](const Cluster& a, const Cluster& b) { return a.share > b.share; });
    for (const Cluster& c : cl) {
        Cluster* m = nullptr;
        for (auto& o : out)
            if (std::hypot(o.lab[0] - c.lab[0], o.lab[1] - c.lab[1], o.lab[2] - c.lab[2]) < 0.045f) { m = &o; break; }
        if (m) {
            const float t = m->share + c.share;
            for (int i = 0; i < 3; i++) m->lab[i] = (m->lab[i] * m->share + c.lab[i] * c.share) / t;
            m->share = t;
            const Lch l = toLch(m->lab[0], m->lab[1], m->lab[2]);
            m->L = l.L; m->C = l.C; m->h = l.h;
        } else {
            out.push_back(c);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Cluster& a, const Cluster& b) { return a.share > b.share; });
    return out;
}

/** Raise/lower L of a colour until it reaches `target` contrast vs `bg`. */
Lch enforceContrast(Lch col, const Rgb& bg, float target, float dir) {
    for (int i = 0; i < 60; i++) {
        if (contrast(lchToSrgb(col), bg) >= target) return col;
        col.L = clampf(col.L + dir * 0.012f, 0, 1);
        if (col.L <= 0 || col.L >= 1) break;
    }
    return col;
}

Lch lchOf(const Rgb& c) {
    float lab[3];
    linearRgbToOklab(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), lab);
    return toLch(lab[0], lab[1], lab[2]);
}

/** Build a complete UI palette from clusters: palette-core.js buildPalette(clusters, opts), line for line. */
Palette buildPalette(std::vector<Cluster> clusters, const PaletteOpts& opts) {
    const float intensity = opts.intensity;
    if (clusters.empty()) clusters.push_back({ { 0.3f, 0, -0.02f }, 1, 0.3f, 0.02f, 260 });

    const Cluster& dom = clusters[0];
    float avgL = 0, colorfulness = 0, hx = 0, hy = 0;
    for (const auto& c : clusters) {
        avgL += c.L * c.share;
        colorfulness += c.C * c.share;
        // Hue of the "colourful mass": chromatic clusters weighted by share×C
        const float w = c.share * c.C;
        hx += std::cos(c.h * PI / 180) * w;
        hy += std::sin(c.h * PI / 180) * w;
    }
    float massHue = std::atan2(hy, hx) * 180 / PI;
    if (massHue < 0) massHue += 360;

    const Lch askedBg = opts.hasBg ? lchOf(opts.bg) : Lch{};
    bool dark = true;
    if (opts.mode == PaletteOpts::Light) dark = false;
    else if (opts.hasBg && opts.mode != PaletteOpts::Dark) dark = askedBg.L < 0.62f;
    else if (opts.mode == PaletteOpts::Auto) dark = !(avgL > 0.74f && dom.L > 0.78f && dom.share > 0.35f);

    // ---- accent: vivid, sufficiently present, not near-black/white ----
    auto scoreAccent = [](const Cluster& c) -> float {
        if (c.C < 0.035f || c.L < 0.18f || c.L > 0.97f) return -1;
        const float presence = std::sqrt(std::min(c.share, 0.35f));
        const float lightFit = 1 - std::fabs(c.L - 0.64f) * 0.9f;
        return presence * std::pow(c.C, 0.9f) * lightFit;
    };
    std::vector<std::pair<const Cluster*, float>> ranked;
    for (const auto& c : clusters) { const float s = scoreAccent(c); if (s > 0) ranked.push_back({ &c, s }); }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.second > b.second; });
    const bool monochrome = ranked.empty();
    Lch acc = monochrome ? Lch{ 0.8f, std::min(0.03f, dom.C + 0.01f), dom.C > 0.01f ? dom.h : 250.f }
                         : Lch{ ranked[0].first->L, ranked[0].first->C, ranked[0].first->h };
    if (opts.hasAccent) acc = lchOf(opts.accent);
    const Cluster* acc2Src = nullptr;
    for (auto& r : ranked)
        if (hueDist(r.first->h, acc.h) > 28) { acc2Src = r.first; break; }
    Lch acc2 = opts.hasAccent2 ? lchOf(opts.accent2)
             : acc2Src && !opts.hasAccent ? Lch{ acc2Src->L, acc2Src->C, acc2Src->h }
                                          : Lch{ acc.L, acc.C * 0.85f, std::fmod(acc.h + 38, 360.f) };

    // ---- background: tinted by dominant hue, or by colourful mass when dominant is grey ----
    const float bgHue = dom.C > 0.025f ? dom.h : (colorfulness > 0.02f ? massHue : acc.h);
    const float bgChromaSrc = std::max(dom.C, colorfulness * 0.8f);
    Lch bg = dark ? Lch{ clampf(0.155f + dom.L * 0.06f, 0.15f, 0.215f), clampf(bgChromaSrc * 0.42f * intensity, 0.006f, 0.052f), bgHue }
                  : Lch{ clampf(0.955f + (dom.L - 0.9f) * 0.08f, 0.93f, 0.975f), clampf(bgChromaSrc * 0.22f * intensity, 0.004f, 0.03f), bgHue };
    // your background: kept within what text can be read on (a dark theme stays dark, a light one light)
    if (opts.hasBg)
        bg = dark ? Lch{ clampf(askedBg.L, 0.1f, 0.32f), clampf(askedBg.C, 0, 0.09f), askedBg.h }
                  : Lch{ clampf(askedBg.L, 0.88f, 0.99f), clampf(askedBg.C, 0, 0.05f), askedBg.h };
    const Rgb bgRgb = lchToSrgb(bg);

    const float sign = dark ? 1 : -1;
    const Lch surface{ bg.L + sign * 0.038f, bg.C * 1.05f, bg.h };
    const Lch surface2{ bg.L + sign * 0.075f, bg.C * 1.1f, bg.h };
    const Rgb surfaceRgb = lchToSrgb(surface);

    // ---- text: slight hue tint ties text to the album; contrast enforced on bg and surface ----
    const float tC = std::min(0.018f, bg.C * 0.5f);
    const Rgb white{ 1, 1, 1 };
    const Rgb worst = ((contrast(bgRgb, white) < contrast(surfaceRgb, white)) == dark) ? bgRgb : surfaceRgb;
    const Lch text = enforceContrast(dark ? Lch{ 0.965f, tC * 0.6f, bg.h } : Lch{ 0.2f, tC, bg.h }, worst, 12, sign);
    const Lch text2 = enforceContrast(dark ? Lch{ 0.8f, tC, bg.h } : Lch{ 0.4f, tC, bg.h }, worst, 6.2f, sign);
    const Lch text3 = enforceContrast(dark ? Lch{ 0.64f, tC, bg.h } : Lch{ 0.54f, tC, bg.h }, worst, 4.0f, sign);

    // ---- accent normalisation: fill accent may stay vivid, "ink" accent must meet 4.5:1 ----
    const float accC = clampf(acc.C * 1.08f * intensity, monochrome ? 0 : 0.06f, 0.2f);
    acc = opts.hasAccent ? Lch{ acc.L, clampf(acc.C, 0, 0.26f), acc.h }
        : dark           ? Lch{ clampf(acc.L, 0.7f, 0.86f), accC, acc.h }
                         : Lch{ clampf(acc.L, 0.5f, 0.78f), accC, acc.h };
    acc = enforceContrast(acc, worst, dark ? 3.2f : 2.1f, sign);
    const Lch accInk = enforceContrast(acc, worst, 4.6f, sign);
    acc2 = opts.hasAccent2 ? Lch{ acc2.L, clampf(acc2.C, 0, 0.26f), acc2.h }
         : dark            ? Lch{ clampf(acc2.L, 0.66f, 0.84f), clampf(acc2.C * intensity, 0.04f, 0.19f), acc2.h }
                           : Lch{ clampf(acc2.L, 0.44f, 0.6f), clampf(acc2.C * intensity, 0.04f, 0.19f), acc2.h };
    acc2 = enforceContrast(acc2, worst, 2.6f, sign);
    const Rgb accRgb = lchToSrgb(acc);
    const Rgb onAccentDark = lchToSrgb(0.17f, 0.02f, acc.h);
    const Lch onAccent = contrast(accRgb, onAccentDark) >= contrast(accRgb, white) ? Lch{ 0.17f, 0.02f, acc.h } : Lch{ 1, 0, 0 };

    // ---- ambient blobs: the cover's real colour structure, re-lit for the theme ----
    std::vector<Lch> blobSrc;
    for (size_t i = 0; i < clusters.size() && i < 5; i++)
        if (clusters[i].share > 0.03f) blobSrc.push_back({ clusters[i].L, clusters[i].C, clusters[i].h });
    while (blobSrc.size() < 4) blobSrc.push_back(blobSrc.size() % 2 ? acc : acc2);
    Lch blobs[4];
    for (int i = 0; i < 4; i++) {
        const Lch& c = blobSrc[i];
        const float C = clampf(std::max(c.C, i == 3 ? acc.C * 0.8f : 0.f) * 1.15f * intensity, 0.01f, 0.17f);
        blobs[i] = dark ? Lch{ clampf(c.L * 0.55f + 0.14f, 0.24f, 0.5f), C, c.h } : Lch{ clampf(0.78f + c.L * 0.14f, 0.8f, 0.93f), C * 0.75f, c.h };
    }
    // Ensure the accent hue is represented in at least one blob (synergy with controls)
    bool accentInBlobs = false;
    for (auto& b : blobs) if (hueDist(b.h, acc.h) < 25) accentInBlobs = true;
    if (!accentInBlobs && !monochrome)
        blobs[3] = dark ? Lch{ 0.42f, clampf(acc.C * 0.9f, 0.04f, 0.16f), acc.h } : Lch{ 0.84f, clampf(acc.C * 0.6f, 0.03f, 0.1f), acc.h };
    const Lch glow{ dark ? 0.55f : 0.62f, clampf(acc.C, 0.03f, 0.18f), acc.h };

    Palette p;
    p.dark = dark;
    p.monochrome = monochrome;
    p.bg = bgRgb; p.surface = surfaceRgb; p.surface2 = lchToSrgb(surface2);
    p.text = lchToSrgb(text); p.text2 = lchToSrgb(text2); p.text3 = lchToSrgb(text3);
    p.accent = accRgb; p.accentInk = lchToSrgb(accInk); p.accent2 = lchToSrgb(acc2);
    p.onAccent = lchToSrgb(onAccent); p.glow = lchToSrgb(glow);
    for (int i = 0; i < 4; i++) p.blobs[i] = lchToSrgb(blobs[i]);
    for (size_t i = 0; i < clusters.size() && i < 8; i++)
        p.swatches[p.swatchCount++] = { lchToSrgb(clusters[i].L, clusters[i].C, clusters[i].h), clusters[i].share };
    p.avgL = avgL;
    p.clusters = int(clusters.size());
    return p;
}

PaletteOpts gDefaults;

Rgb hex(uint32_t v) { return { ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f }; }

Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

}  // namespace

Palette paletteDefault() {
    Palette p;
    p.bg = hex(0x14161b); p.surface = hex(0x1b1e24); p.surface2 = hex(0x23272e);
    p.text = hex(0xf3f4f6); p.text2 = hex(0xbfc3ca); p.text3 = hex(0x8b9099);
    p.accent = hex(0x8fb4ff); p.accentInk = hex(0x8fb4ff); p.accent2 = hex(0xc49bff);
    p.onAccent = hex(0x0d1220); p.glow = hex(0x5770b0);
    p.blobs[0] = hex(0x26324f); p.blobs[1] = hex(0x3a2d55); p.blobs[2] = hex(0x1f3d48); p.blobs[3] = hex(0x4a3a62);
    return p;
}

Palette paletteFixed(FixedTheme id) {
    // theme.js FIXED: OKLCH triples, text and accents checked to WCAG AA by Gatefold
    struct L3 { float L, C, h; };
    struct Def { L3 bg, surface, surface2, text, text2, text3, accent, accentInk, accent2, onAccent, glow, blobs[4]; };
    static const Def dark = { { 0.172f, 0.012f, 266 }, { 0.207f, 0.013f, 266 }, { 0.247f, 0.014f, 266 }, { 0.93f, 0.008f, 266 },
                              { 0.795f, 0.012f, 266 }, { 0.655f, 0.014f, 266 }, { 0.74f, 0.12f, 270 },   { 0.82f, 0.095f, 270 },
                              { 0.76f, 0.09f, 205 },   { 0.18f, 0.03f, 270 },   { 0.56f, 0.11f, 270 },
                              { { 0.27f, 0.035f, 266 }, { 0.3f, 0.045f, 285 }, { 0.25f, 0.03f, 225 }, { 0.31f, 0.05f, 270 } } };
    static const Def gray = { { 0.262f, 0.003f, 250 }, { 0.298f, 0.003f, 250 }, { 0.338f, 0.004f, 250 }, { 0.94f, 0.003f, 250 },
                              { 0.83f, 0.003f, 250 },  { 0.708f, 0.004f, 250 }, { 0.88f, 0.006f, 250 },  { 0.93f, 0.004f, 250 },
                              { 0.74f, 0.01f, 250 },   { 0.22f, 0.004f, 250 },  { 0.62f, 0.006f, 250 },
                              { { 0.33f, 0.004f, 250 }, { 0.36f, 0.005f, 250 }, { 0.31f, 0.003f, 250 }, { 0.38f, 0.006f, 250 } } };
    static const Def light = { { 0.972f, 0.005f, 85 }, { 0.993f, 0.003f, 85 }, { 0.94f, 0.007f, 85 }, { 0.235f, 0.012f, 266 },
                               { 0.42f, 0.013f, 266 },  { 0.53f, 0.012f, 266 },  { 0.53f, 0.17f, 268 },   { 0.47f, 0.17f, 268 },
                               { 0.56f, 0.11f, 200 },   { 0.99f, 0.004f, 268 },  { 0.64f, 0.12f, 268 },
                               { { 0.93f, 0.02f, 85 }, { 0.92f, 0.03f, 268 }, { 0.94f, 0.022f, 205 }, { 0.9f, 0.035f, 268 } } };
    const Def& d = id == FixedTheme::Gray ? gray : id == FixedTheme::Light ? light : dark;
    auto c = [](const L3& v) { return lchToSrgb(v.L, v.C, v.h); };
    Palette p;
    p.dark = id != FixedTheme::Light;
    p.monochrome = id == FixedTheme::Gray;
    p.bg = c(d.bg); p.surface = c(d.surface); p.surface2 = c(d.surface2);
    p.text = c(d.text); p.text2 = c(d.text2); p.text3 = c(d.text3);
    p.accent = c(d.accent); p.accentInk = c(d.accentInk); p.accent2 = c(d.accent2); p.onAccent = c(d.onAccent); p.glow = c(d.glow);
    for (int i = 0; i < 4; i++) p.blobs[i] = c(d.blobs[i]);
    const Rgb sw[8] = { p.bg, p.surface, p.surface2, p.text3, p.text2, p.text, p.accent, p.accent2 };
    for (const Rgb& s : sw) p.swatches[p.swatchCount++] = { s, 0.125f };
    return p;
}

void paletteSetDefaults(const PaletteOpts& o) { gDefaults = o; }

Palette paletteFromPixels(const uint8_t* rgba, int w, int h) { return paletteFromPixels(rgba, w, h, gDefaults); }

Palette paletteFromPixels(const uint8_t* rgba, int w, int h, const PaletteOpts& o) {
    return buildPalette(mergeClusters(kmeans(samplePixels(rgba, w, h), 8, 14)), o);
}

Palette paletteCustom(const PaletteOpts& in) {
    // theme.js clustersOf: your colours stand in for a cover's, the background the largest
    PaletteOpts o = in;
    if (o.hasBg) o.mode = PaletteOpts::Auto;
    std::vector<Cluster> cl;
    auto add = [&](const Rgb& c, float share) {
        const Lch l = lchOf(c);
        cl.push_back({ { l.L, 0, 0 }, share, l.L, l.C, l.h });
    };
    if (o.hasBg) add(o.bg, 0.7f);
    if (o.hasAccent) add(o.accent, 0.2f);
    if (o.hasAccent2) add(o.accent2, 0.1f);
    return buildPalette(cl, o);
}

Palette paletteMix(const Palette& a, const Palette& b, float t) {
    Palette p = t < 0.5f ? a : b;
    p.bg = mix(a.bg, b.bg, t); p.surface = mix(a.surface, b.surface, t); p.surface2 = mix(a.surface2, b.surface2, t);
    p.text = mix(a.text, b.text, t); p.text2 = mix(a.text2, b.text2, t); p.text3 = mix(a.text3, b.text3, t);
    p.accent = mix(a.accent, b.accent, t); p.accentInk = mix(a.accentInk, b.accentInk, t);
    p.accent2 = mix(a.accent2, b.accent2, t); p.onAccent = mix(a.onAccent, b.onAccent, t); p.glow = mix(a.glow, b.glow, t);
    for (int i = 0; i < 4; i++) p.blobs[i] = mix(a.blobs[i], b.blobs[i], t);
    return p;
}
