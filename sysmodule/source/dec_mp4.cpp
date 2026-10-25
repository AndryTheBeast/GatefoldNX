/*
 * Gatefold NX background player · dec_mp4.cpp
 * AAC in MP4/M4A: a demuxer of our own and Helix's fixed-point decoder (HE-AAC included).
 *
 * The demuxer keeps only the sample tables, packed (sizes as u16 when they fit), and walks
 * them as it plays. A general MP4 library would load the whole moov, which carries the cover
 * art and can be megabytes. Tags and the cover's position come from moov/udta/meta/ilst.
 */
#include <cstdlib>
#include <vector>

#include "aacdec.h"
#include "dec_common.hpp"

namespace {

constexpr uint32_t fourcc(const char* s) { return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]); }

const int kRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

/** MSB-first bit reader for the AudioSpecificConfig. */
struct Bits {
    const uint8_t* p;
    size_t n, at = 0;
    unsigned get(int k) {
        unsigned v = 0;
        for (int i = 0; i < k; i++, at++) v = v << 1 | (at / 8 < n ? (p[at / 8] >> (7 - at % 8) & 1) : 0);
        return v;
    }
};

class Mp4Decoder : public FrameDecoder {
public:
    ~Mp4Decoder() override { if (aac_) AACFreeDecoder(aac_); }

    bool open(const char* path) {
        if (!r_.open(path)) return false;
        walk(0, r_.size(), 0);
        if (!audio_ || (sizes16_.empty() && sizes32_.empty() && constSize_ == 0)) return false;
        if (chunks_.empty() || stsc_.empty() || timescale_ == 0) return false;
        if (ascRate_ == 0 || ascChannels_ <= 0) return false;

        aac_ = AACInitDecoder();
        if (!aac_) return false;
        AACFrameInfo fi = {};
        fi.nChans = ascChannels_;
        fi.sampRateCore = ascRate_;
        fi.profile = AAC_PROFILE_LC;
        if (AACSetRawBlockParams(aac_, 0, &fi) != 0) return false;

        codec = 1;
        channels = ascChannels_;
        rate = ascRate_ * (sbr_ ? 2 : 1);
        uint64_t total = 0;
        for (const auto& e : stts_) total += uint64_t(e.count) * e.delta;
        duration = double(total - std::min<uint64_t>(total, uint64_t(mediaStart_))) / timescale_;   // without the priming
        if (bitrate == 0 && duration > 0) bitrate = int(double(r_.size()) * 8 / duration / 1000);
        rewind();
        // The first frame says whether SBR is there (implicit signalling): the output rate follows it.
        const int got = decodeFrame(pcm_);
        if (got <= 0) return false;
        AACFrameInfo info = {};
        AACGetLastFrameInfo(aac_, &info);
        if (info.sampRateOut > 0) rate = info.sampRateOut;
        rewind();
        return true;
    }

    bool seek(double seconds) override {
        // stts: from time to sample index, in the media's timescale. The priming samples (elst)
        // come first in the media, and one frame before the target is decoded and thrown away:
        // AAC's overlap needs the frame before to sound right.
        const uint64_t want = uint64_t(std::max(0.0, seconds) * timescale_) + uint64_t(mediaStart_);
        uint64_t t = 0, sample = 0, sampleTime = 0;
        for (const auto& e : stts_) {
            const uint64_t span = uint64_t(e.count) * e.delta;
            if (want < t + span) {
                const uint64_t k = e.delta ? (want - t) / e.delta : 0;
                sample += k;
                sampleTime = t + k * e.delta;
                break;
            }
            t += span;
            sample += e.count;
            sampleTime = t;
        }
        uint64_t warm = 0;
        if (sample > 0 && !stts_.empty()) { sample--; warm = stts_[0].delta; }
        if (sample >= sampleCount_) sample = sampleCount_ ? sampleCount_ - 1 : 0;
        goToSample(uint32_t(sample));
        AACFlushCodec(aac_);
        dropPending();
        skip_ = int((want - std::min(want, sampleTime) + warm) * uint64_t(rate) / timescale_);
        return true;
    }

protected:
    int decodeFrame(int16_t* out) override {
        for (int tries = 0; tries < 8; tries++) {
            if (sample_ >= sampleCount_) return 0;
            const uint32_t size = sampleSize(sample_);
            if (size == 0 || size > sizeof frame_) { advance(); continue; }
            if (!r_.readAt(offset_, frame_, size)) return -1;
            advance();
            unsigned char* in = frame_;
            int left = int(size);
            const int err = AACDecode(aac_, &in, &left, pcm_);
            if (err != 0) continue;   // one damaged frame is skipped, not the whole song
            AACFrameInfo info = {};
            AACGetLastFrameInfo(aac_, &info);
            if (info.nChans <= 0 || info.outputSamps <= 0) continue;
            const int frames = std::min(MaxFrame, info.outputSamps / info.nChans);
            toStereo(pcm_, frames, info.nChans, out);
            return frames;
        }
        return -1;
    }

private:
    struct Stts { uint32_t count, delta; };
    struct Stsc { uint32_t first, perChunk; };

    Reader r_;
    HAACDecoder aac_ = nullptr;
    bool audio_ = false, sbr_ = false;
    int ascRate_ = 0, ascChannels_ = 0;
    uint32_t timescale_ = 0;
    int64_t mediaStart_ = 0;   // elst: the priming samples to drop
    std::vector<Stts> stts_;
    std::vector<Stsc> stsc_;
    std::vector<uint64_t> chunks_;
    std::vector<uint16_t> sizes16_;
    std::vector<uint32_t> sizes32_;
    uint32_t constSize_ = 0, sampleCount_ = 0;
    uint8_t frame_[8192];
    short pcm_[MaxFrame * AAC_MAX_NCHANS];   // members, not locals: a sysmodule's thread stacks are small

    // the play head in the tables
    uint32_t sample_ = 0, chunk_ = 0, inChunk_ = 0, stscAt_ = 0;
    uint64_t offset_ = 0;

    uint32_t sampleSize(uint32_t i) const {
        if (constSize_) return constSize_;
        if (!sizes16_.empty()) return i < sizes16_.size() ? sizes16_[i] : 0;
        return i < sizes32_.size() ? sizes32_[i] : 0;
    }

    void rewind() {
        goToSample(0);
        AACFlushCodec(aac_);
        dropPending();
        // elst's media time is in the media timescale; drop it in output frames.
        if (mediaStart_ > 0) skip_ = int(mediaStart_ * rate / timescale_);
    }

    void goToSample(uint32_t k) {
        uint32_t acc = 0;
        for (uint32_t e = 0; e < stsc_.size(); e++) {
            const uint32_t lastChunk = e + 1 < stsc_.size() ? stsc_[e + 1].first - 1 : uint32_t(chunks_.size());
            const uint32_t nChunks = lastChunk >= stsc_[e].first ? lastChunk - stsc_[e].first + 1 : 0;
            const uint64_t inEntry = uint64_t(nChunks) * stsc_[e].perChunk;
            if (k < acc + inEntry || e + 1 == stsc_.size()) {
                const uint32_t rel = k - acc;
                const uint32_t per = std::max<uint32_t>(1, stsc_[e].perChunk);
                stscAt_ = e;
                chunk_ = stsc_[e].first - 1 + rel / per;
                inChunk_ = rel % per;
                sample_ = k;
                offset_ = chunk_ < chunks_.size() ? chunks_[chunk_] : r_.size();
                for (uint32_t i = k - inChunk_; i < k; i++) offset_ += sampleSize(i);
                return;
            }
            acc += uint32_t(inEntry);
        }
        sample_ = sampleCount_;
    }

    void advance() {
        offset_ += sampleSize(sample_);
        sample_++;
        if (++inChunk_ >= stsc_[stscAt_].perChunk) {
            inChunk_ = 0;
            chunk_++;
            if (stscAt_ + 1 < stsc_.size() && chunk_ + 1 >= stsc_[stscAt_ + 1].first) stscAt_++;
            offset_ = chunk_ < chunks_.size() ? chunks_[chunk_] : r_.size();
        }
    }

    // ---------------------------------------------------------------- boxes

    /** Walks the boxes in [start, end). depth guards against files that nest without end. */
    void walk(uint64_t start, uint64_t end, int depth) {
        uint64_t at = start;
        while (at + 8 <= end && depth < 12) {
            r_.seek(at);
            uint64_t size = r_.u32();
            const uint32_t type = r_.u32();
            uint64_t head = 8;
            if (size == 1) { size = r_.u64(); head = 16; }
            else if (size == 0) size = end - at;
            if (size < head || at + size > end) break;
            box(type, at + head, at + size, depth);
            at += size;
        }
    }

    void box(uint32_t type, uint64_t start, uint64_t end, int depth) {
        switch (type) {
        case fourcc("moov"): case fourcc("mdia"): case fourcc("minf"): case fourcc("stbl"): case fourcc("udta"):
        case fourcc("ilst"): case fourcc("edts"):
            walk(start, end, depth + 1);
            break;
        case fourcc("trak"):
            if (!audio_) {
                trakAudio_ = false;
                walk(start, end, depth + 1);
                if (trakAudio_ && ascRate_) audio_ = true;
                else { stts_.clear(); stsc_.clear(); chunks_.clear(); sizes16_.clear(); sizes32_.clear(); constSize_ = 0; mediaStart_ = 0; }
            }
            break;
        case fourcc("meta"): {
            // ISO's meta is a full box (4 bytes of version and flags); QuickTime's is not.
            r_.seek(start + 4);
            const uint32_t t = r_.u32();
            walk(t == fourcc("hdlr") ? start : start + 4, end, depth + 1);
            break;
        }
        case fourcc("hdlr"):
            r_.seek(start + 8);
            if (r_.u32() == fourcc("soun")) trakAudio_ = true;
            break;
        case fourcc("mdhd"): {
            r_.seek(start);
            const uint8_t v = r_.u8();
            r_.seek(start + (v == 1 ? 20 : 12));
            timescale_ = r_.u32();
            break;
        }
        case fourcc("elst"): {
            r_.seek(start);
            const uint8_t v = r_.u8();
            r_.seek(start + 4);
            if (r_.u32() == 0) break;
            if (v == 1) { r_.u64(); mediaStart_ = int64_t(r_.u64()); }
            else { r_.u32(); mediaStart_ = int32_t(r_.u32()); }
            if (mediaStart_ < 0) mediaStart_ = 0;
            break;
        }
        case fourcc("stsd"): stsd(start, end); break;
        case fourcc("stts"): {
            r_.seek(start + 4);
            const uint32_t n = r_.u32();
            if (n > 1u << 22) break;
            stts_.resize(n);
            for (auto& e : stts_) { e.count = r_.u32(); e.delta = r_.u32(); }
            break;
        }
        case fourcc("stsc"): {
            r_.seek(start + 4);
            const uint32_t n = r_.u32();
            if (n > 1u << 22) break;
            stsc_.resize(n);
            for (auto& e : stsc_) { e.first = r_.u32(); e.perChunk = r_.u32(); r_.u32(); }
            break;
        }
        case fourcc("stsz"): {
            r_.seek(start + 4);
            constSize_ = r_.u32();
            sampleCount_ = r_.u32();
            if (constSize_ || sampleCount_ > 1u << 24) break;
            // Read in blocks: one u32() call per sample would be slow on long files.
            std::vector<uint32_t> all(sampleCount_);
            uint32_t big = 0;
            uint8_t blk[4096];
            for (uint32_t i = 0; i < sampleCount_;) {
                const uint32_t k = std::min<uint32_t>(sampleCount_ - i, sizeof blk / 4);
                if (r_.read(blk, k * 4) != k * 4) { sampleCount_ = i; break; }
                for (uint32_t j = 0; j < k; j++, i++) {
                    all[i] = uint32_t(blk[j * 4]) << 24 | uint32_t(blk[j * 4 + 1]) << 16 | uint32_t(blk[j * 4 + 2]) << 8 | blk[j * 4 + 3];
                    big = std::max(big, all[i]);
                }
            }
            all.resize(sampleCount_);
            if (big < 65536) sizes16_.assign(all.begin(), all.end());
            else sizes32_ = std::move(all);
            break;
        }
        case fourcc("stco"): case fourcc("co64"): {
            r_.seek(start + 4);
            const uint32_t n = r_.u32();
            if (n > 1u << 22) break;
            chunks_.resize(n);
            for (auto& c : chunks_) c = type == fourcc("co64") ? r_.u64() : r_.u32();
            break;
        }
        case 0xA96E616D: tagItem(start, end, tags.title); break;    // ©nam
        case 0xA9415254: tagItem(start, end, tags.artist); break;   // ©ART
        case 0xA9616C62: tagItem(start, end, tags.album); break;    // ©alb
        case fourcc("aART"): if (tags.artist.empty()) tagItem(start, end, tags.artist); break;
        case fourcc("covr"): cover(start, end); break;
        default: break;
        }
    }

    bool trakAudio_ = false;

    void stsd(uint64_t start, uint64_t end) {
        r_.seek(start + 8);   // version/flags, entry count
        const uint64_t entry = r_.tell();
        const uint32_t size = r_.u32();
        const uint32_t format = r_.u32();
        if (format != fourcc("mp4a") || entry + size > end) return;   // ALAC and the rest are not ours
        r_.seek(entry + 16);
        const uint16_t version = r_.u16();
        r_.seek(entry + 24);
        ascChannels_ = r_.u16();
        r_.seek(entry + 32);
        ascRate_ = int(r_.u32() >> 16);
        const uint64_t kids = entry + 36 + (version == 1 ? 16 : version == 2 ? 36 : 0);
        findEsds(kids, entry + size, 0);
    }

    void findEsds(uint64_t start, uint64_t end, int depth) {
        uint64_t at = start;
        while (at + 8 <= end && depth < 3) {
            r_.seek(at);
            const uint32_t size = r_.u32(), type = r_.u32();
            if (size < 8 || at + size > end) return;
            if (type == fourcc("esds")) { esds(at + 12, at + size); return; }
            if (type == fourcc("wave")) findEsds(at + 8, at + size, depth + 1);   // QuickTime wraps it
            at += size;
        }
    }

    uint32_t descLen() {
        uint32_t n = 0;
        for (int i = 0; i < 4; i++) { const uint8_t b = r_.u8(); n = n << 7 | (b & 0x7F); if (!(b & 0x80)) break; }
        return n;
    }

    void esds(uint64_t start, uint64_t end) {
        r_.seek(start);
        if (r_.u8() != 0x03) return;   // ES_Descriptor
        descLen();
        r_.u16();
        const uint8_t flags = r_.u8();
        if (flags & 0x80) r_.u16();
        if (flags & 0x40) r_.seek(r_.tell() + r_.u8());
        if (flags & 0x20) r_.u16();
        if (r_.u8() != 0x04) return;   // DecoderConfigDescriptor
        descLen();
        const uint8_t oti = r_.u8();
        if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) return;   // MPEG-4 or MPEG-2 AAC
        r_.u8();
        r_.u8(); r_.u16();   // buffer size, 24 bits
        r_.u32();            // max bitrate
        const uint32_t avg = r_.u32();
        if (avg) bitrate = int(avg / 1000);
        if (r_.u8() != 0x05) return;   // DecoderSpecificInfo: the AudioSpecificConfig
        const uint32_t n = std::min<uint32_t>(descLen(), 64);
        uint8_t asc[64];
        if (r_.tell() + n > end || r_.read(asc, n) != n || n < 2) return;
        Bits b{ asc, n };
        auto aot = [&] { unsigned a = b.get(5); return a == 31 ? 32 + b.get(6) : a; };
        auto freq = [&] { const unsigned i = b.get(4); return i == 15 ? int(b.get(24)) : i < 13 ? kRates[i] : 0; };
        unsigned type = aot();
        int f = freq();
        const int ch = int(b.get(4));
        if (type == 5 || type == 29) {   // explicit SBR (and PS): the core rate comes first
            sbr_ = true;
            freq();
            type = aot();
        }
        if (type != 2 && type != 5 && type != 29) return;   // AAC LC, as Helix decodes it
        if (f) ascRate_ = f;
        if (ch >= 1 && ch <= 2) ascChannels_ = ch;
        else if (ch > 2) ascChannels_ = 2;   // Helix handles 2 at most as built
    }

    void tagItem(uint64_t start, uint64_t end, std::string& out) {
        r_.seek(start);
        const uint32_t size = r_.u32();
        if (r_.u32() != fourcc("data") || size < 16 || start + size > end) return;
        const uint32_t kind = r_.u32();
        r_.u32();
        const uint32_t n = std::min<uint32_t>(size - 16, 512);
        if (kind != 1) return;
        out.resize(n);
        out.resize(r_.read(&out[0], n));
    }

    void cover(uint64_t start, uint64_t end) {
        if (tags.coverKind) return;
        r_.seek(start);
        const uint32_t size = r_.u32();
        if (r_.u32() != fourcc("data") || size < 16 || start + size > end) return;
        const uint32_t kind = r_.u32();
        tags.coverOffset = start + 16;
        tags.coverSize = size - 16;
        uint8_t magic[2] = {};
        r_.readAt(tags.coverOffset, magic, 2);
        tags.coverKind = kind == 14 || (magic[0] == 0x89 && magic[1] == 'P') ? 2 : 1;
    }
};

}  // namespace

std::unique_ptr<Decoder> openMp4(const char* path) {
    auto d = std::make_unique<Mp4Decoder>();
    if (!d->open(path)) return nullptr;
    return d;
}
