/* Gatefold NX preview · the waveform worker: 16-bit WAV, plus M4A when built with FFmpeg. */
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../source/ffdecoder.hpp"
#include "../../source/waveform.hpp"

void Waveform::cancel() {
    gen_++;
    if (thread_.joinable()) thread_.join();
    ready_ = false;
    path_.clear();
    peaks_.clear();
}

void Waveform::request(const std::string& path) {
    if (path == path_ && (ready_ || thread_.joinable())) return;
    cancel();
    ready_ = false;
    peaks_.clear();
    path_ = path;
    thread_ = std::thread(&Waveform::run, this, path, gen_.load());
}

void Waveform::run(std::string path, unsigned gen) {
    std::vector<float> raw;
    if (ffHandles(path)) {
        FfDecoder d;
        if (d.open(path, 12000, 1)) {
            std::vector<int16_t> buf(256);
            int n;
            while (gen_.load() == gen && (n = d.read(buf.data(), int(buf.size()))) > 0) {
                double sq = 0;
                for (int i = 0; i < n; i++) sq += double(buf[i]) * buf[i];
                raw.push_back(float(sq / n / (32768.0 * 32768.0)));
            }
        }
    }
    FILE* f = raw.empty() ? fopen(path.c_str(), "rb") : nullptr;
    uint8_t hdr[12], ck[8];
    int ch = 1;
    if (f && fread(hdr, 1, 12, f) == 12)
        while (fread(ck, 1, 8, f) == 8) {
            const uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | uint32_t(ck[7]) << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fmt[16];
                if (fread(fmt, 1, 16, f) != 16) break;
                ch = std::max(1, fmt[2] | fmt[3] << 8);
                fseek(f, long(size - 16), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                std::vector<int16_t> buf(1024 * ch);
                size_t n;
                while (gen_.load() == gen && (n = fread(buf.data(), 2, buf.size(), f)) > 0) {
                    double sq = 0;
                    for (size_t i = 0; i < n; i++) sq += double(buf[i]) * buf[i];
                    raw.push_back(float(sq / n / (32768.0 * 32768.0)));
                }
                break;
            } else {
                fseek(f, long(size + (size & 1)), SEEK_CUR);
            }
        }
    if (f) fclose(f);
    if (raw.empty() || gen_.load() != gen) return;
    peaks_ = shapeWaveform(raw, Resolution);
    ready_ = true;
}
