/*
 * Songate · report.cpp
 * See report.hpp. report_key.inc (git-ignored; tools/report-key.py makes it from report.key) holds the header line
 * "Authorization: Bearer <token>" scrambled: each byte XORed with a xorshift stream whose seed is four numbers there and one
 * here, then stored at a shuffled place among random bytes. It is put back together on the stack for one request only.
 */
#include "report.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "net.hpp"

#if __has_include("report_key.inc")
#include "report_key.inc"
#define REPORT_HAS_KEY 1
#else
#define REPORT_HAS_KEY 0
#endif

#include "platform.hpp"

namespace report {

bool available() { return REPORT_HAS_KEY != 0; }

namespace {

#if REPORT_HAS_KEY
/** The header line, put back together into out (size at least kLen + 1). */
void unscramble(char* out) {
    uint32_t x = key::kSeed[0] ^ key::kSeed[1] ^ key::kSeed[2] ^ key::kSeed[3] ^ 0x9E3779B9u;
    for (size_t i = 0; i < key::kLen; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = char(key::kData[key::kPlace[i]] ^ uint8_t(x >> 8));
    }
    out[key::kLen] = 0;
}
#endif

/** A JSON string's contents (quotes, backslashes and control characters escaped; UTF-8 as it is). */
[[maybe_unused]] std::string esc(const std::string& s) {   // (a build without the key sends nothing)
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

std::string stamp(long long t) {
    const time_t tt = time_t(t);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", localtime(&tt));
    return b;
}

/** A crash report's line after "<key>:", trimmed ("" if none). */
std::string field(const std::string& text, const char* key) {
    const size_t k = text.find(key);
    if (k == std::string::npos) return {};
    size_t a = k + strlen(key), b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\r')) b--;
    return text.substr(a, b - a);
}

}  // namespace

System system() {
    System s;
    plat::sysInfo(s.firmware, s.atmosphere, s.docked);
    if (s.firmware.empty()) s.firmware = "unknown";
    if (s.atmosphere.empty()) s.atmosphere = "unknown";
    return s;
}

std::vector<Crash> crashes(size_t max) {
    std::vector<Crash> out;
    auto scan = [&](const std::string& dir, bool fatal) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() < 5 || n[0] == '.') continue;
            // crash_reports: the text reports only (Atmosphère keeps more beside each one: read alone they were a second
            // "A program · No details" line at every crash, seen on the console, 2026-10-30)
            if (!fatal && n.compare(n.size() - 4, 4, ".log") != 0) continue;
            const std::string p = dir + "/" + n;
            struct stat st;
            if (stat(p.c_str(), &st) != 0 || (st.st_mode & S_IFMT) == S_IFDIR) continue;
            Crash c;
            c.fatal = fatal;
            // crash_reports/<seconds>_<program id>.log; else (fatal_errors' report_<…>.bin) the file's own time
            c.at = !fatal && n[0] >= '0' && n[0] <= '9' ? atoll(n.c_str()) : (long long)st.st_mtime;
            if (c.at <= 0) c.at = (long long)st.st_mtime;
            c.when = stamp(c.at);
            if (fatal) {
                c.program = "A fatal error (" + n + ")";
                c.result = "Shown by Atmosphère's fatal screen";
            } else {
                std::string text(4096, '\0');
                if (FILE* f = fopen(p.c_str(), "rb")) { text.resize(fread(&text[0], 1, text.size(), f)); fclose(f); }
                else text.clear();
                const std::string name = field(text, "Process Name:"), id = field(text, "Program ID:");
                // (Atmosphère names any program in application mode "Application": a game, or homebrew run as one)
                const std::string who = name.empty() ? "A program" : name == "Application" ? "An app or game" : name;
                c.program = who + (id.empty() ? std::string() : " (" + id + ")");
                std::string r = field(text, "Result:");
                if (r.empty()) r = field(text, "Crash Type:");
                c.result = r.empty() ? std::string("No details") : "Result " + r;
            }
            out.push_back(std::move(c));
        }
        closedir(d);
    };
#ifndef GF_WINDOWED
    scan("sdmc:/atmosphere/crash_reports", false);
    scan("sdmc:/atmosphere/fatal_errors", true);
#else
    (void)scan;
#endif
    std::sort(out.begin(), out.end(), [](const Crash& a, const Crash& b) { return a.at > b.at; });
    if (out.size() > max) out.resize(max);
    return out;
}

bool send(const std::string& title, const std::string& body, const char* label, int& number, std::string& err) {
    number = 0;
#if REPORT_HAS_KEY
    const std::string json = "{\"title\":\"" + esc(title) + "\",\"body\":\"" + esc(body) + "\",\"labels\":[\"" + esc(label) + "\"]}";
    char secret[key::kLen + 1];
    unscramble(secret);
    std::string answer;
    const bool ok = net::post(std::string("https://api.github.com/repos/") + key::kRepo + "/issues", json, secret, answer, err);   // (wipes it)
    { volatile char* d = secret; for (size_t i = 0; i <= key::kLen; i++) d[i] = 0; }
    if (!ok) return false;
    const size_t k = answer.find("\"number\"");
    if (k != std::string::npos) {
        const size_t c = answer.find(':', k);
        if (c != std::string::npos) number = atoi(answer.c_str() + c + 1);
    }
    return true;
#else
    (void)title; (void)body; (void)label;
    err = "This build cannot send reports";
    return false;
#endif
}

}  // namespace report
