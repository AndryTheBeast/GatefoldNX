/*
 * Gatefold NX · ffdecoder.hpp
 * FFmpeg for what SDL_mixer cannot open: M4A/MP4 (AAC, ALAC). Decodes to
 * interleaved 16-bit PCM at a chosen rate and channel count, and reads MP4 tags.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;
struct TrackInfo;

/** True for the extensions FFmpeg handles here (m4a, m4b, mp4, aac, alac). */
bool ffHandles(const std::string& path);

/** Tags, format and embedded cover of an MP4/AAC file. */
bool ffReadInfo(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics);

class FfDecoder {
public:
    ~FfDecoder() { close(); }
    bool open(const std::string& path, int outRate, int outChannels);
    void close();
    /** Fills up to `frames` frames; returns how many were written (0 at the end). */
    int read(int16_t* out, int frames);
    bool seek(double seconds);
    bool isOpen() const { return fmt_ != nullptr; }
    bool atEnd() const { return eof_ && pos_ >= fifo_.size(); }

private:
    bool decodeMore();

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1;
    int outRate_ = 48000, outCh_ = 2;
    std::vector<int16_t> fifo_;
    size_t pos_ = 0;
    bool eof_ = false;
};
