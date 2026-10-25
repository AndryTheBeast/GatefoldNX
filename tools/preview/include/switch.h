/* Gatefold NX preview · switch.h
 * The slice of libnx the app uses, stubbed for a Windows build of the UI. */
#pragma once
#include <cstddef>
#include <cstdint>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef u32 Result;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)

enum {
    HidNpadButton_A = 1u << 0, HidNpadButton_B = 1u << 1, HidNpadButton_X = 1u << 2, HidNpadButton_Y = 1u << 3,
    HidNpadButton_L = 1u << 6, HidNpadButton_R = 1u << 7, HidNpadButton_ZL = 1u << 8, HidNpadButton_ZR = 1u << 9,
    HidNpadButton_Plus = 1u << 10, HidNpadButton_Minus = 1u << 11, HidNpadButton_Left = 1u << 12, HidNpadButton_Up = 1u << 13,
    HidNpadButton_Right = 1u << 14, HidNpadButton_Down = 1u << 15, HidNpadButton_StickLLeft = 1u << 16, HidNpadButton_StickLUp = 1u << 17,
    HidNpadButton_StickLRight = 1u << 18, HidNpadButton_StickLDown = 1u << 19,
};
enum { HidNpadStyleSet_NpadStandard = 0x1f };

struct PadState { u64 buttons = 0, old = 0; };
struct HidAnalogStickState { s32 x, y; };
void padConfigureInput(u32, u32);
void padInitializeDefault(PadState*);
void padUpdate(PadState*);
inline u64 padGetButtons(const PadState* p) { return p->buttons; }
inline u64 padGetButtonsDown(const PadState* p) { return p->buttons & ~p->old; }
HidAnalogStickState padGetStickPos(const PadState*, u32);

bool appletMainLoop();
inline Result appletSetMediaPlaybackState(bool) { return 0; }
enum AppletHookType { AppletHookType_OnFocusState = 0, AppletHookType_OnOperationMode, AppletHookType_OnPerformanceMode, AppletHookType_OnExitRequest };
enum AppletFocusState { AppletFocusState_InFocus = 1, AppletFocusState_OutOfFocus = 2, AppletFocusState_Background = 3 };
typedef void (*AppletHookFn)(AppletHookType hook, void* param);
struct AppletHookCookie { AppletHookCookie* next; AppletHookFn callback; void* param; };
inline void appletHook(AppletHookCookie*, AppletHookFn, void*) {}
inline void appletUnhook(AppletHookCookie*) {}
inline AppletFocusState appletGetFocusState() { return AppletFocusState_InFocus; }
inline Result appletLockExit() { return 0; }
inline Result appletUnlockExit() { return 0; }
inline Result romfsInit() { return 0; }
inline Result romfsExit() { return 0; }

enum PlServiceType { PlServiceType_User };
enum PlSharedFontType { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                        PlSharedFontType_ChineseTraditional, PlSharedFontType_KO };
struct PlFontData { void* address; u32 size; };
inline Result plInitialize(PlServiceType) { return 0; }
inline void plExit() {}
inline Result plGetSharedFontByType(PlFontData*, PlSharedFontType) { return 1; }

inline Result psmInitialize() { return 0; }
inline void psmExit() {}
inline Result psmGetBatteryChargePercentage(u32* p) { *p = 76; return 0; }

struct SwkbdConfig { int unused; };
inline Result swkbdCreate(SwkbdConfig*, s32) { return 0; }
inline void swkbdConfigMakePresetDefault(SwkbdConfig*) {}
inline void swkbdConfigSetGuideText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetHeaderText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetInitialText(SwkbdConfig*, const char*) {}
Result swkbdShow(SwkbdConfig*, char* out, size_t size);
inline void swkbdClose(SwkbdConfig*) {}
