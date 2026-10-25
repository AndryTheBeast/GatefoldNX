/*
 * Gatefold NX background player · dec_common.hpp
 * What the frame-based decoders (AAC, MP3) share: a frame is decoded whole into a pending
 * buffer, and read() hands it out in whatever slices the mixer asks for.
 */
#pragma once
#include <algorithm>
#include <cstring>

#include "decoder.hpp"

class FrameDecoder : public Decoder {
public:
    int read(int16_t* out, int maxFrames) override {
        int done = 0;
        while (done < maxFrames) {
            if (pendPos_ >= pendLen_) {
                pendPos_ = 0;
                pendLen_ = decodeFrame(pend_);
                if (pendLen_ < 0) return done ? done : pendLen_;
                if (pendLen_ == 0) break;
                // The encoder's priming samples (AAC's edit list, LAME's delay) are not part of the song.
                if (skip_ > 0) {
                    const int s = skip_ < pendLen_ ? skip_ : pendLen_;
                    pendPos_ = s;
                    skip_ -= s;
                    continue;
                }
            }
            int n = std::min(maxFrames - done, pendLen_ - pendPos_);
            // The encoder's padding at the end (LAME's) is not part of it either.
            if (left_ >= 0) {
                n = int(std::min<int64_t>(n, left_));
                if (n == 0) break;
                left_ -= n;
            }
            memcpy(out + done * 2, pend_ + pendPos_ * 2, size_t(n) * 4);
            done += n;
            pendPos_ += n;
        }
        return done;
    }

protected:
    /** One frame as stereo s16 into out (MaxFrame frames at most); 0 at the end, negative on error. */
    virtual int decodeFrame(int16_t* out) = 0;
    void dropPending() { pendPos_ = pendLen_ = 0; skip_ = 0; }

    static constexpr int MaxFrame = 2048;   // HE-AAC's SBR doubles AAC's 1024
    int skip_ = 0;
    int64_t left_ = -1;   // frames still to hand out, −1 when the end is wherever the data ends

private:
    int16_t pend_[MaxFrame * 2];
    int pendPos_ = 0, pendLen_ = 0;
};

/** Latin-1 or UTF-16 (with or without a BOM) text from a tag, as UTF-8. */
std::string tagText(const uint8_t* p, size_t n, int encoding);

std::unique_ptr<Decoder> openMp4(const char* path);
std::unique_ptr<Decoder> openMp3(const char* path);
std::unique_ptr<Decoder> openFlac(const char* path);
std::unique_ptr<Decoder> openWav(const char* path);
