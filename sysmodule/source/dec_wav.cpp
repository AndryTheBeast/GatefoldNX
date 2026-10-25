/*
 * Gatefold NX background player · dec_wav.cpp
 * WAV: 8/16/24/32-bit PCM and 32-bit float, any channel count, as stereo s16.
 */
#include <cmath>
#include <cstdlib>

#include "dec_common.hpp"

namespace {

class WavDecoder : public Decoder {
public:
    bool open(const char* path) {
        if (!r_.open(path)) return false;
        uint64_t at = 12;
        int format = 0;
        while (at + 8 <= r_.size()) {
            uint8_t h[8];
            if (!r_.readAt(at, h, 8)) return false;
            const uint32_t size = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
            if (!memcmp(h, "fmt ", 4) && size >= 16) {
                uint8_t f[40] = {};
                r_.read(f, std::min<uint32_t>(size, sizeof f));
                format = f[0] | f[1] << 8;
                channels = f[2] | f[3] << 8;
                rate = int(uint32_t(f[4]) | uint32_t(f[5]) << 8 | uint32_t(f[6]) << 16 | uint32_t(f[7]) << 24);
                bits_ = f[14] | f[15] << 8;
                if (format == 0xFFFE && size >= 26) format = f[24] | f[25] << 8;   // WAVE_FORMAT_EXTENSIBLE: the sub-format
            } else if (!memcmp(h, "data", 4)) {
                data_ = at + 8;
                end_ = std::min<uint64_t>(data_ + size, r_.size());
                break;
            }
            at += 8 + size + (size & 1);
        }
        float_ = format == 3;
        if (!data_ || channels <= 0 || rate <= 0 || (format != 1 && format != 3)) return false;
        if (float_ ? bits_ != 32 : (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32)) return false;
        frameBytes_ = channels * bits_ / 8;
        codec = 4;
        bitrate = rate * frameBytes_ * 8 / 1000;
        duration = double(end_ - data_) / frameBytes_ / rate;
        r_.seek(data_);
        return true;
    }

    int read(int16_t* out, int maxFrames) override {
        int done = 0;
        uint8_t raw[4096];
        while (done < maxFrames) {
            const uint64_t left = (end_ - r_.tell()) / frameBytes_;
            const int want = int(std::min<uint64_t>({ uint64_t(maxFrames - done), left, uint64_t(sizeof raw / frameBytes_) }));
            if (want <= 0) break;
            if (r_.read(raw, size_t(want) * frameBytes_) != size_t(want) * frameBytes_) break;
            for (int i = 0; i < want; i++) {
                const uint8_t* fr = raw + i * frameBytes_;
                for (int c = 0; c < 2; c++) out[(done + i) * 2 + c] = sample(fr, channels == 1 ? 0 : c);
            }
            done += want;
        }
        return done;
    }

    bool seek(double seconds) override {
        uint64_t f = uint64_t(std::max(0.0, seconds) * rate);
        return r_.seek(std::min(end_, data_ + f * frameBytes_));
    }

private:
    Reader r_;
    uint64_t data_ = 0, end_ = 0;
    int bits_ = 0, frameBytes_ = 0;
    bool float_ = false;

    int16_t sample(const uint8_t* fr, int c) const {
        const uint8_t* p = fr + c * bits_ / 8;
        switch (bits_) {
        case 8: return int16_t((p[0] - 128) << 8);
        case 16: return int16_t(p[0] | p[1] << 8);
        case 24: return int16_t(p[1] | p[2] << 8);
        default:
            if (float_) {
                float v;
                memcpy(&v, p, 4);
                return int16_t(std::lrint(std::fmax(-1.f, std::fmin(1.f, v)) * 32767));
            }
            return int16_t(p[2] | p[3] << 8);
        }
    }
};

}  // namespace

std::unique_ptr<Decoder> openWav(const char* path) {
    auto d = std::make_unique<WavDecoder>();
    if (!d->open(path)) return nullptr;
    return d;
}
