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
    Slider2D = 4,      // with Slider: ↑/↓ change it too (a colour square), and a drag reports y as well
    Hold = 8,          // a short press (A or a tap) activates it on release; held for half a second it is a long press (longPressed)
    NoSideEntry = 16,  // the D-pad's Left/Right never land on it, only Up/Down (the player bar's seek bar, under the controls)
    NoUpDownEntry = 32,   // … Up/Down never land on it, only Left/Right (a playlist's join buttons, between its rows)
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
    /** Where the D-pad goes from an item in one direction, when the layout would pick wrong (this frame; 0: nowhere). */
    void link(Id from, int dx, int dy, Id to) { linksCur_.push_back({ from, int8_t(dx), int8_t(dy), to }); }
    bool focused(Id id) const { return focus_ == id; }
    bool pressed(Id id) const { return press_ == id || (holdPad_ && holding_ == id); }
    /** A Hold item held down for half a second: true for that one frame (its short press then never comes). */
    bool longPressed(Id id) const { return long_ && long_ == id; }
    /** Any other item a finger held still on for half a second: its id for that one frame, focused (the app opens what
     *  + would; the tap then never comes). 0 otherwise. */
    Id touchHeld() const { return touchHold_; }
    /** Every frame, whether A is held: a Hold item pressed with A activates when it is let go. */
    void padHeld(bool a);
    /** For a focused slider: −1/+1 from ←/→ this frame. */
    int sliderStep(Id id) const { return focus_ == id ? sliderStep_ : 0; }
    /** For a slider being dragged by touch: the finger's x, else −1. */
    float sliderDrag(Id id) const { return drag_ == id ? dragX_ : -1; }
    /** True once when a touch drag of this slider ends. */
    bool sliderReleased(Id id) const { return released_ == id; }
    float lastDragX() const { return dragX_; }
    /** A Slider2D's other axis: −1/+1 from ↑/↓ this frame, and the finger's y while it is dragged (else −1). */
    int sliderStepY(Id id) const { return focus_ == id ? sliderStepY_ : 0; }
    float sliderDragY(Id id) const { return drag_ == id ? dragY_ : -1; }

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
    /** As rectOf, and also an item drawn under a modal (not focusable then); its scroll area if area is given. */
    SDL_FRect rectAny(Id id, ScrollArea** area = nullptr) const;
    float radiusOf(Id id) const;

    // ---- input ----
    void navigate(int dx, int dy);
    void activate();
    void slide(int dir) { sliderStep_ = dir; padMode_ = true; }
    void slideY(int dir) { sliderStepY_ = dir; padMode_ = true; }
    /** True while the focused item is a Slider2D picked up with A: ↑/↓ go to it as well. */
    bool focusIsSlider2D() const;
    /** True while the focused item is a slider picked up with A: ←/→ go to it. */
    bool focusIsSlider() const;
    bool engaged(Id id) const { return engaged_ && engaged_ == id; }
    bool anyEngaged() const { return engaged_ != 0; }
    void disengage() { engaged_ = 0; }
    /** Picks a slider up as A would (the equalizer's bands hand it on with ←/→). */
    void engage(Id id) { engaged_ = id; }
    /** This frame's ←/→ is used up (a band handed it on: the next band, drawn after it, must not take it too). */
    void useSlide() { sliderStep_ = 0; }
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
    static bool shown(const Item& it);
    struct Link { Id from; int8_t dx, dy; Id to; };
    std::vector<Link> linksPrev_, linksCur_;
    std::vector<Item> shadowPrev_, shadowCur_;   // items under a modal: where they are, for rectAny only
    std::vector<ScrollArea*> glideCur_;          // ... and their lists, which still ease to a target
    /** blocked: a link said "nowhere" (the list is not nudged either). */
    const Item* pick(const std::vector<Item>& items, const std::vector<Link>& links, const Item* f, int dx, int dy, bool* blocked = nullptr) const;

public:
#ifndef __SWITCH__
    void audit(const char* label, FILE* out) const;   // the preview's D-pad audit (GF_SCRIPT "audit <label>")
#endif

private:

    std::vector<Item> prev_, cur_;
    std::vector<ScrollArea*> areasPrev_, areasCur_;
    Region region_ = Region::Main;
    ScrollArea* area_ = nullptr;

    Id focus_ = 0, activate_ = 0, activateNext_ = 0, engaged_ = 0;
    Id press_ = 0, drag_ = 0, released_ = 0, releasedNext_ = 0;
    Id holding_ = 0, long_ = 0;   // a Hold item being pressed (by A when holdPad_, else a finger), and its long press this frame
    float holdT_ = 0;
    bool holdPad_ = false, holdFired_ = false;
    std::unordered_map<int, Id> regionMemory_;
    bool scrollToFocus_ = false, wantDefault_ = false, wantDefaultNext_ = false, padMode_ = true, modal_ = false;
    Region fallbackRegion() const { return modal_ ? Region::Modal : Region::Main; }
    int sliderStep_ = 0, sliderStepY_ = 0;
    float dt_ = 0;
    double clock_ = 0;

    // touch
    bool touching_ = false, scrolling_ = false;
    float downX_ = 0, downY_ = 0, lastY_ = 0, dragX_ = -1, dragY_ = -1;
    ScrollArea* touchArea_ = nullptr;
    Id touchHold_ = 0;              // touchHeld(): this frame's long touch
    float pressT_ = 0;              // how long the finger has been still on press_
    bool pressFired_ = false;       // ... and its long touch came: the release taps nothing
};
