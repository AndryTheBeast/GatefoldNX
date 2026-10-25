#include "tag_reader.hpp"

#include <cstring>
#include <algorithm>
#include <memory>
#include <switch.h>   // u8, u16, u32

namespace {

static u32 ta_be32(const u8 *p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
}
static u32 ta_le32(const u8 *p) {
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static u32 ta_syncsafe(const u8 *p) {
    return ((u32)(p[0]&0x7F)<<21)|((u32)(p[1]&0x7F)<<14)|
           ((u32)(p[2]&0x7F)<<7) |(u32)(p[3]&0x7F);
}
static bool ta_keycmp(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if ((*a | 0x20) != (*b | 0x20)) return false;
    return *a == '\0' && *b == '\0';
}

static std::string ta_decodeString(const u8 *payload, size_t size) {
    if (size < 1) return {};
    u8 enc = payload[0];
    const u8 *s = payload + 1;
    size_t    n = size   - 1;

    if (enc == 0 || enc == 3) {
        size_t len = 0;
        while (len < n && s[len]) len++;
        return std::string(reinterpret_cast<const char*>(s), len);
    }

    bool be = (enc == 2);
    size_t i = 0;
    if (enc == 1 && n >= 2) {
        if      (s[0]==0xFE && s[1]==0xFF) { be=true;  i=2; }
        else if (s[0]==0xFF && s[1]==0xFE) { be=false; i=2; }
    }
    std::string r;
    r.reserve(n / 2);
    for (; i+1 < n; i+=2) {
        u16 cp = be ? (u16)((s[i]<<8)|s[i+1]) : (u16)(s[i]|(s[i+1]<<8));
        if (!cp) break;
        if      (cp < 0x80)  { r += (char)cp; }
        else if (cp < 0x800) { r += (char)(0xC0|(cp>>6));
                               r += (char)(0x80|(cp&0x3F)); }
        else                 { r += (char)(0xE0|(cp>>12));
                               r += (char)(0x80|((cp>>6)&0x3F));
                               r += (char)(0x80|(cp&0x3F)); }
    }
    return r;
}

// ---- ID3v2 (MP3) ------------------------------------------------------------

/* Streaming frame scanner — walks ID3v2 frames directly from FILE*, with only
 * a 512-byte stack buffer for text payloads.  Zero heap allocation. */
static void ta_scanID3Stream(FILE *f, u8 ver, long end, TitleArtist &ta) {
    constexpr size_t kTextMax = 512;
    u8 textBuf[kTextMax];

    while (ta.title.empty() || ta.artist.empty()) {
        const long here = ftell(f);
        if (here < 0 || here >= end) break;

        if (ver == 2) {
            if (here + 6 > end) break;
            u8 fh[6];
            if (fread(fh,1,6,f) != 6 || !fh[0]) break;
            const u32 sz = ((u32)fh[3]<<16)|((u32)fh[4]<<8)|(u32)fh[5];
            if (sz == 0 || here + 6 + (long)sz > end) break;
            const long frameEnd = here + 6 + (long)sz;
            if (ta.title.empty()  && memcmp(fh,"TT2",3) == 0) {
                const size_t rd = std::min((size_t)sz, kTextMax);
                if (fread(textBuf,1,rd,f) == rd) ta.title  = ta_decodeString(textBuf, rd);
            } else if (ta.artist.empty() && memcmp(fh,"TP1",3) == 0) {
                const size_t rd = std::min((size_t)sz, kTextMax);
                if (fread(textBuf,1,rd,f) == rd) ta.artist = ta_decodeString(textBuf, rd);
            }
            fseek(f, frameEnd, SEEK_SET);
        } else {
            if (here + 10 > end) break;
            u8 fh[10];
            if (fread(fh,1,10,f) != 10 || !fh[0]) break;
            const u32 sz = (ver == 4) ? ta_syncsafe(fh+4) : ta_be32(fh+4);
            if (sz == 0) continue;
            if (here + 10 + (long)sz > end) break;
            const long frameEnd = here + 10 + (long)sz;
            if (ta.title.empty()  && memcmp(fh,"TIT2",4) == 0) {
                const size_t rd = std::min((size_t)sz, kTextMax);
                if (fread(textBuf,1,rd,f) == rd) ta.title  = ta_decodeString(textBuf, rd);
            } else if (ta.artist.empty() && memcmp(fh,"TPE1",4) == 0) {
                const size_t rd = std::min((size_t)sz, kTextMax);
                if (fread(textBuf,1,rd,f) == rd) ta.artist = ta_decodeString(textBuf, rd);
            }
            fseek(f, frameEnd, SEEK_SET);
        }
    }
}

static TitleArtist ta_readID3(FILE *f) {
    fseek(f, 0, SEEK_SET);
    u8 hdr[10];
    if (fread(hdr,1,10,f) != 10 || memcmp(hdr,"ID3",3) != 0) return {};
    const u8  ver     = hdr[3];
    const u8  flags   = hdr[5];
    const u32 tagSize = ta_syncsafe(hdr+6);
    if (tagSize > 32u * 1024u * 1024u) return {};

    /* Skip extended header (ID3v2.3/v2.4). */
    if ((flags & 0x40) && ver >= 3) {
        u8 ex[4];
        if (fread(ex,1,4,f) != 4) return {};
        const u32 exSz = (ver == 4) ? ta_syncsafe(ex) : ta_be32(ex);
        if (exSz < 4) return {};
        fseek(f, (long)(exSz - 4), SEEK_CUR);
    }

    TitleArtist ta;
    ta_scanID3Stream(f, ver, 10 + (long)tagSize, ta);
    return ta;
}

// ---- FLAC / Vorbis comment --------------------------------------------------

static void ta_readVorbisComment(FILE *f, u32 blkLen,
                                 std::string &title, std::string &artist) {
    u8 tmp[4];
    if (fread(tmp,1,4,f)!=4) return;
    u32 vlen = ta_le32(tmp);
    if (vlen > blkLen) return;
    fseek(f, (long)vlen, SEEK_CUR);
    if (fread(tmp,1,4,f)!=4) return;
    u32 count = ta_le32(tmp);
    if (count > 2000) return;

    char buf[512];
    for (u32 i = 0; i < count; i++) {
        if (fread(tmp,1,4,f)!=4) return;
        u32 clen = ta_le32(tmp);
        if (clen == 0) continue;
        if (clen > 65536) { fseek(f, (long)clen, SEEK_CUR); continue; }

        size_t toRead = std::min((size_t)clen, sizeof(buf)-1);
        if (fread(buf, 1, toRead, f) != toRead) return;
        buf[toRead] = '\0';
        if (clen > (u32)toRead) fseek(f, (long)(clen-toRead), SEEK_CUR);

        char *eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;

        if      (title.empty()  && ta_keycmp(buf, "TITLE"))  title  = val;
        else if (artist.empty() && ta_keycmp(buf, "ARTIST")) artist = val;
        if (!title.empty() && !artist.empty()) return;
    }
}

static TitleArtist ta_readFLAC(FILE *f) {
    TitleArtist ta;
    fseek(f, 4, SEEK_SET);
    u8 bh[4];
    while (fread(bh,1,4,f)==4) {
        bool last = (bh[0]&0x80)!=0;
        u8   type = bh[0]&0x7F;
        u32  len  = ((u32)bh[1]<<16)|((u32)bh[2]<<8)|(u32)bh[3];
        if (len > 16*1024*1024) break;
        long blockStart = ftell(f);
        if (type == 4 && (ta.title.empty() || ta.artist.empty()))
            ta_readVorbisComment(f, len, ta.title, ta.artist);
        if (last) break;
        if (!ta.title.empty() && !ta.artist.empty()) break;
        fseek(f, blockStart + (long)len, SEEK_SET);
    }
    return ta;
}

// ---- WAV / ID3-in-RIFF ------------------------------------------------------

static TitleArtist ta_readWAV(FILE *f) {
    fseek(f, 12, SEEK_SET);
    u8 ch[8];
    while (fread(ch,1,8,f) == 8) {
        const u32 sz = ta_le32(ch+4);
        if (sz > 32u * 1024u * 1024u) break;
        if (memcmp(ch,"id3 ",4) == 0 || memcmp(ch,"ID3 ",4) == 0) {
            const long chunkStart = ftell(f);
            const long chunkEnd   = chunkStart + (long)sz;

            u8 id3hdr[10];
            if (fread(id3hdr,1,10,f) != 10 || memcmp(id3hdr,"ID3",3) != 0) return {};
            const u8  ver     = id3hdr[3];
            const u8  flags   = id3hdr[5];
            const u32 tagSize = ta_syncsafe(id3hdr + 6);

            if ((flags & 0x40) && ver >= 3) {
                u8 ex[4];
                if (fread(ex,1,4,f) != 4) return {};
                const u32 exSz = (ver == 4) ? ta_syncsafe(ex) : ta_be32(ex);
                if (exSz < 4) return {};
                fseek(f, (long)(exSz - 4), SEEK_CUR);
            }

            const long end = std::min(chunkEnd, chunkStart + (long)(10 + tagSize));
            TitleArtist ta;
            ta_scanID3Stream(f, ver, end, ta);
            return ta;
        }
        fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    return {};
}

// ---- MP4 / M4A (Gatefold) ---------------------------------------------------
// moov/udta/meta/ilst: the ©nam and ©ART items, each holding a "data" box of UTF-8.
// Boxes are walked with seeks; the cover art (covr, often megabytes) is never read.

static void ta_mp4Walk(FILE *f, long start, long end, int depth, TitleArtist &ta) {
    long at = start;
    while (at + 8 <= end && depth < 8) {
        u8 h[8];
        if (fseek(f, at, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) return;
        long size = (long)ta_be32(h);
        long head = 8;
        if (size == 1) {
            u8 big[8];
            if (fread(big, 1, 8, f) != 8) return;
            size = (long)(((u64)ta_be32(big) << 32) | ta_be32(big + 4));
            head = 16;
        } else if (size == 0) {
            size = end - at;
        }
        if (size < head || at + size > end) return;
        const long body = at + head, boxEnd = at + size;
        if (!memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "udta", 4) || !memcmp(h + 4, "ilst", 4)) {
            ta_mp4Walk(f, body, boxEnd, depth + 1, ta);
        } else if (!memcmp(h + 4, "meta", 4)) {
            /* ISO's meta is a full box (4 bytes of version and flags); QuickTime's is not. */
            u8 peek[8];
            fseek(f, body, SEEK_SET);
            const bool quicktime = fread(peek, 1, 8, f) == 8 && !memcmp(peek + 4, "hdlr", 4);
            ta_mp4Walk(f, quicktime ? body : body + 4, boxEnd, depth + 1, ta);
        } else if ((h[4] == 0xA9 && !memcmp(h + 5, "nam", 3)) || (h[4] == 0xA9 && !memcmp(h + 5, "ART", 3))) {
            std::string &out = h[5] == 'n' ? ta.title : ta.artist;
            u8 d[16];
            fseek(f, body, SEEK_SET);
            if (out.empty() && fread(d, 1, 16, f) == 16 && !memcmp(d + 4, "data", 4)) {
                const u32 dsize = ta_be32(d);
                if (dsize > 16 && body + (long)dsize <= boxEnd) {
                    const size_t n = std::min<size_t>(dsize - 16, 512);
                    out.resize(n);
                    out.resize(fread(&out[0], 1, n, f));
                }
            }
        }
        if (!ta.title.empty() && !ta.artist.empty()) return;
        at = boxEnd;
    }
}

static TitleArtist ta_readMP4(FILE *f) {
    TitleArtist ta;
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    ta_mp4Walk(f, 0, end, 0, ta);
    return ta;
}

} // anonymous namespace

// ---- Public entry point -----------------------------------------------------

TitleArtist readTitleArtist(const char *path) {
    TitleArtist ta;
    FILE *f = fopen(path, "rb");
    if (!f) return ta;

    u8 magic[8] = {};
    fread(magic, 1, 8, f);

    if      (memcmp(magic + 4, "ftyp", 4) == 0) ta = ta_readMP4(f);
    else if (memcmp(magic, "ID3",  3) == 0) ta = ta_readID3(f);
    else if (memcmp(magic, "fLaC", 4) == 0) ta = ta_readFLAC(f);
    else if (memcmp(magic, "RIFF", 4) == 0) ta = ta_readWAV(f);

    fclose(f);

    if (ta.title.empty()) {
        const char *slash = std::strrchr(path, '/');
        const char *name  = slash ? slash+1 : path;
        ta.title = name;
        const size_t dot = ta.title.rfind('.');
        if (dot != std::string::npos && dot > 0)
            ta.title.resize(dot);
    }
    if (ta.artist.empty())
        ta.artist = "Unknown Artist";

    return ta;
}