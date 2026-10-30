/*
 * Songate · platform.hpp
 * Everything that differs from one platform to another, and nothing else. source/ is the same code on every
 * platform: it never names a system library, it asks here. Each platform has one implementation of this file
 * (platform/switch/, platform/windows/, platform/android/); tools/preview/ has its own, for the PC test harness.
 */
#pragma once
#include <cstddef>
#ifdef GF_WINDOWED
struct SDL_Window;
#endif
#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// ---- the app's life
void init();   // before anything else
void quit();   // after SDL_Quit
/** False when the system wants the app to end. */
bool mainLoop();
/** Closing from outside waits (holdExit) until the settings and the session are written (releaseExit). */
void holdExit();
void releaseExit();
/** fn runs whenever the app stops being in front; exiting: it is about to end. */
using LeaveFn = void (*)(bool exiting, void* arg);
void watchLeave(LeaveFn fn, void* arg);
void unwatchLeave();
bool inFocus();
/** The running program, where an update's new one is swapped in. */
std::string selfPath(int argc, char** argv);
/** An update replaces the running program as the app closes (and before the next launch reads it). */
bool swapsSelf();
/** The next launch runs this program. False when the system cannot do that. */
bool relaunch(const std::string& program);
/** Folders and files left by older versions on this platform may exist, to tidy away. */
bool legacyData();

// ---- input: the Joy-Con's buttons are the app's own vocabulary
enum Button : uint64_t {
    BtnA = 1u << 0, BtnB = 1u << 1, BtnX = 1u << 2, BtnY = 1u << 3,
    BtnL = 1u << 6, BtnR = 1u << 7, BtnZL = 1u << 8, BtnZR = 1u << 9,
    BtnPlus = 1u << 10, BtnMinus = 1u << 11,
    BtnLeft = 1u << 12, BtnUp = 1u << 13, BtnRight = 1u << 14, BtnDown = 1u << 15,
};
struct Stick { int x, y; };   // -32767..32767, y up
void padInit();
void padRead();   // once a frame
uint64_t padDown();   // pressed this frame
uint64_t padHeld();
Stick stick(int which);   // 0 left, 1 right

// ---- typing
enum class TextResult { Done, Cancelled, Unavailable };
/** Asks for a line of text and waits for the answer. initial may be null. out holds UTF-8, at most size bytes with its end. */
TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size);
#ifdef GF_WINDOWED
/** No system keyboard to hand over to: askText goes to the app's own card (view_textcard.cpp). */
using TextFn = TextResult (*)(const char* header, const char* guide, const char* initial, char* out, size_t size);
void setTextCard(TextFn fn);
/** A tablet with no keyboard attached: the system's on-screen one, while the card is open. */
void showTouchKeyboard(bool on);
/** A button's name as drawn on a key cap, for what was used last: the keyboard's key, or a pad's label. */
std::string capName(const std::string& button);
/** The last thing pressed was on a pad. */
bool padWasLast();
/** The window as the system sees it: the tray's icon, the taskbar's buttons, the media overlay. */
void windowReady(SDL_Window* window);
enum class Cmd { None, PlayPause, Play, Pause, Next, Previous, Show, Quit };
/** What the system's controls asked for, oldest first; once a frame until None. */
Cmd nextCommand();
void trayIcon(bool on);
/** The window's title bar in the app's colours (0xRRGGBB). */
void titleColours(uint32_t background, uint32_t text);
/** What is playing, for the system's media controls and the taskbar. cover: RGBA, may be null; jpeg: the same as a file. */
void mediaInfo(const std::string& title, const std::string& artist, const std::string& album, const uint8_t* rgba, int w, int h,
               const std::vector<uint8_t>& jpeg);
void mediaState(bool hasTrack, bool playing);
/** The usual place for music, offered at the first launch. */
std::string musicFolder();
#endif

// ---- the machine
/** Music playing, or something else that keeps the screen from dimming and the machine from sleeping. */
void setMediaPlaying(bool on);
/** No dimming or sleeping by itself. */
void setKeepAwake(bool on);
void requestSleep();
/** The screen's own light down; saved keeps the level. False: not possible here, a veil is drawn instead. */
bool dimBacklight(float& saved);
void restoreBacklight(float saved);
/** False when there is no battery. */
bool batteryLevel(unsigned& percent);
/** percent with its decimals; charging: a charger is connected. */
bool batteryRaw(double& percent, bool& charging);
void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked);
/** When the machine last started, in time() seconds: a different one means it restarted. */
long long bootStamp();
/** Moves the calling thread to that core and priority, as cores.hpp lists them. */
void placeThread(int core, int priority);
/** The network library's start and end. */
bool netStart();
void netStop();

// ---- the background player
int bgProtocol();
/** Ends it and starts it again. False, with why in err, when that failed. */
bool bgRestart(std::string& err);
/** Ends it for now: true means bgStart(true) is needed after. */
bool bgEnd();
void bgStart(bool ended);

// ---- files
/** The app's own read-only files (fonts). */
std::string assetPath(const char* relative);
struct FontData { void* data; size_t size; };
/** The system's fonts, as fallbacks for what Inter does not have. */
std::vector<FontData> systemFonts();
const char* configParent();   // the folder holding configDir
const char* configDir();
const char* backupDir();      // where backups go unless the user chose
const char* root();           // what browsers start from: the top of everything the app can see
const char* rootName();       // the same in words
bool isRoot(const std::string& path);
std::string parentDir(const std::string& path);
/** A name inside a folder. */
std::string join(const std::string& dir, const std::string& name);
/** The length of an absolute path's root (0 when it has none). */
size_t rootLen(const std::string& path);
/** "/a/b" from a playlist written elsewhere → the same place here. */
std::string fromRoot(const std::string& path);
/** Folders the system needs (Files asks more firmly before touching them). */
const std::vector<const char*>& systemFolders();
/** Files and bytes below a folder, the system's own fast way. False: use stat(). */
bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap);
/** … the same for Storage, only names keep accepts at its top. */
bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&));

}  // namespace plat
