/*
 * Gatefold NX background player · decoder.cpp
 * Picking a decoder by the file's first bytes (the extension only breaks ties), the buffered
 * reader, and the conversions every decoder needs.
 */
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "dec_common.hpp"

// ---------------------------------------------------------------- Reader

bool Reader::open(const char* path) {
    close();
    f_ = fopen(path, "rb");
    if (!f_) return false;
    setvbuf(f_, nullptr, _IONBF, 0);   // our own buffer below; two layers of copying buy nothing
    fseek(f_, 0, SEEK_END);
    size_ = uint64_t(ftell(f_));
    fseek(f_, 0, SEEK_SET);
    buf_ = static_cast<uint8_t*>(malloc(Buf));
    pos_ = bufStart_ = bufLen_ = 0;
    return buf_ != nullptr;
}

void Reader::close() {
    if (f_) fclose(f_);
    f_ = nullptr;
    free(buf_);
    buf_ = nullptr;
}

bool Reader::seek(uint64_t pos) {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
}

size_t Reader::read(void* dst, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < n && pos_ < size_) {
        if (pos_ >= bufStart_ && pos_ < bufStart_ + bufLen_) {
            const size_t off = size_t(pos_ - bufStart_);
            const size_t k = std::min(n - done, bufLen_ - off);
            memcpy(d + done, buf_ + off, k);
            done += k;
            pos_ += k;
            continue;
        }
        // Big reads skip the buffer; small ones refill it from here.
        if (n - done >= Buf) {
            if (fseek(f_, long(pos_), SEEK_SET) != 0) break;
            const size_t k = fread(d + done, 1, n - done, f_);
            if (k == 0) break;
            done += k;
            pos_ += k;
            continue;
        }
        if (fseek(f_, long(pos_), SEEK_SET) != 0) break;
        bufStart_ = pos_;
        bufLen_ = fread(buf_, 1, Buf, f_);
        if (bufLen_ == 0) break;
    }
    return done;
}

uint8_t Reader::u8() { uint8_t b = 0; read(&b, 1); return b; }
uint16_t Reader::u16() { uint8_t b[2] = {}; read(b, 2); return uint16_t(b[0] << 8 | b[1]); }
uint32_t Reader::u32() { uint8_t b[4] = {}; read(b, 4); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
uint64_t Reader::u64() { const uint64_t hi = u32(); return hi << 32 | u32(); }

// ---------------------------------------------------------------- conversions

void toStereo(const int16_t* in, int frames, int channels, int16_t* out) {
    if (channels == 2) { memmove(out, in, size_t(frames) * 4); return; }
    if (channels == 1) {
        for (int i = frames - 1; i >= 0; i--) out[i * 2] = out[i * 2 + 1] = in[i];   // backwards: in and out may overlap
        return;
    }
    for (int i = 0; i < frames; i++) { out[i * 2] = in[i * channels]; out[i * 2 + 1] = in[i * channels + 1]; }
}

static void putUtf8(std::string& s, unsigned c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | (c >> 6 & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | c >> 18); s += char(0x80 | (c >> 12 & 0x3F)); s += char(0x80 | (c >> 6 & 0x3F)); s += char(0x80 | (c & 0x3F)); }
}

// ID3v2 encodings: 0 Latin-1, 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8.
std::string tagText(const uint8_t* p, size_t n, int encoding) {
    std::string s;
    if (encoding == 3) s.assign(reinterpret_cast<const char*>(p), n);
    else if (encoding == 0) for (size_t i = 0; i < n; i++) putUtf8(s, p[i]);
    else {
        bool be = encoding == 2;
        size_t i = 0;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        for (; i + 1 < n; i += 2) {
            unsigned c = be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                const unsigned lo = be ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            if (c == 0) break;
            putUtf8(s, c);
        }
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ---------------------------------------------------------------- picking one

static std::string extension(const char* path) {
    const char* dot = strrchr(path, '.');
    std::string e = dot ? dot + 1 : "";
    for (auto& c : e) c = char(tolower(c));
    return e;
}

std::unique_ptr<Decoder> openDecoder(const char* path) {
    uint8_t head[12] = {};
    if (FILE* f = fopen(path, "rb")) {
        const size_t got = fread(head, 1, sizeof head, f);
        fclose(f);
        if (got < sizeof head) return nullptr;
    } else {
        return nullptr;
    }
    if (!memcmp(head + 4, "ftyp", 4)) return openMp4(path);
    if (!memcmp(head, "fLaC", 4)) return openFlac(path);
    if (!memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) return openWav(path);
    if (!memcmp(head, "ID3", 3) || (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) return openMp3(path);
    // Odd files (an ID3 tag in front of a FLAC, MP4 without ftyp first): trust the extension.
    const std::string e = extension(path);
    if (e == "m4a" || e == "mp4" || e == "aac" || e == "m4b") return openMp4(path);
    if (e == "flac") return openFlac(path);
    if (e == "mp3") return openMp3(path);
    if (e == "wav") return openWav(path);
    return nullptr;
}
