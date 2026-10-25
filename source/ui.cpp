/*
 * Gatefold NX · ui.cpp
 */
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

Id uid(const char* s, int64_t i) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= uint8_t(*s); h *= 1099511628211ull; }
    h ^= uint64_t(i) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
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

void Ui::beginFrame(float dt) {
    dt_ = dt;
    clock_ += dt;
    prev_.swap(cur_);
    cur_.clear();
    areasPrev_.swap(areasCur_);
    areasCur_.clear();
    activate_ = activateNext_;
    activateNext_ = 0;
    released_ = releasedNext_;
    releasedNext_ = 0;
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
    if (modal_ && region_ != Region::Modal) return false;
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
        if (it.area != a || intersect(it.r, it.clip).w <= 0) continue;
        if (!best || it.r.y < best->r.y - 1 || (std::fabs(it.r.y - best->r.y) <= 1 && it.r.x < best->r.x)) best = &it;
    }
    return best ? best->id : 0;
}

SDL_FRect Ui::rectOf(Id id) const {
    const Item* it = find(cur_, id);
    if (!it) it = find(prev_, id);   // between beginFrame and the draw (input), this frame has no items yet
    return it ? it->r : SDL_FRect{};
}
float Ui::radiusOf(Id id) const {
    const Item* it = find(cur_, id);
    return it ? it->radius : 0;
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
    sliderStep_ = 0;
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
    const float fcx = f->r.x + f->r.w / 2, fcy = f->r.y + f->r.h / 2;
    const Item* best = nullptr;
    float bestScore = 1e30f;
    for (const Item& c : prev_) {
        if (c.id == f->id) continue;
        // Items scrolled out of view are still candidates within the same area (that is how lists scroll),
        // but only the visible ones of other areas.
        if (c.area != f->area && intersect(c.r, c.clip).w <= 0) continue;
        if (c.area && c.area == f->area) {
            const float top = c.r.y - c.area->view.y;
            if (top < -c.area->view.h || top > c.area->view.h * 2) continue;
        }
        const float ccx = c.r.x + c.r.w / 2, ccy = c.r.y + c.r.h / 2;
        float primary, ortho, overlap;
        if (dx) {
            primary = dx > 0 ? c.r.x - (f->r.x + f->r.w) : f->r.x - (c.r.x + c.r.w);
            if ((dx > 0 && ccx <= fcx + 1) || (dx < 0 && ccx >= fcx - 1)) continue;
            overlap = std::min(f->r.y + f->r.h, c.r.y + c.r.h) - std::max(f->r.y, c.r.y);
            ortho = overlap > 0 ? 0 : std::fabs(ccy - fcy);
        } else {
            primary = dy > 0 ? c.r.y - (f->r.y + f->r.h) : f->r.y - (c.r.y + c.r.h);
            if ((dy > 0 && ccy <= fcy + 1) || (dy < 0 && ccy >= fcy - 1)) continue;
            overlap = std::min(f->r.x + f->r.w, c.r.x + c.r.w) - std::max(f->r.x, c.r.x);
            ortho = overlap > 0 ? std::fabs(ccx - fcx) * 0.05f : std::fabs(ccx - fcx);
        }
        float score = std::max(primary, -8.f) + ortho * 2.5f;
        if (c.region != f->region) score += 40;   // stay within a region when there is a choice
        if (score < bestScore) { bestScore = score; best = &c; }
    }
    if (!best) {
        // Nothing further in that direction inside a scrolled list: nudge the list.
        if (dy && f->area) { f->area->target += dy * 120; f->area->clamp(); }
        return;
    }
    // Entering another region: return to where focus was last time. Not for the title bar: it is one row, and
    // remembering the search field there made the sidebar button left of it unreachable from the sidebar.
    if (best->region != f->region && best->region != Region::Title) {
        auto mem = regionMemory_.find(int(best->region));
        if (mem != regionMemory_.end() && find(prev_, mem->second)) {
            const Item* m = find(prev_, mem->second);
            if (intersect(m->r, m->clip).w > 0) best = m;
        }
    }
    focus_ = best->id;
    scrollToFocus_ = true;
}

void Ui::activate() {
    padMode_ = true;
    // A on a slider picks it up (or puts it down) instead of activating it.
    if (const Item* it = find(prev_, focus_); it && (it->flags & Slider)) {
        engaged_ = engaged_ == focus_ ? 0 : focus_;
        return;
    }
    if (focus_) activateNext_ = focus_;
}

void Ui::scrollFocusedArea(float dy) {
    const Item* f = find(prev_, focus_);
    ScrollArea* a = f && f->area ? f->area : (areasPrev_.empty() ? nullptr : areasPrev_.front());
    if (!a) return;
    a->target += dy;
    a->clamp();
    a->manual = clock_;
}

void Ui::touchDown(float x, float y) {
    padMode_ = false;
    touching_ = true;
    scrolling_ = false;
    downX_ = x; downY_ = y; lastY_ = y;
    press_ = 0;
    touchArea_ = nullptr;
    for (auto it = prev_.rbegin(); it != prev_.rend(); ++it) {
        if (!contains(it->r, x, y) || !contains(it->clip, x, y)) continue;
        if (it->flags & Slider) { drag_ = it->id; dragX_ = x; focus_ = it->id; return; }
        press_ = it->id;
        break;
    }
    for (ScrollArea* a : areasPrev_)
        if (contains(a->view, x, y)) { touchArea_ = a; a->velocity = 0; a->target = a->y; break; }
}

void Ui::touchMove(float x, float y) {
    if (!touching_) return;
    if (drag_) { dragX_ = x; return; }
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
    if (drag_) { releasedNext_ = drag_; dragX_ = x; drag_ = 0; return; }
    if (press_ && !scrolling_) {
        focus_ = press_;
        activateNext_ = press_;
    }
    press_ = 0;
    (void)y;
}
