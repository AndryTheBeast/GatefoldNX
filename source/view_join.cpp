/*
 * Gatefold NX · view_join.cpp
 * The join editor: a playlist's join between two of its tracks, after Spotify's transition editor (the user,
 * 2026-10-28). Opened from the join buttons between a playlist page's rows. Over every page, as the tour is.
 *
 * Top: both tracks' waveforms around the join, the first above the second, with what the join does drawn over
 * them: the volume curves, the bass (or bass, mids and treble) changing hands, a filter sweep, an echo, the
 * looped bars, the beats when on the beat. Listen renders the join (automix::Fader, as the player plays it) and
 * plays it from memory, the music paused, the background player's too. Then the presets (Auto, Fade, Rise,
 * Blend, Echo, Cut) and a row for each choice; changing a row makes it Custom. Save keeps it for this playlist
 * (mixes.tsv): Automix uses it whenever the playlist plays these two tracks one after the other. Auto is not
 * kept (its choices are made afresh, from the analyses).
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "cores.hpp"
#include "ffdecoder.hpp"
#include "icons.inc"
#include "waveform.hpp"

namespace {
constexpr double SecA = 45, SecB = 40, Pre = 5, Post = 6, Block = 0.02;

/** Loudness every 20 ms as heights 0..1 (waveform.hpp's shape: RMS in dB against the track's own range). */
std::vector<float> envelope(const std::vector<int16_t>& pcm, int rate) {
    const size_t blk = size_t(rate * Block), n = pcm.size() / 2 / blk;
    std::vector<float> ms(n);
    for (size_t i = 0; i < n; i++) {
        double s = 0;
        for (size_t k = i * blk * 2; k < (i + 1) * blk * 2; k++) s += double(pcm[k]) * pcm[k];
        ms[i] = float(s / (blk * 2) / (32768.0 * 32768.0));
    }
    return shapeWaveform(ms, int(n));
}
}  // namespace

automix::Plan App::joinPlan() const {
    const mixan::Info* a = je.infoA.seconds != 0 ? &je.infoA : nullptr;
    const mixan::Info* b = je.infoB.seconds != 0 ? &je.infoB : nullptr;
    return automix::plan(a, je.durA, b, store.crossfade, &je.join, mixModel());
}

void App::openJoinEditor(const std::string& playlist, int a, int b) {
    if (a < 0 || b < 0 || a >= int(lib.tracks.size()) || b >= int(lib.tracks.size())) return;
    closeJoinEditor();
    je.open = true;
    je.playlist = playlist;
    je.a = a;
    je.b = b;
    je.join = automix::Join{};
    if (const auto it = store.joins.find(Store::joinKey(playlist, lib.tracks[a].path, lib.tracks[b].path)); it != store.joins.end())
        automix::Join::unpack(it->second, je.join);
    je.kept = je.join;
    je.viewT0 = je.viewT1 = 0;
    je.drag = 0;
    const Level* la = levels.find(lib.tracks[a].path);
    const Level* lb = levels.find(lib.tracks[b].path);
    je.infoA = la ? la->mix : mixan::Info{};
    je.infoB = lb ? lb->mix : mixan::Info{};
    je.durA = lib.tracks[a].duration > 0 ? lib.tracks[a].duration : je.infoA.seconds;
    je.rate = player.sampleRate();
    je.cancel = false;
    je.loaded = 0;
    je.rendered = 0;
    je.listening = false;
    je.pcmA.clear();
    je.pcmB.clear();
    je.pcm.clear();
    je.envA.clear();
    je.envB.clear();
    // the two tracks around the join, decoded on a worker (the first one from its start, so every sample sits where
    // the analysis and the player put it)
    je.worker = std::thread([this, pa = lib.tracks[a].path, pb = lib.tracks[b].path, durA = je.durA, rate = je.rate] {
        cores::moveHere(cores::Loader);
        FfDecoder da, db;
        if (!da.open(pa, rate, 2) || !db.open(pb, rate, 2)) { je.loaded = 2; return; }
        std::vector<int16_t> buf(4096 * 2), A, B;
        long long skip = std::max(0LL, (long long)std::llround((durA - SecA) * rate));
        const double from = double(skip) / rate;
        while (skip > 0 && !je.cancel) {
            const int got = da.read(buf.data(), int(std::min<long long>(4096, skip)));
            if (got <= 0) break;
            skip -= got;
        }
        A.reserve(size_t((SecA + 2) * rate * 2));
        for (int got; !je.cancel && (got = da.read(buf.data(), 4096)) > 0;) A.insert(A.end(), buf.begin(), buf.begin() + got * 2);
        B.reserve(size_t(SecB * rate * 2));
        for (int got; !je.cancel && B.size() < size_t(SecB * rate * 2) && (got = db.read(buf.data(), 4096)) > 0;)
            B.insert(B.end(), buf.begin(), buf.begin() + got * 2);
        if (je.cancel || A.empty() || B.empty()) { je.loaded = 2; return; }
        je.envA = envelope(A, rate);
        je.envB = envelope(B, rate);
        je.aFrom = from;
        je.pcmA = std::move(A);
        je.pcmB = std::move(B);
        je.loaded = 1;
    });
}

void App::closeJoinEditor() {
    if (je.worker.joinable()) {
        je.cancel = true;
        je.worker.join();
    }
    if (je.listening) player.stopPreview();
    je.listening = false;
    je.open = false;
    je.pcmA.clear();
    je.pcmA.shrink_to_fit();
    je.pcmB.clear();
    je.pcmB.shrink_to_fit();
    je.pcm.clear();
    je.pcm.shrink_to_fit();
}

void App::drawJoinEditor() {
    if (!je.open) return;
    if (je.a >= int(lib.tracks.size()) || je.b >= int(lib.tracks.size())) { closeJoinEditor(); return; }
    ui.region(Region::Modal);
    const Track& ta = lib.tracks[je.a];
    const Track& tb = lib.tracks[je.b];
    const automix::Plan pl = joinPlan();
    const bool ready = je.loaded == 1;
    const bool off = je.join.preset == automix::Preset::Off;   // no Automix for this join

    // a rendered join (Listen) goes to the player once the worker is done
    if (je.rendered == 2) {
        if (je.worker.joinable()) je.worker.join();
        player.preview(std::move(je.pcm));
        je.pcm.clear();
        je.rendered = 0;
        je.listening = true;
    }
    if (je.listening && !player.previewing() && je.rendered == 0) {
        player.stopPreview();
        je.listening = false;
    }

    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.6f);
    const float W = 1080, H = 676, X = (css::W - W) / 2, Y = (css::H - H) / 2;
    g.roundRect(X, Y, W, H, 18, pal.surface);
    g.roundStroke(X, Y, W, H, 18, 1, pal.text, 0.08f);
    const float pad = 24, ix = X + pad, iw = W - pad * 2;

    // ---- header: what joins what, and Save
    g.roundRect(ix, Y + pad, 40, 40, 10, pal.accent, 0.14f);
    g.icon(ICON_LINK, ix + 20, Y + pad + 20, 20, pal.accentInk);
    kicker("Automix · " + je.playlist, ix + 54, Y + pad + 7, pal.accentInk);
    const float titleW = iw - 54 - 300;
    g.text(ta.title + "  ›  " + tb.title, { css::Lg, Weight::Bold }, ix + 54, Y + pad + 16, pal.text, 1, Align::Left, int(titleW));
    bool c = false;
    if (iconButton(uid("je.x"), ICON_X, X + W - pad - 17, Y + pad + 20)) { closeJoinEditor(); return; }
    {   // Save, left of ×: kept for this playlist (Auto: forgotten, so its choices are made afresh)
        const Font bf{ css::Sm, Weight::SemiBold };
        const float sw = 16 + 24 + g.measure("Save", bf) + 16;
        button(uid("je.save"), ICON_CHECK, je.join == je.kept ? "Saved" : "Save", X + W - pad - 34 - 12 - sw, Y + pad + 3, !(je.join == je.kept), &c);
        if (c) {
            const std::string key = Store::joinKey(je.playlist, ta.path, tb.path);
            if (je.join.preset == automix::Preset::Auto) store.joins.erase(key);
            else store.joins[key] = je.join.pack();
            store.saveJoins();
            je.kept = je.join;
            toast(je.join.preset == automix::Preset::Auto ? "This join is Auto again" : "Saved the join");
        }
    }

    // ---- the picture: both waveforms around the join, what it does over them
    const float vx = ix, vy = Y + pad + 58, vw = iw, vh = 200;
    // the join's span in the first track's own time (v3: it is stretched to the next one's tempo, its seconds go by at its speed)
    const double L = pl.length * pl.speed;
    g.roundRect(vx, vy, vw, vh, 12, pal.text, 0.035f);
    // the first track's time across the box: 4 s either side of the join, kept while the editor is open
    if (je.viewT1 <= je.viewT0) { je.viewT0 = pl.startOut - 4; je.viewT1 = pl.startOut + L + 4; }
    if (!je.drag) {
        je.viewT0 = std::min(je.viewT0, pl.startOut - 4);
        je.viewT1 = std::max(je.viewT1, pl.startOut + L + 4);
    }
    const double t0 = je.viewT0, t1 = je.viewT1;
    auto xAt = [&](double t) { return float(vx + (t - t0) / (t1 - t0) * vw); };
    const float laneA = vy + 12, laneB = vy + vh / 2 + 8, laneH = vh / 2 - 20;

    // ---- moving it, as on Spotify: drag the second track left or right (it comes in earlier or later: whole beats on
    // the beat), or the join's end (its length: whole bars on the beat); with the D-pad, A on either, then ←/→
    if (ready && !off) {
        const bool onBeat = pl.beatmatched && je.infoA.period > 0;
        const double P = onBeat ? je.infoA.period : 0;
        const Id sid = uid("je.shift"), lid = uid("je.end");
        const float xe = xAt(pl.startOut + L);
        ui.item(sid, { vx, laneB - 6, vw, laneH + 12 }, 8, Slider);
        ui.item(lid, { xe - 12, vy + 4, 24, vh - 8 }, 8, Slider);   // after the lane: a touch on the end takes it
        auto custom = [&](const automix::Join& from) {
            automix::Join j = from.preset == automix::Preset::Auto ? pl.join : from;   // what Auto chose, to start from
            j.preset = automix::Preset::Custom;
            return j;
        };
        auto setLength = [&](automix::Join& j, double secs) {
            if (onBeat) j.beats = std::clamp(int(std::lround(secs / P / 4)) * 4, 4, 64);
            else j.seconds = float(std::clamp(std::round(secs * 2) / 2, 0.5, 20.0));
        };
        for (const Id id : { sid, lid }) {
            const float fx = ui.sliderDrag(id);
            if (fx >= 0) {
                if (!je.drag) {
                    je.drag = id == sid ? 1 : 2;
                    je.dragX = fx;
                    je.dragT0 = t0;
                    je.dragT1 = t1;
                    je.dragFrom = je.join.preset == automix::Preset::Auto ? pl.join : je.join;
                    if (je.drag == 2 && onBeat && je.dragFrom.beats == 0) je.dragFrom.beats = pl.beats;
                    if (je.drag == 2 && !onBeat && je.dragFrom.seconds == 0) je.dragFrom.seconds = float(pl.length);
                }
                const double sec = (fx - je.dragX) / vw * (je.dragT1 - je.dragT0);
                automix::Join j = custom(je.dragFrom);
                if (je.drag == 1) {   // the finger moves the second track: later when it goes right
                    if (onBeat) j.shiftBeats = je.dragFrom.shiftBeats + int(std::lround(sec / P));
                    else j.shiftSec = float(je.dragFrom.shiftSec + std::round(sec * 10) / 10);
                } else {   // the end moves, the start stays (the plan keeps a join's end: it is moved back by as much)
                    setLength(j, (onBeat ? je.dragFrom.beats * P : je.dragFrom.seconds) + sec);
                    if (onBeat) j.shiftBeats = je.dragFrom.shiftBeats + (j.beats - je.dragFrom.beats);
                    else j.shiftSec = je.dragFrom.shiftSec + (j.seconds - je.dragFrom.seconds);
                }
                je.join = j;
            } else if (je.drag && (id == sid) == (je.drag == 1)) {
                je.drag = 0;   // let go
            }
            if (const int st = ui.sliderStep(id)) {   // the D-pad
                automix::Join j = custom(je.join);
                if (id == sid) {
                    if (onBeat) j.shiftBeats += st;
                    else j.shiftSec = float(std::round((j.shiftSec + st * 0.25) * 100) / 100);
                } else {
                    const int nb = j.beats ? j.beats : pl.beats;
                    const float ns = j.seconds > 0 ? j.seconds : float(pl.length);
                    setLength(j, L + st * (onBeat ? 4 * P : 0.5));
                    if (onBeat) j.shiftBeats += j.beats - nb;
                    else j.shiftSec += j.seconds - ns;
                }
                je.join = j;
            }
        }
        // the end's grip, and a ring while the second track is picked up
        const bool endHot = je.drag == 2 || ui.engaged(lid) || (ui.focused(lid) && ui.showFocusRing());
        g.roundRect(xe - 3, vy + vh / 2 - 22, 6, 44, 3, pal.accent, endHot ? 1.f : 0.7f);
        if (je.drag == 1 || ui.engaged(sid)) g.roundStroke(vx + 2, laneB - 6, vw - 4, laneH + 12, 8, 2, pal.accent, 0.7f);
    }
    // the join's span
    if (!off) g.rect(xAt(pl.startOut), vy, xAt(pl.startOut + L) - xAt(pl.startOut), vh, pal.accent, 0.06f);
    if (pl.beatmatched && pl.bpm > 0) {   // its beats
        const double P = je.infoA.period > 0 ? je.infoA.period : 60.0 / pl.bpm;
        for (int k = 0; k <= pl.beats; k++) {
            const float x = xAt(pl.startOut + k * P);
            g.rect(x, vy + 6, 1, vh - 12, pal.text, k % 4 == 0 ? 0.16f : 0.07f);
        }
    }
    if (!off && (pl.structured || pl.drop)) {   // the crossover: the next track's first loud section (its drop), the outro of this one
        const float xc = xAt(pl.startOut + pl.cross * L);
        g.rect(xc - 1, vy + 4, 2, vh - 8, pal.accent, 0.8f);
    }
    if (pl.loopTo > 0) {   // the looped bars, and where they repeat
        g.rect(xAt(pl.loopFrom), laneA - 4, xAt(pl.loopTo) - xAt(pl.loopFrom), laneH + 8, { 1.f, 0.62f, 0.2f }, 0.12f);
        g.rect(xAt(pl.loopTo), laneA - 4, std::max(0.f, std::min(vx + vw, xAt(pl.startOut + L)) - xAt(pl.loopTo)), laneH + 8,
               { 1.f, 0.62f, 0.2f }, 0.05f);
    }
    if (off) {
        g.textC("No Automix for this join: “" + ta.title + "” plays to its end, then “" + tb.title + "”", { css::Sm, Weight::SemiBold },
                vx + vw / 2, vy + vh / 2, pal.text2, 1, Align::Center, int(vw - 40));
    } else if (!ready) {
        g.textC(je.loaded == 2 ? "Could not read the two tracks" : "Reading the two tracks…", { css::Sm, Weight::SemiBold }, vx + vw / 2,
                vy + vh / 2, pal.text2, 1, Align::Center);
    } else {
        automix::Gains gn;
        const float step = 3;
        for (float x = vx + 2; x < vx + vw - 2; x += step) {
            const double ta0 = t0 + (x - vx) / vw * (t1 - t0);
            const double tt = (ta0 - pl.startOut) / std::max(0.05, L);
            automix::gainsAt(pl, tt, gn);
            const bool before = tt < 0, after = tt > 1;
            // the first track: its own time, or within its loop past loopTo
            double sa = ta0;
            if (pl.loopTo > 0 && sa > pl.loopTo) sa = pl.loopFrom + std::fmod(sa - pl.loopTo, pl.loopTo - pl.loopFrom);
            const long ia = long((sa - je.aFrom) / Block);
            const float ha = ia >= 0 && ia < long(je.envA.size()) && sa <= je.durA ? je.envA[size_t(ia)] : 0.f;
            const float ga = before ? 1.f : after ? 0.f : std::max(gn.out * gn.dry, gn.bandOut[0] * gn.dry);
            g.rect(x, laneA + laneH / 2 - ha * laneH / 2, step - 1, std::max(1.f, ha * laneH), pal.text, 0.12f + 0.4f * ga);
            // the second one: from its start offset at the join's start
            const double sb = pl.offset + automix::inTime(pl, ta0 - pl.startOut);   // the first track's seconds at its matched tempo
            const long ib = long(sb / Block);
            const float hb = ib >= 0 && ib < long(je.envB.size()) ? je.envB[size_t(ib)] : 0.f;
            const float gb = before ? 0.f : after ? 1.f : std::max(gn.in, gn.bandIn[0]);
            g.rect(x, laneB + laneH / 2 - hb * laneH / 2, step - 1, std::max(1.f, hb * laneH), pal.accent, 0.15f + 0.45f * gb);
        }
        // the curves over them: volume (blue), the bass (yellow), a filter's cut-off (purple), the echo (orange)
        const Rgb blue{ 0.42f, 0.71f, 0.95f }, yellow{ 0.98f, 0.8f, 0.3f }, purple{ 0.78f, 0.55f, 0.98f }, orange{ 1.f, 0.62f, 0.2f };
        // each curve an antialiased line (Gfx::polyline) through a point every pixel; broken where it has no value
        std::vector<SDL_FPoint> pts;
        auto curve = [&](float lane, const Rgb& col, auto value) {
            pts.clear();
            const float xa = xAt(pl.startOut), xb = xAt(pl.startOut + L);
            for (float x = xa; x <= xb + 0.5f; x += 1) {
                const double tt = (t0 + (std::min(x, xb) - vx) / vw * (t1 - t0) - pl.startOut) / std::max(0.05, L);
                automix::gainsAt(pl, tt, gn);
                const float v = value(gn);
                if (v < 0) {
                    g.polyline(pts, 2.5f, col, 0.95f);
                    pts.clear();
                    continue;
                }
                pts.push_back({ std::min(x, xb), lane + laneH - std::clamp(v, 0.f, 1.f) * laneH });
            }
            g.polyline(pts, 2.5f, col, 0.95f);
        };
        curve(laneA, blue, [](const automix::Gains& q) { return q.out * q.dry; });
        curve(laneB, blue, [](const automix::Gains& q) { return q.in; });
        if (pl.join.eq != automix::Eq::None) {
            curve(laneA, yellow, [](const automix::Gains& q) { return q.bandOut[0] * q.dry; });
            curve(laneB, yellow, [](const automix::Gains& q) { return q.bandIn[0]; });
        }
        auto cut = [](float hz) { return hz > 0 ? float(std::log(hz / 20) / std::log(1000.0)) : -1.f; };   // 20 Hz-20 kHz up the lane
        if (pl.join.filter == automix::Filter::HighPassOut || pl.join.filter == automix::Filter::Both)
            curve(laneA, purple, [&](const automix::Gains& q) { return cut(q.hpOut); });
        if (pl.join.filter == automix::Filter::LowPassIn || pl.join.filter == automix::Filter::Both)
            curve(laneB, purple, [&](const automix::Gains& q) { return cut(q.lpIn); });
        if (pl.join.effect == automix::Effect::Echo) curve(laneA, orange, [](const automix::Gains& q) { return q.wet > 0 ? q.wet * 0.8f : -1.f; });
        // Listen's playhead
        if (je.listening) {
            const float x = xAt(je.listenFrom + player.previewPos());
            if (x >= vx && x <= vx + vw) g.rect(x - 1, vy + 4, 2, vh - 8, pal.text, 0.9f);
        }
    }
    g.text(ta.title, { css::Xs, Weight::SemiBold }, vx + 10, vy + 4, pal.text2, 0.9f, Align::Left, int(vw / 2));
    g.text(tb.title, { css::Xs, Weight::SemiBold }, vx + 10, laneB - 10, pal.accentInk, 0.9f, Align::Left, int(vw / 2));

    // ---- Listen, and what the join is
    float y = vy + vh + 12;
    {
        const bool busy = je.rendered == 1;
        bool lc = false;
        button(uid("je.listen"), je.listening ? ICON_PAUSE : ICON_PLAY, je.listening ? "Stop" : busy ? "Preparing…" : "Listen", ix, y, true, &lc, false,
               false, DefaultFocus);
        if (lc && ready && !busy && !off) {
            if (je.listening) {
                player.stopPreview();
                je.listening = false;
            } else {
                if (isPlaying()) togglePlay();   // the music pauses (the background player's too)
                if (je.worker.joinable()) je.worker.join();
                je.rendered = 1;
                je.listenFrom = std::max(je.aFrom, pl.startOut - Pre);
                je.worker = std::thread([this, pl, from = je.listenFrom] {
                    cores::moveHere(cores::Loader);
                    const int rate = je.rate;
                    const std::vector<int16_t>& A = je.pcmA;
                    const std::vector<int16_t>& B = je.pcmB;
                    size_t ia = size_t(std::max(0.0, (from - je.aFrom) * rate)), ib = size_t(std::max(0.0, pl.offset * rate));
                    auto readA = [&](int16_t* o, int n) {
                        const int k = int(std::min<size_t>(size_t(n), A.size() / 2 - std::min(ia, A.size() / 2)));
                        std::copy(A.begin() + long(ia * 2), A.begin() + long((ia + size_t(k)) * 2), o);
                        ia += size_t(k);
                        return k;
                    };
                    auto readB = [&](int16_t* o, int n) {
                        const int k = int(std::min<size_t>(size_t(n), B.size() / 2 - std::min(ib, B.size() / 2)));
                        std::copy(B.begin() + long(ib * 2), B.begin() + long((ib + size_t(k)) * 2), o);
                        ib += size_t(k);
                        return k;
                    };
                    // as the player plays it (automix::render: the first track stretched to the second's tempo, the fader)
                    std::vector<int16_t> out;
                    out.reserve(size_t((Pre + pl.length + Post) * rate * 2));
                    automix::render(pl, rate, pl.startOut - from, Post, readA, readB, [&](const float* x, int n) {
                        for (int i = 0; i < n * 2; i++) out.push_back(int16_t(std::lrint(std::clamp(x[i] * 32767.f, -32768.f, 32767.f))));
                    }, nullptr, &je.cancel);
                    je.pcm = std::move(out);
                    je.rendered = je.cancel ? 0 : 2;
                });
            }
        }
    }
    {
        std::string what;
        char b[160];
        if (off) {
            what = "Off: these two tracks are not mixed";
        } else if (je.infoA.seconds == 0 || je.infoB.seconds == 0) {
            what = "Not analysed yet: a plain fade. Automix playlists' tracks are analysed in the background";
        } else if (pl.beatmatched) {
            snprintf(b, sizeof b, "%s · %d beats at %.0f BPM · %.1f s", pl.drop ? "On the next track's drop, on the bars" : pl.structured ? "By the songs' sections, on the bars" : pl.onBars ? "On the beat and the bars" : "On the beat", pl.beats,
                     double(pl.bpm), pl.length);
            what = b;
            if (pl.speed != 1) {   // tempo matching (Stretcher)
                snprintf(b, sizeof b, " · this track %+.1f %% from a few beats before, the next as it is", (pl.speed - 1) * 100);
                what += b;
            }
        } else if (pl.drop || pl.cut) {   // between different tempos (v3): never two beats at once
            snprintf(b, sizeof b, "%s · %.0f → %.0f BPM, no tempo change · %.1f s",
                     pl.cut ? "A cut on a phrase, into the next track's downbeat" : "A bridge into the next track's drop, on a phrase",
                     double(je.infoA.bpm), double(je.infoB.bpm), pl.length);
            what = b;
        } else {
            snprintf(b, sizeof b, "Timed to the music · %.1f s", pl.length);
            what = b;
            if (pl.join.beatmatch && !pl.beatmatchable) {
                if (je.infoA.confidence < 0.5f || je.infoB.confidence < 0.5f) what += " · Beatmatch unavailable: no steady beat";
                else {
                    snprintf(b, sizeof b, " · Beatmatch unavailable: %.0f and %.0f BPM", double(je.infoA.bpm), double(je.infoB.bpm));
                    what += b;
                }
            }
        }
        if (!off && je.infoA.key >= 0 && je.infoB.key >= 0) {   // the keys, on the Camelot wheel (v2 step 5)
            what += " · " + mixan::camelot(je.infoA.key) + " → " + mixan::camelot(je.infoB.key);
            if (pl.keyClash) what += ": the keys clash, kept short";
        }
        if (player.remote()) what += " · In the queue only with Keep playing outside Gatefold off, for now";
        g.textC(what, { css::Sm }, ix + 150, y + 17, pal.text2, 1, Align::Left, int(iw - 150));
    }

    // ---- presets
    y += 34 + 14;
    {
        float px = ix;
        using automix::Preset;
        for (const Preset pr : { Preset::Off, Preset::Auto, Preset::Fade, Preset::Rise, Preset::Blend, Preset::Echo, Preset::Cut }) {   // Off first (the user)
            bool pc = false;
            px += button(uid("je.pre", int(pr)), pr == Preset::Auto ? ICON_SPARKLE : nullptr, automix::name(pr), px, y, false, &pc, je.join.preset == pr) + 8;
            if (pc) {
                je.join = automix::fromPreset(pr);
                je.viewT0 = je.viewT1 = 0;   // a fresh start: the picture fits the new join
            }
        }
        if (je.join.preset == automix::Preset::Custom) chip("Custom", px + 4, y + 17, true);
    }

    // ---- the rows: a change makes the join Custom (Auto's choices taken as they were)
    y += 34 + 12;
    auto edit = [&](auto apply) {
        if (je.join.preset == automix::Preset::Auto) je.join = pl.join;   // what Auto chose, to start from
        apply(je.join);
        je.join.preset = automix::Preset::Custom;
    };
    const float rowH = 38, rx = ix + iw;
    auto row = [&](const char* label, const char* key, const std::vector<std::string>& opts, int cur, unsigned disabled, auto apply) {
        g.textC(label, { css::Md, Weight::SemiBold }, ix, y + rowH / 2, pal.text, 1);
        int picked = -1;
        segmented(uid(key), rx, y + rowH / 2, opts, cur, &picked, disabled);
        if (picked >= 0 && picked != cur) edit([&](automix::Join& j) { apply(j, picked); });
        y += rowH + 4;
    };
    if (off) return;   // no rows: nothing to choose
    const automix::Join& J = pl.join;
    row("Volume", "je.vol", { "Smooth", "Linear", "Both up", "Cut" }, int(J.volume), 0u,
        [](automix::Join& j, int i) { j.volume = automix::Volume(i); });
    row("EQ", "je.eq", { "None", "Bass swap", "Early bass", "Late bass", "Full swap" }, int(J.eq), 0u, [](automix::Join& j, int i) { j.eq = automix::Eq(i); });
    row("Filter", "je.filter", { "None", "High-pass out", "Low-pass in", "Both" }, int(J.filter), 0u,
        [](automix::Join& j, int i) { j.filter = automix::Filter(i); });
    row("Effects", "je.fx", { "None", "Echo out" }, int(J.effect), 0u, [](automix::Join& j, int i) { j.effect = automix::Effect(i); });
    const bool grid = je.infoA.confidence >= 0.5f && je.infoA.period > 0;
    row("Looping", "je.loop", { "None", "Last bar", "Last 2 bars" }, J.loopBars, grid ? 0u : 6u, [](automix::Join& j, int i) { j.loopBars = i; });
    if (pl.beatmatched) {
        static const int beats[] = { 0, 4, 8, 16, 32 };
        int cur = je.join.preset == automix::Preset::Auto || je.join.beats == 0 ? 0 : -1;   // −1: a length dragged in between
        for (int i = 1; i < 5; i++) if (cur != 0 && J.beats == beats[i]) cur = i;
        row("Length", "je.len", { "Auto", "4", "8", "16", "32 beats" }, cur, 0u, [](automix::Join& j, int i) { j.beats = beats[i]; });
    } else {
        static const float secs[] = { 0, 2, 4, 6, 8, 12 };
        int cur = je.join.preset == automix::Preset::Auto || je.join.seconds == 0 ? 0 : -1;
        for (int i = 1; i < 6; i++) if (cur != 0 && std::fabs(J.seconds - secs[i]) < 0.01f) cur = i;
        row("Length", "je.len", { "Auto", "2", "4", "6", "8", "12 s" }, cur, 0u, [](automix::Join& j, int i) { j.seconds = secs[i]; });
    }
    {
        g.textC("Beatmatching", { css::Md, Weight::SemiBold }, ix, y + rowH / 2, pal.text, 1);
        g.textC(pl.beatmatchable ? "Starts the next track on a beat of this one" : "Unavailable for these two tracks", { css::Sm }, ix + 150,
                y + rowH / 2, pal.text3, 1);
        const bool on = J.beatmatch && pl.beatmatchable;
        if (toggle(uid("je.beat"), rx - 40, y + rowH / 2, on) && pl.beatmatchable) edit([&](automix::Join& j) { j.beatmatch = !on; });
    }
}
