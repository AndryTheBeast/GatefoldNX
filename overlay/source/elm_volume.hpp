#pragma once

#include <tesla.hpp>
#include <functional>
#include <algorithm>

// =============================================================================
// VolumeTrackBar
//
// Extends TrackBar with two behaviours:
//
//  1. Mute visualisation — when volume == 0, draws the speaker glyph
//     left-half only (via scissor) plus a cross glyph in the right half,
//     giving a clear muted state without any overdraw artefacts.
//
//  2. Icon tap → mute/unmute — tapping the speaker icon fires a callback
//     (set via setIconTapCallback) so the caller can toggle mute and
//     preserve the pre-mute volume level.
//
//  3. setLabel() — lets update() relabel the slider live when the running
//     title ID changes without rebuilding the whole list.
// =============================================================================

class VolumeTrackBar final : public tsl::elm::TrackBar {
public:
    using TrackBar::TrackBar;

    // -----------------------------------------------------------------------
    // draw — muted state shows half-speaker + cross instead of full icon.
    // -----------------------------------------------------------------------
    void draw(tsl::gfx::Renderer *renderer) override {
        if (this->m_value > 0 || this->m_icon == nullptr || this->m_icon[0] == '\0') {
            TrackBar::draw(renderer);
            return;
        }

        // --- muted path ---
        constexpr s32 kIconSize  = 30;
        constexpr s32 kIconX     = 42;   // TrackBar hardcodes getX()+42
        constexpr s32 kIconBaseY = 54;   // TrackBar hardcodes getY()+54 as baseline

        const s32 iconX    = this->getX() + kIconX;
        const s32 iconBase = this->getY() + kIconBaseY;

        // Measure glyph once.
        const auto [gw, gh] = renderer->getTextDimensions("\uE13C", false, kIconSize);
        const s32 glyphW   = static_cast<s32>(gw);
        const s32 glyphH   = static_cast<s32>(gh);
        const s32 glyphTop = iconBase - glyphH;

        // 1. Draw everything EXCEPT the icon by temporarily clearing m_icon.
        const char *savedIcon = this->m_icon;
        this->m_icon = " ";
        TrackBar::draw(renderer);
        this->m_icon = savedIcon;

        // 2. Draw only the LEFT half of the speaker glyph under a scissor so
        //    the right half is geometrically never written — no background rect,
        //    no overdraw artefacts.
        const s32 scissorX = iconX;
        const s32 scissorY = std::max(glyphTop,
                                      static_cast<s32>(ult::activeHeaderHeight));
        const s32 scissorW = glyphW / 2;
        const s32 scissorH = std::min(glyphTop + glyphH,
                                      static_cast<s32>(tsl::cfg::FramebufferHeight) - 73)
                             - scissorY;

        renderer->enableScissoring(scissorX, scissorY, scissorW, scissorH);
        renderer->drawString("\uE13C", false, iconX, iconBase, kIconSize,
                             a(tsl::style::color::ColorText));
        renderer->disableScissoring();

        // 3. Cross centred in the right-half region — drawn outside any scissor.
        const s32 rightX    = iconX + glyphW / 2;
        const s32 rightW    = glyphW - glyphW / 2;
        const s32 crossSize = std::max(8, glyphH * 45 / 100);
        const auto [cw, ch] = renderer->getTextDimensions("\uE14C", false, crossSize);
        const s32 crossX    = rightX + (rightW - static_cast<s32>(cw)) / 2;
        const s32 crossY    = (iconBase - glyphH / 2) + static_cast<s32>(ch) / 2 + 2;

        renderer->drawString("\uE14C", false, crossX, crossY, crossSize,
                             a(tsl::style::color::ColorText));
    }

    // -----------------------------------------------------------------------
    // onTouch — tapping the speaker icon fires m_iconTapCallback.
    // The icon is drawn at getX()+42, baseline getY()+54, fontSize 30.
    // A generous hit area is used so the tap is easy to land.
    // -----------------------------------------------------------------------
    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (m_iconTapCallback) {
            const bool inIcon =
                (initialX >= this->getX() + 30 && initialX <= this->getX() + 75 &&
                 initialY >= this->getY() + 22  && initialY <= this->getY() + 65);
            if (inIcon) {
                if (event == tsl::elm::TouchEvent::Touch) {
                    touchInSliderBounds = true;
                    triggerNavigationFeedback();
                } else if (event == tsl::elm::TouchEvent::Release) {
                    touchInSliderBounds = false;
                    const bool stillInIcon =
                        (currX >= this->getX() + 20 && currX <= this->getX() + 85 &&
                         currY >= this->getY() + 12  && currY <= this->getY() + 75);
                    if (stillInIcon) {
                        m_iconTapCallback();
                        tsl::shiftItemFocus(this);
                        triggerNavigationFeedback();
                    }
                }
                return true; // consume so the slider doesn't also react
            }
        }
        return TrackBar::onTouch(event, currX, currY, prevX, prevY, initialX, initialY);
    }

    // -----------------------------------------------------------------------
    // setIconTapCallback — called when the speaker icon is tapped.
    // -----------------------------------------------------------------------
    void setIconTapCallback(std::function<void()> cb) {
        m_iconTapCallback = std::move(cb);
    }

    // -----------------------------------------------------------------------
    // setLabel — lets update() relabel the slider live when the running
    // title ID changes without rebuilding the whole settings list.
    // -----------------------------------------------------------------------
    void setLabel(const std::string& label) { m_label = label; }

private:
    std::function<void()> m_iconTapCallback;
};