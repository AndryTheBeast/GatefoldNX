/* Gatefold NX preview · metadata without the codec libraries: WAV, plus M4A when built with FFmpeg. */
#include <cstring>

#include "../../source/ffdecoder.hpp"
#include "../../source/metadata.hpp"
#include "../../source/coverio.hpp"
#include <strings.h>

void metadataInit() {}

bool readTrackInfo(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    const size_t fdot = path.find_last_of('.');
    const bool flac = fdot != std::string::npos && !strcasecmp(path.c_str() + fdot, ".flac");
    if (ffHandles(path) || flac) {   // (a FLAC's tags through FFmpeg here; the console reads them with libFLAC)
        const bool ok = ffReadInfo(path, out, wantCover, wantLyrics);
        if (flac) out.codec = "FLAC";
        // a FLAC's cover by the console's own reader (coverio.cpp), so the preview shows what the console reads
        const size_t dot = fdot;
        if (ok && wantCover && flac) {
            std::vector<uint8_t> pic;
            if (flacPicture(path, pic)) out.cover.swap(pic);
            else out.cover.clear();
        }
        return ok;
    }
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = char(toupper(c));
    out.codec = ext;
    if (ext != "WAV") return true;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[12], ck[8];
    uint32_t byteRate = 0;
    if (fread(hdr, 1, 12, f) == 12)
        while (fread(ck, 1, 8, f) == 8) {
            const uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | uint32_t(ck[7]) << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fmt[16];
                if (fread(fmt, 1, 16, f) != 16) break;
                out.sampleRate = int(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | uint32_t(fmt[7]) << 24);
                byteRate = fmt[8] | fmt[9] << 8 | fmt[10] << 16 | uint32_t(fmt[11]) << 24;
                out.bitsPerSample = fmt[14] | fmt[15] << 8;
                fseek(f, long(size - 16), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                if (byteRate) out.duration = double(size) / byteRate;
                break;
            } else {
                fseek(f, long(size + (size & 1)), SEEK_CUR);
            }
        }
    fclose(f);
    return true;
}

std::string readSidecarLyrics(const std::string& path) {
    const std::string stem = path.substr(0, path.find_last_of('.'));
    FILE* f = fopen((stem + ".lrc").c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
