/*
 * Gatefold NX · ui.cpp
 */
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef __SWITCH__
// The preview's D-pad audit (Ui::audit) names items by the strings they were made from
static std::unordered_map<Id, std::string>& idNames() { static std::unordered_map<Id, std::string> m; return m; }
#endif

Id uid(const char* s, int64_t i) {
#ifndef __SWITCH__
    const char* s0 = s;
#endif
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= uint8_t(*s); h *= 1099511628211ull; }
    h ^= uint64_t(i) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
#ifndef __SWITCH__
    if (idNames().size() < 200000) idNames().emplace(h ? h : 1, i ? std::string(s0) + "#" + std::to_string(i) : std::string(s0));
#endif
    return h ? h : 1;
}
Id uid(const std::string& s, int64_t i) { return uid(s.c_str(), i); }

namespace {
bool contains(const SDL_FRect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
SDL_FRect intersect(const SDL_FRect& a, const SDL_FRect& b) {
    const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const float r = std::min(a.x + a.w, b.x + b.w), btm = std::min(a.y + a.h, b.y + b.h);
    return { x, y, std::max(0.f, r - x), std::max(0.f, btm - y) };
}
}  // namespace

/** Whether any of an item shows (inside its scroll area's view): both width and height. */
bool Ui::shown(const Item& it) {
    const SDL_FRect v = intersect(it.r, it.clip);
    return v.w > 0 && v.h > 0;
}

void Ui::beginFrame(float dt) {
    dt_ = dt;
    clock_ += dt;
    prev_.swap(cur_);
    cur_.clear();
    areasPrev_.swap(areasCur_);
    areasCur_.clear();
    linksPrev_.swap(linksCur_);
    linksCur_.clear();
    shadowPrev_.swap(shadowCur_);
    shadowCur_.clear();
    // Lists under a modal: no kinetic scroll or touch, only easing to a target set from code
    for (ScrollArea* a : glideCur_) {
        if (std::find(areasPrev_.begin(), areasPrev_.end(), a) != areasPrev_.end()) continue;
        a->clamp();
        a->y += (a->target - a->y) * (1 - std::exp(-dt / 0.07f));
        if (std::fabs(a->target - a->y) < 0.5f) a->y = std::round(a->target);
    }
    glideCur_.clear();
    activate_ = activateNext_;
    activateNext_ = 0;
    released_ = releasedNext_;
    releasedNext_ = 0;
    // A held Hold item: after half a second it is a long press, once
    long_ = 0;
    if (holding_ && !holdPad_ && press_ != holding_) holding_ = 0;   // the finger moved off or scrolled
    if (holding_) {
        holdT_ += dt;
        if (holdT_ >= 0.5f && !holdFired_) { holdFired_ = true; long_ = holding_; }
    }
    // Any other item held still under a finger: after half a second a long touch, what + is to the D-pad
    touchHold_ = 0;
    if (touching_ && press_ && !scrolling_ && holding_ != press_ && !pressFired_) {
        pressT_ += dt;
        if (pressT_ >= 0.5f) { pressFired_ = true; touchHold_ = press_; focus_ = press_; }
    }
    region_ = Region::Main;
    area_ = nullptr;
    if (wantDefaultNext_) { wantDefault_ = true; wantDefaultNext_ = false; }

    // Kinetic scrolling after a flick.
    for (ScrollArea* a : areasPrev_) {
        if (std::fabs(a->velocity) > 5 && !(touching_ && touchArea_ == a)) {
            a->manual = clock_;
            a->target += a->velocity * dt;
            a->velocity *= std::pow(0.04f, dt);
        } else if (!(touching_ && touchArea_ == a)) {
            a->velocity = 0;
        }
        a->clamp();
        a->y += (a->target - a->y) * (1 - std::exp(-dt / 0.07f));
        // At rest on a whole pixel, so its text is crisp; moving, text follows sub-pixel (Ui::onArea).
        if (std::fabs(a->target - a->y) < 0.5f && !(touching_ && touchArea_ == a)) a->y = std::round(a->target);
    }
}

bool Ui::item(Id id, SDL_FRect r, float radius, uint8_t flags) {
    if (quiet_) return false;
    if (modal_ && region_ != Region::Modal) {
        // Under a modal: not focusable nor pressable, but where it is stays known (the tour lights it up, rectAny)
        shadowCur_.push_back({ id, r, area_ ? area_->view : SDL_FRect{ -1e5f, -1e5f, 2e5f, 2e5f }, 0, 0, region_, area_ });
        // its list still glides when told to (the tour scrolls to what it lights), but takes no touch
        if (area_ && std::find(glideCur_.begin(), glideCur_.end(), area_) == glideCur_.end()) glideCur_.push_back(area_);
        return false;
    }
    SDL_FRect clip = area_ ? area_->view : SDL_FRect{ -1e5f, -1e5f, 2e5f, 2e5f };
    if (area_ && std::find(areasCur_.begin(), areasCur_.end(), area_) == areasCur_.end()) areasCur_.push_back(area_);
    cur_.push_back({ id, r, clip, radius, flags, region_, area_ });
    if (wantDefault_ && (flags & DefaultFocus)) { focus_ = id; wantDefault_ = false; scrollToFocus_ = true; }
    const bool hit = activate_ == id;
    if (hit) activate_ = 0;
    return hit;
}

const Ui::Item* Ui::find(const std::vector<Item>& v, Id id) const {
    for (const Item& it : v) if (it.id == id) return &it;
    return nullptr;
}

bool Ui::hasItem(Id id) const { return find(cur_, id) || find(prev_, id); }
Id Ui::firstIn(const ScrollArea* a) const {
    const Item* best = nullptr;
    for (const Item& it : prev_) {
        if (it.area != a || !shown(it)) continue;
        if (!best || it.r.y < best->r.y - 1 || (std::fabs(it.r.y - best->r.y) <= 1 && it.r.x < best->r.x)) best = &it;
    }
    return best ? best->id : 0;
}

SDL_FRect Ui::rectOf(Id id) const {
    const Item* it = find(cur_, id);
    if (!it) it = find(prev_, id);   // between beginFrame and the draw (input), this frame has no items yet
    return it ? it->r : SDL_FRect{};
}
SDL_FRect Ui::rectAny(Id id, ScrollArea** area) const {
    for (const auto* v : { &cur_, &prev_, &shadowCur_, &shadowPrev_ })
        if (const Item* it = find(*v, id)) {
            if (area) *area = it->area;
            return it->r;
        }
    if (area) *area = nullptr;
    return SDL_FRect{};
}

float Ui::radiusOf(Id id) const {
    const Item* it = find(cur_, id);
    return it ? it->radius : 0;
}
bool Ui::focusIsSlider2D() const {
    const Item* it = find(prev_, focus_);
    return it && (it->flags & Slider2D) && engaged_ == focus_;
}

bool Ui::focusIsSlider() const {
    const Item* it = find(prev_, focus_);
    return it && (it->flags & Slider) && engaged_ == focus_;
}

Id Ui::itemAt(float x, float y) const {
    for (auto it = prev_.rbegin(); it != prev_.rend(); ++it)
        if (contains(it->r, x, y) && contains(it->clip, x, y)) return it->id;
    return 0;
}

void Ui::endFrame() {
    sliderStep_ = sliderStepY_ = 0;
    if (engaged_ && engaged_ != focus_) engaged_ = 0;   // focus moved on: the slider is put down
    if (wantDefault_) {
        // No DefaultFocus item in this view: take the first item in the main area.
        for (const Item& it : cur_)
            if (it.region == fallbackRegion()) { focus_ = it.id; scrollToFocus_ = true; break; }
        wantDefault_ = false;
    }
    const Item* f = find(cur_, focus_);
    if (!f) {
        // The focused item disappeared (view change, list refresh): fall back sensibly.
        if (!cur_.empty() && focus_) {
            for (const Item& it : cur_) if (it.region == fallbackRegion()) { focus_ = it.id; break; }
        }
        return;
    }
    regionMemory_[int(f->region)] = f->id;
    if (scrollToFocus_ && f->area) {
        ScrollArea* a = f->area;
        const float top = f->r.y - a->view.y + a->y, bottom = top + f->r.h;
        const float pad = 12;
        if (top - pad < a->target) a->target = top - pad;
        else if (bottom + pad > a->target + a->view.h) a->target = bottom + pad - a->view.h;
        a->clamp();
        if (padMode_) a->manual = clock_;   // moved there with the D-pad
    }
    scrollToFocus_ = false;
}

void Ui::navigate(int dx, int dy) {
    padMode_ = true;
    engaged_ = 0;
    const Item* f = find(prev_, focus_);
    if (!f) {
        for (const Item& it : prev_) if (it.flags & DefaultFocus) { focus_ = it.id; scrollToFocus_ = true; return; }
        for (const Item& it : prev_) if (it.region == Region::Main) { focus_ = it.id; scrollToFocus_ = true; return; }
        if (!prev_.empty()) focus_ = prev_.front().id;
        return;
    }
    bool blocked = false;
    const Item* best = pick(prev_, linksPrev_, f, dx, dy, &blocked);
    if (!best) {
        // Nothing further in that direction inside a scrolled list: nudge the list.
        if (dy && f->area && !blocked) { f->area->target += dy * 120; f->area->clamp(); }
        return;
    }
    // Entering another region: return to where focus was last time. Not for the title bar: it is one row, and
    // remembering the search field there made the sidebar button left of it unreachable from the sidebar.
    if (best->region != f->region && best->region != Region::Title) {
        auto mem = regionMemory_.find(int(best->region));
        if (mem != regionMemory_.end() && find(prev_, mem->second)) {
            const Item* m = find(prev_, mem->second);
            if (shown(*m)) best = m;
        }
    }
    focus_ = best->id;
    scrollToFocus_ = true;
}

/** The item the D-pad goes to from f among items (no side effects): Ui::navigate, and the preview's audit. */
const Ui::Item* Ui::pick(const std::vector<Item>& items, const std::vector<Link>& links, const Item* f, int dx, int dy,
                         bool* blocked) const {
    // A link the view set wins over the layout (the album grid's top row, the Canvas's bottom row)
    for (const Link& l : links)
        if (l.from == f->id && l.dx == dx && l.dy == dy) {
            if (!l.to) { if (blocked) *blocked = true; return nullptr; }
            if (const Item* t = find(items, l.to)) return t;
        }
    // What shows of an item: a row half under the player bar is judged by its visible strip (the player bar's
    // Left/Right once went to rows hidden under it). Out of view entirely, it keeps its own rectangle.
    auto part = [](const Item& it) { const SDL_FRect v = intersect(it.r, it.clip); return v.w > 0 && v.h > 0 ? v : it.r; };
    const SDL_FRect fr = part(*f);
    const float fcx = fr.x + fr.w / 2, fcy = fr.y + fr.h / 2;
    // The title bar and the player bar are rows of their own: Left/Right never go into or out of them
    auto barRow = [](Region r) { return r == Region::Title || r == Region::Bar; };
    const Item* best = nullptr;
    const Item* bestIn = nullptr;   // the best in f's own scroll area (Up/Down stay in it while there is one)
    float bestScore = 1e30f, bestInScore = 1e30f;
    for (const Item& c : items) {
        if (c.id == f->id) continue;
        const bool sameArea = c.area && c.area == f->area;
        // Items scrolled out of view are still candidates within the same area (that is how lists scroll),
        // but only the visible ones of other areas: visible in both directions (a row scrolled out below the
        // page still spans its width, and counting width alone sent the player bar's Down back into the page).
        if (!sameArea && !shown(c)) continue;
        if (sameArea) {
            const float top = c.r.y - c.area->view.y;
            if (top < -c.area->view.h || top > c.area->view.h * 2) continue;
        }
        if (dx && (barRow(f->region) || barRow(c.region)) && c.region != f->region) continue;
        if (dx && (c.flags & NoSideEntry)) continue;
        if (!dx && (c.flags & NoUpDownEntry)) continue;
        const SDL_FRect cr = sameArea ? c.r : part(c);
        const float ccx = cr.x + cr.w / 2, ccy = cr.y + cr.h / 2;
        float primary, ortho, overlap;
        if (dx) {
            primary = dx > 0 ? cr.x - (fr.x + fr.w) : fr.x - (cr.x + cr.w);
            if ((dx > 0 && ccx <= fcx + 1) || (dx < 0 && ccx >= fcx - 1)) continue;
            overlap = std::min(fr.y + fr.h, cr.y + cr.h) - std::max(fr.y, cr.y);
            ortho = overlap > 0 ? 0 : std::fabs(ccy - fcy);
        } else {
            primary = dy > 0 ? cr.y - (fr.y + fr.h) : fr.y - (cr.y + cr.h);
            if ((dy > 0 && ccy <= fcy + 1) || (dy < 0 && ccy >= fcy - 1)) continue;
            overlap = std::min(fr.x + fr.w, cr.x + cr.w) - std::max(fr.x, cr.x);
            ortho = overlap > 0 ? std::fabs(ccx - fcx) * 0.05f : std::fabs(ccx - fcx);
        }
        // Up/Down within one page: the next row first, wherever its control sits (a switch at the card's right edge was
        // passed over for a control two rows down that lined up); in a grid the column below still wins
        float score = std::max(primary, -8.f) + ortho * (dy && sameArea ? 0.5f : 2.5f);
        if (c.region != f->region) score += 40;   // stay within a region when there is a choice
        if (f->area && c.area != f->area) score += 200;
        if (score < bestScore) { bestScore = score; best = &c; }
        if (sameArea && score < bestInScore) { bestInScore = score; bestIn = &c; }
    }
    // Up/Down inside a scrolled page stay in it while it goes on that way: a control at the far end of the next
    // row (Settings' switches) lost to the player bar's button just below, and the rest of the page was out of reach
    if (dy && bestIn) return bestIn;
    return best;
}

#ifndef __SWITCH__
/** The preview's D-pad audit (GF_SCRIPT "audit <label>"): every item of this frame, where each direction goes from
 *  it, and what cannot be reached from the focus by the D-pad. Region memory is left out (it changes with history). */
void Ui::audit(const char* label, FILE* out) const {
    auto name = [](Id id) {
        auto& m = idNames();
        if (auto it = m.find(id); it != m.end()) return it->second;
        for (uint64_t k = 1; k <= 12; k++)   // a segmented control's option: id + golden × (k)
            if (auto it = m.find(id - 0x9e3779b97f4a7c15ull * k); it != m.end()) return it->second + "[" + std::to_string(k - 1) + "]";
        char b[24];
        snprintf(b, sizeof b, "?%016llx", (unsigned long long)id);
        return std::string(b);
    };
    static const char* regions[] = { "Title", "Side", "Main", "Bar", "Overlay", "Modal" };
    const std::vector<Item>& v = cur_;
    if (label[0] == '@') {   // a trace step: only where the focus is
        const Item* f = find(v, focus_);
        fprintf(out, "%s %s %s %.0f,%.0f\n", label, name(focus_).c_str(), f ? regions[int(f->region)] : "?", f ? f->r.x : 0.f, f ? f->r.y : 0.f);
        fflush(out);
        return;
    }
    fprintf(out, "=== %s: %zu items, focus %s\n", label, v.size(), name(focus_).c_str());
    auto at = [&](const Item* it) { return it ? name(it->id) : std::string("-"); };
    for (const Item& it : v) {
        const bool seen = intersect(it.r, it.clip).w > 0 && intersect(it.r, it.clip).h > 0;
        fprintf(out, "  %-6s %-34s %4.0f,%4.0f %4.0fx%-4.0f%s  L:%s R:%s U:%s D:%s\n", regions[int(it.region)], name(it.id).c_str(),
                it.r.x, it.r.y, it.r.w, it.r.h, seen ? "" : " (hidden)", at(pick(v, linksCur_, &it, -1, 0)).c_str(), at(pick(v, linksCur_, &it, 1, 0)).c_str(),
                at(pick(v, linksCur_, &it, 0, -1)).c_str(), at(pick(v, linksCur_, &it, 0, 1)).c_str());
    }
    // Reachable from the focus (or the first item)
    std::vector<const Item*> todo;
    std::unordered_map<Id, bool> got;
    const Item* start = find(v, focus_);
    if (!start && !v.empty()) start = &v.front();
    if (start) { todo.push_back(start); got[start->id] = true; }
    while (!todo.empty()) {
        const Item* f = todo.back();
        todo.pop_back();
        for (int d = 0; d < 4; d++) {
            const Item* n = pick(v, linksCur_, f, d == 0 ? -1 : d == 1 ? 1 : 0, d == 2 ? -1 : d == 3 ? 1 : 0);
            if (n && !got[n->id]) { got[n->id] = true; todo.push_back(n); }
        }
    }
    for (const Item& it : v)
        if (!got[it.id] && shown(it)) fprintf(out, "  UNREACHABLE %s (%s, %.0f,%.0f)\n", name(it.id).c_str(), regions[int(it.region)], it.r.x, it.r.y);
    // One-way moves: A → B, but the opposite way from B is not A
    for (const Item& it : v) {
        if (!shown(it)) continue;
        for (int d = 0; d < 4; d++) {
            const int dx = d == 0 ? -1 : d == 1 ? 1 : 0, dy = d == 2 ? -1 : d == 3 ? 1 : 0;
            const Item* n = pick(v, linksCur_, &it, dx, dy);
            if (!n || n->region != it.region) continue;
            const Item* back = pick(v, linksCur_, n, -dx, -dy);
            if (back != &it) fprintf(out, "  ONEWAY %s -%c-> %s -%c-> %s\n", name(it.id).c_str(), "LRUD"[d], name(n->id).c_str(), "RLDU"[d], at(back).c_str());
        }
    }
    fflush(out);
}
#endif

void Ui::activate() {
    padMode_ = true;
    // A on a slider picks it up (or puts it down) instead of activating it.
    if (const Item* it = find(prev_, focus_); it && (it->flags & Slider)) {
        engaged_ = engaged_ == focus_ ? 0 : focus_;
        return;
    }
    if (const Item* it = find(prev_, focus_); it && (it->flags & Hold)) {   // on release, or a long press (padHeld)
        holding_ = focus_;
        holdPad_ = true;
        holdT_ = 0;
        holdFired_ = false;
        return;
    }
    if (focus_) activateNext_ = focus_;
}

void Ui::padHeld(bool a) {
    if (!holdPad_ || a) return;
    if (!holdFired_ && holding_) activateNext_ = holding_;
    holding_ = 0;
    holdPad_ = false;
}

void Ui::scrollFocusedArea(float dy) {
    const Item* f = find(prev_, focus_);
    ScrollArea* a = f && f->area ? f->area : (areasPrev_.empty() ? nullptr : areasPrev_.front());
    if (!a) return;
    a->target += dy;
    a->clamp();
    a->manual = clock_;
}

bool Ui::scrollAreaAt(float x, float y, float dy) {
    for (auto it = areasPrev_.rbegin(); it != areasPrev_.rend(); ++it) {   // the last drawn is on top (a sheet over the page)
        ScrollArea* a = *it;
        if (!contains(a->view, x, y) || a->max() <= 0) continue;   // (one that cannot scroll lets the one under it)
        a->target += dy;
        a->clamp();
        a->manual = clock_;
        return true;
    }
    return false;
}

void Ui::touchDown(float x, float y) {
    padMode_ = false;
    touching_ = true;
    scrolling_ = false;
    downX_ = x; downY_ = y; lastY_ = y;
    press_ = 0;
    pressT_ = 0;
    pressFired_ = false;
    touchArea_ = nullptr;
    for (auto it = prev_.rbegin(); it != prev_.rend(); ++it) {
        if (!contains(it->r, x, y) || !contains(it->clip, x, y)) continue;
        if (it->flags & Slider) { drag_ = it->id; dragX_ = x; dragY_ = y; focus_ = it->id; return; }
        press_ = it->id;
        if (it->flags & Hold) { holding_ = it->id; holdPad_ = false; holdT_ = 0; holdFired_ = false; }
        break;
    }
    for (ScrollArea* a : areasPrev_)
        if (contains(a->view, x, y)) { touchArea_ = a; a->velocity = 0; a->target = a->y; break; }
}

void Ui::touchMove(float x, float y) {
    if (!touching_) return;
    if (drag_) { dragX_ = x; dragY_ = y; return; }
    if (!scrolling_ && touchArea_ && std::fabs(y - downY_) > 10) { scrolling_ = true; press_ = 0; }
    if (scrolling_ && touchArea_) {
        touchArea_->manual = clock_;
        const float d = lastY_ - y;
        touchArea_->target += d;
        touchArea_->clamp();
        touchArea_->y = touchArea_->target;
        if (dt_ > 0) touchArea_->velocity = touchArea_->velocity * 0.6f + (d / dt_) * 0.4f;
    }
    if (std::hypot(x - downX_, y - downY_) > 14) press_ = 0;
    lastY_ = y;
}

void Ui::touchUp(float x, float y) {
    if (!touching_) return;
    touching_ = false;
    if (drag_) { releasedNext_ = drag_; dragX_ = x; dragY_ = y; drag_ = 0; return; }
    if (press_ && !scrolling_) {
        focus_ = press_;
        if (!(holdFired_ && holding_ == press_) && !pressFired_) activateNext_ = press_;   // a long press was its own action
    }
    if (!holdPad_) holding_ = 0;
    press_ = 0;
    (void)y;
}
