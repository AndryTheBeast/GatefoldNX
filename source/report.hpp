/*
 * Songate · report.hpp
 * Settings › Support: a bug report or an idea, sent from the console to a GitHub repository of its own (report_key.inc:
 * the repository and a fine-grained token that can only open issues there, scrambled; git-ignored, made by
 * tools/report-key.py from report.key). Without it the build cannot send (Support says so).
 *
 * The token is as hard to dig out as we could make it, never impossible: the app must use it, so whoever runs the .nro can
 * read it as it is sent. Hence a repository just for reports and a token that can only open issues in it. In the binary it is
 * never text (neither the token nor "Authorization: Bearer"): XORed against a generated stream, stored shuffled, its seed in
 * pieces; rebuilt on the stack for the request only and wiped after it, curl's copy too.
 *
 * Nothing about the user's music is sent: no file names or paths, only what the form says and what it shows.
 */
#pragma once
#include <string>
#include <vector>

namespace report {

/** Was this build given a key (report_key.inc)? */
bool available();

/** The console as the report gives it: Songate's version, the firmware, Atmosphère, docked or not. */
struct System {
    std::string firmware, atmosphere;
    bool docked = false;
};
System system();   // (libnx: set:sys and spl; the preview says "PC preview")

/** One of Atmosphère's crash reports (atmosphere/crash_reports, fatal_errors): when, which program, what it says. */
struct Crash {
    std::string when;      // "2026-10-29 21:05"
    std::string program;   // "hbloader (010000000000100d)", or the fatal error's file
    std::string result;    // "Result 0x2A8" / the crash type
    long long at = 0;      // seconds (sorting)
    bool fatal = false;
};
/** The newest first, at most `max`; reads the card: a worker's job, never the main thread's. */
std::vector<Crash> crashes(size_t max);

/** Opens an issue: true and its number, or false and why. Blocks (the network): a worker's job. */
bool send(const std::string& title, const std::string& body, const char* label, int& number, std::string& err);

}  // namespace report
