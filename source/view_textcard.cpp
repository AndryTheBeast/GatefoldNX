/*
 * Songate · view_textcard.cpp
 * Typing where there is no system keyboard to hand over to (a PC): a card of the app's own, with paste and the system's input
 * methods. Like the console's keyboard it holds the app until it is answered, because the eighteen places that ask for text
 * are in the middle of a frame: the card is drawn over the last finished frame, and Ui is not touched meanwhile.
 */
#ifdef GF_WINDOWED
#include "app.hpp"
#include "icons.inc"

namespace {

App* owner = nullptr;

/** Where the code point before or after byte i starts. */
size_t stepBack(const std::string& s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
    return i;
}
size_t stepOn(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    i++;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
    return i;
}
/** Ctrl+arrow: to the next word's start. */
size_t wordBack(const std::string& s, size_t i) {
    while (i > 0 && s[i - 1] == ' ') i--;
    while (i > 0 && s[i - 1] != ' ') i--;
    return i;
}
size_t wordOn(const std::string& s, size_t i) {
    while (i < s.size() && s[i] != ' ') i++;
    while (i < s.size() && s[i] == ' ') i++;
    return i;
}

}  // namespace

plat::TextResult App::textCardHook(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    return owner ? owner->textCard(header, guide, initial, out, size) : plat::TextResult::Unavailable;
}

void App::textCardOwner() { owner = this; plat::setTextCard(&App::textCardHook); }

plat::TextResult App::textCard(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    // The frame under way stays as it is in its texture; the last finished one is what shows behind the card
    SDL_Rect clipWas{};
    const bool clipped = SDL_RenderIsClipEnabled(renderer_);
    if (clipped) SDL_RenderGetClipRect(renderer_, &clipWas);
    const float fadeWas = g.fade();
    SDL_Texture* under = frameTex_[frameCur_ ^ 1];
    SDL_Texture* onto = frameTex_[frameCur_];
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_RenderSetScale(renderer_, scale_, scale_);
    SDL_RenderSetClipRect(renderer_, nullptr);
    g.setFade(1);

    std::string text = initial ? initial : "", composing;
    size_t caret = text.size(), anchor = 0;   // all of it chosen at first: typing replaces what was there
    bool dragging = false;
    float scroll = 0;
    const Font titleF{ css::Lg, Weight::Bold }, fieldF{ css::Md }, buttonF{ css::Base, Weight::SemiBold };
    const float w = std::min(620.f, css::W - 40.f), pad = 24, fieldH = 46, buttonH = 36;
    const float h = pad + 26 + 14 + fieldH + 18 + buttonH + pad;
    const float x = (css::W - w) / 2, textX = x + pad + 14, textW = w - pad * 2 - 28, okW = 92, cancelW = 92;
    float y = 0;
    SDL_FRect field{}, ok{}, cancel{};
    /** room: how much of the window's height can be seen (the on-screen keyboard takes the rest). */
    auto place = [&](float room) {
        y = std::max(8.f, std::min(std::max(40.f, css::H * 0.28f - h / 2), (room - h) / 2));
        field = { x + pad, y + pad + 26 + 14, w - pad * 2, fieldH };
        const float by = field.y + fieldH + 18;
        ok = { x + w - pad - okW, by, okW, buttonH };
        cancel = { ok.x - 8 - cancelW, by, cancelW, buttonH };
    };
    place(float(css::H));
    const Uint32 opened = SDL_GetTicks();
    uint64_t padBefore = ~0ull;

    SDL_Rect imeAt{ int(textX * scale_), int(field.y * scale_), int(textW * scale_), int(fieldH * scale_) };
    SDL_SetTextInputRect(&imeAt);
#ifdef GF_DEV
    if (const char* items = getenv("GF_ITEMS"))   // tests: what shows now is this card
        if (FILE* f = fopen(items, "w")) { fprintf(f, "=== scale %.3f size %d %d: 0 items, focus textcard\n", double(scale_), css::W, css::H); fclose(f); }
#endif
    SDL_StartTextInput();
#ifdef GF_DEV
    if (getenv("GF_TOUCHKB")) typingByTouch_ = true;   // tests
#endif
    if (typingByTouch_) plat::showTouchKeyboard(true);   // opened with a keyboard or a mouse: it would only be in the way

    auto chosen = [&] { return std::pair<size_t, size_t>(std::min(caret, anchor), std::max(caret, anchor)); };
    auto cutChosen = [&] {
        const auto [a, b] = chosen();
        text.erase(a, b - a);
        caret = anchor = a;
    };
    auto insert = [&](const std::string& s) {
        cutChosen();
        const size_t room = size > text.size() + 1 ? size - text.size() - 1 : 0;
        size_t n = std::min(s.size(), room);
        while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;   // not in the middle of a letter
        std::string in = s.substr(0, n);
        for (char& c : in) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        text.insert(caret, in);
        caret = anchor = caret + in.size();
    };
    auto caretAt = [&](float px) {
        size_t best = 0;
        float bestD = 1e9f;
        for (size_t i = 0;; i = stepOn(text, i)) {
            const float d = std::fabs(textX - scroll + g.measureF(text.substr(0, i), fieldF) - px);
            if (d < bestD) { bestD = d; best = i; }
            if (i >= text.size()) break;
        }
        return best;
    };
    auto inside = [](const SDL_FRect& r, float px, float py) { return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; };
    // The window can change under the card (the on-screen keyboard makes room for itself, or the user drags its edge). The frame
    // keeps the window's width, its bottom going out of sight, and the card moves up into what can be seen; App::fitWindow
    // lays out the new size once the card closes
    int seenW = 0, seenH = 0;
    float fit = scale_;
    auto toApp = [&](int px, int py, float& ax, float& ay) {
        ax = px / fit;
        ay = py / fit;
    };

    plat::TextResult answer = plat::TextResult::Cancelled;
    bool open = true;
    while (open) {
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
            if (e.type == SDL_QUIT) { running = false; open = false; }
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) { closePending_ = true; open = false; }
            else if (e.type == SDL_TEXTINPUT) { composing.clear(); insert(e.text.text); }
            else if (e.type == SDL_TEXTEDITING) composing = e.edit.text;
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                float ax, ay;
                toApp(e.button.x, e.button.y, ax, ay);
                if (inside(ok, ax, ay)) { answer = plat::TextResult::Done; open = false; }
                else if (inside(cancel, ax, ay)) open = false;
                else if (inside(field, ax, ay)) {
                    caret = caretAt(ax);
                    if (!(SDL_GetModState() & KMOD_SHIFT)) anchor = caret;
                    dragging = true;
                }
            } else if (e.type == SDL_MOUSEMOTION && dragging) {
                float ax, ay;
                toApp(e.motion.x, e.motion.y, ax, ay);
                caret = caretAt(ax);
            } else if (e.type == SDL_MOUSEBUTTONUP) dragging = false;
            else if (e.type == SDL_KEYDOWN && composing.empty()) {
                const SDL_Keycode k = e.key.keysym.sym;
                const bool ctrl = e.key.keysym.mod & KMOD_CTRL, shift = e.key.keysym.mod & KMOD_SHIFT;
                const auto [a, b] = chosen();
                auto move = [&](size_t to) { caret = to; if (!shift) anchor = to; };
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { answer = plat::TextResult::Done; open = false; }
                else if (k == SDLK_ESCAPE) open = false;
                else if (k == SDLK_LEFT) move(!shift && a != b ? a : ctrl ? wordBack(text, caret) : stepBack(text, caret));
                else if (k == SDLK_RIGHT) move(!shift && a != b ? b : ctrl ? wordOn(text, caret) : stepOn(text, caret));
                else if (k == SDLK_HOME) move(0);
                else if (k == SDLK_END) move(text.size());
                else if (k == SDLK_BACKSPACE) {
                    if (a == b) anchor = ctrl ? wordBack(text, caret) : stepBack(text, caret);
                    cutChosen();
                } else if (k == SDLK_DELETE) {
                    if (a == b) anchor = ctrl ? wordOn(text, caret) : stepOn(text, caret);
                    cutChosen();
                } else if (ctrl && k == SDLK_a) { anchor = 0; caret = text.size(); }
                else if (ctrl && (k == SDLK_c || k == SDLK_x) && a != b) {
                    SDL_SetClipboardText(text.substr(a, b - a).c_str());
                    if (k == SDLK_x) cutChosen();
                } else if (ctrl && k == SDLK_v) {
                    if (char* clip = SDL_GetClipboardText()) { insert(clip); SDL_free(clip); }
                }
            }
        }
        // the tray's Quit and a second launch's Show still work; the rest waits for the card
        for (plat::Cmd c = plat::nextCommand(); c != plat::Cmd::None; c = plat::nextCommand()) {
            if (c == plat::Cmd::Quit) { running = false; open = false; }
            else if (c == plat::Cmd::Show) showWindow();
        }
        plat::padRead();
        const uint64_t pads = plat::padHeld(), down = pads & ~padBefore;
        padBefore = pads;
        if (down & plat::BtnA) { answer = plat::TextResult::Done; open = false; }
        if (down & plat::BtnB) open = false;

        // Keep the caret in the box
        const float caretPx = g.measureF(text.substr(0, caret), fieldF), compW = composing.empty() ? 0 : g.measureF(composing, fieldF);
        scroll = std::clamp(scroll, std::max(0.f, caretPx + compW - textW + 2), std::max(0.f, caretPx));
        scroll = std::min(scroll, std::max(0.f, g.measureF(text, fieldF) + compW - textW + 2));

        const float a = std::clamp((SDL_GetTicks() - opened) / 150.f, 0.f, 1.f), lift = (1 - a) * 10;
        int pw = 1, ph = 1;
        SDL_GetRendererOutputSize(renderer_, &pw, &ph);
        if (pw != seenW || ph != seenH) {
            if (seenW) fitPending_ = true;
            seenW = pw;
            seenH = ph;
            fit = pw / float(css::W);
            SDL_RenderSetScale(renderer_, fit, fit);
            place(ph / fit);
            const SDL_Rect ime{ int(textX * fit), int(field.y * fit), int(textW * fit), int(fieldH * fit) };
            SDL_SetTextInputRect(&ime);
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        const SDL_Rect all{ 0, 0, css::W, css::H };
        const SDL_Rect whole{ 0, 0, framePw_, framePh_ };
        SDL_RenderCopy(renderer_, under, &whole, &all);
        g.rect(0, 0, float(css::W), float(css::H), { 0, 0, 0 }, 0.45f * a);
        g.roundRect(x, y + 10 + lift, w, h, 18, { 0, 0, 0 }, 0.3f * a);
        g.roundRect(x, y + lift, w, h, 18, pal.surface2, a);
        g.roundStroke(x, y + lift, w, h, 18, 1, pal.text, 0.1f * a);
        g.text(header, titleF, x + pad, y + pad + lift, pal.text, a, Align::Left, int(w - pad * 2));

        const float fy = field.y + lift, ty = fy + (fieldH - g.lineHeight(fieldF)) / 2;
        g.roundRect(field.x, fy, field.w, fieldH, 10, pal.text, 0.06f * a);
        g.roundStroke(field.x, fy, field.w, fieldH, 10, 1.5f, pal.accent, 0.8f * a);
        g.clip(textX - 2, fy, textW + 4, fieldH);
        const auto [sa, sb] = chosen();
        if (sa != sb) {
            const float x0 = g.measureF(text.substr(0, sa), fieldF), x1 = g.measureF(text.substr(0, sb), fieldF);
            g.rect(textX - scroll + x0, fy + 9, x1 - x0, fieldH - 18, pal.accent, 0.35f * a);
        }
        if (text.empty() && composing.empty()) g.text(guide, fieldF, textX, ty, pal.text3, a);
        else {
            g.text(text.substr(0, caret), fieldF, textX - scroll, ty, pal.text, a);
            if (!composing.empty()) {
                g.text(composing, fieldF, textX - scroll + caretPx, ty, pal.accentInk, a);
                g.rect(textX - scroll + caretPx, fy + fieldH - 11, compW, 1.5f, pal.accentInk, a);
            }
            g.text(text.substr(caret), fieldF, textX - scroll + caretPx + compW, ty, pal.text, a);
        }
        if ((SDL_GetTicks() - opened) % 1060 < 620) g.rect(textX - scroll + caretPx + compW, fy + 11, 1.5f, fieldH - 22, pal.text, a);
        g.unclip();

        g.roundRect(cancel.x, cancel.y + lift, cancel.w, cancel.h, 10, pal.text, 0.08f * a);
        g.textC("Cancel", buttonF, cancel.x + cancel.w / 2, cancel.y + lift + cancel.h / 2, pal.text, a, Align::Center);
        g.roundRect(ok.x, ok.y + lift, ok.w, ok.h, 10, pal.accent, a);
        g.textC("OK", buttonF, ok.x + ok.w / 2, ok.y + lift + ok.h / 2, pal.onAccent, a, Align::Center);
        SDL_RenderPresent(renderer_);
        g.endFrame();
    }
    SDL_StopTextInput();
    plat::showTouchKeyboard(false);
    if (answer == plat::TextResult::Done) snprintf(out, size, "%s", text.c_str());

    // The frame goes on where it stopped
    SDL_SetRenderTarget(renderer_, onto);
    SDL_RenderSetScale(renderer_, scale_, scale_);
    SDL_RenderSetClipRect(renderer_, clipped ? &clipWas : nullptr);
    g.setFade(fadeWas);
    lastTick_ = SDL_GetPerformanceCounter();
    return answer;
}
#endif
