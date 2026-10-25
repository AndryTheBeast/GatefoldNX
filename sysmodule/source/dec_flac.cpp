/*
 * Gatefold NX background player · dec_flac.cpp
 * FLAC through dr_flac, which streams and seeks on its own. Pictures are not loaded into memory
 * (DR_FLAC_NO_PICTURE_METADATA_MALLOC): only where the front cover sits is kept, for the overlay.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_PICTURE_METADATA_MALLOC
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"

#include <cstdlib>
#include <strings.h>

#include "dec_common.hpp"

namespace {

class FlacDecoder : public Decoder {
public:
    ~FlacDecoder() override {
        if (f_) drflac_close(f_);
        free(tmp_);
    }

    bool open(const char* path) {
        f_ = drflac_open_file_with_metadata(path, &FlacDecoder::meta, this, nullptr);
        if (!f_ || f_->channels == 0 || f_->sampleRate == 0) return false;
        codec = 3;
        rate = int(f_->sampleRate);
        channels = f_->channels;
        duration = double(f_->totalPCMFrameCount) / rate;
        if (FILE* fp = fopen(path, "rb")) {
            fseek(fp, 0, SEEK_END);
            if (duration > 0) bitrate = int(double(ftell(fp)) * 8 / duration / 1000);
            fclose(fp);
        }
        if (channels != 2) tmp_ = static_cast<int16_t*>(malloc(size_t(Chunk) * channels * 2));
        return channels == 2 || tmp_;
    }

    int read(int16_t* out, int maxFrames) override {
        if (channels == 2) return int(drflac_read_pcm_frames_s16(f_, drflac_uint64(maxFrames), out));
        int done = 0;
        while (done < maxFrames) {
            const int want = std::min(Chunk, maxFrames - done);
            const int got = int(drflac_read_pcm_frames_s16(f_, drflac_uint64(want), tmp_));
            if (got <= 0) break;
            toStereo(tmp_, got, channels, out + done * 2);
            done += got;
        }
        return done;
    }

    bool seek(double seconds) override { return drflac_seek_to_pcm_frame(f_, drflac_uint64(seconds * rate)); }

private:
    static constexpr int Chunk = 1024;
    drflac* f_ = nullptr;
    int16_t* tmp_ = nullptr;
    int pictureType_ = -1;

    static void meta(void* self, drflac_metadata* m) {
        auto* d = static_cast<FlacDecoder*>(self);
        if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            drflac_vorbis_comment_iterator it;
            drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
            drflac_uint32 len;
            while (const char* c = drflac_next_vorbis_comment(&it, &len)) {
                const char* eq = static_cast<const char*>(memchr(c, '=', len));
                if (!eq) continue;
                const size_t k = size_t(eq - c);
                const std::string v(eq + 1, len - k - 1);
                if (k == 5 && !strncasecmp(c, "TITLE", 5) && d->tags.title.empty()) d->tags.title = v;
                else if (k == 6 && !strncasecmp(c, "ARTIST", 6) && d->tags.artist.empty()) d->tags.artist = v;
                else if (k == 5 && !strncasecmp(c, "ALBUM", 5) && d->tags.album.empty()) d->tags.album = v;
            }
        } else if (m->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
            // The front cover (type 3) wins over whatever came first.
            const int type = int(m->data.picture.type);
            if (d->pictureType_ == 3 || (d->pictureType_ >= 0 && type != 3)) return;
            const char* mime = m->data.picture.mime;
            const uint32_t ml = m->data.picture.mimeLength;
            const bool png = mime && ml >= 9 && !strncasecmp(mime, "image/png", 9);
            d->pictureType_ = type;
            d->tags.coverKind = png ? 2 : 1;
            d->tags.coverOffset = m->data.picture.pictureDataOffset;
            d->tags.coverSize = m->data.picture.pictureDataSize;
        }
    }
};

}  // namespace

std::unique_ptr<Decoder> openFlac(const char* path) {
    auto d = std::make_unique<FlacDecoder>();
    if (!d->open(path)) return nullptr;
    return d;
}
