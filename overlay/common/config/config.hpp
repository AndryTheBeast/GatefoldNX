#pragma once

#include <switch.h>

namespace config {

// tune shuffle
auto get_shuffle() -> bool;
void set_shuffle(bool value);

// tune repeat
auto get_repeat() -> int;
void set_repeat(int value);

// tune volume
auto get_volume() -> float;
void set_volume(float value);

// per title tune enable (Play On Start — music plays when this title launches)
auto has_title_enabled(u64 tid) -> bool;
auto get_title_enabled(u64 tid) -> bool;
void set_title_enabled(u64 tid, bool value);

// per title Pause On Start — music pauses when this title launches.
// Stored separately from title_enabled so the UI can offer a tri-state
// (Play On Start, Pause On Start, or neither). The two are mutually
// exclusive by convention enforced in the UI.
auto has_title_pause_on_start(u64 tid) -> bool;
auto get_title_pause_on_start(u64 tid) -> bool;
void set_title_pause_on_start(u64 tid, bool value);
void clear_title_pause_on_start(u64 tid);
void clear_title_enabled(u64 tid);

// default for tune for every title (LEGACY — kept for config-file
// backward compatibility; no longer consulted by the policy engine).
auto get_title_enabled_default() -> bool;
void set_title_enabled_default(bool value);

// Auto-play Startup — when the sysmodule first launches, start music
// playing automatically if a startup playlist is set. Applies regardless
// of which title is in foreground at boot; per-title Play On Start and
// Pause On Start take over on subsequent title transitions.
auto get_auto_play_startup() -> bool;
void set_auto_play_startup(bool value);

// Global title-transition defaults — applied to any title whose
// per-title "Default On Start" flag is ON (the factory default).
// Mutually exclusive by convention (UI enforces it).
//   Play On Title  -> music plays  when any title launches
//   Pause On Title -> music pauses when any title launches
auto get_play_on_title() -> bool;
void set_play_on_title(bool value);
auto get_pause_on_title() -> bool;
void set_pause_on_title(bool value);

// Per-title opt-in to the global defaults above.
// Defaults to true — a fresh title uses Play/Pause On Title until the
// user explicitly turns this OFF and configures per-title overrides.
auto get_default_on_start(u64 tid) -> bool;
void set_default_on_start(u64 tid, bool value);

// per title volume
auto has_title_volume(u64 tid) -> bool;
auto get_title_volume(u64 tid) -> float;
void set_title_volume(u64 tid, float value);

// default volume for every title
auto get_default_title_volume() -> float;
void set_default_title_volume(float value);

// returns the length of the string
auto get_load_path(char* out, int max_len) -> int;
void set_load_path(const char* path);

}