/*
 * Gatefold NX · m3u.cpp
 */
#include "m3u.hpp"
#include "platform.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace {

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; k++)
            if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

std::string latin1ToUtf8(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) out += char(c);
        else { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

/** file:///C:/Music/a%20b.mp3 → C:/Music/a b.mp3 */
std::string fromUrl(std::string p) {
    if (p.compare(0, 7, "file://") != 0) return p;
    p = p.substr(7);
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);   // file:///C:/…
    std::string out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size() && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            out += char(strtol(p.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += p[i];
        }
    }
    return out;
}

std::string clean(std::string s) {   // a tab or a line break would break the line it goes on
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

}  // namespace

bool readM3u(const std::string& file, std::vector<M3uLine>& out) {
    out.clear();
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a BOM
    if (!validUtf8(text)) text = latin1ToUtf8(text);                 // an old .m3u in the PC's code page
    std::string title;
    size_t at = 0;
    while (at <= text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(at, eol - at);
        at = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        line.erase(0, lead);
        if (line.empty()) { if (eol == text.size()) break; continue; }
        if (line[0] == '#') {
            // #EXTINF:<seconds>,<Artist - Title>
            if (line.compare(0, 8, "#EXTINF:") == 0) {
                const size_t comma = line.find(',');
                title = comma == std::string::npos ? std::string() : line.substr(comma + 1);
            }
        } else {
            std::string p = fromUrl(line);
            for (char& c : p) if (c == '\\') c = '/';
            out.push_back({ p, title });
            title.clear();
        }
        if (eol == text.size()) break;
    }
    return true;
}

std::string normalisePath(const std::string& path) {
    std::string head, rest = path;
    if (const size_t n = plat::rootLen(rest)) { head = rest.substr(0, n); rest = rest.substr(n); }
    else if (!rest.empty() && rest[0] == '/') { head = "/"; rest = rest.substr(1); }
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= rest.size()) {
        size_t s = rest.find('/', at);
        if (s == std::string::npos) s = rest.size();
        const std::string part = rest.substr(at, s - at);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        at = s + 1;
    }
    std::string out = head;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
    return out;
}

bool writeM3u(const std::string& file, const std::string& name, const std::vector<M3uTrack>& tracks) {
    const size_t slash = file.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : file.substr(0, slash);
    mkdir(dir.c_str(), 0777);
    // Relative from the file's folder when they share the folder above it (the music folder), else from the card's root
    const size_t up = dir.find_last_of('/');
    const std::string root = up == std::string::npos ? dir : dir.substr(0, up);
    std::string out = "#EXTM3U\n#PLAYLIST:" + clean(name) + "\n";
    for (const M3uTrack& t : tracks) {
        std::string p = t.path;
        if (p.size() > dir.size() && p.compare(0, dir.size() + 1, dir + "/") == 0) p = p.substr(dir.size() + 1);
        else if (p.size() > root.size() && p.compare(0, root.size() + 1, root + "/") == 0) p = "../" + p.substr(root.size() + 1);
        else if (const size_t n = plat::rootLen(p)) p = p.substr(n - 1);
        const std::string who = t.artist.empty() ? t.title : t.artist + " - " + t.title;
        char head[32];
        snprintf(head, sizeof head, "#EXTINF:%d,", t.seconds > 0 ? int(std::lround(t.seconds)) : -1);
        out += head + clean(who) + "\n" + clean(p) + "\n";
    }
    const std::string tmp = file + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;   // a full SD card can fail only here: the old file stays
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(file.c_str());
    return rename(tmp.c_str(), file.c_str()) == 0;
}
