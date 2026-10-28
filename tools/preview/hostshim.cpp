/* Gatefold NX preview · hostshim.cpp
 * libnx stand-ins, path mapping, keyboard input and the screenshot script.
 *
 * Environment:
 *   GF_ROOT    folder holding sdmc/ (the fake SD card)          default: current folder
 *   GF_ROMFS   the project's romfs/ folder                      default: ../../romfs
 *   GF_SEARCH  text the "keyboard" returns when it opens; "a|b" answers a, then b
 *   GF_SCRIPT  lines of "<frame> <action> [args]":
 *                press A|B|X|Y|L|R|ZL|ZR|UP|DOWN|LEFT|RIGHT|PLUS|MINUS
 *                hold <button> <frames>   tap <x> <y>   shot <file.bmp>   quit
 *                touch <x> <y> … release <x> <y>   (a finger held down: a long touch)
 *                audit <label>   (the D-pad audit of that frame, appended to dpad-audit.txt: App, Ui::audit)
 *                stick <x> <y> <frames>   (the left stick, −1..1, y up)
 *   GF_JOBS    show the three background jobs (app covers, overlay covers, levelling) as running (shell.cpp)
 *   GF_BGCFG   show the background player's settings (Settings → Playback, a playlist's Games) as if it were connected
 *   GF_SHOW    show the window (keyboard: arrows, Enter=A, Backspace=B, Space=X,
 *              F=Y, Q/E=L/R, 1/2=ZL/ZR, Tab=Minus, Esc=Plus)
 */
// windows.h first: SDL_endian.h and clang's intrinsics disagree the other way round.
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <direct.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "include/dirent.h"
#include "include/switch.h"

namespace {

std::string env(const char* k, const char* d) {
    const char* v = getenv(k);
    return v ? v : d;
}

std::string mapPath(const char* p) {
    std::string s = p ? p : "";
    if (s.rfind("file:", 0) == 0) s.erase(0, 5);   // FFmpeg's file protocol prefix
    if (s.rfind("sdmc:/", 0) == 0) return env("GF_ROOT", ".") + "/sdmc/" + s.substr(6);
    if (s.rfind("romfs:/", 0) == 0) return env("GF_ROMFS", "../../romfs") + "/" + s.substr(7);
    return s;
}

struct Step {
    std::string action;
    std::vector<std::string> args;
};
std::multimap<long, Step> script;
bool scriptLoaded = false;
long frame = 0;
float backlight = 0.6f;
bool quitting = false;
std::map<std::string, long> holdUntil;
float stickX = 0, stickY = 0;   // the left stick, −1..1
long stickUntil = 0;

const std::map<std::string, u64> kButtons = {
    { "A", HidNpadButton_A },       { "B", HidNpadButton_B },         { "X", HidNpadButton_X },       { "Y", HidNpadButton_Y },
    { "L", HidNpadButton_L },       { "R", HidNpadButton_R },         { "ZL", HidNpadButton_ZL },     { "ZR", HidNpadButton_ZR },
    { "UP", HidNpadButton_Up },     { "DOWN", HidNpadButton_Down },   { "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right },
    { "PLUS", HidNpadButton_Plus }, { "MINUS", HidNpadButton_Minus },
};

void loadScript() {
    scriptLoaded = true;
    const std::string path = env("GF_SCRIPT", "");
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        long f;
        Step s;
        if (!(ls >> f >> s.action)) continue;
        std::string a;
        while (ls >> a) s.args.push_back(a);
        script.emplace(f, s);
    }
}

void pushFinger(Uint32 type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.x = x / 1280.f;
    e.tfinger.y = y / 720.f;
    e.tfinger.fingerId = 1;
    SDL_PushEvent(&e);
}

}  // namespace

// ---------------------------------------------------------------- files

const char* hostPath(const char* p) {
    thread_local std::string s;
    s = mapPath(p);
    return s.c_str();
}

FILE* hostFopen(const char* p, const char* m) { return fopen(mapPath(p).c_str(), m); }
int hostStat(const char* p, struct stat* s) { return stat(mapPath(p).c_str(), s); }
int hostMkdir(const char* p, int) { return _mkdir(mapPath(p).c_str()); }
int hostRemove(const char* p) { return remove(mapPath(p).c_str()); }
int hostRename(const char* a, const char* b) { return rename(mapPath(a).c_str(), mapPath(b).c_str()); }
int hostRmdir(const char* p) { return _rmdir(mapPath(p).c_str()); }
TTF_Font* hostOpenFont(const char* p, int size) { return TTF_OpenFont(mapPath(p).c_str(), size); }
Mix_Music* hostLoadMUS(const char* p) { return Mix_LoadMUS(mapPath(p).c_str()); }

struct DIR {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    bool first;
    dirent ent;
};
DIR* opendir(const char* path) {
    DIR* d = new DIR{};
    d->h = FindFirstFileA((mapPath(path) + "/*").c_str(), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
    d->first = true;
    return d;
}
dirent* readdir(DIR* d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return nullptr;
    d->first = false;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof d->ent.d_name - 1);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->ent;
}
int closedir(DIR* d) {
    FindClose(d->h);
    delete d;
    return 0;
}

// ---------------------------------------------------------------- input

void padConfigureInput(u32, u32) {}
void padInitializeDefault(PadState* p) { *p = PadState{}; }

void padUpdate(PadState* p) {
    if (!scriptLoaded) loadScript();
    p->old = p->buttons;
    u64 b = 0;
    if (getenv("GF_SHOW")) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        const std::pair<SDL_Scancode, u64> keys[] = {
            { SDL_SCANCODE_UP, HidNpadButton_Up },       { SDL_SCANCODE_DOWN, HidNpadButton_Down }, { SDL_SCANCODE_LEFT, HidNpadButton_Left },
            { SDL_SCANCODE_RIGHT, HidNpadButton_Right }, { SDL_SCANCODE_RETURN, HidNpadButton_A },  { SDL_SCANCODE_BACKSPACE, HidNpadButton_B },
            { SDL_SCANCODE_SPACE, HidNpadButton_X },     { SDL_SCANCODE_F, HidNpadButton_Y },       { SDL_SCANCODE_Q, HidNpadButton_L },
            { SDL_SCANCODE_E, HidNpadButton_R },         { SDL_SCANCODE_1, HidNpadButton_ZL },      { SDL_SCANCODE_2, HidNpadButton_ZR },
            { SDL_SCANCODE_TAB, HidNpadButton_Minus },   { SDL_SCANCODE_ESCAPE, HidNpadButton_Plus },
        };
        for (auto& [sc, bit] : keys) if (k[sc]) b |= bit;
    }
    std::vector<Step> later;
    auto range = script.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it) {
        const Step& s = it->second;
        if (s.action == "press" && !s.args.empty() && kButtons.count(s.args[0])) b |= kButtons.at(s.args[0]);
        else if (s.action == "hold" && s.args.size() >= 2) holdUntil[s.args[0]] = frame + atol(s.args[1].c_str());
        else if (s.action == "tap" && s.args.size() >= 2) {
            pushFinger(SDL_FINGERDOWN, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
            later.push_back({ "release", s.args });
        } else if (s.action == "audit" && !s.args.empty()) {   // the D-pad audit of the next frame drawn (App, dpad-audit.txt)
            std::string label;
            for (const auto& a : s.args) label += (label.empty() ? "" : " ") + a;
            _putenv(("GF_AUDIT=" + label).c_str());
        } else if (s.action == "touch" && s.args.size() >= 2) {   // a finger down that stays (release x y lifts it)
            pushFinger(SDL_FINGERDOWN, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
        } else if (s.action == "release" && s.args.size() >= 2) {
            pushFinger(SDL_FINGERUP, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
        } else if (s.action == "stick" && s.args.size() >= 3) {
            stickX = float(atof(s.args[0].c_str()));
            stickY = float(atof(s.args[1].c_str()));
            stickUntil = frame + atol(s.args[2].c_str());
        } else if (s.action == "quit") {
            quitting = true;
        }
    }
    for (auto& s : later) script.emplace(frame + 2, s);
    for (auto& [name, until] : holdUntil) if (frame < until && kButtons.count(name)) b |= kButtons.at(name);
    p->buttons = b;
}

HidAnalogStickState padGetStickPos(const PadState*, u32 i) {
    if (i != 0) return { 0, 0 };
    float x = frame < stickUntil ? stickX : 0, y = frame < stickUntil ? stickY : 0;
    if (getenv("GF_SHOW")) {   // I/J/K/L: the left stick
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_J]) x = -1;
        if (k[SDL_SCANCODE_L]) x = 1;
        if (k[SDL_SCANCODE_I]) y = 1;
        if (k[SDL_SCANCODE_K]) y = -1;
    }
    return { s32(x * 32767), s32(y * 32767) };
}
bool appletMainLoop() { return !quitting; }

Result swkbdShow(SwkbdConfig*, char* out, size_t size) {
    // "a|b|c": the first time the keyboard opens it answers a, then b, then c (and c again after that)
    static int asked = 0;
    const std::string all = env("GF_SEARCH", "");
    if (all.empty()) return 1;
    std::string q = all;
    size_t at = 0;
    for (int i = 0; i < asked; i++) {
        const size_t bar = all.find('|', at);
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    q = all.substr(at, all.find('|', at) == std::string::npos ? std::string::npos : all.find('|', at) - at);
    asked++;
    snprintf(out, size, "%s", q.c_str());
    return 0;
}

// ---------------------------------------------------------------- frames

void hostPresent(SDL_Renderer* r) {
    auto range = script.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.action != "shot" || it->second.args.empty()) continue;
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(r, &w, &h);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, it->second.args[0].c_str());
        if (s) SDL_FreeSurface(s);
    }
    SDL_RenderPresent(r);
    frame++;
}

// ---- battery, dock, sleep and backlight (switch.h)
Result psmGetChargerType(PsmChargerType* out) {
    *out = getenv("GF_CHARGING") ? PsmChargerType_EnoughPower : PsmChargerType_Unconnected;
    return 0;
}
Result psmGetRawBatteryChargePercentage(double* out) {
    const double minutes = frame / 60.0 / 60.0;
    const double v = getenv("GF_CHARGING") ? 40 + minutes : 76 - minutes * 0.5;   // windows.h's min/max macros: no std::min here
    *out = v > 100 ? 100 : v < 1 ? 1 : v;
    return 0;
}
AppletOperationMode appletGetOperationMode() { return getenv("GF_DOCKED") ? AppletOperationMode_Console : AppletOperationMode_Handheld; }
Result appletRequestToSleep() {
    printf("[frame %ld] the console goes to sleep\n", frame);
    fflush(stdout);
    return 0;
}
Result lblGetCurrentBrightnessSetting(float* out) { *out = backlight; return 0; }
Result lblSetCurrentBrightnessSetting(float b) {
    backlight = b;
    printf("[frame %ld] backlight %.2f\n", frame, double(b));
    fflush(stdout);
    return 0;
}
