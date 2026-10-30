/*
 * Gatefold NX · updater.cpp
 * See updater.hpp. Self-contained on purpose: a small JSON reader, SHA-256, CRC-32 and an inflater for the release
 * zips (stored and deflated entries, as every zip tool writes them), so the PC preview needs no extra library.
 */
#include "updater.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "automix.hpp"
#include "cores.hpp"

#ifdef __SWITCH__
#include <switch.h>
#include "gfbg.h"
#define GF_PROTOCOL GFBG_PROTOCOL
#else
#define GF_PROTOCOL 6   // gfbg.h's GFBG_PROTOCOL (the PC preview builds without it, as thumbs.cpp)
#endif

#ifndef GF_VERSION
#define GF_VERSION "2.0.0 BETA"
#endif

namespace {

constexpr const char* kApi = "https://api.github.com/repos/AndryTheBeast/Songate/releases?per_page=30";
// Automix's models (the open-source model repository): its latest release's assets, by GitHub's own redirect (no API call)
constexpr const char* kModels = "https://github.com/AndryTheBeast/songate-automix/releases/latest/download/";
constexpr const char* kModelsManifest = "songate-automix-models.json";
constexpr const char* kManifest = "songate-manifest.json";
constexpr const char* kSysDir = "atmosphere/contents/4200000047464E58/";   // the background player's: the same ID as before
constexpr const char* kNro = "switch/Songate/Songate.nro";
constexpr const char* kOverlay = "switch/.overlays/Songate.ovl";
// Before the rebranding (1.x, as Gatefold NX): where its files are, for going back to one, and cleared after (cleanLegacy)
constexpr const char* kNroV1 = "switch/Gatefold/Gatefold.nro";
constexpr const char* kOverlayV1 = "switch/.overlays/Gatefold.ovl";
bool isNro(const std::string& p) { return p == kNro || p == kNroV1; }

/** The only places an update may write, from the SD card's root (a manifest naming anything else is refused). */
bool allowed(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find("..") != std::string::npos || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
        return false;
    return isNro(p) || p == kOverlay || p == kOverlayV1 || p.rfind(kSysDir, 0) == 0;
}

/** 1.0.0 and 1.1.0: no manifest. Their zips (GitHub's digests agree) and the background player's protocol. */
struct Legacy { const char* version; const char* sha; int64_t size; int protocol; };
constexpr Legacy kLegacy[] = {
    { "1.0.0", "7f90fad29c0ceb2cdde061067a8d360b9f2a473d037a7a4fcef1e80f30fdebf0", 12843254, 3 },
    { "1.1.0", "13b2cb7efd70af0242350b85bd8de6ea9e0922a67dd471dd69d21dcad86aab3d", 12914951, 4 },
};
const char* kLegacyFiles[] = { kNroV1, kOverlayV1, "atmosphere/contents/4200000047464E58/exefs.nsp",
                               "atmosphere/contents/4200000047464E58/toolbox.json", "atmosphere/contents/4200000047464E58/flags/boot2.flag" };

// ------------------------------------------------------------------ files

std::string join(const std::string& a, const std::string& b) { return a.empty() || a.back() == '/' ? a + b : a + "/" + b; }
bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
int64_t fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? int64_t(st.st_size) : -1; }

void mkdirs(const std::string& path) {   // every folder of path (a folder)
    for (size_t i = path.find('/', 6); ; i = path.find('/', i + 1)) {
        mkdir(path.substr(0, i).c_str(), 0777);
        if (i == std::string::npos) break;
    }
}

std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool readAll(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool writeAll(const std::string& p, const void* data, size_t len) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(p.c_str());
    return ok;
}
bool writeAll(const std::string& p, const std::string& s) { return writeAll(p, s.data(), s.size()); }

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (size_t n; (n = fread(buf.data(), 1, buf.size(), in)) > 0;)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to.c_str());
    return ok;
}

/** Copies a folder, leaving out the names in skip (at its top only). */
bool copyTree(const std::string& from, const std::string& to, const std::vector<std::string>& skip, int64_t& bytes, int& files) {
    mkdirs(to);
    bool ok = true;
    for (const auto& n : names(from)) {
        if (std::find(skip.begin(), skip.end(), n) != skip.end()) continue;
        const std::string a = join(from, n), b = join(to, n);
        if (isDir(a)) ok = copyTree(a, b, {}, bytes, files) && ok;
        else if (copyFile(a, b)) { bytes += std::max<int64_t>(0, fileSize(b)); files++; }
        else ok = false;
    }
    return ok;
}

bool removeTree(const std::string& p) {
    if (!isDir(p)) return remove(p.c_str()) == 0;
    bool ok = true;
    for (const auto& n : names(p)) ok = removeTree(join(p, n)) && ok;
    return rmdir(p.c_str()) == 0 && ok;
}

void measure(const std::string& p, int64_t& bytes, int& files) {
    for (const auto& n : names(p)) {
        const std::string a = join(p, n);
        if (isDir(a)) measure(a, bytes, files);
        else { bytes += std::max<int64_t>(0, fileSize(a)); files++; }
    }
}

/** Renames over a file that may exist (FAT does not rename onto one). The old one is kept as .gfold until the new one is in. */
bool swapIn(const std::string& target) {
    const std::string fresh = target + ".gfnew", old = target + ".gfold";
    if (!exists(fresh)) return true;
    remove(old.c_str());
    const bool had = exists(target);
    if (had && rename(target.c_str(), old.c_str()) != 0) return false;   // in use: it waits
    if (rename(fresh.c_str(), target.c_str()) != 0) {
        if (had) rename(old.c_str(), target.c_str());
        return false;
    }
    remove(old.c_str());
    return true;
}

// ------------------------------------------------------------------ JSON (just what GitHub's answers need)

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json& operator[](const char* k) const {
        static const Json none;
        for (const auto& [key, v] : obj) if (key == k) return v;
        return none;
    }
    std::string s(const char* k) const { const Json& v = (*this)[k]; return v.kind == Str ? v.str : std::string(); }
    int64_t i(const char* k) const { const Json& v = (*this)[k]; return v.kind == Num ? int64_t(v.num) : 0; }
};

struct JsonReader {
    const char* p;
    const char* end;
    int depth = 0;
    bool fail = false;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    uint32_t hex4() {
        if (end - p < 4) { fail = true; return 0; }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = *p++;
            v = v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    std::string string() {
        std::string out;
        p++;   // the opening quote
        while (p < end && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= end) break;
            const char e = *p++;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t c = hex4();
                if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // a surrogate pair
                    p += 2;
                    const uint32_t lo = hex4();
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, c);
                break;
            }
            default: out += e;
            }
        }
        if (p >= end) fail = true; else p++;
        return out;
    }
    Json value() {
        Json v;
        ws();
        if (p >= end || ++depth > 64) { fail = true; return v; }
        if (*p == '{') {
            v.kind = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}') p++;
            else
                while (!fail) {
                    ws();
                    if (p >= end || *p != '"') { fail = true; break; }
                    std::string k = string();
                    ws();
                    if (p >= end || *p != ':') { fail = true; break; }
                    p++;
                    v.obj.emplace_back(std::move(k), value());
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    fail = true;
                }
        } else if (*p == '[') {
            v.kind = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']') p++;
            else
                while (!fail) {
                    v.arr.push_back(value());
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    fail = true;
                }
        } else if (*p == '"') {
            v.kind = Json::Str;
            v.str = string();
        } else if (end - p >= 4 && !strncmp(p, "true", 4)) { v.kind = Json::Bool; v.b = true; p += 4; }
        else if (end - p >= 5 && !strncmp(p, "false", 5)) { v.kind = Json::Bool; p += 5; }
        else if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; }
        else {
            char* e = nullptr;
            const std::string num(p, size_t(std::min<ptrdiff_t>(end - p, 40)));
            v.num = strtod(num.c_str(), &e);
            if (e == num.c_str()) { fail = true; return v; }
            v.kind = Json::Num;
            p += e - num.c_str();
        }
        depth--;
        return v;
    }
};

bool parseJson(const std::string& text, Json& out) {
    JsonReader r{ text.data(), text.data() + text.size() };
    out = r.value();
    return !r.fail;
}

// ------------------------------------------------------------------ SHA-256 and CRC-32

struct Sha256 {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t buf[64];
    size_t used = 0;
    uint64_t len = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* d) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(d[i * 4]) << 24 | uint32_t(d[i * 4 + 1]) << 16 | uint32_t(d[i * 4 + 2]) << 8 | d[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const void* data, size_t n) {
        const uint8_t* d = static_cast<const uint8_t*>(data);
        len += n;
        while (n) {
            const size_t take = std::min(n, 64 - used);
            memcpy(buf + used, d, take);
            used += take; d += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = len * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (used != 56) add(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        add(l, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

std::string shaFile(const std::string& p, net::Progress* prog = nullptr) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    Sha256 s;
    std::vector<uint8_t> buf(256 * 1024);
    for (size_t n; (n = fread(buf.data(), 1, buf.size(), f)) > 0;) {
        s.add(buf.data(), n);
        if (prog) prog->done += int64_t(n);
    }
    fclose(f);
    return s.hex();
}

uint32_t crc32(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------ inflate (RFC 1951), after zlib's puff.c

struct Inflater {
    const uint8_t* in;
    size_t inLen, pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool fail = false;
    std::vector<uint8_t>& out;
    struct Huff { uint16_t count[16], symbol[320]; };

    int bits(int need) {
        uint32_t v = bitBuf;
        while (bitCnt < need) {
            if (pos >= inLen) { fail = true; return 0; }
            v |= uint32_t(in[pos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = v >> need;
        bitCnt -= need;
        return int(v & ((1u << need) - 1));
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (fail) return -1;
        }
        fail = true;
        return -1;
    }
    static void build(Huff& h, const short* length, int n) {
        memset(h.count, 0, sizeof h.count);
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        h.count[0] = 0;
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = short(offs[len] + h.count[len]);
        for (int s = 0; s < n; s++) if (length[s]) h.symbol[offs[length[s]]++] = uint16_t(s);
    }
    bool codes(const Huff& lit, const Huff& dist) {
        static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                         4097, 6145, 8193, 12289, 16385, 24577 };
        static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;) {
            int sym = decode(lit);
            if (fail || sym < 0) return false;
            if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            const int len = lbase[sym] + bits(lext[sym]);
            const int ds = decode(dist);
            if (fail || ds < 0 || ds >= 30) return false;
            const size_t d = size_t(dbase[ds] + bits(dext[ds]));
            if (fail || d > out.size()) return false;
            const size_t from = out.size() - d;
            for (int k = 0; k < len; k++) out.push_back(out[from + size_t(k)]);
        }
    }
    bool stored() {
        bitBuf = 0;
        bitCnt = 0;
        if (pos + 4 > inLen) return false;
        const unsigned len = in[pos] | in[pos + 1] << 8, nlen = in[pos + 2] | in[pos + 3] << 8;
        pos += 4;
        if (len != (~nlen & 0xffff) || pos + len > inLen) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }
    bool fixed() {
        static Huff lit, dist;
        static bool made = false;
        if (!made) {
            short l[288];
            for (int s = 0; s < 144; s++) l[s] = 8;
            for (int s = 144; s < 256; s++) l[s] = 9;
            for (int s = 256; s < 280; s++) l[s] = 7;
            for (int s = 280; s < 288; s++) l[s] = 8;
            build(lit, l, 288);
            for (int s = 0; s < 30; s++) l[s] = 5;
            build(dist, l, 30);
            made = true;
        }
        return codes(lit, dist);
    }
    bool dynamic() {
        static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (fail || nlen > 286 || ndist > 30) return false;
        short lengths[320] = {};
        for (int k = 0; k < ncode; k++) lengths[order[k]] = short(bits(3));
        Huff lencode, lit, dist;
        build(lencode, lengths, 19);
        int idx = 0;
        while (idx < nlen + ndist) {
            int sym = decode(lencode);
            if (fail || sym < 0) return false;
            if (sym < 16) { lengths[idx++] = short(sym); continue; }
            short len = 0;
            int rep;
            if (sym == 16) {
                if (idx == 0) return false;
                len = lengths[idx - 1];
                rep = 3 + bits(2);
            } else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (idx + rep > nlen + ndist) return false;
            while (rep--) lengths[idx++] = len;
        }
        if (lengths[256] == 0) return false;
        build(lit, lengths, nlen);
        build(dist, lengths + nlen, ndist);
        return codes(lit, dist);
    }
    bool run() {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (fail) return false;
            const bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

// ------------------------------------------------------------------ zip (no zip64: the releases are ~13 MB)

struct ZipEntry { std::string name; int method = 0; uint32_t crc = 0; uint32_t csize = 0, usize = 0, local = 0; };

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool zipList(FILE* f, std::vector<ZipEntry>& out) {
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long tail = std::min<long>(size, 65536 + 22);
    std::vector<uint8_t> t;
    t.resize(size_t(tail));
    fseek(f, size - tail, SEEK_SET);
    if (fread(t.data(), 1, t.size(), f) != t.size()) return false;
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) if (le32(&t[size_t(i)]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return false;
    const uint16_t count = le16(&t[size_t(eocd) + 10]);
    const uint32_t cdSize = le32(&t[size_t(eocd) + 12]), cdAt = le32(&t[size_t(eocd) + 16]);
    if (long(cdAt) + long(cdSize) > size) return false;
    std::vector<uint8_t> cd(cdSize);
    fseek(f, long(cdAt), SEEK_SET);
    if (fread(cd.data(), 1, cd.size(), f) != cd.size()) return false;
    size_t p = 0;
    for (int i = 0; i < count; i++) {
        if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) return false;
        ZipEntry e;
        e.method = le16(&cd[p + 10]);
        e.crc = le32(&cd[p + 16]);
        e.csize = le32(&cd[p + 20]);
        e.usize = le32(&cd[p + 24]);
        const uint16_t n = le16(&cd[p + 28]), x = le16(&cd[p + 30]), c = le16(&cd[p + 32]);
        e.local = le32(&cd[p + 42]);
        if (p + 46 + n > cd.size()) return false;
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), n);
        for (char& ch : e.name) if (ch == '\\') ch = '/';
        out.push_back(std::move(e));
        p += 46 + size_t(n) + x + c;
    }
    return true;
}

bool zipRead(FILE* f, const ZipEntry& e, std::vector<uint8_t>& out, std::string& err) {
    uint8_t lh[30];
    fseek(f, long(e.local), SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50) { err = "The zip is damaged"; return false; }
    fseek(f, long(e.local) + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
    std::vector<uint8_t> comp(e.csize);
    if (fread(comp.data(), 1, comp.size(), f) != comp.size()) { err = "The zip is cut short"; return false; }
    out.clear();
    if (e.method == 0) out = std::move(comp);
    else if (e.method == 8) {
        out.reserve(e.usize);
        Inflater inf{ comp.data(), comp.size(), 0, 0, 0, false, out };
        if (!inf.run()) { err = "Could not unpack " + e.name; return false; }
    } else { err = "The zip packs " + e.name + " in a way Songate cannot read"; return false; }
    if (out.size() != e.usize || crc32(out.data(), out.size()) != e.crc) { err = e.name + " did not unpack correctly"; return false; }
    return true;
}

// ------------------------------------------------------------------ releases

int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/** The next UTF-8 code point at s[i] (advancing i). */
uint32_t nextCp(const std::string& s, size_t& i) {
    const uint8_t c = uint8_t(s[i]);
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    uint32_t cp = n == 0 ? c : n == 1 ? c & 31 : n == 2 ? c & 15 : c & 7;
    i++;
    while (n-- > 0 && i < s.size()) cp = cp << 6 | (uint8_t(s[i++]) & 63);
    return cp;
}

/** Emoji Inter and the console's fonts cannot draw: picture characters, their joiners and variation selectors. */
bool isEmoji(uint32_t cp) {
    return cp >= 0x1F000 || (cp >= 0x2300 && cp <= 0x2BFF) || cp == 0xFE0F || cp == 0x200D || cp == 0x21E7 || cp == 0x2328;
}

/** An emoji as one of Gatefold's icons (by name; App::changelogIcon draws it). */
const char* emojiIcon(uint32_t cp) {
    switch (cp) {
    case 0x2728: return "sparkle";
    case 0x1F527: case 0x1F6E0: return "fix";
    case 0x2B07: case 0x1F4E5: return "download";
    case 0x1F3B5: case 0x1F3B6: return "music";
    case 0x1F3A7: return "headphones";
    case 0x1F9E9: return "game";
    case 0x1F3A8: return "palette";
    case 0x1F3A4: return "mic";
    case 0x1F9FE: case 0x1F4CB: return "list";
    case 0x1F4CA: case 0x1F4C8: return "history";
    case 0x1F319: return "moon";
    case 0x1F506: case 0x2600: return "sun";
    case 0x1F50A: case 0x1F509: return "volume";
    case 0x1F50B: case 0x26A1: return "bolt";
    case 0x1F500: return "shuffle";
    case 0x1F5C2: case 0x1F4C1: case 0x1F4C2: return "folder";
    case 0x1F3AE: case 0x1F579: return "game";
    case 0x1F648: case 0x2139: return "info";
    case 0x2764: case 0x1F496: return "heart";
    case 0x1F50D: case 0x1F50E: return "search";
    case 0x1F4BF: case 0x1F4C0: return "disc";
    case 0x23F1: case 0x23F3: return "hourglass";
    case 0x1F5BC: case 0x1F4F7: return "image";
    case 0x1F39A: case 0x1F39B: return "eq";
    case 0x1F512: case 0x1F6E1: return "shield";
    case 0x1F504: case 0x1F501: return "refresh";
    case 0x1F4DA: case 0x1F4D6: return "book";
    case 0x270F: case 0x1F3F7: return "tag";
    case 0x1F5D1: case 0x274C: return "x";
    default: return nullptr;
    }
}

/** A line of Markdown as plain text: emoji out (the first one's icon kept), links and emphasis unwrapped. */
std::string plainLine(const std::string& md, std::string* icon) {
    std::string out;
    for (size_t i = 0; i < md.size();) {
        const size_t at = i;
        const uint32_t cp = nextCp(md, i);
        if (isEmoji(cp)) {
            if (icon && icon->empty()) if (const char* n = emojiIcon(cp)) *icon = n;
            continue;
        }
        if (cp == 0x2192) { out += "\u203A"; continue; }   // Inter has no →: ›
        out.append(md, at, i - at);
    }
    std::string clean;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '*' || out[i] == '`' || (out[i] == '_' && i + 1 < out.size() && out[i + 1] == '_')) continue;
        if (out[i] == '!' && i + 1 < out.size() && out[i + 1] == '[') continue;
        if (out[i] == '[') {   // [text](url) → text
            const size_t close = out.find("](", i);
            const size_t paren = close == std::string::npos ? close : out.find(')', close);
            if (paren != std::string::npos) { clean += out.substr(i + 1, close - i - 1); i = paren; continue; }
        }
        clean += out[i];
    }
    while (!clean.empty() && (clean[0] == ' ' || clean[0] == '\t')) clean.erase(0, 1);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\r')) clean.pop_back();
    return clean;
}

/** The notes of a release without a manifest, as sections: each "## " heading with its "- " items (install steps and tables left out). */
std::vector<UpdLog> logFromNotes(const std::string& body) {
    std::vector<UpdLog> out;
    size_t a = 0;
    UpdLog* cur = nullptr;
    while (a <= body.size()) {
        size_t b = body.find('\n', a);
        if (b == std::string::npos) b = body.size();
        std::string line = body.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0 || line.rfind("### ", 0) == 0) {
            std::string icon;
            const std::string t = plainLine(line.substr(line.find(' ') + 1), &icon);
            std::string lt;
            for (char c : t) lt += char(lower(c));
            if (lt.find("install") != std::string::npos) { cur = nullptr; continue; }   // how to install by hand: not news
            out.push_back({ icon.empty() ? "sparkle" : icon, t, {} });
            cur = &out.back();
        } else if (cur && (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)) {
            std::string icon;
            std::string t = plainLine(line.substr(2), &icon);
            if (!t.empty()) cur->items.push_back({ icon, t });
        }
        if (b == body.size()) break;
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const UpdLog& l) { return l.items.empty(); }), out.end());
    return out;
}

/** gatefold-manifest.json → the release's own fields. */
bool readManifest(const std::string& text, UpdRelease& r) {
    Json m;
    if (!parseJson(text, m) || m.kind != Json::Obj) return false;
    if (m.i("format") > 1) return false;   // a newer manifest than this Gatefold knows: treated as none
    r.manifest = true;
    r.protocol = int(m.i("protocol"));
    if (!m.s("version").empty()) r.version = Updater::pretty(m.s("version"));
    const Json& zip = m["zip"];
    if (zip.kind == Json::Obj) {
        if (!zip.s("name").empty()) r.zipName = zip.s("name");
        if (zip.i("size")) r.zipSize = zip.i("size");
        if (!zip.s("sha256").empty()) r.zipSha = zip.s("sha256");
    }
    r.files.clear();
    for (const Json& f : m["files"].arr) {
        UpdFile u{ f.s("path"), f.i("size"), f.s("sha256") };
        if (allowed(u.path) && u.sha256.size() == 64) r.files.push_back(u);
    }
    r.log.clear();
    for (const Json& s : m["changelog"].arr) {
        UpdLog l{ s.s("icon"), s.s("title"), {} };
        // An item is a line (a leading emoji becomes its icon) or { "icon": …, "text": … }
        for (const Json& it : s["items"].arr) {
            UpdItem u;
            if (it.kind == Json::Str) u.text = plainLine(it.str, &u.icon);
            else if (it.kind == Json::Obj) { u.icon = it.s("icon"); u.text = plainLine(it.s("text"), u.icon.empty() ? &u.icon : nullptr); }
            if (!u.text.empty()) l.items.push_back(u);
        }
        if (!l.items.empty()) r.log.push_back(l);
    }
    return true;
}

std::vector<UpdRelease> readReleases(const std::string& api) {
    std::vector<UpdRelease> out;
    Json list;
    if (!parseJson(api, list) || list.kind != Json::Arr) return out;
    for (const Json& j : list.arr) {
        if (j["draft"].b) continue;
        UpdRelease r;
        r.tag = j.s("tag_name");
        // in the app's own spelling: the tag v2.0.0-beta.1 is "2.0.0 BETA 1", as a beta's GF_VERSION says it
        r.version = Updater::pretty(r.tag.size() > 1 && (r.tag[0] == 'v' || r.tag[0] == 'V') ? r.tag.substr(1) : r.tag);
        r.beta = j["prerelease"].b || Updater::isBeta(r.version);   // (listed apart: Settings › Updates › Beta versions)
        r.name = j.s("name");
        r.date = j.s("published_at").substr(0, 10);
        r.body = j.s("body");
        for (const Json& a : j["assets"].arr) {
            const std::string n = a.s("name");
            if (n.size() > 4 && n.substr(n.size() - 4) == ".zip" && r.zipUrl.empty()) {
                r.zipName = n;
                r.zipUrl = a.s("browser_download_url");
                r.zipSize = a.i("size");
                const std::string d = a.s("digest");   // "sha256:…" (GitHub fills it in for assets)
                if (d.rfind("sha256:", 0) == 0) r.zipSha = d.substr(7);
            }
        }
        if (r.zipUrl.empty()) continue;   // nothing to install
        for (const Legacy& l : kLegacy)
            if (r.version == l.version) {
                r.protocol = l.protocol;
                if (r.zipSha.empty()) r.zipSha = l.sha;
            }
        for (const char* f : kLegacyFiles) r.files.push_back({ f, 0, {} });
        r.log = logFromNotes(r.body);
        out.push_back(std::move(r));
    }
    return out;
}

/** songate-automix-models.json → the models it lists (a newer format than this Songate knows: none). */
std::vector<UpdModel> readModels(const std::string& text) {
    std::vector<UpdModel> out;
    Json m;
    if (!parseJson(text, m) || m.kind != Json::Obj || m.i("format") > 1) return out;
    for (const Json& j : m["models"].arr) {
        UpdModel x;
        x.name = j.s("name");
        x.file = j.s("file");
        x.sha256 = j.s("sha256");
        x.date = j.s("date");
        if (!j.s("app").empty()) x.app = j.s("app");
        x.size = j.i("size");
        x.format = int(j.i("format"));
        x.mixan = int(j.i("mixan"));
        for (const Json& n : j["notes"].arr) if (n.kind == Json::Str) x.notes.push_back(n.str);
        // a plain file name only: it is written into config/gatefold-nx/models/
        const bool plain = !x.file.empty() && x.file.find_first_of("/\\:") == std::string::npos && x.file[0] != '.' &&
                           x.file.size() > 6 && x.file.compare(x.file.size() - 6, 6, ".model") == 0;
        if (!x.name.empty() && plain && x.sha256.size() == 64) out.push_back(std::move(x));
    }
    return out;
}

std::string manifestUrl(const UpdRelease& r) {
    // A release's assets all share its download folder
    const size_t s = r.zipUrl.find_last_of('/');
    return s == std::string::npos ? std::string() : r.zipUrl.substr(0, s + 1) + kManifest;
}

std::string stamp() {
    const time_t t = time(nullptr);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H.%M.%S", localtime(&t));
    return b;
}

/** Settings → Updates' notes for the last install, as key=value lines. */
std::string readKey(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        if (text.compare(a, k.size(), k) == 0) {
            std::string v = text.substr(a + k.size(), b - a - k.size());
            if (!v.empty() && v.back() == '\r') v.pop_back();
            return v;
        }
        a = b + 1;
    }
    return {};
}

/** Where a file of a release goes: the app's own .nro where this one runs (wherever the user keeps it); the .nro of a release
 *  under the other name (going back to 1.x, Gatefold NX) in that one's own place: this one goes when the app closes. */
std::string sdPath(const std::string& rel, const std::string& self) { return rel == kNro ? self : "sdmc:/" + rel; }

/** When the console last started, in time() seconds: a different one means it restarted (the background player with it). */
long long bootStamp() {
#ifdef __SWITCH__
    return (long long)time(nullptr) - (long long)(armTicksToNs(armGetSystemTick()) / 1000000000ull);
#else
    static const long long at = (long long)time(nullptr);   // the preview: the run is the "boot"
    return at;
#endif
}

}  // namespace

// ================================================================== Updater

std::string Updater::thisVersion() { return GF_VERSION; }
int Updater::thisProtocol() { return GF_PROTOCOL; }

namespace {

/** A version taken apart: "2.0.0 BETA 10" and the tag's "2.0.0-beta.10" give the same parts. */
struct VerParts {
    int num[3] = {};
    std::string label;   // after the numbers, in capitals ("BETA"); empty: the release itself
    int n = 0;           // the label's number (BETA 10: 10; none: 0)
    bool ok = false;     // nothing left over that these parts do not say
};

VerParts verParts(const std::string& v) {
    VerParts p;
    int used = 0;
    if (sscanf(v.c_str(), "%d.%d.%d%n", &p.num[0], &p.num[1], &p.num[2], &used) != 3) return p;
    size_t i = size_t(used);
    const auto sep = [&] { while (i < v.size() && (v[i] == ' ' || v[i] == '-' || v[i] == '.' || v[i] == '_')) i++; };
    sep();
    while (i < v.size() && isalpha((unsigned char)v[i])) p.label += char(toupper((unsigned char)v[i++]));
    sep();
    if (i < v.size() && isdigit((unsigned char)v[i])) {
        p.n = atoi(v.c_str() + i);
        while (i < v.size() && isdigit((unsigned char)v[i])) i++;
    }
    p.ok = i == v.size() && (!p.label.empty() || p.n == 0);
    return p;
}

int labelRank(const std::string& l) { return l == "ALPHA" ? 0 : l == "BETA" ? 1 : l == "RC" ? 2 : 3; }

}  // namespace

int Updater::compare(const std::string& a, const std::string& b) {
    const VerParts x = verParts(a), y = verParts(b);
    for (int i = 0; i < 3; i++) if (x.num[i] != y.num[i]) return x.num[i] < y.num[i] ? -1 : 1;
    // the same numbers: one with something after them ("2.0.0 BETA 1") comes before the release itself ("2.0.0"), so a beta
    // is offered the release when it is out; betas by their number as a number (BETA 10 after BETA 2)
    if (x.label.empty() != y.label.empty()) return x.label.empty() ? 1 : -1;
    if (x.label != y.label) {
        const int rx = labelRank(x.label), ry = labelRank(y.label);
        if (rx != ry) return rx < ry ? -1 : 1;
        return x.label < y.label ? -1 : 1;
    }
    return x.n != y.n ? (x.n < y.n ? -1 : 1) : 0;
}

std::string Updater::pretty(const std::string& v) {
    const VerParts p = verParts(v);
    if (!p.ok || p.label.empty()) return v;
    char b[48];
    snprintf(b, sizeof b, "%d.%d.%d", p.num[0], p.num[1], p.num[2]);
    return std::string(b) + " " + p.label + (p.n > 0 ? " " + std::to_string(p.n) : std::string());
}

bool Updater::isBeta(const std::string& v) {
    int x[3] = {}, n = 0;
    return sscanf(v.c_str(), "%d.%d.%d%n", &x[0], &x[1], &x[2], &n) == 3 && n > 0 && size_t(n) < v.size();
}

int Updater::newer(bool betas) const {
    int best = -1;
    for (int i = 0; i < int(releases.size()); i++)
        if ((betas || !releases[size_t(i)].beta) && compare(releases[size_t(i)].version, thisVersion()) > 0 && (best < 0 || compare(releases[size_t(i)].version, releases[size_t(best)].version) > 0))
            best = i;
    return best;
}

void Updater::start(const std::string& configDir, const std::string& self) {
    dir_ = configDir;
    self_ = self;
    readCache();
    readInstalled();
    if (readAll(dir_ + "/updates/restore.txt", restore_))
        while (!restore_.empty() && (restore_.back() == '\n' || restore_.back() == '\r')) restore_.pop_back();
}

void Updater::stop() {
    prog_.cancel = true;
    if (worker_.joinable()) worker_.join();
}

void Updater::readCache() {
    std::string api;
    if (!readAll(dir_ + "/updates/releases.json", api)) return;
    releases = readReleases(api);
    for (UpdRelease& r : releases) {
        std::string m;
        if (readAll(dir_ + "/updates/manifest-" + r.tag + ".json", m)) readManifest(m, r);
    }
    std::string when;
    if (readAll(dir_ + "/updates/checked.txt", when)) checkedAt = atoll(when.c_str());
    std::string mods;
    if (readAll(dir_ + "/updates/models.json", mods)) models = readModels(mods);
}

void Updater::readInstalled() {
    std::string t;
    installed = UpdInstalled{};
    if (!readAll(dir_ + "/updates/installed.txt", t)) return;
    installed.version = readKey(t, "version");
    installed.from = readKey(t, "from");
    installed.backup = readKey(t, "backup");
    // The background player's new file runs once it was restarted here, or the console was
    installed.bgChanged = readKey(t, "bgChanged") == "1" && readKey(t, "bgRestarted") != "1" &&
                          std::llabs(atoll(readKey(t, "boot").c_str()) - bootStamp()) < 120;
    installed.fresh = exists(dir_ + "/updates/fresh.txt");   // still to do when Gatefold closes
    std::string pend;
    installed.appPending = exists(self_ + ".gfnew") || (readAll(dir_ + "/updates/pending.txt", pend) && (pend[0] == '@' || pend.find("\n@") != std::string::npos));
    installed.overlayPending = exists(sdPath(kOverlay, self_) + ".gfnew");
    // Once this version is the one running and nothing waits, the note is history.
    if (installed.version == thisVersion() && !installed.appPending && !installed.overlayPending && !installed.bgChanged) installed = UpdInstalled{};
}

void Updater::run(std::function<void()> work) {   // the callers checked busy() and set the phase
    if (worker_.joinable()) worker_.join();
    prog_.cancel = false;
    prog_.done = prog_.total = 0;
    worker_ = std::thread([this, work] {
        cores::moveHere(cores::Net);
        work();
        std::lock_guard<std::mutex> g(m_);
        doneFlag_ = true;
    });
}

void Updater::check() {
    if (busy()) return;
    phase_ = Checking;
    run([this] { doCheck(); });
}

void Updater::install(int i, bool fresh, const std::string& backupRoot) {
    if (busy() || i < 0 || i >= int(releases.size())) return;
    working_ = releases[size_t(i)].version;
    phase_ = Downloading;
    run([this, r = releases[size_t(i)], fresh, backupRoot] { doInstall(r, fresh, backupRoot); });
}

bool Updater::poll() {
    justChecked = justInstalled = justModel = false;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!doneFlag_) return false;
        doneFlag_ = false;
        error = err_;
        if (checked_) {
            justChecked = true;
            if (err_.empty()) { releases = std::move(got_); checkedAt = int64_t(time(nullptr)); }
            if (gotModelsSet_) models = std::move(gotModels_);   // (the model list can come even when the releases did not)
        }
        if (installedOk_) justInstalled = true;
        if (modelOk_) justModel = true;
        checked_ = installedOk_ = modelOk_ = gotModelsSet_ = false;
        err_.clear();
        got_.clear();
        gotModels_.clear();
    }
    if (worker_.joinable()) worker_.join();
    phase_ = Idle;
    readInstalled();
    return true;
}

void Updater::doCheck() {
    std::string api, err;
    std::vector<UpdRelease> list;
    if (net::get(kApi, api, err, &prog_)) {
        list = readReleases(api);
        if (list.empty()) err = "GitHub's answer had no releases in it";
    }
    if (err.empty()) {
        mkdirs(dir_ + "/updates");
        writeAll(dir_ + "/updates/releases.json", api);
        writeAll(dir_ + "/updates/checked.txt", std::to_string((long long)time(nullptr)));
        // Each release's manifest (a few KB; not counted by GitHub's hourly limit for the API)
        for (UpdRelease& r : list) {
            if (prog_.cancel) break;
            std::string m, e;
            if (net::get(manifestUrl(r), m, e) && readManifest(m, r)) writeAll(dir_ + "/updates/manifest-" + r.tag + ".json", m);
        }
    }
    // Automix's models: the model repository's list (a missing one, as before it has a release, is no error)
    std::string mods, me;
    std::vector<UpdModel> models;
    const bool gotModels = !prog_.cancel && net::get(std::string(kModels) + kModelsManifest, mods, me);
    if (gotModels) {
        models = readModels(mods);
        mkdirs(dir_ + "/updates");
        writeAll(dir_ + "/updates/models.json", mods);
    }
    std::lock_guard<std::mutex> g(m_);
    checked_ = true;
    err_ = err;
    got_ = std::move(list);
    gotModels_ = std::move(models);
    gotModelsSet_ = gotModels;
}

bool Updater::modelUsable(const UpdModel& m, std::string* why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (m.format > automix::ModelFile::Format) return no("It needs a newer Songate: it is planned by logic this version does not have");
    if (m.mixan > mixan::Version) return no("It needs a newer Songate: it was trained on a newer analysis");
    if (compare(m.app, thisVersion()) > 0) return no(("It needs Songate " + m.app + " or newer").c_str());
    return true;
}

void Updater::installModel(int i, const std::string& modelsDir) {
    if (busy() || i < 0 || i >= int(models.size())) return;
    working_ = models[size_t(i)].name;
    phase_ = Downloading;
    run([this, m = models[size_t(i)], modelsDir] { doInstallModel(m, modelsDir); });
}

void Updater::doInstallModel(UpdModel m, std::string modelsDir) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> g(m_);
        err_ = why;
    };
    mkdirs(modelsDir);
    const std::string dst = modelsDir + "/" + m.file, tmp = dst + ".gfnew";
    phase_ = Downloading;
    prog_.done = 0;
    prog_.total = m.size;
    std::string err;
    if (!net::download(std::string(kModels) + m.file, tmp, err, &prog_)) { fail(err); return; }
    phase_ = Verifying;
    if ((m.size && fileSize(tmp) != m.size) || shaFile(tmp) != m.sha256) {
        remove(tmp.c_str());
        fail("The download does not match its checksum: try again");
        return;
    }
    // it must be a model this Songate plans, as the list said
    std::string text, why;
    automix::ModelFile f;
    if (!readAll(tmp, text) || !f.parse(text, &why) || f.name != m.name) {
        remove(tmp.c_str());
        fail(why.empty() ? "The download is not the model listed" : why);
        return;
    }
    remove(dst.c_str());
    if (rename(tmp.c_str(), dst.c_str()) != 0) { remove(tmp.c_str()); fail("Could not write to the SD card"); return; }
    std::lock_guard<std::mutex> g(m_);
    modelOk_ = true;
}

void Updater::doInstall(UpdRelease r, bool fresh, std::string backupRoot) {
    std::string err;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> g(m_);
        err_ = why;
    };
    const std::string up = dir_ + "/updates";
    mkdirs(up);
    const std::string zip = up + "/" + (r.zipName.empty() ? "release.zip" : r.zipName);
    if (r.zipSha.size() != 64) { fail("This release has no checksum to check the download against: it was not installed"); return; }
    for (const Legacy& l : kLegacy)   // GitHub's digest and Gatefold's own table must agree
        if (r.version == l.version && r.zipSha != l.sha) { fail("The release's checksum is not the one Songate knows: it was not installed"); return; }

    // ---- 1. the zip (kept from an earlier try when it is already whole and right)
    phase_ = Downloading;
    bool have = fileSize(zip) == r.zipSize && r.zipSize > 0;
    if (have) {
        phase_ = Verifying;
        prog_.done = 0;
        prog_.total = r.zipSize;
        have = shaFile(zip, &prog_) == r.zipSha;
    }
    if (!have) {
        phase_ = Downloading;
        prog_.done = 0;
        prog_.total = r.zipSize;
        if (!net::download(r.zipUrl, zip, err, &prog_)) { fail(err); return; }
        phase_ = Verifying;
        prog_.done = 0;
        prog_.total = r.zipSize;
        if (r.zipSize && fileSize(zip) != r.zipSize) { remove(zip.c_str()); fail("The download is not the size GitHub gives: try again"); return; }
        if (shaFile(zip, &prog_) != r.zipSha) { remove(zip.c_str()); fail("The download does not match its checksum: try again"); return; }
    }
    if (prog_.cancel) { fail("Stopped"); return; }

    // ---- 2. each file, unpacked next to where it goes as .gfnew
    phase_ = Unpacking;
    FILE* f = fopen(zip.c_str(), "rb");
    if (!f) { fail("Could not open the download"); return; }
    std::vector<ZipEntry> entries;
    if (!zipList(f, entries)) { fclose(f); fail("The download is not a zip Songate can read"); return; }
    std::vector<std::string> placed;   // the .gfnew files written, to take back on failure
    auto undo = [&] { for (const auto& p : placed) remove((p + ".gfnew").c_str()); };
    prog_.done = 0;
    prog_.total = int64_t(r.files.size());
    bool hasNro = false, bgChanged = false, overlayV1 = false;
    std::string movedTo;   // the app's .nro in another place than this one's (a release under the other name)
    for (const UpdFile& uf : r.files) {
        if (prog_.cancel) { fclose(f); undo(); fail("Stopped"); return; }
        auto it = std::find_if(entries.begin(), entries.end(), [&](const ZipEntry& e) { return e.name == uf.path; });
        if (it == entries.end()) {
            if (r.manifest || isNro(uf.path)) { fclose(f); undo(); fail(uf.path + " is missing from the zip"); return; }
            prog_.done++;
            continue;   // an old zip without that file: left as it is
        }
        std::vector<uint8_t> data;
        if (!zipRead(f, *it, data, err)) { fclose(f); undo(); fail(err); return; }
        if (!uf.sha256.empty()) {
            Sha256 s;
            s.add(data.data(), data.size());
            if ((uf.size && int64_t(data.size()) != uf.size) || s.hex() != uf.sha256) { fclose(f); undo(); fail(uf.path + " does not match the manifest"); return; }
        }
        const std::string target = sdPath(uf.path, self_);
        const size_t slash = target.find_last_of('/');
        if (slash != std::string::npos) mkdirs(target.substr(0, slash));
        if (!writeAll(target + ".gfnew", data.data(), data.size())) { fclose(f); undo(); fail("Could not write " + uf.path + " (is the SD card full?)"); return; }
        placed.push_back(target);
        if (isNro(uf.path)) hasNro = true;
        if (uf.path == kNroV1 && target != self_) movedTo = target;   // going back to 1.x: the app moves home
        if (uf.path == kOverlayV1) overlayV1 = true;
        if (uf.path.rfind(kSysDir, 0) == 0 && uf.path.find("exefs.nsp") != std::string::npos) {
            Sha256 s;
            s.add(data.data(), data.size());
            bgChanged = shaFile(target) != s.hex();
        }
        prog_.done++;
    }
    fclose(f);
    if (!hasNro) { undo(); fail("The zip has no Songate.nro"); return; }

    // ---- 3. the settings, copied first (never moved: a fresh start clears them only when Gatefold closes)
    phase_ = BackingUp;
    const std::string backup = backupNow(backupRoot, r.version);
    if (backup.empty()) {
        undo();
        fail("Could not back up config/gatefold-nx to " + backupRoot + ": nothing was changed");
        return;
    }

    // ---- 4. in: the background player and the overlay now; the app when Gatefold closes
    phase_ = Installing;
    std::string pending;
    for (const auto& t : placed) {
        if (t == self_) { pending += t + "\n"; continue; }
        if (!swapIn(t)) pending += t + "\n";   // in use (an overlay Ultrahand holds): at the next close or launch
    }
    // A release under the other name (1.x, Gatefold NX) is in its own places now: this app's .nro and overlay go when it
    // closes ("-": removed), and the one it put in starts instead ("@"), so the HOME menu's homebrew and Ultrahand list one
    if (!movedTo.empty()) pending += "-" + self_ + "\n@" + movedTo + "\n";
    if (overlayV1) pending += "-sdmc:/" + std::string(kOverlay) + "\n";
    writeAll(up + "/pending.txt", pending);
    writeAll(up + "/installed.txt", "version=" + r.version + "\nfrom=" + thisVersion() + "\nbackup=" + backup + "\nbgChanged=" + (bgChanged ? "1" : "0") +
                                        "\nfresh=" + (fresh ? "1" : "0") + "\ndate=" + stamp() +
                                        "\nboot=" + std::to_string(bootStamp()) + "\n");
    if (fresh) writeAll(up + "/fresh.txt", backup + "\n");
    else remove((up + "/fresh.txt").c_str());
    remove(zip.c_str());   // installed: the download is not needed again
    std::lock_guard<std::mutex> g(m_);
    installedOk_ = true;
}

void Updater::markBgRestarted() {
    std::string t;
    if (!readAll(dir_ + "/updates/installed.txt", t)) return;
    writeAll(dir_ + "/updates/installed.txt", t + "bgRestarted=1\n");
    readInstalled();
}

std::string Updater::backupNow(const std::string& root, const std::string& note) {
    // "… 1.2.0 before 1.1.0" before an install; "… 1.2.0 by hand", "… 1.2.0 before a restore" otherwise
    const bool version = !note.empty() && note[0] >= '0' && note[0] <= '9';
    const std::string backup = join(root, stamp() + " " + thisVersion() + (version ? " before " : " ") + note);
    int64_t bytes = 0;
    int files = 0;
    // Left out: the downloads, and caches Gatefold makes again by itself (the overlay's covers, the tag cache)
    // (art/ is the covers cache: thousands of small files, and copying them took the console a minute and a half)
    if (!copyTree(dir_, backup, { "updates", "thumbs", "art", "tags.tsv" }, bytes, files)) {
        removeTree(backup);
        return {};
    }
    writeAll(join(backup, "gatefold-backup.txt"), "from=" + thisVersion() + "\nto=" + note + "\ndate=" + stamp() + "\n");
    return backup;
}

void Updater::restoreAtExit(const std::string& backup) {
    mkdirs(dir_ + "/updates");
    writeAll(dir_ + "/updates/restore.txt", backup + "\n");
    restore_ = backup;
}

void Updater::cancelRestore() {
    remove((dir_ + "/updates/restore.txt").c_str());
    restore_.clear();
}

// Read once at start (Settings builds every page each frame: a file read per frame stalled the console)
std::string Updater::restorePending() const { return restore_; }

std::vector<UpdBackup> Updater::backups(const std::string& root) const {
    std::vector<UpdBackup> out;
    for (const auto& n : names(root)) {
        const std::string p = join(root, n);
        std::string about;
        if (!isDir(p) || !readAll(join(p, "gatefold-backup.txt"), about)) continue;
        UpdBackup b{ p, n, {}, 0, 0 };
        const std::string to = readKey(about, "to");
        b.note = "Settings of " + readKey(about, "from") + (!to.empty() && to[0] >= '0' && to[0] <= '9' ? ", before " : ", ") + to;
        out.push_back(b);
    }
    std::reverse(out.begin(), out.end());   // newest first (the names start with the date)
    return out;
}

std::string Updater::finishPending(const std::string& configDir, const std::string& self) {
    const std::string up = configDir + "/updates";
    bool app = false;
    std::string next;   // "@": the app's new home (going back to a release under the other name)
    std::string t;
    if (readAll(up + "/pending.txt", t)) {
        std::string left;
        for (size_t a = 0; a < t.size();) {
            size_t b = t.find('\n', a);
            if (b == std::string::npos) b = t.size();
            const std::string p = t.substr(a, b - a);
            a = b + 1;
            if (p.empty()) continue;
            if (p[0] == '@') { next = p.substr(1); continue; }
            if (p[0] == '-') {   // a file of this name's, gone (and its folder, the app's own, when that is empty now)
                const std::string f = p.substr(1);
                if (exists(f) && remove(f.c_str()) != 0) { left += p + "\n"; continue; }
                const size_t s = f.find_last_of('/');
                if (f.size() > 4 && f.compare(f.size() - 4, 4, ".nro") == 0 && s != std::string::npos) rmdir(f.substr(0, s).c_str());
                continue;
            }
            if (!swapIn(p)) left += p + "\n";
            else if (p == self) app = true;
        }
        if (left.empty()) remove((up + "/pending.txt").c_str());
        else writeAll(up + "/pending.txt", left);
    }
    // A fresh start (going back with new settings), or a backup put back: the settings folder is cleared (it is in the
    // backup) but for the downloads, then the backup's copy goes in for a restore.
    std::string fresh, restore;
    const bool doFresh = readAll(up + "/fresh.txt", fresh), doRestore = readAll(up + "/restore.txt", restore);
    if (doFresh || doRestore) {
        while (!restore.empty() && (restore.back() == '\n' || restore.back() == '\r')) restore.pop_back();
        if (!doRestore || isDir(restore)) {
            // The caches stay (thousands of covers: removing them held Gatefold's exit for minutes; they are made from the
            // music files, not the settings, and backups leave them out)
            for (const auto& n : names(configDir))
                if (n != "updates" && n != "art" && n != "thumbs" && n != "tags.tsv") removeTree(join(configDir, n));
            int64_t bytes = 0;
            int files = 0;
            if (doRestore) {
                copyTree(restore, configDir, { "gatefold-backup.txt" }, bytes, files);
                // the restored settings are the ones a later install is compared with: no fresh start waits any more
            }
        }
        remove((up + "/fresh.txt").c_str());
        remove((up + "/restore.txt").c_str());
    }
    return !next.empty() ? next : app ? self : std::string();
}

void Updater::cleanLegacy(const std::string& configDir, const std::string& self) {
    if (exists(configDir + "/updates/pending.txt")) return;
    const std::string nro = "sdmc:/" + std::string(kNroV1), ovl = "sdmc:/" + std::string(kOverlayV1);
    if (self != nro && exists(nro) && remove(nro.c_str()) == 0) rmdir("sdmc:/switch/Gatefold");   // (only when empty)
    if (exists("sdmc:/" + std::string(kOverlay)) && exists(ovl)) remove(ovl.c_str());   // (Ultrahand holding it: next launch)
}

bool Updater::restartBackground(std::string& err) {
#ifdef __SWITCH__
    constexpr u64 tid = 0x4200000047464E58ull;
    if (R_FAILED(pmshellInitialize())) { err = "The console did not allow it: restart the console instead"; return false; }
    pmshellTerminateProgram(tid);   // fails harmlessly when it is not running
    svcSleepThread(300'000'000ull);
    NcmProgramLocation loc{};
    loc.program_id = tid;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_FAILED(rc)) { err = "It did not start again: restart the console"; return false; }
    return true;
#else
    printf("[updater] the background player restarts\n");
    (void)err;
    return true;
#endif
}

const std::vector<std::vector<const char*>>& Updater::factoryFiles() {
    // In App::FactoryPart's order. Settings and the music folder live in settings.txt with each other: Gatefold resets
    // them in memory before it closes (App::factoryReset), so settings.txt is never moved but for everything.
    static const std::vector<std::vector<const char*>> files = {
        { "bg-config.txt" },                                                          // settings: the background player's
        {},                                                                           // the music folder (settings.txt)
        { "playlists.txt", "lists.txt", "playlist-icons.tsv", "bg-playlists.txt", "mixes.tsv", "automix-lists.tsv" },   // playlists
        { "liked.txt" },                                                              // Liked
        { "edits.tsv", "covers.tsv", "covers" },                                      // tag edits and chosen covers
        { "lyric-offsets.tsv" },                                                      // lyric timings
        { "eq-presets.txt" },                                                         // saved equalizer presets
        { "stats.tsv", "bg-plays.txt" },                                              // play counts and statistics
        { "session.txt", "bg-session.txt", "bg-places.txt", "overlay-playlist.txt" }, // the queue
        { "art", "thumbs", "tags.tsv", "library.tsv", "loudness.tsv", "mix.tsv", "mood.tsv", "gains.bin", "updates" },   // caches
    };
    return files;
}

void Updater::factoryReset(const std::string& configDir, unsigned mask) {
    if (!mask) return;
    const bool all = (mask & ((1u << factoryFiles().size()) - 1)) == (1u << factoryFiles().size()) - 1;
    // The background player first: running, it writes its session, places and settings back into the folder
#ifdef __SWITCH__
    constexpr u64 tid = 0x4200000047464E58ull;
    const bool pm = R_SUCCEEDED(pmshellInitialize());
    if (pm) {
        pmshellTerminateProgram(tid);   // fails harmlessly when it is not running
        svcSleepThread(300'000'000ull);
    }
#else
    printf("[updater] factory reset: the background player ends\n");
#endif
    const std::string aside = configDir + ".erased";
    if (all) {
        if (isDir(aside)) removeTree(aside);   // one left from an earlier reset that was never launched after (rare)
        if (rename(configDir.c_str(), aside.c_str()) != 0) removeTree(configDir);
    } else {
        mkdir(aside.c_str(), 0777);
        for (size_t part = 0; part < factoryFiles().size(); part++) {
            if (!(mask & (1u << part))) continue;
            for (const char* n : factoryFiles()[part]) {
                const std::string from = join(configDir, n), to = join(aside, n);
                struct stat st;
                if (stat(from.c_str(), &st) != 0) continue;
                if (stat(to.c_str(), &st) == 0) removeTree(to);
                if (rename(from.c_str(), to.c_str()) != 0) removeTree(from);
            }
        }
    }
#ifdef __SWITCH__
    if (pm) {
        NcmProgramLocation loc{};
        loc.program_id = tid;
        loc.storageID = NcmStorageId_None;
        u64 pid = 0;
        pmshellLaunchProgram(0, &loc, &pid);   // not installed: fails harmlessly
        pmshellExit();
    }
#else
    printf("[updater] factory reset (parts %x): moved aside, the background player starts again\n", mask);
#endif
}

void Updater::eraseLeftovers(const std::string& configDir) {
    const std::string aside = configDir + ".erased";
    if (isDir(aside)) removeTree(aside);
}
