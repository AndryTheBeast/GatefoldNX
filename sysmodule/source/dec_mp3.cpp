/*
 * Gatefold NX background player · dec_mp3.cpp
 * MP3 through minimp3's frame decoder, streamed from a 16 KB window. Duration and seeking use the
 * Xing/Info header (with its TOC) when there is one, the bitrate otherwise; LAME's encoder delay
 * is dropped so albums meant to be gapless stay so. Tags and the cover come from ID3v2.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD   // aarch64 NEON works, but this keeps the PC test bit-identical
#include "minimp3.h"

#include <cstdlib>
#include <vector>

#include "dec_common.hpp"

namespace {

class Mp3Decoder : public FrameDecoder {
public:
    bool open(const char* path) {
        if (!r_.open(path)) return false;
        mp3dec_init(&dec_);
        dataStart_ = id3(0);
        dataEnd_ = r_.size();
        uint8_t tag[3] = {};
        if (dataEnd_ >= 128 && r_.readAt(dataEnd_ - 128, tag, 3) && !memcmp(tag, "TAG", 3)) dataEnd_ -= 128;
        if (dataEnd_ <= dataStart_) return false;
        if (!firstFrame()) return false;
        codec = 2;
        rewind();
        return true;
    }

    bool seek(double seconds) override {
        if (seconds <= 0 || duration <= 0) { rewind(); return true; }
        const uint64_t target = uint64_t(seconds * rate);
        if (buildIndex()) {
            // Exact: start two frames early (the bit reservoir reaches back into earlier frames),
            // drop whole frames up to the target's, then the samples before it in that frame.
            const uint64_t abs = target + uint64_t(delay_);
            const uint64_t f = std::min<uint64_t>(abs / spf_, indexFrames_ ? indexFrames_ - 1 : 0);
            const uint64_t from = f >= 2 ? f - 2 : 0;
            const size_t e = std::min<size_t>(size_t(from / IndexStep), index_.size() - 1);
            fill(firstFrame_ + index_[e]);
            mp3dec_init(&dec_);
            dropPending();
            dropFrames_ = int(f - uint64_t(e) * IndexStep);
            skip_ = int(abs - f * spf_);
        } else {
            const double fr = std::min(seconds / duration, 0.999);
            double at;
            if (hasToc_) {
                // Xing's TOC: 100 points, each a fraction (of 256) of the file at that percentage.
                const double pct = fr * 100;
                const int i = std::min(99, int(pct));
                const double a = toc_[i], b = i < 99 ? toc_[i + 1] : 256.0;
                at = (a + (b - a) * (pct - i)) / 256.0 * double(audioBytes_);
            } else {
                at = fr * double(dataEnd_ - firstFrame_);
            }
            fill(firstFrame_ + uint64_t(at));
            mp3dec_init(&dec_);
            dropPending();
        }
        left_ = total_ > 0 ? int64_t(total_) - int64_t(std::min<uint64_t>(target, total_)) : -1;
        return true;
    }

protected:
    int decodeFrame(int16_t* out) override {
        for (;;) {
            if (len_ - at_ < 4096 && filePos_ < dataEnd_) refill();
            if (at_ >= len_) return 0;
            mp3dec_frame_info_t info;
            const int n = mp3dec_decode_frame(&dec_, buf_ + at_, int(len_ - at_), pcm_, &info);
            if (info.frame_bytes == 0) {
                if (filePos_ >= dataEnd_) return 0;
                at_ = len_;   // nothing decodable in the window: move on
                continue;
            }
            at_ += size_t(info.frame_bytes);
            if (dropFrames_ > 0 && info.hz > 0) { dropFrames_--; continue; }
            if (n <= 0) continue;   // the Xing frame, or garbage minimp3 skipped
            toStereo(pcm_, n, info.channels, out);
            return n;
        }
    }

private:
    Reader r_;
    mp3dec_t dec_;
    uint64_t dataStart_ = 0, dataEnd_ = 0, firstFrame_ = 0, audioBytes_ = 0;
    bool hasToc_ = false;
    uint8_t toc_[100] = {};
    int delay_ = 0, padding_ = 0, spf_ = 1152, dropFrames_ = 0;
    uint64_t total_ = 0;      // the song's own samples (without delay and padding), 0 when unknown
    // Seeking: the offset (from firstFrame_) of every IndexStep-th frame, built on the first seek.
    static constexpr int IndexStep = 8;
    std::vector<uint32_t> index_;
    uint64_t indexFrames_ = 0;
    bool indexTried_ = false;
    static constexpr size_t Win = 16384;
    uint8_t buf_[Win];
    size_t at_ = 0, len_ = 0;
    uint64_t filePos_ = 0;
    int16_t pcm_[MINIMP3_MAX_SAMPLES_PER_FRAME];

    void fill(uint64_t pos) {
        filePos_ = std::min(pos, dataEnd_);
        at_ = len_ = 0;
        refill();
    }

    void refill() {
        if (at_ > 0) { memmove(buf_, buf_ + at_, len_ - at_); len_ -= at_; at_ = 0; }
        const size_t want = std::min<uint64_t>(Win - len_, dataEnd_ - filePos_);
        if (want == 0) return;
        r_.seek(filePos_);
        const size_t got = r_.read(buf_ + len_, want);
        len_ += got;
        filePos_ += got;
        if (got == 0) filePos_ = dataEnd_;
    }

    void rewind() {
        fill(firstFrame_);
        mp3dec_init(&dec_);
        dropPending();
        skip_ = delay_;
        dropFrames_ = 0;
        left_ = total_ > 0 ? int64_t(total_) : -1;
    }

    /** The length of the frame whose header is h, 0 when h is not one (or is free-format). */
    static int frameLength(const uint8_t* h) {
        if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
        const int ver = h[1] >> 3 & 3, layer = h[1] >> 1 & 3, bri = h[2] >> 4, sri = h[2] >> 2 & 3, pad = h[2] >> 1 & 1;
        if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) return 0;
        static const int br[2][3][15] = {
            { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
              { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
              { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
            { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
              { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
              { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
        static const int sr[3] = { 44100, 48000, 32000 };
        const bool mpeg1 = ver == 3;
        const int rateHz = sr[sri] >> (mpeg1 ? 0 : ver == 2 ? 1 : 2);
        const int kbps = br[mpeg1 ? 0 : 1][3 - layer][bri];
        if (layer == 3) return (12 * kbps * 1000 / rateHz + pad) * 4;   // Layer I
        if (layer == 1 && !mpeg1) return 72 * kbps * 1000 / rateHz + pad;   // Layer III, MPEG-2/2.5
        return 144 * kbps * 1000 / rateHz + pad;
    }

    /** Walks the frame headers once; false when the stream does not walk cleanly (free-format, junk). */
    bool buildIndex() {
        if (indexTried_) return !index_.empty();
        indexTried_ = true;
        uint64_t at = firstFrame_, frames = 0;
        int junk = 0;
        uint8_t h[4];
        while (at + 4 <= dataEnd_) {
            if (!r_.readAt(at, h, 4)) break;
            const int len = frameLength(h);
            if (len <= 0) {
                // A stray byte or a tag in the middle: look for the next header, but not for long.
                if (++junk > 65536) { index_.clear(); return false; }
                at++;
                continue;
            }
            if (frames % IndexStep == 0) index_.push_back(uint32_t(at - firstFrame_));
            frames++;
            at += uint64_t(len);
        }
        indexFrames_ = frames;
        if (frames < 2) index_.clear();
        return !index_.empty();
    }

    /** Finds the first frame, reads Xing/Info and LAME from it, and works out the duration. */
    bool firstFrame() {
        fill(dataStart_);
        mp3dec_frame_info_t info;
        int samples = 0;
        for (int tries = 0; tries < 64; tries++) {
            if (len_ - at_ < 4096 && filePos_ < dataEnd_) refill();
            if (at_ >= len_) return false;
            const size_t before = at_;
            samples = mp3dec_decode_frame(&dec_, buf_ + at_, int(len_ - at_), pcm_, &info);
            if (info.frame_bytes == 0) { if (filePos_ >= dataEnd_) return false; at_ = len_; continue; }
            at_ += size_t(info.frame_bytes);
            if (info.hz <= 0) continue;
            // minimp3 counts skipped bytes in frame_bytes: the frame itself is the last frame_bytes' tail.
            const size_t frameLen = size_t(info.frame_bytes);
            const uint8_t* frame = buf_ + at_ - frameLen;
            const uint64_t frameFile = filePos_ - (len_ - (at_ - frameLen));
            (void)before;
            rate = info.hz;
            channels = info.channels;
            bitrate = info.bitrate_kbps;
            const int perFrame = samples > 0 ? samples : (info.layer == 1 ? 384 : (info.layer == 3 && rate < 32000 ? 576 : 1152));
            spf_ = perFrame;
            firstFrame_ = frameFile;
            if (xing(frame, frameLen, info, perFrame)) firstFrame_ = frameFile + frameLen;   // that frame is silence
            break;
        }
        if (rate <= 0) return false;
        if (duration <= 0 && bitrate > 0) duration = double(dataEnd_ - firstFrame_) * 8 / (bitrate * 1000.0);
        if (audioBytes_ == 0) audioBytes_ = dataEnd_ - firstFrame_;
        return true;
    }

    bool xing(const uint8_t* f, size_t n, const mp3dec_frame_info_t& info, int perFrame) {
        const bool mpeg1 = (f[1] & 0x18) == 0x18;
        const size_t side = mpeg1 ? (info.channels == 1 ? 17 : 32) : (info.channels == 1 ? 9 : 17);
        const size_t x = 4 + side;
        if (n < x + 8 || (memcmp(f + x, "Xing", 4) && memcmp(f + x, "Info", 4))) return false;
        const uint32_t flags = be32(f + x + 4);
        size_t p = x + 8;
        uint32_t frames = 0;
        if (flags & 1) { if (p + 4 > n) return true; frames = be32(f + p); p += 4; }
        if (flags & 2) { if (p + 4 > n) return true; audioBytes_ = be32(f + p); p += 4; }
        if (flags & 4) { if (p + 100 > n) return true; memcpy(toc_, f + p, 100); hasToc_ = true; p += 100; }
        if (flags & 8) p += 4;
        if (frames) duration = double(frames) * perFrame / rate;
        // LAME's tag: the encoder delay (12 bits) and padding (12 bits) 21 bytes in.
        if (p + 24 <= n && (!memcmp(f + p, "LAME", 4) || !memcmp(f + p, "Lavc", 4) || !memcmp(f + p, "Lavf", 4))) {
            delay_ = (f[p + 21] << 4 | f[p + 22] >> 4) + 529;   // + the decoder's own delay
            padding_ = ((f[p + 22] & 0xF) << 8 | f[p + 23]) - 529;
            if (delay_ > 4096 + 529 || padding_ > 4096) delay_ = padding_ = 0;   // (padding is below 529 when it is short)
        }
        if (frames) {
            const int64_t t = int64_t(frames) * perFrame - delay_ - padding_;
            if (t > 0) { total_ = uint64_t(t); duration = double(total_) / rate; }
        }
        if (frames && audioBytes_ && duration > 0) bitrate = int(double(audioBytes_) * 8 / duration / 1000);   // VBR: the average
        return true;
    }

    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    /** Skips every ID3v2 tag at pos (some files have two), reading the first one's text and picture. */
    uint64_t id3(uint64_t pos) {
        for (int n = 0; n < 4; n++) {
            uint8_t h[10];
            if (!r_.readAt(pos, h, 10) || memcmp(h, "ID3", 3)) return pos;
            const uint32_t size = uint32_t(h[6] & 0x7F) << 21 | uint32_t(h[7] & 0x7F) << 14 | uint32_t(h[8] & 0x7F) << 7 | (h[9] & 0x7F);
            if (n == 0) frames(pos + 10, pos + 10 + size, h[3], h[5]);
            pos += 10 + size + ((h[5] & 0x10) ? 10 : 0);
        }
        return pos;
    }

    void frames(uint64_t at, uint64_t end, int version, int flags) {
        if (flags & 0x40) {   // extended header
            uint8_t e[4];
            if (!r_.readAt(at, e, 4)) return;
            at += version == 4 ? (uint32_t(e[0] & 0x7F) << 21 | uint32_t(e[1] & 0x7F) << 14 | uint32_t(e[2] & 0x7F) << 7 | (e[3] & 0x7F)) : be32(e) + 4;
        }
        const bool v22 = version == 2;
        const size_t hl = v22 ? 6 : 10;
        while (at + hl <= end) {
            uint8_t h[10];
            if (!r_.readAt(at, h, hl) || h[0] == 0) return;
            uint32_t size;
            if (v22) size = uint32_t(h[3]) << 16 | uint32_t(h[4]) << 8 | h[5];
            else if (version == 4) size = uint32_t(h[4] & 0x7F) << 21 | uint32_t(h[5] & 0x7F) << 14 | uint32_t(h[6] & 0x7F) << 7 | (h[7] & 0x7F);
            else size = be32(h + 4);
            const uint64_t body = at + hl;
            if (size == 0 || body + size > end) return;
            const std::string id(reinterpret_cast<char*>(h), v22 ? 3 : 4);
            std::string* text = nullptr;
            if (id == "TIT2" || id == "TT2") text = &tags.title;
            else if (id == "TPE1" || id == "TP1") text = &tags.artist;
            else if (id == "TALB" || id == "TAL") text = &tags.album;
            if (text && text->empty() && size > 1) {
                uint8_t b[512];
                const size_t k = std::min<size_t>(size, sizeof b);
                if (r_.readAt(body, b, k)) *text = tagText(b + 1, k - 1, b[0]);
            } else if ((id == "APIC" || id == "PIC") && !tags.coverKind) {
                picture(body, size, v22);
            }
            at = body + size;
        }
    }

    void picture(uint64_t body, uint32_t size, bool v22) {
        uint8_t b[600];
        const size_t k = std::min<size_t>(size, sizeof b);
        if (!r_.readAt(body, b, k)) return;
        const int enc = b[0];
        size_t p = 1;
        if (v22) p += 3;   // a three-letter format instead of a MIME type
        else { while (p < k && b[p]) p++; p++; }
        p++;   // picture type
        if (enc == 1 || enc == 2) { while (p + 1 < k && (b[p] || b[p + 1])) p += 2; p += 2; }
        else { while (p < k && b[p]) p++; p++; }
        if (p + 2 > k || p >= size) return;
        tags.coverOffset = body + p;
        tags.coverSize = size - uint32_t(p);
        tags.coverKind = b[p] == 0x89 && b[p + 1] == 'P' ? 2 : b[p] == 0xFF && b[p + 1] == 0xD8 ? 1 : 0;
    }
};

}  // namespace

std::unique_ptr<Decoder> openMp3(const char* path) {
    auto d = std::make_unique<Mp3Decoder>();
    if (!d->open(path)) return nullptr;
    return d;
}
