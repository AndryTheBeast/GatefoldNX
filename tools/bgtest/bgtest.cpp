/*
 * Gatefold NX · tools/bgtest/bgtest.cpp
 * Runs the background player's decoders and resampler on the PC, so they can be checked
 * against FFmpeg before they reach the console (see check.py).
 *
 *   bgtest <in> <raw.wav> <48k.wav> [seek seconds <seek.wav>]
 * raw.wav is the decoder's own output (stereo s16 at the file's rate), 48k.wav the same after
 * the resampler, and seek.wav one second read after seeking.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "decoder.hpp"
#include "resampler.hpp"

static void wav(const char* path, const std::vector<int16_t>& s, int rate) {
    FILE* f = fopen(path, "wb");
    const uint32_t data = uint32_t(s.size() * 2), riff = 36 + data, fmt = 16, br = uint32_t(rate) * 4;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: bgtest in raw.wav 48k.wav [seek out.wav]\n"); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    auto d = openDecoder(argv[1]);
    if (!d) { fprintf(stderr, "cannot open\n"); return 1; }
    auto t1 = std::chrono::steady_clock::now();
    printf("codec=%d rate=%d ch=%d kbps=%d dur=%.3f open=%.1fms\n", d->codec, d->rate, d->channels, d->bitrate, d->duration,
           std::chrono::duration<double, std::milli>(t1 - t0).count());
    printf("title=%s\nartist=%s\nalbum=%s\ncover kind=%d at %llu size %u\n", d->tags.title.c_str(), d->tags.artist.c_str(), d->tags.album.c_str(),
           d->tags.coverKind, (unsigned long long)d->tags.coverOffset, d->tags.coverSize);

    std::vector<int16_t> raw, out48;
    Resampler rs;
    rs.reset(d->rate, 48000);
    int16_t buf[1024 * 2];
    std::vector<float> mix(2048 * 2);
    for (;;) {
        const int n = d->read(buf, 1024);
        if (n <= 0) break;
        raw.insert(raw.end(), buf, buf + n * 2);
        rs.push(buf, n);
        for (int got; (got = rs.pull(mix.data(), 2048)) > 0;)
            for (int i = 0; i < got * 2; i++) out48.push_back(int16_t(std::lrint(std::fmax(-32768.f, std::fmin(32767.f, mix[i] * 32767.f)))));
    }
    auto t2 = std::chrono::steady_clock::now();
    printf("frames=%zu (%.3f s) decode+resample=%.1fms\n", raw.size() / 2, raw.size() / 2.0 / d->rate,
           std::chrono::duration<double, std::milli>(t2 - t1).count());
    wav(argv[2], raw, d->rate);
    wav(argv[3], out48, 48000);
    if (argc >= 6) {
        d->seek(atof(argv[4]));
        std::vector<int16_t> s;
        while (int(s.size()) < d->rate * 2) {
            const int n = d->read(buf, 1024);
            if (n <= 0) break;
            s.insert(s.end(), buf, buf + n * 2);
        }
        wav(argv[5], s, d->rate);
    }
    return 0;
}
