/*
 * Songate · platform_switch.cpp
 * The platform on the Nintendo Switch: libnx.
 */
#include "platform.hpp"

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "gfbg.h"

namespace plat {

static_assert(u64(BtnA) == u64(HidNpadButton_A) && u64(BtnB) == u64(HidNpadButton_B) && u64(BtnX) == u64(HidNpadButton_X) && u64(BtnY) == u64(HidNpadButton_Y));
static_assert(u64(BtnL) == u64(HidNpadButton_L) && u64(BtnR) == u64(HidNpadButton_R) && u64(BtnZL) == u64(HidNpadButton_ZL) && u64(BtnZR) == u64(HidNpadButton_ZR));
static_assert(u64(BtnPlus) == u64(HidNpadButton_Plus) && u64(BtnMinus) == u64(HidNpadButton_Minus));
static_assert(u64(BtnLeft) == u64(HidNpadButton_Left) && u64(BtnUp) == u64(HidNpadButton_Up) && u64(BtnRight) == u64(HidNpadButton_Right) && u64(BtnDown) == u64(HidNpadButton_Down));

namespace {

constexpr u64 kBgProgram = 0x4200000047464E58ull;

PadState pad;
AppletHookCookie cookie{};
LeaveFn leaveFn = nullptr;
void* leaveArg = nullptr;

void onApplet(AppletHookType type, void*) {
    if (leaveFn && (type == AppletHookType_OnExitRequest || (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)))
        leaveFn(type == AppletHookType_OnExitRequest, leaveArg);
}

/** The console's file system read directly: up to 64 entries with their sizes a request, where stat() asked once a file
 *  (a folder of thousands held the app for many seconds: Details and Delete measure before they show). */
void measureFs(FsFileSystem* fs, const std::string& path, int& files, uint64_t& bytes, int cap) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) return;
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (files >= cap || R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += uint64_t(e.file_size); files++; }
        }
    }
    fsDirClose(&d);
    for (const auto& n : dirs) {
        if (files >= cap) break;
        measureFs(fs, (path == "/" ? "/" : path + "/") + n, files, bytes, cap);
    }
}

/** The same for Storage: stat() asked once per file (thousands of requests for the covers cache, each one queued with
 *  everything else). */
void measureUse(FsFileSystem* fs, const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&), bool top) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) {
        FsFile f;   // a file, not a folder
        s64 size = 0;
        if (R_SUCCEEDED(fsFsOpenFile(fs, p, FsOpenMode_Read, &f))) {
            if (R_SUCCEEDED(fsFileGetSize(&f, &size))) { bytes += size; files++; }
            fsFileClose(&f);
        }
        return;
    }
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (top && keep && !keep(e.name)) continue;
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += e.file_size; files++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a breath between batches for the rest of the app
    }
    fsDirClose(&d);
    for (const auto& n : dirs) measureUse(fs, (path == "/" ? "/" : path + "/") + n, bytes, files, keep, false);
}

}  // namespace

// ---- the app's life

void init() {
    romfsInit();
    plInitialize(PlServiceType_User);
    psmInitialize();
    lblInitialize();   // Dim the screen: the handheld backlight (failing is fine: a veil instead)
}

void quit() {
    lblExit();
    psmExit();
    plExit();
    romfsExit();
}

bool mainLoop() { return appletMainLoop(); }
void holdExit() { appletLockExit(); }
void releaseExit() { appletUnlockExit(); }

void watchLeave(LeaveFn fn, void* arg) {
    leaveFn = fn;
    leaveArg = arg;
    appletHook(&cookie, onApplet, nullptr);
}
void unwatchLeave() { appletUnhook(&cookie); }

bool inFocus() { return appletGetFocusState() == AppletFocusState_InFocus; }

std::string selfPath(int argc, char** argv) {
    // hbloader gives the .nro's path
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0 && strstr(argv[0], ".nro")) return argv[0];
    return "sdmc:/switch/Songate/Songate.nro";
}

bool swapsSelf() { return true; }

bool relaunch(const std::string& program) {
    if (!envHasNextLoad()) return false;
    envSetNextLoad(program.c_str(), program.c_str());
    return true;
}

bool legacyData() { return true; }

// ---- input

void padInit() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
}
void padRead() { padUpdate(&pad); }
uint64_t padDown() { return padGetButtonsDown(&pad); }
uint64_t padHeld() { return padGetButtons(&pad); }
Stick stick(int which) {
    const HidAnalogStickState s = padGetStickPos(&pad, which);
    return { s.x, s.y };
}

// ---- typing

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return TextResult::Unavailable;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    const Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? TextResult::Done : TextResult::Cancelled;
}

// ---- the machine

void setMediaPlaying(bool on) { appletSetMediaPlaybackState(on); }

void setKeepAwake(bool on) {
    // (1.1 called appletSetAutoSleepTimeAndDimmingTimeEnabled, which only an overlay applet may: from an application it did
    // nothing, so the console dimmed by itself whenever the music was paused.) An application's own ways: the idle
    // time stretched as far as it goes, and auto-sleep off.
    appletSetIdleTimeDetectionExtension(on ? AppletIdleTimeDetectionExtension_ExtendedUnsafe : AppletIdleTimeDetectionExtension_None);
    appletSetAutoSleepDisabled(on);
}

void requestSleep() { appletRequestToSleep(); }

/** Handheld only: docked, the TV is not ours. */
bool dimBacklight(float& saved) {
    if (appletGetOperationMode() != AppletOperationMode_Handheld || R_FAILED(lblGetCurrentBrightnessSetting(&saved)) ||
        R_FAILED(lblSetCurrentBrightnessSetting(std::min(saved, 0.04f))))
        return false;
    lblApplyCurrentBrightnessSettingToBacklight();
    return true;
}

void restoreBacklight(float saved) {
    lblSetCurrentBrightnessSetting(saved);
    lblApplyCurrentBrightnessSettingToBacklight();
}

bool batteryLevel(unsigned& percent) {
    u32 p = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&p))) return false;
    percent = p;
    return true;
}

bool batteryRaw(double& percent, bool& charging) {
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetRawBatteryChargePercentage(&percent)) || R_FAILED(psmGetChargerType(&ct))) return false;
    charging = ct != PsmChargerType_Unconnected;
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked) {
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) firmware = fw.display_version;
        setsysExit();
    }
    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem(65000), &v)))   // Exosphère's API version: Atmosphère's own
            atmosphere = std::to_string((v >> 56) & 0xFF) + "." + std::to_string((v >> 48) & 0xFF) + "." + std::to_string((v >> 40) & 0xFF);
        splExit();
    }
    docked = appletGetOperationMode() == AppletOperationMode_Console;
}

long long bootStamp() { return (long long)time(nullptr) - (long long)(armTicksToNs(armGetSystemTick()) / 1000000000ull); }

void placeThread(int core, int priority) {
    svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority));
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

bool netStart() { return R_SUCCEEDED(socketInitializeDefault()); }
void netStop() { socketExit(); }

// ---- the background player

int bgProtocol() { return GFBG_PROTOCOL; }

bool bgRestart(std::string& err) {
    if (R_FAILED(pmshellInitialize())) { err = "The console did not allow it: restart the console instead"; return false; }
    pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
    svcSleepThread(300'000'000ull);
    NcmProgramLocation loc{};
    loc.program_id = kBgProgram;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_FAILED(rc)) { err = "It did not start again: restart the console"; return false; }
    return true;
}

bool bgEnd() {
    const bool pm = R_SUCCEEDED(pmshellInitialize());
    if (pm) {
        pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
        svcSleepThread(300'000'000ull);
    }
    return pm;
}

void bgStart(bool ended) {
    if (!ended) return;
    NcmProgramLocation loc{};
    loc.program_id = kBgProgram;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    pmshellLaunchProgram(0, &loc, &pid);   // not installed: fails harmlessly
    pmshellExit();
}

// ---- files

std::string assetPath(const char* relative) { return std::string("romfs:/") + relative; }

std::vector<FontData> systemFonts() {
    std::vector<FontData> out;
    for (PlSharedFontType t : { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                                PlSharedFontType_ChineseTraditional, PlSharedFontType_KO }) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t))) out.push_back({ fd.address, fd.size });
    }
    return out;
}

const char* configParent() { return "sdmc:/config"; }
const char* configDir() { return "sdmc:/config/gatefold-nx"; }
const char* backupDir() { return "sdmc:/config/gatefold-nx-backups"; }
const char* root() { return "sdmc:/"; }
const char* rootName() { return "the SD card"; }
bool isRoot(const std::string& path) { return path.empty() || path == "sdmc:/" || path == "sdmc:"; }

std::string parentDir(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : path.substr(0, s);
}

std::string join(const std::string& dir, const std::string& name) { return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name; }
size_t rootLen(const std::string& path) { return path.compare(0, 6, "sdmc:/") == 0 ? 6 : 0; }
std::string fromRoot(const std::string& path) { return "sdmc:" + path; }

const std::vector<const char*>& systemFolders() {
    static const std::vector<const char*> v = { "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/switch", "sdmc:/Nintendo", "sdmc:/config", "sdmc:/emuMMC" };
    return v;
}

bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureFs(fs, path.substr(5), files, bytes, cap);
    return true;
}

bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&)) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureUse(fs, path.substr(5), bytes, files, keep, true);
    return true;
}

}  // namespace plat
