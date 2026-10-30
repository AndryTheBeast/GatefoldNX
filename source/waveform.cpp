/*
 * Gatefold NX · waveform.cpp
 */
#include "waveform.hpp"

#include "cores.hpp"
#include "ffdecoder.hpp"

#include <FLAC/stream_decoder.h>
#include <mpg123.h>
#include <opus/opusfile.h>
#include <tremor/ivorbisfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int Block = 1024;   // frames per raw peak

/** Collects the mean square of each block (loudness, not peak: see waveform.hpp). */
struct Sink {
    const std::atomic<unsigned>* gen;
    unsigned want;
    std::vector<float> raw;
    double cur = 0;
    int n = 0;
    bool cancelled() const { return gen->load() != want; }
    void push(float v) {
        cur += double(v) * v;
        if (++n == Block) { raw.push_back(float(cur / Block)); cur = 0; n = 0; }
    }
    void push16(const int16_t* s, int frames, int ch) {
        for (int i = 0; i < frames; i++) {
            int m = 0;
            for (int c = 0; c < ch; c++) m = std::max(m, std::abs(int(s[i * ch + c])));
            push(m / 32768.f);
        }
    }
};

FLAC__StreamDecoderWriteStatus flacWrite(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* ud) {
    auto* s = static_cast<Sink*>(ud);
    if (s->cancelled()) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    const float scale = 1.f / float(1u << (frame->header.bits_per_sample - 1));
    for (unsigned i = 0; i < frame->header.blocksize; i++) {
        int m = 0;
        for (unsigned c = 0; c < frame->header.channels; c++) m = std::max(m, std::abs(int(buffer[c][i])));
        s->push(m * scale);
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
void flacError(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

void decodeFlac(const std::string& path, Sink& s) {
    FLAC__StreamDecoder* d = FLAC__stream_decoder_new();
    if (!d) return;
    if (FLAC__stream_decoder_init_file(d, path.c_str(), flacWrite, nullptr, flacError, &s) == FLAC__STREAM_DECODER_INIT_STATUS_OK)
        FLAC__stream_decoder_process_until_end_of_stream(d);
    FLAC__stream_decoder_delete(d);
}

void decodeMp3(const std::string& path, Sink& s) {
    mpg123_handle* mh = mpg123_new(nullptr, nullptr);
    if (!mh) return;
    // 16-bit output at a quarter of the rate: peaks survive, decoding is much cheaper.
    mpg123_param(mh, MPG123_DOWN_SAMPLE, 2, 0);
    mpg123_format_none(mh);
    const long* rates = nullptr;
    size_t nr = 0;
    mpg123_rates(&rates, &nr);
    for (size_t i = 0; i < nr; i++) mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    if (mpg123_open(mh, path.c_str()) == MPG123_OK) {
        long rate;
        int ch = 2, enc;
        mpg123_getformat(mh, &rate, &ch, &enc);
        std::vector<int16_t> buf(8192);
        size_t got = 0;
        int err;
        while (!s.cancelled() &&
               ((err = mpg123_read(mh, reinterpret_cast<unsigned char*>(buf.data()), buf.size() * 2, &got)) == MPG123_OK || err == MPG123_NEW_FORMAT)) {
            if (err == MPG123_NEW_FORMAT) { mpg123_getformat(mh, &rate, &ch, &enc); continue; }
            // Four output frames per input frame were dropped; repeat the peak so blocks match the other codecs.
            const int frames = int(got / 2 / ch);
            for (int i = 0; i < frames; i++) {
                int m = 0;
                for (int c = 0; c < ch; c++) m = std::max(m, std::abs(int(buf[i * ch + c])));
                for (int k = 0; k < 4; k++) s.push(m / 32768.f);
            }
        }
        mpg123_close(mh);
    }
    mpg123_delete(mh);
}

void decodeOpus(const std::string& path, Sink& s) {
    int err = 0;
    OggOpusFile* of = op_open_file(path.c_str(), &err);
    if (!of) return;
    const int ch = op_channel_count(of, -1);
    std::vector<int16_t> buf(5760 * 2 * 2);
    while (!s.cancelled()) {
        const int n = op_read(of, buf.data(), int(buf.size()), nullptr);
        if (n <= 0) break;
        s.push16(buf.data(), n, ch);
    }
    op_free(of);
}

void decodeVorbis(const std::string& path, Sink& s) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    OggVorbis_File vf;
    if (ov_open(f, &vf, nullptr, 0) != 0) { fclose(f); return; }
    const int ch = ov_info(&vf, -1) ? ov_info(&vf, -1)->channels : 2;
    std::vector<int16_t> buf(4096 * 2);
    int bs = 0;
    while (!s.cancelled()) {
        const long n = ov_read(&vf, reinterpret_cast<char*>(buf.data()), int(buf.size() * 2), &bs);
        if (n <= 0) break;
        s.push16(buf.data(), int(n / 2 / ch), ch);
    }
    ov_clear(&vf);
}

void decodeFf(const std::string& path, Sink& s) {
    // Mono at a quarter of 48 kHz is plenty for peaks; repeat each frame so blocks match the other codecs.
    FfDecoder d;
    if (!d.open(path, 12000, 1)) return;
    std::vector<int16_t> buf(4096);
    int n;
    while (!s.cancelled() && (n = d.read(buf.data(), int(buf.size()))) > 0)
        for (int i = 0; i < n; i++) {
            const float v = std::abs(int(buf[i])) / 32768.f;
            for (int k = 0; k < 4; k++) s.push(v);
        }
}

void decodeWav(const std::string& path, Sink& s) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    uint8_t hdr[12], ck[8];
    int ch = 2, bits = 16;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4)) {
        while (fread(ck, 1, 8, f) == 8) {
            const uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | uint32_t(ck[7]) << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fmt[16];
                if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
                ch = std::max(1, fmt[2] | fmt[3] << 8);
                bits = fmt[14] | fmt[15] << 8;
                fseek(f, long(size - 16 + (size & 1)), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                if (bits != 16) break;   // the prototype draws 16-bit PCM only
                std::vector<int16_t> buf(8192);
                size_t n;
                while (!s.cancelled() && (n = fread(buf.data(), 2, buf.size(), f)) > 0) s.push16(buf.data(), int(n / ch), ch);
                break;
            } else {
                fseek(f, long(size + (size & 1)), SEEK_CUR);
            }
        }
    }
    fclose(f);
}

}  // namespace

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
    cores::moveHere(cores::Wave);
    Sink s{ &gen_, gen, {} };
    std::string ext = path.substr(path.find_last_of('.') + 1);
    for (char& c : ext) c = char(tolower(c));
    if (ext == "flac") decodeFlac(path, s);
    else if (ext == "mp3") decodeMp3(path, s);
    else if (ext == "opus") decodeOpus(path, s);
    else if (ext == "ogg") decodeVorbis(path, s);
    else if (ext == "wav") decodeWav(path, s);
    else if (ffHandles(path)) decodeFf(path, s);
    if (s.cancelled() || s.raw.empty()) return;

    std::vector<float> out = shapeWaveform(s.raw, Resolution);
    if (gen_.load() != gen) return;
    peaks_ = std::move(out);
    ready_ = true;
}
