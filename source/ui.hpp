/*
 * Gatefold NX · ui.hpp
 * Immediate-mode focus and touch handling, so Gatefold's mouse-first interface
 * works with a controller and the touch screen alike.
 *
 * Every frame, widgets register a rectangle with a stable id. The D-pad moves
 * focus spatially among last frame's rectangles; A or a tap activates one,
 * which the widget sees as `true` from item() on the next frame.
 *
 * Sliders (volume, the seek bars) are picked up with A before ←/→ move them, and put down with
 * A, B or moving away, so passing over one with the D-pad never changes it. The left stick
 * drives a pointer, which taps with A like a finger (App::input).
 */
#pragma once
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using Id = uint64_t;
Id uid(const char* s, int64_t i = 0);
Id uid(const std::string& s, int64_t i = 0);

enum class Region : uint8_t { Title, Side, Main, Bar, Overlay, Modal };

enum ItemFlags : uint8_t {
    Slider = 1,        // once picked up with A, ←/→ change its value instead of moving focus; touch drags it
    DefaultFocus = 2,  // where focus lands when a view opens
};

struct ScrollArea {
    SDL_FRect view{};
    float y = 0, target = 0, content = 0, velocity = 0;
    double manual = -100;   // Ui::now() when it was last scrolled by hand (touch, right stick, D-pad): followers wait a while
    float max() const { return std::max(0.f, content - view.h); }
    void clamp() { target = std::min(std::max(target, 0.f), max()); y = std::min(std::max(y, 0.f), max()); }
};

class Ui {
public:
    // ---- frame ----
    void beginFrame(float dt);
    /** Seconds since start, on the frame clock (ScrollArea::manual). */
    double now() const { return clock_; }
    void endFrame();   // scrolls the focused item into view

    // ---- registration ----
    void region(Region r) { region_ = r; }
    /** Registered here, not only through its items: a pane of plain text (About) must still scroll by touch and stick. */
    /** While a modal sheet is open, only its items (Region::Modal) register: focus and touch stay inside it. */
    void setModal(bool on) { modal_ = on; }
    bool modal() const { return modal_; }
    void pushArea(ScrollArea* a) {
        area_ = a;
        if (onArea) onArea(a);
        if (modal_ && region_ != Region::Modal) return;
        if (std::find(areasCur_.begin(), areasCur_.end(), a) == areasCur_.end()) areasCur_.push_back(a);
    }
    void popArea() { area_ = nullptr; if (onArea) onArea(nullptr); }
    /** Told when a scroll area starts and ends (nullptr): the app shifts text by its scroll, sub-pixel (Gfx::setShift). */
    std::function<void(const ScrollArea*)> onArea;
    /** Registers an interactive rectangle; true when it was activated. */
    bool item(Id id, SDL_FRect r, float radius = 8, uint8_t flags = 0);
    bool focused(Id id) const { return focus_ == id; }
    bool pressed(Id id) const { return press_ == id; }
    /** For a focused slider: −1/+1 from ←/→ this frame. */
    int sliderStep(Id id) const { return focus_ == id ? sliderStep_ : 0; }
    /** For a slider being dragged by touch: the finger's x, else −1. */
    float sliderDrag(Id id) const { return drag_ == id ? dragX_ : -1; }
    /** True once when a touch drag of this slider ends. */
    bool sliderReleased(Id id) const { return released_ == id; }
    float lastDragX() const { return dragX_; }

    void focus(Id id) { focus_ = id; scrollToFocus_ = true; wantDefault_ = false; }
    /** Focus that follows the pointer: no scrolling to it, the pointer is already there. */
    void point(Id id) { if (id != focus_) { focus_ = id; padMode_ = true; } }
    Id focusedId() const { return focus_; }
    /** Moves focus to the DefaultFocus item of the next frame. Asked for mid-frame (a page switching while it
     *  draws), it must not be settled by this frame's end, when the new page's items do not exist yet. */
    void focusDefault() { wantDefaultNext_ = true; }
    bool showFocusRing() const { return padMode_; }
    bool hasItem(Id id) const;
    SDL_FRect rectOf(Id id) const;
    float radiusOf(Id id) const;

    // ---- input ----
    void navigate(int dx, int dy);
    void activate();
    void slide(int dir) { sliderStep_ = dir; padMode_ = true; }
    /** True while the focused item is a slider picked up with A: ←/→ go to it. */
    bool focusIsSlider() const;
    bool engaged(Id id) const { return engaged_ && engaged_ == id; }
    bool anyEngaged() const { return engaged_ != 0; }
    void disengage() { engaged_ = 0; }
    /** The topmost visible item of a scroll area (last frame's), 0 when none. */
    Id firstIn(const ScrollArea* a) const;
    /** The item under a point (the stick's pointer), 0 when none. */
    Id itemAt(float x, float y) const;
    void touchDown(float x, float y);
    void touchMove(float x, float y);
    void touchUp(float x, float y);
    void scrollFocusedArea(float dy);
    void setPadMode(bool on) { padMode_ = on; }

private:
    struct Item {
        Id id;
        SDL_FRect r;
        SDL_FRect clip;
        float radius;
        uint8_t flags;
        Region region;
        ScrollArea* area;
    };
    const Item* find(const std::vector<Item>& v, Id id) const;

    std::vector<Item> prev_, cur_;
    std::vector<ScrollArea*> areasPrev_, areasCur_;
    Region region_ = Region::Main;
    ScrollArea* area_ = nullptr;

    Id focus_ = 0, activate_ = 0, activateNext_ = 0, engaged_ = 0;
    Id press_ = 0, drag_ = 0, released_ = 0, releasedNext_ = 0;
    std::unordered_map<int, Id> regionMemory_;
    bool scrollToFocus_ = false, wantDefault_ = false, wantDefaultNext_ = false, padMode_ = true, modal_ = false;
    Region fallbackRegion() const { return modal_ ? Region::Modal : Region::Main; }
    int sliderStep_ = 0;
    float dt_ = 0;
    double clock_ = 0;

    // touch
    bool touching_ = false, scrolling_ = false;
    float downX_ = 0, downY_ = 0, lastY_ = 0, dragX_ = -1;
    ScrollArea* touchArea_ = nullptr;
};
