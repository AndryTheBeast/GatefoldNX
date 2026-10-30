/*
 * Songate · winshell.cpp
 * Songate in Windows' own places: the icon by the clock, the buttons under the taskbar's preview with the cover in it, and the
 * system's media controls (the flyout, the keyboard's media keys, Bluetooth headphones).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

// WinRT, through its plain COM face: no C++/WinRT, no exceptions
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.foundation.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "platform.hpp"
#include "winpath.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wm = ABI::Windows::Media;
namespace wf = ABI::Windows::Foundation;
namespace ws = ABI::Windows::Storage::Streams;

namespace plat {
namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kBtnPrev = 40001, kBtnPlay = 40002, kBtnNext = 40003;
constexpr UINT kMenuShow = 40010, kMenuPlay = 40011, kMenuNext = 40012, kMenuPrev = 40013, kMenuQuit = 40014;
constexpr UINT_PTR kSubclass = 1;

HWND hwnd = nullptr;
UINT taskbarMade = 0, showMsg = 0;
ComPtr<ITaskbarList3> taskbar;
HICON icons[4] = {};   // previous, play, pause, next
bool buttonsAdded = false, trayOn = false, hasTrack = false, isPlayingNow = false;
std::mutex lock;
std::deque<Cmd> commands;
std::vector<uint8_t> coverBgra;
int coverW = 0, coverH = 0;
std::wstring tipText = L"Songate";
ComPtr<wm::ISystemMediaTransportControls> smtc;

void push(Cmd c) {
    std::lock_guard<std::mutex> g(lock);
    commands.push_back(c);
}

/** A small white glyph, drawn by coverage: triangles and bars. */
HICON glyph(int kind) {
    constexpr int S = 32;
    std::vector<uint32_t> px(S * S, 0);
    auto bar = [&](float x0, float x1) {
        for (int y = 7; y < 25; y++) for (int x = 0; x < S; x++) if (x + 0.5f >= x0 && x + 0.5f <= x1) px[y * S + x] = 0xFFFFFFFF;
    };
    auto tri = [&](float tip, float base) {   // pointing at tip, 18 tall
        const float cy = 16;
        for (int y = 7; y < 25; y++) {
            const float h = 1 - std::abs(y + 0.5f - cy) / 9.f;
            for (int x = 0; x < S; x++) {
                const float at = (x + 0.5f - base) / (tip - base);
                if (at >= 0 && at <= h) px[y * S + x] = 0xFFFFFFFF;
            }
        }
    };
    if (kind == 0) { tri(8, 24); bar(6, 9); }
    if (kind == 1) tri(26, 8);
    if (kind == 2) { bar(9, 14); bar(18, 23); }
    if (kind == 3) { tri(24, 8); bar(23, 26); }
    BITMAPV5HEADER b{};
    b.bV5Size = sizeof b;
    b.bV5Width = S;
    b.bV5Height = -S;
    b.bV5Planes = 1;
    b.bV5BitCount = 32;
    b.bV5Compression = BI_RGB;
    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&b), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

void fillButton(THUMBBUTTON& b, UINT id, HICON icon, const wchar_t* tip) {
    b = {};
    b.dwMask = THUMBBUTTONMASK(THB_ICON | THB_TOOLTIP | THB_FLAGS);
    b.iId = id;
    b.hIcon = icon;
    b.dwFlags = hasTrack ? THBF_ENABLED : THBF_DISABLED;
    wcsncpy(b.szTip, tip, 259);
}

void updateButtons() {
    if (!taskbar || !hwnd) return;
    THUMBBUTTON b[3];
    fillButton(b[0], kBtnPrev, icons[0], L"Previous");
    fillButton(b[1], kBtnPlay, icons[isPlayingNow ? 2 : 1], isPlayingNow ? L"Pause" : L"Play");
    fillButton(b[2], kBtnNext, icons[3], L"Next");
    if (!buttonsAdded) buttonsAdded = SUCCEEDED(taskbar->ThumbBarAddButtons(hwnd, 3, b));
    else taskbar->ThumbBarUpdateButtons(hwnd, 3, b);
}

/** The cover as a window's preview: Windows asks, in the size it wants. */
void sendThumbnail(int maxW, int maxH) {
    if (coverBgra.empty()) return;
    const float k = std::min(float(maxW) / coverW, float(maxH) / coverH);
    const int w = std::max(1, int(coverW * k)), h = std::max(1, int(coverH * k));
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    auto* out = static_cast<uint32_t*>(bits);
    for (int y = 0; y < h; y++) {
        const int sy = std::min(coverH - 1, y * coverH / h);
        for (int x = 0; x < w; x++) out[y * w + x] = reinterpret_cast<const uint32_t*>(coverBgra.data())[sy * coverW + std::min(coverW - 1, x * coverW / w)];
    }
    DwmSetIconicThumbnail(hwnd, bmp, 0);
    DeleteObject(bmp);
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kMenuShow, L"Show Songate");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (hasTrack ? 0 : MF_GRAYED), kMenuPlay, isPlayingNow ? L"Pause" : L"Play");
    AppendMenuW(m, MF_STRING | (hasTrack ? 0 : MF_GRAYED), kMenuNext, L"Next");
    AppendMenuW(m, MF_STRING | (hasTrack ? 0 : MF_GRAYED), kMenuPrev, L"Previous");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuQuit, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);   // or the menu stays open after a click elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == taskbarMade) {
        buttonsAdded = false;
        updateButtons();
    } else if (msg == showMsg) {
        push(Cmd::Show);
    } else if (msg == WM_COMMAND) {
        switch (LOWORD(w)) {
            case kBtnPrev: case kMenuPrev: push(Cmd::Previous); return 0;
            case kBtnPlay: case kMenuPlay: push(Cmd::PlayPause); return 0;
            case kBtnNext: case kMenuNext: push(Cmd::Next); return 0;
            case kMenuShow: push(Cmd::Show); return 0;
            case kMenuQuit: push(Cmd::Quit); return 0;
            default: break;
        }
    } else if (msg == WM_NCCALCSIZE && w && IsZoomed(h)) {
        // maximised, a frameless window would hang over the screen's edges by its invisible frame
        MONITORINFO mi{ sizeof mi };
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] = mi.rcWork;
            return 0;
        }
    } else if (msg == kTrayMsg) {
        if (LOWORD(l) == WM_LBUTTONUP) push(Cmd::Show);
        else if (LOWORD(l) == WM_RBUTTONUP) trayMenu();
        return 0;
    } else if (msg == WM_DWMSENDICONICTHUMBNAIL) {
        sendThumbnail(HIWORD(l), LOWORD(l));
        return 0;
    }
    return DefSubclassProc(h, msg, w, l);
}

void startMediaControls() {
    ComPtr<ISystemMediaTransportControlsInterop> interop;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(), IID_PPV_ARGS(&interop))) || !interop) return;
    if (FAILED(interop->GetForWindow(hwnd, IID_PPV_ARGS(&smtc))) || !smtc) return;
    smtc->put_IsEnabled(true);
    smtc->put_IsPlayEnabled(true);
    smtc->put_IsPauseEnabled(true);
    smtc->put_IsNextEnabled(true);
    smtc->put_IsPreviousEnabled(true);
    smtc->put_PlaybackStatus(wm::MediaPlaybackStatus_Closed);
    EventRegistrationToken token;
    smtc->add_ButtonPressed(Callback<wf::ITypedEventHandler<wm::SystemMediaTransportControls*, wm::SystemMediaTransportControlsButtonPressedEventArgs*>>(
        [](wm::ISystemMediaTransportControls*, wm::ISystemMediaTransportControlsButtonPressedEventArgs* e) -> HRESULT {
            wm::SystemMediaTransportControlsButton b;
            if (FAILED(e->get_Button(&b))) return S_OK;
            if (b == wm::SystemMediaTransportControlsButton_Play) push(Cmd::Play);
            else if (b == wm::SystemMediaTransportControlsButton_Pause) push(Cmd::Pause);
            else if (b == wm::SystemMediaTransportControlsButton_Next) push(Cmd::Next);
            else if (b == wm::SystemMediaTransportControlsButton_Previous) push(Cmd::Previous);
            return S_OK;
        }).Get(), &token);
}

}  // namespace

void windowReady(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    hwnd = info.info.win.window;
    taskbarMade = RegisterWindowMessageW(L"TaskbarButtonCreated");
    showMsg = RegisterWindowMessageW(L"Songate.Show");
    SetWindowSubclass(hwnd, windowProc, kSubclass, 0);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);   // DWMWA_USE_IMMERSIVE_DARK_MODE: never the white default
    titleColours(0x0B0F1A, 0xF2F4F8);
    const MARGINS shadow{ 0, 0, 0, 1 };   // Windows keeps the shadow of a window with no frame
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    RoInitialize(RO_INIT_SINGLETHREADED);
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) taskbar->HrInit();
    for (int i = 0; i < 4; i++) icons[i] = glyph(i);
    updateButtons();
    startMediaControls();
}

Cmd nextCommand() {
    std::lock_guard<std::mutex> g(lock);
    if (commands.empty()) return Cmd::None;
    const Cmd c = commands.front();
    commands.pop_front();
    return c;
}

/** The window goes by the clock. Minimised and out of the taskbar, not hidden: Windows ends the media session of a hidden window, and
 *  the media keys with it. */
void titleColours(uint32_t background, uint32_t text) {
    if (!hwnd) return;
    const auto ref = [](uint32_t c) -> COLORREF { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); };
    const COLORREF bg = ref(background), fg = ref(text);
    constexpr DWORD kCaption = 35, kBorder = 34, kText = 36;   // Windows 11; on 10 these fail harmlessly and the dark mode below stands
    DwmSetWindowAttribute(hwnd, kCaption, &bg, sizeof bg);
    DwmSetWindowAttribute(hwnd, kBorder, &bg, sizeof bg);
    DwmSetWindowAttribute(hwnd, kText, &fg, sizeof fg);
}

void trayIcon(bool on) {
    if (on == trayOn || !hwnd) return;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (on) ShowWindow(hwnd, SW_MINIMIZE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, on ? ex | WS_EX_TOOLWINDOW : ex & ~LONG_PTR(WS_EX_TOOLWINDOW));
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof n;
    n.hWnd = hwnd;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = kTrayMsg;
    n.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wcsncpy(n.szTip, tipText.c_str(), 127);
    Shell_NotifyIconW(on ? NIM_ADD : NIM_DELETE, &n);
    trayOn = on;
}

void mediaInfo(const std::string& title, const std::string& artist, const std::string& album, const uint8_t* rgba, int w, int h, const std::vector<uint8_t>& jpeg) {
    tipText = winpath::wideText(title + (artist.empty() ? "" : " · " + artist));
    if (tipText.empty()) tipText = L"Songate";
    if (trayOn) {
        NOTIFYICONDATAW n{};
        n.cbSize = sizeof n;
        n.hWnd = hwnd;
        n.uID = 1;
        n.uFlags = NIF_TIP;
        wcsncpy(n.szTip, tipText.c_str(), 127);
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }
    if (hwnd) SetWindowTextW(hwnd, (title.empty() ? L"Songate" : tipText + L" · Songate").c_str());

    coverBgra.clear();
    coverW = coverH = 0;
    if (rgba && w > 0 && h > 0) {
        coverW = w;
        coverH = h;
        coverBgra.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; i++) {
            coverBgra[i * 4] = rgba[i * 4 + 2];
            coverBgra[i * 4 + 1] = rgba[i * 4 + 1];
            coverBgra[i * 4 + 2] = rgba[i * 4];
            coverBgra[i * 4 + 3] = 255;
        }
    }
    if (hwnd) {
        const BOOL iconic = !coverBgra.empty();
        DwmSetWindowAttribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &iconic, sizeof iconic);
        DwmSetWindowAttribute(hwnd, DWMWA_HAS_ICONIC_BITMAP, &iconic, sizeof iconic);
        DwmInvalidateIconicBitmaps(hwnd);
    }

    if (!smtc) return;
    ComPtr<wm::ISystemMediaTransportControlsDisplayUpdater> du;
    if (FAILED(smtc->get_DisplayUpdater(&du)) || !du) return;
    du->put_Type(wm::MediaPlaybackType_Music);
    ComPtr<wm::IMusicDisplayProperties> mp;
    if (SUCCEEDED(du->get_MusicProperties(&mp)) && mp) {
        const std::wstring wt = winpath::wideText(title), wa = winpath::wideText(artist), wb = winpath::wideText(album);
        mp->put_Title(HStringReference(wt.c_str(), UINT(wt.size())).Get());
        mp->put_Artist(HStringReference(wa.c_str(), UINT(wa.size())).Get());
        ComPtr<wm::IMusicDisplayProperties2> mp2;
        if (SUCCEEDED(mp.As(&mp2)) && mp2) mp2->put_AlbumTitle(HStringReference(wb.c_str(), UINT(wb.size())).Get());
    }
    if (!jpeg.empty()) {
        ComPtr<IStream> mem;
        mem.Attach(SHCreateMemStream(jpeg.data(), UINT(jpeg.size())));
        ComPtr<ws::IRandomAccessStream> ras;
        ComPtr<ws::IRandomAccessStreamReferenceStatics> refs;
        ComPtr<ws::IRandomAccessStreamReference> ref;
        if (mem && SUCCEEDED(CreateRandomAccessStreamOverStream(mem.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&ras))) &&
            SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(), IID_PPV_ARGS(&refs))) &&
            SUCCEEDED(refs->CreateFromStream(ras.Get(), &ref)))
            du->put_Thumbnail(ref.Get());
    } else {
        du->put_Thumbnail(nullptr);
    }
    du->Update();
}

void mediaState(bool track, bool playing) {
    hasTrack = track;
    isPlayingNow = playing;
    updateButtons();
    if (smtc) smtc->put_PlaybackStatus(!track ? wm::MediaPlaybackStatus_Closed : playing ? wm::MediaPlaybackStatus_Playing : wm::MediaPlaybackStatus_Paused);
}

}  // namespace plat
