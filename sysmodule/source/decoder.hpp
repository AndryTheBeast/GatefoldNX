/*
 * Gatefold NX background player · decoder.hpp
 * Small, streaming decoders that fit a sysmodule's few MB: AAC in MP4/M4A (our own demuxer and
 * Helix), MP3 (minimp3), FLAC (dr_flac) and WAV. Nothing here uses libnx, so the PC test
 * (tools/bgtest) runs the very same code.
 *
 * Every decoder hands out interleaved 16-bit stereo, whatever the file has.
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

struct Tags {
    std::string title, artist, album;
    int coverKind = 0;          // 0 none, 1 JPEG, 2 PNG (as GfbgTrackInfo)
    uint64_t coverOffset = 0;   // in the file
    uint32_t coverSize = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    /** Up to maxFrames stereo frames into out; 0 at the end, negative on a broken file. */
    virtual int read(int16_t* out, int maxFrames) = 0;
    virtual bool seek(double seconds) = 0;

    int rate = 0;          // the file's sample rate
    int channels = 0;      // the file's channel count (the output is stereo regardless)
    int codec = 0;         // GfbgCodec
    int bitrate = 0;       // kbit/s
    double duration = 0;
    Tags tags;
};

/** Opens path with whichever decoder its contents call for; null when none can. */
std::unique_ptr<Decoder> openDecoder(const char* path);

// ---------------------------------------------------------------- shared helpers

/** A FILE* with a read buffer of its own: newlib's is small, and SD reads like big blocks. */
class Reader {
public:
    ~Reader() { close(); }
    bool open(const char* path);
    void close();
    size_t read(void* dst, size_t n);
    bool seek(uint64_t pos);
    uint64_t tell() const { return pos_; }
    uint64_t size() const { return size_; }
    bool readAt(uint64_t pos, void* dst, size_t n) { return seek(pos) && read(dst, n) == n; }
    uint32_t u32();
    uint16_t u16();
    uint8_t u8();
    uint64_t u64();

private:
    FILE* f_ = nullptr;
    uint64_t pos_ = 0, size_ = 0;
    uint64_t bufStart_ = 0;
    size_t bufLen_ = 0;
    static constexpr size_t Buf = 16384;
    uint8_t* buf_ = nullptr;
};

/** Mono, 3+ channels and 8/24/32-bit all come out as interleaved stereo s16. */
void toStereo(const int16_t* in, int frames, int channels, int16_t* out);
