/*
 * Songate · platform_windows.cpp
 * The platform on Windows: Win32 and SDL. The app's own vocabulary is the Joy-Con's buttons (platform.hpp): a keyboard and an
 * Xbox-style pad are read into those, by label (Xbox A confirms, as its label says).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include "platform.hpp"
#include "winpath.hpp"

namespace plat {

namespace {

LeaveFn leaveFn = nullptr;
void* leaveArg = nullptr;
SDL_GameController* padDev = nullptr;
uint64_t heldNow = 0, heldBefore = 0;
bool padLast = false;   // the last thing pressed was on a pad: key caps show its labels
EXECUTION_STATE awakeBits = 0;

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) out = winpath::slashes(winpath::utf8(p));
    CoTaskMemFree(p);
    return out;
}

int leaveWatch(void*, SDL_Event* e) {
    if (leaveFn && e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) leaveFn(false, leaveArg);
    return 0;
}

void openPad() {
    if (padDev) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (padDev = SDL_GameControllerOpen(i))) return;
}

void awake() { SetThreadExecutionState(ES_CONTINUOUS | awakeBits); }

void measureTree(const std::wstring& dir, int& files, uint64_t& bytes, int cap, bool (*keep)(const std::string&), bool top) {
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = dir + (dir.back() == L'\\' ? L"*" : L"\\*");
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (cap > 0 && files >= cap) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (top && keep && !keep(winpath::utf8(fd.cFileName))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))   // a junction would count its target, or loop
                measureTree(dir + (dir.back() == L'\\' ? L"" : L"\\") + fd.cFileName, files, bytes, cap, keep, false);
        } else {
            bytes += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            files++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

// ---- the app's life

void init() {
    // One Songate: a second launch brings the first one forward (tests run several, each with its own settings)
    if (!getenv("GF_CONFIG")) {
        HANDLE one = CreateMutexW(nullptr, TRUE, L"Local\\Songate.Single");
        // started by the one before it (Restart now): that one is on its way out
        if (GetLastError() == ERROR_ALREADY_EXISTS && wcsstr(GetCommandLineW(), L"--after")) WaitForSingleObject(one, 5000);
        else if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND h = FindWindowW(L"SDL_app", nullptr)) PostMessageW(h, RegisterWindowMessageW(L"Songate.Show"), 0, 0);
            ExitProcess(0);
        }
    }
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");   // real pixels: Gfx draws at them
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");   // no title bar of Windows', but its edges, snapping and shadow
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");   // songate.rc's, on the window and the taskbar
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");   // × may only go to the tray: App::closeWindow decides
}

void quit() {
    if (padDev) SDL_GameControllerClose(padDev);
    padDev = nullptr;
}

bool mainLoop() { return true; }
void holdExit() {}
void releaseExit() {}

void watchLeave(LeaveFn fn, void* arg) {
    leaveFn = fn;
    leaveArg = arg;
    SDL_AddEventWatch(leaveWatch, nullptr);
}
void unwatchLeave() {
    SDL_DelEventWatch(leaveWatch, nullptr);
    leaveFn = nullptr;
}

bool inFocus() { return SDL_GetKeyboardFocus() != nullptr; }

std::string selfPath(int, char**) {
    wchar_t p[32768];
    const DWORD n = GetModuleFileNameW(nullptr, p, 32767);
    p[n] = 0;
    return winpath::slashes(winpath::utf8(p));
}

bool swapsSelf() { return true; }   // only settings wait for a launch here (a restore, a fresh start): the installer replaces the program

/** SongateWIN-<version>.exe, never named .zip: consoles on 2.0.0 BETA 1 take the first .zip they find. */
bool isInstaller(const std::string& name) {
    const size_t cut = name.find_last_of("/\\");
    const std::string n = cut == std::string::npos ? name : name.substr(cut + 1);
    return n.rfind("SongateWIN-", 0) == 0 && n.size() > 15 && n.compare(n.size() - 4, 4, ".exe") == 0;
}

/** The next launch, or an update's installer, which starts Songate again when it is done. */
bool relaunch(const std::string& program) {
    const bool setup = isInstaller(program);
    std::wstring cmd = L"\"" + winpath::wide(program.c_str()) + L"\"" + (setup ? L" /SILENT /SUPPRESSMSGBOXES /NORESTART" : L" --after");
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
bool legacyData() { return false; }

// ---- input

void padInit() {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    openPad();
}

void padRead() {
    heldBefore = heldNow;
    uint64_t b = 0;
    if (SDL_GetKeyboardFocus()) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        const SDL_Keymod mod = SDL_GetModState();
        const bool ctrl = mod & KMOD_CTRL;
        if (ctrl) {
            if (k[SDL_SCANCODE_LEFT]) b |= BtnL;     // Ctrl+← →: the track before and after
            if (k[SDL_SCANCODE_RIGHT]) b |= BtnR;
            if (k[SDL_SCANCODE_F]) b |= BtnY;        // Ctrl+F: Search
        } else if (mod & KMOD_SHIFT) {
            if (k[SDL_SCANCODE_LEFT]) b |= BtnZL;    // Shift+← →: ten seconds back and on
            if (k[SDL_SCANCODE_RIGHT]) b |= BtnZR;
        } else {
            if (k[SDL_SCANCODE_LEFT]) b |= BtnLeft;
            if (k[SDL_SCANCODE_RIGHT]) b |= BtnRight;
            if (k[SDL_SCANCODE_UP]) b |= BtnUp;
            if (k[SDL_SCANCODE_DOWN]) b |= BtnDown;
            if ((k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) && !(mod & KMOD_ALT)) b |= BtnA;   // Alt+Enter: full screen (App::input)
            if (k[SDL_SCANCODE_ESCAPE]) b |= BtnB;
            if (k[SDL_SCANCODE_SPACE]) b |= BtnX;
            if (k[SDL_SCANCODE_APPLICATION]) b |= BtnPlus;     // the menu key: a row's menu
            if (k[SDL_SCANCODE_C]) b |= BtnMinus;              // the Canvas
        }
    }
    const uint64_t fromKeys = b;
    if (!padDev) openPad();
    if (padDev && !SDL_GameControllerGetAttached(padDev)) {
        SDL_GameControllerClose(padDev);
        padDev = nullptr;
    }
    if (padDev) {
        // By label: Xbox's A is the bottom button and confirms, where Nintendo's A is the right one
        const std::pair<SDL_GameControllerButton, Button> map[] = {
            { SDL_CONTROLLER_BUTTON_A, BtnA },           { SDL_CONTROLLER_BUTTON_B, BtnB },           { SDL_CONTROLLER_BUTTON_X, BtnX },
            { SDL_CONTROLLER_BUTTON_Y, BtnY },           { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BtnL }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BtnR },
            { SDL_CONTROLLER_BUTTON_START, BtnPlus },    { SDL_CONTROLLER_BUTTON_BACK, BtnMinus },    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, BtnLeft },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, BtnUp },    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BtnRight }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, BtnDown },
        };
        for (const auto& [from, to] : map) if (SDL_GameControllerGetButton(padDev, from)) b |= to;
        if (SDL_GameControllerGetAxis(padDev, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= BtnZL;
        if (SDL_GameControllerGetAxis(padDev, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= BtnZR;
    }
    if (fromKeys) padLast = false;
    else if (b) padLast = true;
    heldNow = b;
}

uint64_t padDown() { return heldNow & ~heldBefore; }
uint64_t padHeld() { return heldNow; }

Stick stick(int which) {
    if (!padDev) return { 0, 0 };
    const int x = SDL_GameControllerGetAxis(padDev, which ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(padDev, which ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
    return { std::clamp(x, -32767, 32767), std::clamp(-y, -32767, 32767) };   // SDL's down is positive, the app's up
}

// ---- typing

namespace {
TextFn textCard = nullptr;
// The system's on-screen keyboard has no documented way to open: this interface is what Windows' own shell uses
struct ITipInvocation : IUnknown { virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0; };
const CLSID kTipHost = { 0x4CE576FA, 0x83DC, 0x4F88, { 0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76 } };
const IID kTipInvocation = { 0x37c994e7, 0x432b, 0x4834, { 0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b } };
}  // namespace

void setTextCard(TextFn fn) { textCard = fn; }

bool padWasLast() { return padLast; }

std::string capName(const std::string& b) {
    static const std::pair<const char*, const char*> keys[] = { { "A", "Enter" }, { "B", "Esc" }, { "X", "Space" }, { "Y", "Ctrl F" }, { "L", "Ctrl ←" },
                                                                { "R", "Ctrl →" }, { "ZL", "Shift ←" }, { "ZR", "Shift →" }, { "+", "Right-click" }, { "-", "C" } };
    static const std::pair<const char*, const char*> pad[] = { { "L", "LB" }, { "R", "RB" }, { "ZL", "LT" }, { "ZR", "RT" }, { "+", "Menu" }, { "-", "View" } };
    if (padLast) {
        for (const auto& [from, to] : pad) if (b == from) return to;
        return b;
    }
    if (b.rfind("DPAD", 0) == 0) return "Arrows";
    if (b == "BTN-UP") return "↑";
    if (b == "BTN-DOWN") return "↓";
    if (b == "BTN-LEFT") return "←";
    if (b == "BTN-RIGHT") return "→";
    for (const auto& [from, to] : keys) if (b == from) return to;
    return b;
}

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    return textCard ? textCard(header, guide, initial, out, size) : TextResult::Unavailable;
}

/** Toggle is all that interface has: the system's own input pane says whether it is showing, so a keyboard the user closed stays closed. */
void showTouchKeyboard(bool on) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFrameworkInputPane* pane = nullptr;
    RECT r{};
    if (SUCCEEDED(CoCreateInstance(CLSID_FrameworkInputPane, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pane))) && pane) {
        pane->Location(&r);
        pane->Release();
    }
    if ((r.bottom > r.top) == on) return;
    ITipInvocation* tip = nullptr;
    if (SUCCEEDED(CoCreateInstance(kTipHost, nullptr, CLSCTX_LOCAL_SERVER, kTipInvocation, reinterpret_cast<void**>(&tip))) && tip) {
        tip->Toggle(GetDesktopWindow());
        tip->Release();
    }
}

// ---- the machine

void setMediaPlaying(bool on) {
    awakeBits = on ? awakeBits | ES_SYSTEM_REQUIRED : awakeBits & ~EXECUTION_STATE(ES_SYSTEM_REQUIRED);
    awake();
}

void setKeepAwake(bool on) {
    awakeBits = on ? awakeBits | ES_DISPLAY_REQUIRED : awakeBits & ~EXECUTION_STATE(ES_DISPLAY_REQUIRED);
    awake();
}

void requestSleep() { SetSuspendState(FALSE, FALSE, FALSE); }

bool dimBacklight(float&) { return false; }   // a veil instead: the screen's light is the system's on a PC
void restoreBacklight(float) {}

bool batteryRaw(double& percent, bool& charging) {
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s) || (s.BatteryFlag & 128) || s.BatteryLifePercent > 100) return false;
    percent = s.BatteryLifePercent;
    charging = s.ACLineStatus == 1;
    return true;
}

bool batteryLevel(unsigned& percent) {
    double p = 0;
    bool c = false;
    if (!batteryRaw(p, c)) return false;
    percent = unsigned(p);
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked) {
    using Rtl = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{ sizeof v };
    if (auto f = reinterpret_cast<Rtl>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) f(&v);
    firmware = std::string(v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") + " build " + std::to_string(v.dwBuildNumber);
    atmosphere.clear();
    docked = false;
}

long long bootStamp() { return (long long)time(nullptr) - (long long)(GetTickCount64() / 1000); }

/** Windows places threads itself, and well: only the order of importance is kept. */
void placeThread(int, int priority) {
    int p = THREAD_PRIORITY_NORMAL;
    if (priority <= 0x2B) p = THREAD_PRIORITY_ABOVE_NORMAL;       // audio and its read-ahead
    else if (priority >= 0x30) p = THREAD_PRIORITY_BELOW_NORMAL;  // covers, levelling, downloads
    SetThreadPriority(GetCurrentThread(), p);
}

bool netStart() { return true; }   // curl starts the sockets
void netStop() {}

// ---- the background player: none on a PC

int bgProtocol() { return 0; }
bool bgRestart(std::string& err) { err = "There is no background player on a PC"; return false; }
bool bgEnd() { return false; }
void bgStart(bool) {}

// ---- files

std::string assetPath(const char* relative) {
    static const std::string dir = [] {
        const std::string self = selfPath(0, nullptr);
        return self.substr(0, self.find_last_of('/'));
    }();
    return dir + "/romfs/" + relative;
}

std::vector<FontData> systemFonts() {
    // Mapped, not read: the CJK ones are tens of megabytes, and FreeType touches only the glyphs it uses
    std::vector<FontData> out;
    const std::string fonts = knownFolder(FOLDERID_Fonts);
    for (const char* name : { "segoeui.ttf", "seguisym.ttf", "msyh.ttc", "msjh.ttc", "malgun.ttf", "YuGothR.ttc", "Nirmala.ttf", "ebrima.ttf" }) {
        const std::wstring p = winpath::wide((fonts + "/" + name).c_str());
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size;
        GetFileSizeEx(f, &size);
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(f);
        if (!m) continue;
        void* at = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(m);
        if (at) out.push_back({ at, size_t(size.QuadPart) });
    }
    return out;
}

const char* configParent() {
    static const std::string p = [] {
#ifdef GF_DEV
        if (const char* e = getenv("GF_CONFIG")) return winpath::slashes(e);   // tests keep to a folder of their own
#endif
        return knownFolder(FOLDERID_RoamingAppData);
    }();
    return p.c_str();
}
const char* configDir() {
    static const std::string p = std::string(configParent()) + "/Songate";
    return p.c_str();
}
const char* backupDir() {
    static const std::string p = std::string(configParent()) + "/Songate-backups";
    return p.c_str();
}
const char* root() { return "/"; }
const char* rootName() { return "This PC"; }
bool isRoot(const std::string& path) { return path.empty() || path == "/"; }

std::string parentDir(const std::string& path) {
    if (path.size() <= 3) return "/";   // "D:/": the drives
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string("/") : s <= 2 ? path.substr(0, 3) : path.substr(0, s);
}

size_t rootLen(const std::string& path) { return path.size() >= 3 && path[1] == ':' && path[2] == '/' ? 3 : 0; }

/** "/a/b" from a playlist written elsewhere: the first drive that has it. */
std::string fromRoot(const std::string& path) {
    const DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++)
        if (drives & (1u << i)) {
            const std::string p = std::string(1, char('A' + i)) + ":" + path;
            if (GetFileAttributesW(winpath::wide(p.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    return "C:" + path;
}

std::string musicFolder() {
    const std::string p = knownFolder(FOLDERID_Music);
    return GetFileAttributesW(winpath::wide(p.c_str()).c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : p;
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir == "/") return name + "/";   // a drive
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

const std::vector<const char*>& systemFolders() {
    static const std::vector<std::string> names = { knownFolder(FOLDERID_Windows), knownFolder(FOLDERID_ProgramFiles), knownFolder(FOLDERID_ProgramFilesX86),
                                                    knownFolder(FOLDERID_ProgramData), knownFolder(FOLDERID_RoamingAppData) };
    static const std::vector<const char*> v = [] {
        std::vector<const char*> o;
        for (const auto& n : names) if (!n.empty()) o.push_back(n.c_str());
        return o;
    }();
    return v;
}

bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap) {
    if (path.empty() || path == "/") return false;
    if (GetFileAttributesW(winpath::wide(path.c_str()).c_str()) & FILE_ATTRIBUTE_DIRECTORY) measureTree(winpath::wide(path.c_str()), files, bytes, cap, nullptr, false);
    return true;
}

bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&)) {
    const DWORD a = GetFileAttributesW(winpath::wide(path.c_str()).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return false;   // a file: stat() says
    uint64_t b = 0;
    measureTree(winpath::wide(path.c_str()), files, b, 0, keep, true);
    bytes += int64_t(b);
    return true;
}

}  // namespace plat
