/*
 * Gatefold NX · ffdecoder.cpp
 */
#include "ffdecoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <thread>

#include "cores.hpp"
#include "metadata.hpp"

namespace {

// the feeder: one thread for every decoder reading ahead (prefetch)
std::mutex regMx;   // the list, and held while one of its decoders is filled: closing waits for that chunk
std::condition_variable regCv;
std::vector<FfDecoder*> reg;
std::thread feeder;
bool feeding = false;
std::atomic<unsigned> starved{ 0 };

void feed() {
    cores::moveHere(cores::Feed);
    std::unique_lock<std::mutex> l(regMx);
    while (feeding) {
        bool more = false;
        for (FfDecoder* d : reg) more |= d->fillAhead(4096);
        if (!more) regCv.wait_for(l, std::chrono::milliseconds(15));
        else {   // let a decoder that closes, or a new one, in between chunks
            l.unlock();
            std::this_thread::yield();
            l.lock();
        }
    }
}

std::string ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : e) c = char(tolower((unsigned char)c));
    return e;
}

std::string tag(AVDictionary* d, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

/** FFmpeg reads "sdmc:/…" as a URL with an unknown "sdmc" protocol; "file:" makes it a plain file path. */
std::string fileUrl(const std::string& path) { return "file:" + path; }

}  // namespace

void ffFeederStart() {
    std::lock_guard<std::mutex> g(regMx);
    if (feeding) return;
    feeding = true;
    feeder = std::thread(feed);
}

void ffFeederStop() {
    {
        std::lock_guard<std::mutex> g(regMx);
        if (!feeding) return;
        feeding = false;
    }
    regCv.notify_all();
    if (feeder.joinable()) feeder.join();
}

unsigned ffStarved() { return starved.load(); }

bool ffHandles(const std::string& path) {
    const std::string e = ext(path);
    return e == "m4a" || e == "m4b" || e == "mp4" || e == "aac" || e == "alac";
}

bool ffReadInfo(const std::string& path, TrackInfo& out, bool wantCover, bool wantLyrics) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, fileUrl(path).c_str(), nullptr, nullptr) != 0) return false;
    // MP4 keeps everything in the moov box, read by the open: no need for the slower stream probe.
    if (fmt->duration > 0) out.duration = double(fmt->duration) / AV_TIME_BASE;
    out.title = tag(fmt->metadata, "title");
    out.artist = tag(fmt->metadata, "artist");
    out.album = tag(fmt->metadata, "album");
    out.albumArtist = tag(fmt->metadata, "album_artist");
    out.date = tag(fmt->metadata, "date");
    out.trackNo = atoi(tag(fmt->metadata, "track").c_str());
    out.genre = cleanGenre(tag(fmt->metadata, "genre"));
    if (wantLyrics) {
        out.lyrics = tag(fmt->metadata, "lyrics");
        if (out.lyrics.empty()) out.lyrics = tag(fmt->metadata, "UNSYNCEDLYRICS");
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream* st = fmt->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            if (wantCover && out.cover.empty() && st->attached_pic.size > 0)
                out.cover.assign(st->attached_pic.data, st->attached_pic.data + st->attached_pic.size);
            continue;
        }
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || !out.codec.empty()) continue;
        const AVCodecParameters* cp = st->codecpar;
        out.sampleRate = cp->sample_rate;
        if (out.duration <= 0 && st->duration > 0) out.duration = st->duration * av_q2d(st->time_base);
        if (cp->codec_id == AV_CODEC_ID_AAC) {
            // Without the stream probe the profile is unknown: the first 5 bits of the AudioSpecificConfig say it.
            int profile = cp->profile;
            if (profile < 0 && cp->extradata_size >= 1) profile = (cp->extradata[0] >> 3) - 1;
            const char* prof = avcodec_profile_name(cp->codec_id, profile);
            out.codec = prof ? std::string("AAC ") + prof : "AAC";
        } else if (cp->codec_id == AV_CODEC_ID_ALAC) {
            out.codec = "ALAC";
            out.bitsPerSample = cp->bits_per_raw_sample ? cp->bits_per_raw_sample : 16;
        } else {
            out.codec = avcodec_get_name(cp->codec_id);
            for (char& c : out.codec) c = char(toupper((unsigned char)c));
        }
        const int64_t br = cp->bit_rate > 0 ? cp->bit_rate : fmt->bit_rate;
        if (br > 0 && cp->codec_id != AV_CODEC_ID_ALAC) out.bitrateKbps = int(br / 1000);
    }
    avformat_close_input(&fmt);
    return true;
}

bool FfDecoder::open(const std::string& path, int outRate, int outChannels) {
    close();
    outRate_ = outRate;
    outCh_ = outChannels;
    if (avformat_open_input(&fmt_, fileUrl(path).c_str(), nullptr, nullptr) != 0) { fmt_ = nullptr; return false; }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) { close(); return false; }
    const AVCodec* codec = nullptr;
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_ < 0 || !codec) { close(); return false; }
    dec_ = avcodec_alloc_context3(codec);
    if (!dec_ || avcodec_parameters_to_context(dec_, fmt_->streams[stream_]->codecpar) < 0 || avcodec_open2(dec_, codec, nullptr) < 0) {
        close();
        return false;
    }
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outCh_);
    if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_S16, outRate_, &dec_->ch_layout, dec_->sample_fmt, dec_->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        close();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    fifo_.clear();
    pos_ = 0;
    eof_ = false;
    return pkt_ && frame_;
}

void FfDecoder::close() {
    if (pf_) {   // off the feeder's list first (waiting for a chunk it may be decoding)
        std::lock_guard<std::mutex> g(regMx);
        const auto it = std::find(reg.begin(), reg.end(), this);
        if (it != reg.end()) reg.erase(it);
        pf_ = false;
    }
    std::lock_guard<std::mutex> g(dm_);
    ring_.clear();
    ringFrames_ = 0;
    rw_ = rr_ = 0;
    rEof_ = false;
    if (swr_) swr_free(&swr_);
    if (dec_) avcodec_free_context(&dec_);
    if (fmt_) avformat_close_input(&fmt_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    fifo_.clear();
    pos_ = 0;
    eof_ = false;
    stream_ = -1;
}

/** Decodes until at least one frame of PCM was added, or the stream ends. */
bool FfDecoder::decodeMore() {
    if (pos_ > 0 && pos_ >= fifo_.size()) { fifo_.clear(); pos_ = 0; }
    else if (pos_ > 65536) { fifo_.erase(fifo_.begin(), fifo_.begin() + pos_); pos_ = 0; }
    const size_t before = fifo_.size();
    while (fifo_.size() == before && !eof_) {
        int r = avcodec_receive_frame(dec_, frame_);
        if (r == 0) {
            const int cap = swr_get_out_samples(swr_, frame_->nb_samples);
            const size_t at = fifo_.size();
            fifo_.resize(at + size_t(cap) * outCh_);
            uint8_t* outp = reinterpret_cast<uint8_t*>(fifo_.data() + at);
            const int got = swr_convert(swr_, &outp, cap, const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
            fifo_.resize(at + size_t(std::max(got, 0)) * outCh_);
            av_frame_unref(frame_);
            continue;
        }
        if (r == AVERROR_EOF) { eof_ = true; break; }
        // Needs input: feed the next packet of our stream (or flush at the end of the file).
        r = av_read_frame(fmt_, pkt_);
        if (r < 0) {
            // End of the file: flush the decoder; the receive above drains it until AVERROR_EOF.
            avcodec_send_packet(dec_, nullptr);
            continue;
        }
        if (pkt_->stream_index == stream_) avcodec_send_packet(dec_, pkt_);
        av_packet_unref(pkt_);
    }
    return fifo_.size() > before;
}

bool FfDecoder::skip(double seconds) {
    if (!fmt_ || seconds <= 0) return fmt_ != nullptr;
    if (seconds > 30) return seek(seconds);
    long long left = (long long)std::llround(seconds * outRate_);
    while (left > 0) {
        if (pos_ >= fifo_.size() && !decodeMore()) return false;
        const long long avail = (long long)((fifo_.size() - pos_) / size_t(outCh_));
        const long long n = std::min(avail, left);
        pos_ += size_t(n) * size_t(outCh_);
        left -= n;
    }
    return true;
}

void FfDecoder::prefetch(int frames, int first) {
    if (!fmt_ || pf_) return;
    size_t n = 4096;
    while (n < size_t(frames)) n <<= 1;
    ringFrames_ = n;
    ring_.assign(n * size_t(outCh_), 0);
    rw_ = rr_ = 0;
    rEof_ = false;
    {
        std::lock_guard<std::mutex> g(dm_);
        fillLocked(first);
    }
    std::lock_guard<std::mutex> g(regMx);
    pf_ = true;
    reg.push_back(this);
    regCv.notify_all();
}

/** Decodes into the ring's free space, up to `frames` frames (dm_ held). */
void FfDecoder::fillLocked(int frames) {
    if (!fmt_ || rEof_ || !ringFrames_) return;
    size_t w = rw_.load(std::memory_order_relaxed);
    const size_t space = ringFrames_ - (w - rr_.load(std::memory_order_acquire));
    if (space < 1024) return;   // nearly full: in larger pieces, later
    size_t want = std::min(space, size_t(std::max(0, frames)));
    while (want > 0) {
        const size_t at = w & (ringFrames_ - 1), n = std::min(want, ringFrames_ - at);
        const int got = readDirect(ring_.data() + at * size_t(outCh_), int(n));
        w += size_t(std::max(0, got));
        rw_.store(w, std::memory_order_release);
        want -= size_t(std::max(0, got));
        if (size_t(std::max(0, got)) < n) { rEof_ = true; break; }
    }
}

bool FfDecoder::fillAhead(int frames) {
    std::lock_guard<std::mutex> g(dm_);
    const size_t before = rw_.load();
    fillLocked(frames);
    return rw_.load() != before;
}

int FfDecoder::takeRing(int16_t* out, int frames) {
    size_t r = rr_.load(std::memory_order_relaxed);
    const size_t n = std::min(size_t(std::max(0, frames)), rw_.load(std::memory_order_acquire) - r);
    for (size_t done = 0; done < n;) {
        const size_t at = r & (ringFrames_ - 1), k = std::min(n - done, ringFrames_ - at);
        memcpy(out + done * size_t(outCh_), ring_.data() + at * size_t(outCh_), k * size_t(outCh_) * sizeof(int16_t));
        done += k;
        r += k;
    }
    rr_.store(r, std::memory_order_release);
    return int(n);
}

int FfDecoder::read(int16_t* out, int frames) {
    if (!fmt_) return 0;
    if (!pf_) return readDirect(out, frames);
    int done = takeRing(out, frames);
    if (done < frames && !(rEof_.load() && rw_.load() == rr_.load())) {
        // the ring ran dry (the card was slow, or the track just started): decode here, after what the feeder has added
        starved++;
        std::lock_guard<std::mutex> g(dm_);
        done += takeRing(out + size_t(done) * size_t(outCh_), frames - done);
        if (done < frames && !rEof_) {
            const int want = frames - done, got = readDirect(out + size_t(done) * size_t(outCh_), want);
            done += std::max(0, got);
            if (got < want) rEof_ = true;
        }
    }
    return done;
}

int FfDecoder::readDirect(int16_t* out, int frames) {
    if (!fmt_) return 0;
    int done = 0;
    while (done < frames) {
        if (pos_ >= fifo_.size() && !decodeMore()) break;
        const size_t avail = (fifo_.size() - pos_) / outCh_;
        const int n = int(std::min<size_t>(avail, size_t(frames - done)));
        memcpy(out + size_t(done) * outCh_, fifo_.data() + pos_, size_t(n) * outCh_ * sizeof(int16_t));
        pos_ += size_t(n) * outCh_;
        done += n;
    }
    return done;
}

bool FfDecoder::seek(double seconds) {
    if (!fmt_) return false;
    std::lock_guard<std::mutex> g(dm_);
    const AVStream* st = fmt_->streams[stream_];
    const int64_t ts = av_rescale_q(int64_t(seconds * AV_TIME_BASE), AVRational{ 1, AV_TIME_BASE }, st->time_base);
    if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(dec_);
    swr_init(swr_);   // drop resampler history from before the jump
    fifo_.clear();
    pos_ = 0;
    eof_ = false;
    rr_.store(rw_.load());   // what was read ahead is from before the jump (the player holds its lock: no read meanwhile)
    rEof_ = false;
    if (pf_) fillLocked(outRate_ / 4);
    return true;
}
