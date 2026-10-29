/*
 * Gatefold NX · ffdecoder.hpp
 * FFmpeg: Gatefold's own player decodes every format through it (player.cpp: AAC, ALAC, FLAC, MP3, Vorbis,
 * Opus, WAV), to interleaved 16-bit PCM at a chosen rate and channel count. It also reads MP4 tags; ffHandles
 * names the files whose tags and waveform come from FFmpeg (the others have readers of their own).
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
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

/**
 * Gatefold's player reads ahead (Automix v3, 2026-10-28: the user heard stutters and once a 2 s stop with Automix):
 * the audio callback used to decode in place, reading the SD card itself, two files at once during a join, while the
 * analysis worker read the card hard. Now a feeder thread (core 1, just below audio: cores::Feed) keeps each playing
 * decoder's ring of decoded PCM full, and the callback only copies from it. Started and stopped by the player.
 */
void ffFeederStart();
void ffFeederStop();
unsigned ffStarved();   // reads that found the ring short and had to decode in place (the dev build's audio trace)

class FfDecoder {
public:
    ~FfDecoder() { close(); }
    bool open(const std::string& path, int outRate, int outChannels);
    void close();
    /** Fills up to `frames` frames; returns how many were written (fewer only at the end). */
    int read(int16_t* out, int frames);
    bool seek(double seconds);
    /**
     * Right after open: drops exactly that much from the start, decoding it (Automix starts a track on its first beat).
     * Decoded from the start as the analysis decoded it, so the two agree to the sample; past 30 s, seek() instead.
     */
    bool skip(double seconds);
    /** After open (and skip): read ahead into a ring of `frames` frames, `first` of them decoded here, now. */
    void prefetch(int frames, int first);
    bool isOpen() const { return fmt_ != nullptr; }
    bool atEnd() const { return pf_ ? rEof_.load() && rw_.load() == rr_.load() : eof_ && pos_ >= fifo_.size(); }
    bool fillAhead(int frames);   // the feeder: decodes up to that many frames into the ring (false: full or ended)

private:
    bool decodeMore();
    int readDirect(int16_t* out, int frames);
    int takeRing(int16_t* out, int frames);
    void fillLocked(int frames);

    // the read-ahead ring (prefetch): frames written and read ever; its size a power of two
    std::vector<int16_t> ring_;
    size_t ringFrames_ = 0;
    std::atomic<size_t> rw_{ 0 }, rr_{ 0 };
    std::atomic<bool> rEof_{ false };
    std::mutex dm_;   // the decoder itself: held by the feeder while it decodes, by seek and close
    bool pf_ = false;

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
