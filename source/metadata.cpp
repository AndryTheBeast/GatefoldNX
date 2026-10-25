/*
 * Gatefold NX · metadata.cpp
 */
#include "metadata.hpp"

#include "ffdecoder.hpp"

#include <FLAC/metadata.h>
#include <mpg123.h>
#include <opus/opusfile.h>
#include <tremor/ivorbisfile.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern "C" {
#include <libavutil/log.h>
}

namespace {

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = char(tolower(c));
    return e;
}

/** "KEY=value" Vorbis comment → value, if the key matches (case-insensitive). */
bool commentValue(const char* entry, size_t len, const char* key, std::string& out) {
    const size_t k = strlen(key);
    if (len <= k || entry[k] != '=' || strncasecmp(entry, key, k) != 0) return false;
    out.assign(entry + k + 1, len - k - 1);
    return true;
}

/** Ogg files carry art as a base64 FLAC picture block; opusfile parses that for Vorbis too. */
void oggPicture(const char* tag, std::vector<uint8_t>& cover) {
    if (!tag) return;
    OpusPictureTag pic;
    opus_picture_tag_init(&pic);
    if (opus_picture_tag_parse(&pic, tag) == 0 && pic.data_length > 0 &&
        (pic.format == OP_PIC_FORMAT_JPEG || pic.format == OP_PIC_FORMAT_PNG))
        cover.assign(pic.data, pic.data + pic.data_length);
    opus_picture_tag_clear(&pic);
}

void readFlac(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    out.codec = "FLAC";
    FLAC__StreamMetadata si;
    if (FLAC__metadata_get_streaminfo(path.c_str(), &si)) {
        const auto& s = si.data.stream_info;
        out.sampleRate = int(s.sample_rate);
        out.bitsPerSample = int(s.bits_per_sample);
        if (s.sample_rate) out.duration = double(s.total_samples) / s.sample_rate;
    }
    FLAC__StreamMetadata* tags = nullptr;
    if (FLAC__metadata_get_tags(path.c_str(), &tags) && tags) {
        const auto& vc = tags->data.vorbis_comment;
        for (unsigned i = 0; i < vc.num_comments; i++) {
            const char* e = reinterpret_cast<const char*>(vc.comments[i].entry);
            const size_t n = vc.comments[i].length;
            std::string v;
            if (out.title.empty() && commentValue(e, n, "TITLE", v)) out.title = v;
            else if (out.artist.empty() && commentValue(e, n, "ARTIST", v)) out.artist = v;
            else if (out.album.empty() && commentValue(e, n, "ALBUM", v)) out.album = v;
            else if (out.date.empty() && commentValue(e, n, "DATE", v)) out.date = v;
            else if (out.albumArtist.empty() && (commentValue(e, n, "ALBUMARTIST", v) || commentValue(e, n, "ALBUM ARTIST", v))) out.albumArtist = v;
            else if (!out.trackNo && commentValue(e, n, "TRACKNUMBER", v)) out.trackNo = atoi(v.c_str());
            else if (commentValue(e, n, "GENRE", v)) out.genre += (out.genre.empty() ? "" : "; ") + v;   // one comment per genre
            else if (wantLyrics && out.lyrics.empty() && (commentValue(e, n, "LYRICS", v) || commentValue(e, n, "UNSYNCEDLYRICS", v))) out.lyrics = v;
        }
        FLAC__metadata_object_delete(tags);
    }
    if (!wantCover) return;
    // Front cover first, then any picture.
    FLAC__StreamMetadata* pic = nullptr;
    const unsigned any = unsigned(-1);
    if (!FLAC__metadata_get_picture(path.c_str(), &pic, FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER, nullptr, nullptr, any, any, any, any))
        FLAC__metadata_get_picture(path.c_str(), &pic, FLAC__StreamMetadata_Picture_Type(-1), nullptr, nullptr, any, any, any, any);
    if (pic) {
        const auto& p = pic->data.picture;
        out.cover.assign(p.data, p.data + p.data_length);
        FLAC__metadata_object_delete(pic);
    }
}

void readMp3(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    out.codec = "MP3";
    mpg123_handle* mh = mpg123_new(nullptr, nullptr);
    if (!mh) return;
    if (wantCover) mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_PICTURE, 0);
    if (mpg123_open(mh, path.c_str()) == MPG123_OK) {
        long rate = 0;
        int ch = 0, enc = 0;
        if (mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK) {
            out.sampleRate = int(rate);
            // Without a full scan this uses the Xing/Info header or a CBR estimate.
            const off_t len = mpg123_length(mh);
            if (len > 0 && rate > 0) out.duration = double(len) / rate;
            mpg123_frameinfo fi;
            if (mpg123_info(mh, &fi) == MPG123_OK) out.bitrateKbps = fi.bitrate;
        }
        mpg123_id3v1* v1 = nullptr;
        mpg123_id3v2* v2 = nullptr;
        if (mpg123_id3(mh, &v1, &v2) == MPG123_OK) {
            auto str = [](mpg123_string* s) { return (s && s->fill > 1) ? std::string(s->p, s->fill - 1) : std::string(); };
            if (v2) {
                out.title = str(v2->title);
                out.artist = str(v2->artist);
                out.album = str(v2->album);
                out.date = str(v2->year);
                out.genre = cleanGenre(str(v2->genre));
                for (size_t i = 0; i < v2->texts; i++) {
                    if (!memcmp(v2->text[i].id, "TPE2", 4)) out.albumArtist = str(&v2->text[i].text);
                    else if (!memcmp(v2->text[i].id, "TRCK", 4)) out.trackNo = atoi(str(&v2->text[i].text).c_str());
                }
                if (wantLyrics)
                    for (size_t i = 0; i < v2->texts; i++)
                        if (!memcmp(v2->text[i].id, "USLT", 4) && v2->text[i].text.fill > 1) { out.lyrics = str(&v2->text[i].text); break; }
                const mpg123_picture* best = nullptr;
                for (size_t i = 0; wantCover && i < v2->pictures; i++)
                    if (!best || v2->picture[i].type == mpg123_id3_pic_front_cover) best = &v2->picture[i];
                if (best && best->size) out.cover.assign(best->data, best->data + best->size);
            } else if (v1) {
                out.title.assign(v1->title, strnlen(v1->title, sizeof v1->title));
                out.artist.assign(v1->artist, strnlen(v1->artist, sizeof v1->artist));
                out.album.assign(v1->album, strnlen(v1->album, sizeof v1->album));
                out.genre = cleanGenre(std::to_string(int(v1->genre)));
            }
        }
        mpg123_close(mh);
    }
    mpg123_delete(mh);
}

void readOpus(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    out.codec = "Opus";
    int err = 0;
    OggOpusFile* of = op_open_file(path.c_str(), &err);
    if (!of) return;
    out.sampleRate = 48000;
    const ogg_int64_t pcm = op_pcm_total(of, -1);
    if (pcm > 0) out.duration = pcm / 48000.0;
    const opus_int32 br = op_bitrate(of, -1);
    if (br > 0) out.bitrateKbps = br / 1000;
    if (const OpusTags* t = op_tags(of, -1)) {
        auto q = [&](const char* k) { const char* v = opus_tags_query(t, k, 0); return v ? std::string(v) : std::string(); };
        out.title = q("TITLE"); out.artist = q("ARTIST"); out.album = q("ALBUM"); out.date = q("DATE");
        out.albumArtist = q("ALBUMARTIST"); if (out.albumArtist.empty()) out.albumArtist = q("ALBUM ARTIST");
        out.trackNo = atoi(q("TRACKNUMBER").c_str());
        out.genre = q("GENRE");
        if (wantLyrics) { out.lyrics = q("LYRICS"); if (out.lyrics.empty()) out.lyrics = q("UNSYNCEDLYRICS"); }
        if (wantCover) oggPicture(opus_tags_query(t, "METADATA_BLOCK_PICTURE", 0), out.cover);
    }
    op_free(of);
}

void readVorbis(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    out.codec = "Vorbis";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    OggVorbis_File vf;
    if (ov_open(f, &vf, nullptr, 0) != 0) { fclose(f); return; }   // ov_clear closes f on success
    if (vorbis_info* vi = ov_info(&vf, -1)) {
        out.sampleRate = int(vi->rate);
        if (vi->bitrate_nominal > 0) out.bitrateKbps = int(vi->bitrate_nominal / 1000);
    }
    const ogg_int64_t ms = ov_time_total(&vf, -1);   // Tremor reports milliseconds
    if (ms > 0) out.duration = ms / 1000.0;
    if (vorbis_comment* vc = ov_comment(&vf, -1)) {
        auto q = [&](const char* k) { char* v = vorbis_comment_query(vc, const_cast<char*>(k), 0); return v ? std::string(v) : std::string(); };
        out.title = q("TITLE"); out.artist = q("ARTIST"); out.album = q("ALBUM"); out.date = q("DATE");
        out.albumArtist = q("ALBUMARTIST"); if (out.albumArtist.empty()) out.albumArtist = q("ALBUM ARTIST");
        out.trackNo = atoi(q("TRACKNUMBER").c_str());
        out.genre = q("GENRE");
        if (wantLyrics) { out.lyrics = q("LYRICS"); if (out.lyrics.empty()) out.lyrics = q("UNSYNCEDLYRICS"); }
        if (wantCover) oggPicture(vorbis_comment_query(vc, const_cast<char*>("METADATA_BLOCK_PICTURE"), 0), out.cover);
    }
    ov_clear(&vf);
}

void readWav(const std::string& path, TrackInfo& out) {
    out.codec = "WAV";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    uint8_t hdr[12];
    uint32_t byteRate = 0;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        uint8_t ck[8];
        while (fread(ck, 1, 8, f) == 8) {
            const uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | uint32_t(ck[7]) << 24;
            if (!memcmp(ck, "fmt ", 4) && size >= 16) {
                uint8_t fmt[16];
                if (fread(fmt, 1, 16, f) != 16) break;
                out.sampleRate = int(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | uint32_t(fmt[7]) << 24);
                byteRate = fmt[8] | fmt[9] << 8 | fmt[10] << 16 | uint32_t(fmt[11]) << 24;
                out.bitsPerSample = fmt[14] | fmt[15] << 8;
                fseek(f, long(size - 16 + (size & 1)), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                if (byteRate) out.duration = double(size) / byteRate;
                break;
            } else {
                fseek(f, long(size + (size & 1)), SEEK_CUR);
            }
        }
    }
    fclose(f);
}

}  // namespace

void metadataInit() {
    mpg123_init();
    av_log_set_level(AV_LOG_QUIET);   // FFmpeg would otherwise print every oddity in a file to the console
}

bool readTrackInfo(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    const std::string ext = lowerExt(path);
    if (ext == "flac") readFlac(path, out, wantCover, wantLyrics);
    else if (ext == "mp3") readMp3(path, out, wantCover, wantLyrics);
    else if (ext == "opus") readOpus(path, out, wantCover, wantLyrics);
    else if (ext == "ogg") readVorbis(path, out, wantCover, wantLyrics);
    else if (ext == "wav") readWav(path, out);
    else if (ffHandles(path)) return ffReadInfo(path, out, wantCover, wantLyrics);
    else return false;
    return true;
}

std::string readSidecarLyrics(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
    for (const char* ext : { ".lrc", ".LRC", ".txt" }) {
        FILE* f = fopen((stem + ext).c_str(), "rb");
        if (!f) continue;
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < 256 * 1024) s.append(buf, n);
        fclose(f);
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
        return s;
    }
    return {};
}
