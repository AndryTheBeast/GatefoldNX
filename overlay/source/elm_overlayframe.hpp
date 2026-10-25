#pragma once

#include <tesla.hpp>
#include <atomic>
#include <optional>
#include <string>

/**
 * @brief Base frame for sys-tune overlay pages.
 *
 * Content pointer is NON-OWNING — MainGui owns both page lists and
 * deletes them in its destructor.
 *
 * NO mutex: Tesla's entire UI path (draw / layout / requestFocus /
 * onTouch / handleInput / switchToPage) is single-threaded.
 * A std::mutex here caused a deadlock because layout() → invalidate()
 * propagates up the element tree back into layout() on the same thread.
 */
class SysTuneOverlayFrame final : public tsl::elm::Element {
public:
    explicit SysTuneOverlayFrame(std::string pageLeftName  = "",
                                 std::string pageRightName = "")
        : tsl::elm::Element()
        , m_pageLeftName(std::move(pageLeftName))
        , m_pageRightName(std::move(pageRightName))
    {
        ult::activeHeaderHeight = 97;
        ult::loadWallpaperFileWhenSafe();
        m_isItem = false;
        disableSound.store(false, std::memory_order_release);
    }

    ~SysTuneOverlayFrame() override = default;

    // -----------------------------------------------------------------------
    void draw(tsl::gfx::Renderer *renderer) override {
        if (ult::expandedMemory && !ult::refreshWallpaper.load(std::memory_order_acquire) &&
            !ult::wallpaperData.empty() && ult::correctFrameSize)
            renderer->drawWallpaper();
        else
            renderer->fillScreen(a(tsl::defaultBackgroundColor));

#if USING_WIDGET_DIRECTIVE
        renderer->drawWidget();
#endif

        // --- Title ---
        calcScrollWidth(renderer, m_titleScroll, TITLE, 32, false);
        drawScrollableText(renderer, m_titleScroll, tsl::defaultOverlayColor, 20, 50, 32, 27, 35);

        // --- Subtitle ---
        calcScrollWidth(renderer, m_subScroll, VERSION, 15, false);
        {
            constexpr int subX = 20, subY = 75;
            if (m_subScroll.trunc) {
                if (!m_subScroll.active) { m_subScroll.active = true; m_subScroll.timeIn = ult::nowNs(); }
                renderer->enableScissoring(subX, subY - 16, m_subScroll.maxW, 24);
                renderer->drawString(m_subScroll.scrollText.c_str(), false,
                    subX - static_cast<s32>(m_subScroll.offset), subY, 15, tsl::bannerVersionTextColor);
                renderer->disableScissoring();
                updateScroll(m_subScroll);
            } else {
                renderer->drawString(VERSION, false, subX, subY, 15, tsl::bannerVersionTextColor);
            }
        }

        // --- Bottom separator ---
        renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73,
                           tsl::cfg::FramebufferWidth - 30, 1, a(tsl::bottomSeparatorColor));

        // --- Width calculations ---
        const auto updateAtomic = [](std::atomic<float>& atom, float val) {
            if (val != atom.load(std::memory_order_acquire))
                atom.store(val, std::memory_order_release);
        };

        const float gapWidth      = renderer->getTextDimensions(ult::GAP_1,                          false, 23).first;
        const float backTextWidth = renderer->getTextDimensions("\uE0E1" + ult::GAP_2 + ult::BACK,   false, 23).first;
        const float selTextWidth  = renderer->getTextDimensions("\uE0E0" + ult::GAP_2 + ult::OK,     false, 23).first;

        const float _halfGap     = gapWidth * 0.5f;
        const float _backWidth   = backTextWidth + gapWidth;
        const float _selectWidth = selTextWidth  + gapWidth;

        updateAtomic(ult::halfGap,     _halfGap);
        updateAtomic(ult::backWidth,   _backWidth);
        updateAtomic(ult::selectWidth, _selectWidth);

        static constexpr float buttonStartX = 30.f;
        const float buttonY = static_cast<float>(tsl::cfg::FramebufferHeight - 73 + 1);

        // Translate the page-left/page-right names individually, up front \u2014
        // same as tsl::elm::OverlayFrame. Once concatenated with icons/gaps
        // into the composed footer string below, that composite can never
        // match a translation cache entry as a whole, so the lookup has to
        // happen here on each raw piece before it's merged in.
        std::string translatedPageLeftName  = m_pageLeftName;
        std::string translatedPageRightName = m_pageRightName;
        if (!m_pageLeftName.empty() || !m_pageRightName.empty()) {
            std::shared_lock<std::shared_mutex> pageNameReadLock(tsl::gfx::s_translationCacheMutex);
            if (!m_pageLeftName.empty()) {
                const auto leftIt = ult::translationCache.find(m_pageLeftName);
                if (leftIt != ult::translationCache.end())
                    translatedPageLeftName = leftIt->second;
            }
            if (!m_pageRightName.empty()) {
                const auto rightIt = ult::translationCache.find(m_pageRightName);
                if (rightIt != ult::translationCache.end())
                    translatedPageRightName = rightIt->second;
            }
        }

        // --- Page navigation ---
        const bool hasNextPage = !m_pageLeftName.empty() || !m_pageRightName.empty();
        if (hasNextPage != ult::hasNextPageButton.load(std::memory_order_acquire))
            ult::hasNextPageButton.store(hasNextPage, std::memory_order_release);

        if (hasNextPage) {
            const std::string pageLabel = !translatedPageLeftName.empty()
                ? ("\uE0ED" + ult::GAP_2 + translatedPageLeftName)
                : ("\uE0EE" + ult::GAP_2 + translatedPageRightName);

            const float _nextPageWidth = renderer->getTextDimensions(pageLabel, false, 23).first + gapWidth;
            updateAtomic(ult::nextPageWidth, _nextPageWidth);

            if (ult::touchingNextPage.load(std::memory_order_acquire)) {
                const float nextX = buttonStartX + 2.f - _halfGap + _backWidth + 1.f + _selectWidth;
                renderer->drawRoundedRect(nextX, buttonY, _nextPageWidth - 2.f, 73.0f, 12.0f, a(tsl::clickColor));
            }
        } else {
            ult::nextPageWidth.store(0.0f, std::memory_order_release);
        }

        // --- Touch highlights ---
        if (ult::touchingBack)
            renderer->drawRoundedRect(buttonStartX + 2.f - _halfGap, buttonY, _backWidth - 1.f, 73.0f, 12.0f, a(tsl::clickColor));
        if (ult::touchingSelect.load(std::memory_order_acquire))
            renderer->drawRoundedRect(buttonStartX + 2.f - _halfGap + _backWidth + 1.f, buttonY, _selectWidth - 2.f, 73.0f, 12.0f, a(tsl::clickColor));

        // --- Footer text ---
        const std::string currentBottomLine =
            "\uE0E1" + ult::GAP_2 + ult::BACK  + ult::GAP_1 +
            "\uE0E0" + ult::GAP_2 + ult::OK    + ult::GAP_1 +
            (!translatedPageLeftName.empty()  ? "\uE0ED" + ult::GAP_2 + translatedPageLeftName  + ult::GAP_1 :
             !translatedPageRightName.empty() ? "\uE0EE" + ult::GAP_2 + translatedPageRightName + ult::GAP_1 : "");

        renderer->drawStringWithColoredSections(currentBottomLine, false, tsl::s_footerSpecialChars,
            buttonStartX, 693, 23, tsl::bottomTextColor, tsl::buttonColor);

        if (!selectIsUsingFocusedColor) {
            static const std::string okOverdraw = "\uE0E0" + ult::GAP_2 + ult::OK + ult::GAP_1;
            renderer->drawStringWithColoredSections(okOverdraw, false, tsl::s_footerSpecialChars,
                buttonStartX + _backWidth, 693, 23, tsl::unfocusedColor, tsl::unfocusedColor);
        }

        // --- Content ---
        if (m_contentElement != nullptr)
            m_contentElement->frame(renderer);

        // --- Edge separator ---
        if (!ult::useRightAlignment)
            renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
        else
            renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));

        // --- Toast ---
        //if (m_toast) {
        //    const s32 toastW = tsl::cfg::FramebufferWidth - 20;
        //    const s32 toastH = 110;
        //    const s32 toastX = 10;
        //    s32 startY = tsl::cfg::FramebufferHeight - toastH;
        //    
        //    constexpr u32 fadeDuration = 10;
        //    if (m_toast->Current < fadeDuration)
        //        startY += toastH - static_cast<s32>(toastH / fadeDuration) * static_cast<s32>(m_toast->Current);
        //    if (m_toast->Duration - m_toast->Current < fadeDuration)
        //        startY += toastH - static_cast<s32>(toastH / fadeDuration) * static_cast<s32>(m_toast->Duration - m_toast->Current);
        //    
        //    renderer->drawRect(toastX,     startY,     toastW,     toastH,     a(tsl::bottomTextColor));
        //    renderer->drawRect(toastX + 3, startY + 3, toastW - 6, toastH - 6, a(tsl::defaultBackgroundColor));
        //    renderer->drawString(m_toast->Header.c_str(),  false, toastX + 10, startY + 40, 26, tsl::bottomTextColor);
        //    renderer->drawString(m_toast->Content.c_str(), false, toastX + 10, startY + 80, 23, tsl::bottomTextColor, toastW - 20);
        //    
        //    if (++m_toast->Current >= m_toast->Duration)
        //        m_toast = std::nullopt;
        //}
    }

    // -----------------------------------------------------------------------
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        setBoundaries(parentX, parentY, parentWidth, parentHeight);
        if (m_contentElement != nullptr) {
            m_contentElement->setBoundaries(parentX + 35, parentY + 97, parentWidth - 85, parentHeight - 73 - 105);
            m_contentElement->invalidate();
        }
    }

    tsl::elm::Element* requestFocus(tsl::elm::Element *oldFocus, tsl::FocusDirection direction) override {
        return m_contentElement ? m_contentElement->requestFocus(oldFocus, direction) : nullptr;
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (!m_contentElement || !m_contentElement->inBounds(currX, currY))
            return false;
        return m_contentElement->onTouch(event, currX, currY, prevX, prevY, initialX, initialY);
    }

    // -----------------------------------------------------------------------
    /**
     * Assign the visible content element — NON-OWNING.
     * MainGui owns both lists and deletes them in its destructor.
     * The previous pointer is overwritten, not deleted.
     */
    void setContent(tsl::elm::Element *content) {
        m_contentElement = content;
        if (content != nullptr) {
            m_contentElement->setParent(this);
            invalidate();
        }
    }

    /** Update the footer page labels. Empty string = hide that button. */
    void setPageNames(std::string left, std::string right) {
        m_pageLeftName  = std::move(left);
        m_pageRightName = std::move(right);
    }

    // -----------------------------------------------------------------------
    //struct Toast { std::string Header, Content; u32 Duration, Current; };
    //
    //void setToast(std::string header, std::string content) {
    //    m_toast = Toast{ std::move(header), std::move(content), 150, 0 };
    //}

private:
    // -----------------------------------------------------------------------
    struct ScrollState {
        u64   timeIn = 0, lastUpd = 0;
        float offset = 0.f;
        u32   maxW = 0, textW = 0;
        bool  active = false, trunc = false;
        std::string scrollText;
    };

    static constexpr const char *TITLE = "Gatefold \u266B";

    void calcScrollWidth(tsl::gfx::Renderer *renderer, ScrollState &s,
                         const char *text, u32 fontSize, bool widgetDrawn) {
        if (s.maxW) return;
        s.maxW = widgetDrawn ? 217u : static_cast<u32>(tsl::cfg::FramebufferWidth - 40);
        const u32 w = renderer->getTextDimensions(text, false, fontSize).first;
        s.trunc = w > s.maxW;
        if (s.trunc) {
            s.scrollText = std::string(text) + "        ";
            s.textW = renderer->getTextDimensions(s.scrollText.c_str(), false, fontSize).first;
            s.scrollText += text;
        } else {
            s.textW = w;
        }
    }

    void drawScrollableText(tsl::gfx::Renderer *renderer, ScrollState &s, tsl::Color clr,
                            int xPos, int yPos, u32 fontSize, int scissorYOff, int scissorH) {
        if (s.trunc) {
            if (!s.active) { s.active = true; s.timeIn = ult::nowNs(); }
            renderer->enableScissoring(xPos, yPos - scissorYOff, s.maxW, scissorH);
            renderer->drawString(s.scrollText.c_str(), false, xPos - static_cast<s32>(s.offset), yPos, fontSize, clr);
            renderer->disableScissoring();
            updateScroll(s);
        } else {
            renderer->drawString(TITLE, false, xPos, yPos, fontSize, clr);
        }
    }

    void updateScroll(ScrollState &s) {
        const u64 now = ult::nowNs();
        if (now - s.lastUpd < 8'333'333ULL) return;

        static constexpr double delay = 3.0, pause = 2.0, vel = 100.0, accel = 0.5, decel = 0.5;
        static constexpr double invBil = 1e-9, invAccel = 2.0, invDecel = 2.0;

        const double minDist   = s.textW;
        const double accelDist = 0.5 * vel * accel;
        const double constDist = std::max(0.0, minDist - accelDist - 0.5 * vel * decel);
        const double constTime = constDist / vel;
        const double totalDur  = delay + accel + constTime + decel + pause;
        const double t         = (now - s.timeIn) * invBil;
        const double cycle     = std::fmod(t, totalDur);

        if (cycle < delay) {
            s.offset = 0.f;
        } else if (cycle < delay + accel + constTime + decel) {
            const double st = cycle - delay;
            double d;
            if (st <= accel) {
                const double r = st * invAccel; d = r * r * accelDist;
            } else if (st <= accel + constTime) {
                d = accelDist + (st - accel) * vel;
            } else {
                const double r = (st - accel - constTime) * invDecel;
                const double omr = 1.0 - r;
                d = accelDist + constDist + (1.0 - omr * omr) * (minDist - accelDist - constDist);
            }
            s.offset = static_cast<float>(std::min(d, minDist));
        } else {
            s.offset = static_cast<float>(s.textW);
        }

        s.lastUpd = now;
        if (t >= totalDur) s.timeIn = now;
    }

    // -----------------------------------------------------------------------
    tsl::elm::Element *m_contentElement = nullptr; ///< Non-owning.
    std::string        m_pageLeftName;
    std::string        m_pageRightName;
    ScrollState        m_titleScroll;
    ScrollState        m_subScroll;
    //std::optional<Toast> m_toast;
};