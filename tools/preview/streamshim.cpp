/* Gatefold NX preview · streamshim.cpp
 * The live test environment (tools/preview/live): with GF_STREAM=<host:port> the preview connects to live/server.py,
 * sends it the frames (JPEG) and the sound (the final mix, S16) and takes its input back: buttons, the stick, touches.
 * It runs in real time (60 frames a second). Preview only: nothing of this is in the Switch build.
 *
 * To the server, messages of [type u8][length u32 LE][time f64 LE, ms since start][payload]:
 *   1 a frame (JPEG)   2 sound (u32 rate, u8 channels, then S16 samples)
 *   3 the keyboard opens: header, guide and initial text, each ended by a 0 byte
 * From the server, lines of text:
 *   b <bits>          the buttons held (HidNpadButton bits, decimal)
 *   s <x> <y>         the left stick, −1..1, y up
 *   d|m|u <x> <y>     a finger down, moved, up, in 1280×720 pixels
 *   p <x> <y>         a mouse over the picture, no button held: the pointer follows it
 *   w <x> <y> <n>     its wheel there, n notches (+ away from you), fractions for a touchpad
 *   q <0..100>        the JPEG quality      f <n>  a frame sent every n frames      z <50..100>  the picture's size, %
 *   k <text> | K      the keyboard's answer: the text, or cancelled
 *   quit              Gatefold closes (as appletMainLoop false)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#ifdef GF_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Stream {
    SOCKET sock = INVALID_SOCKET;
    std::atomic<bool> live{ false };
    std::thread reader, sender;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> sound;   // messages ready to send
    SDL_Surface* pending = nullptr;           // the latest frame, not encoded yet (only one waits: older ones are dropped)
    double pendingAt = 0;
    bool stopping = false;
    std::atomic<uint64_t> buttons{ 0 };
    std::atomic<float> sx{ 0 }, sy{ 0 };
    std::atomic<bool> quit{ false };
    std::atomic<int> quality{ 70 }, every{ 2 }, scale{ 100 };
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next = start;
    uint64_t lastHash = 0;
    long lastSent = -1000;
    bool kbdWaiting = false, kbdDone = false, kbdOk = false;   // under m
    std::string kbdText;

    double now() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }

    static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i))); }
    static std::vector<uint8_t> message(uint8_t type, double at, const void* data, size_t n, const std::vector<uint8_t>& head = {}) {
        std::vector<uint8_t> v;
        v.reserve(13 + head.size() + n);
        v.push_back(type);
        put32(v, uint32_t(8 + head.size() + n));
        uint64_t bits;
        memcpy(&bits, &at, 8);
        for (int i = 0; i < 8; i++) v.push_back(uint8_t(bits >> (8 * i)));
        v.insert(v.end(), head.begin(), head.end());
        v.insert(v.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
        return v;
    }
    bool sendAll(const std::vector<uint8_t>& v) {
        size_t at = 0;
        while (at < v.size()) {
            const int n = send(sock, reinterpret_cast<const char*>(v.data() + at), int(v.size() - at), 0);
            if (n <= 0) return false;
            at += size_t(n);
        }
        return true;
    }

    void open(const char* where) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        std::string s = where, host = "127.0.0.1", port = s;
        if (const size_t c = s.rfind(':'); c != std::string::npos) { host = s.substr(0, c); port = s.substr(c + 1); }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return;
        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET || connect(sock, res->ai_addr, int(res->ai_addrlen)) != 0) {
            fprintf(stderr, "stream: no server at %s\n", where);
            freeaddrinfo(res);
            if (sock != INVALID_SOCKET) closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }
        freeaddrinfo(res);
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
        timeBeginPeriod(1);   // Sleep to the millisecond, for the frame pacing
        live = true;
        reader = std::thread([this] { readLoop(); });
        sender = std::thread([this] { sendLoop(); });
    }

    void close() {
        if (!live.exchange(false)) return;
        {
            std::lock_guard<std::mutex> l(m);
            stopping = true;
        }
        cv.notify_all();
        shutdown(sock, SD_BOTH);   // wakes the reader's recv
        if (sender.joinable()) sender.join();
        closesocket(sock);
        if (reader.joinable()) reader.join();
        if (pending) SDL_FreeSurface(pending);
        pending = nullptr;
        timeEndPeriod(1);
        WSACleanup();
    }

    void readLoop() {
        std::string line;
        char buf[4096];
        for (;;) {
            const int n = recv(sock, buf, sizeof buf, 0);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (buf[i] != '\n') { line += buf[i]; continue; }
                handle(line);
                line.clear();
            }
        }
        {
            std::lock_guard<std::mutex> l(m);
            kbdDone = true;   // a keyboard still open is cancelled
        }
        cv.notify_all();
        quit = true;   // the server went away: Gatefold closes, as on the console's Home menu → Close
    }

    void finger(Uint32 type, float x, float y) {
        SDL_Event e{};
        e.type = type;
        e.tfinger.x = x / 1280.f;
        e.tfinger.y = y / 720.f;
        e.tfinger.fingerId = 1;
        SDL_PushEvent(&e);   // thread-safe
    }

    /** The page's mouse as SDL's (not a touch's: which 0), in the window's pixels (1280×720, as the app's). */
    void mouse(float x, float y) {
        SDL_Event e{};
        e.type = SDL_MOUSEMOTION;
        e.motion.which = 0;
        e.motion.x = int(x);
        e.motion.y = int(y);
        SDL_PushEvent(&e);
    }

    void handle(const std::string& l) {
        if (l.empty()) return;
        float x = 0, y = 0, n = 0;
        if (l[0] == 'b') buttons = strtoull(l.c_str() + 1, nullptr, 10);
        else if (l[0] == 's' && sscanf(l.c_str() + 1, "%f %f", &x, &y) == 2) { sx = x; sy = y; }
        else if ((l[0] == 'd' || l[0] == 'm' || l[0] == 'u') && sscanf(l.c_str() + 1, "%f %f", &x, &y) == 2)
            finger(l[0] == 'd' ? SDL_FINGERDOWN : l[0] == 'm' ? SDL_FINGERMOTION : SDL_FINGERUP, x, y);
        else if (l[0] == 'p' && sscanf(l.c_str() + 1, "%f %f", &x, &y) == 2) mouse(x, y);
        else if (l[0] == 'w' && sscanf(l.c_str() + 1, "%f %f %f", &x, &y, &n) == 3) {
            mouse(x, y);   // (where it turns: the app scrolls the list under it)
            SDL_Event e{};
            e.type = SDL_MOUSEWHEEL;
            e.wheel.which = 0;
            e.wheel.y = int(std::lround(n));
            e.wheel.preciseY = n;
            SDL_PushEvent(&e);
        }
        else if (l == "quit") quit = true;
        else if (l[0] == 'k' || l[0] == 'K') {
            std::lock_guard<std::mutex> lk(m);
            if (kbdWaiting) {
                kbdOk = l[0] == 'k';
                kbdText = l.size() > 2 ? l.substr(2) : "";
                kbdDone = true;
            }
            cv.notify_all();
        }
        else if (l[0] == 'q') quality = std::clamp(atoi(l.c_str() + 1), 20, 95);
        else if (l[0] == 'f') every = std::clamp(atoi(l.c_str() + 1), 1, 6);
        else if (l[0] == 'z') scale = std::clamp(atoi(l.c_str() + 1), 40, 100);
    }

    void sendLoop() {
        std::vector<uint8_t> jpeg;
        for (;;) {
            std::deque<std::vector<uint8_t>> out;
            SDL_Surface* frame = nullptr;
            double at = 0;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return stopping || !sound.empty() || pending; });
                if (stopping) return;
                out.swap(sound);
                std::swap(frame, pending);
                at = pendingAt;
            }
            for (auto& v : out) if (!sendAll(v)) return;
            if (!frame) continue;
            if (const int z = scale; z < 100) frame = shrink(frame, z);
            // Encode on this thread: the main thread only reads the pixels back
            const bool ok = encode(frame, jpeg);
            SDL_FreeSurface(frame);
            if (ok && !sendAll(message(1, at, jpeg.data(), jpeg.size()))) return;
        }
#ifdef GF_FFMPEG
        if (enc) avcodec_free_context(&enc);
        if (yuv) av_frame_free(&yuv);
#endif
    }

#ifdef GF_FFMPEG
    // FFmpeg's MJPEG (4:2:0, Huffman tables of its own): a third of SDL_image's built-in JPEG at the same look
    AVCodecContext* enc = nullptr;
    AVFrame* yuv = nullptr;
    bool encode(SDL_Surface* f, std::vector<uint8_t>& out) {
        if (enc && (enc->width != f->w || enc->height != f->h)) avcodec_free_context(&enc);
        if (!enc) {
            const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
            if (!c || !(enc = avcodec_alloc_context3(c))) return encodeSdl(f, out);
            enc->width = f->w & ~1;
            enc->height = f->h & ~1;
            enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
            enc->time_base = { 1, 60 };
            enc->flags |= AV_CODEC_FLAG_QSCALE;
            enc->thread_count = 1;
            if (avcodec_open2(enc, c, nullptr) < 0) { avcodec_free_context(&enc); return encodeSdl(f, out); }
            if (yuv) av_frame_free(&yuv);
            yuv = av_frame_alloc();
            yuv->format = enc->pix_fmt;
            yuv->width = enc->width;
            yuv->height = enc->height;
            av_frame_get_buffer(yuv, 32);
        }
        av_frame_make_writable(yuv);
        // RGB to full-range BT.601 Y'CbCr, the chroma averaged over 2x2
        const auto* src = static_cast<const uint8_t*>(f->pixels);
        const int w = enc->width, h = enc->height;
        for (int y = 0; y < h; y++) {
            const uint8_t* r = src + size_t(y) * f->pitch;
            uint8_t* Y = yuv->data[0] + size_t(y) * yuv->linesize[0];
            for (int x = 0; x < w; x++, r += 3) Y[x] = uint8_t((77 * r[0] + 150 * r[1] + 29 * r[2] + 128) >> 8);
        }
        for (int y = 0; y < h / 2; y++) {
            const uint8_t* a = src + size_t(2 * y) * f->pitch;
            const uint8_t* b = a + f->pitch;
            uint8_t* U = yuv->data[1] + size_t(y) * yuv->linesize[1];
            uint8_t* V = yuv->data[2] + size_t(y) * yuv->linesize[2];
            for (int x = 0; x < w / 2; x++) {
                const int i = x * 6;
                const int R = a[i] + a[i + 3] + b[i] + b[i + 3], G = a[i + 1] + a[i + 4] + b[i + 1] + b[i + 4],
                          B = a[i + 2] + a[i + 5] + b[i + 2] + b[i + 5];
                U[x] = uint8_t(std::clamp((-43 * R - 85 * G + 128 * B + 512) / 1024 + 128, 0, 255));
                V[x] = uint8_t(std::clamp((128 * R - 107 * G - 21 * B + 512) / 1024 + 128, 0, 255));
            }
        }
        // quality 95..20 as MJPEG's scale 2..21 (lower is sharper)
        const int qs = std::clamp(2 + (95 - int(quality)) / 4, 2, 31);
        yuv->quality = enc->global_quality = FF_QP2LAMBDA * qs;
        yuv->pts++;
        if (avcodec_send_frame(enc, yuv) < 0) return false;
        AVPacket* pkt = av_packet_alloc();
        const bool ok = avcodec_receive_packet(enc, pkt) == 0;
        if (ok) out.assign(pkt->data, pkt->data + pkt->size);
        av_packet_free(&pkt);
        return ok;
    }
#else
    bool encode(SDL_Surface* f, std::vector<uint8_t>& out) { return encodeSdl(f, out); }
#endif
    bool encodeSdl(SDL_Surface* f, std::vector<uint8_t>& out) {
        out.assign(size_t(f->w) * f->h * 4 + 4096, 0);
        SDL_RWops* rw = SDL_RWFromMem(out.data(), int(out.size()));
        const bool ok = rw && IMG_SaveJPG_RW(f, rw, 0, quality) == 0;
        const Sint64 n = rw ? SDL_RWtell(rw) : 0;
        if (rw) SDL_RWclose(rw);
        out.resize(ok && n > 0 ? size_t(n) : 0);
        return !out.empty();
    }

    // The console's keyboard is an applet that holds the app until it closes; this holds the main thread the same way
    bool keyboard(const char* header, const char* guide, const char* initial, char* out, size_t size) {
        if (!live) return false;
        std::vector<uint8_t> text;
        for (const char* t : { header, guide, initial }) {
            text.insert(text.end(), t, t + strlen(t));
            text.push_back(0);
        }
        std::unique_lock<std::mutex> l(m);
        kbdWaiting = true;
        kbdDone = kbdOk = false;
        sound.push_back(message(3, now(), text.data(), text.size()));
        cv.notify_all();
        cv.wait(l, [&] { return kbdDone || stopping; });
        kbdWaiting = false;
        if (!kbdOk) return false;
        snprintf(out, size, "%s", kbdText.c_str());
        next = std::chrono::steady_clock::now();   // the frames go on from now
        return true;
    }

    // A smaller picture for a slow network: each output pixel the average of the source pixels it covers
    static SDL_Surface* shrink(SDL_Surface* src, int percent) {
        const int w = std::max(1, src->w * percent / 100), h = std::max(1, src->h * percent / 100);
        SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
        if (!dst) return src;
        const auto* sp = static_cast<const uint8_t*>(src->pixels);
        auto* dp = static_cast<uint8_t*>(dst->pixels);
        for (int y = 0; y < h; y++) {
            const int y0 = y * src->h / h, y1 = std::max(y0 + 1, (y + 1) * src->h / h);
            for (int x = 0; x < w; x++) {
                const int x0 = x * src->w / w, x1 = std::max(x0 + 1, (x + 1) * src->w / w);
                unsigned sum[3] = { 0, 0, 0 }, n = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++, n++)
                        for (int c = 0; c < 3; c++) sum[c] += sp[size_t(yy) * src->pitch + xx * 3 + c];
                for (int c = 0; c < 3; c++) dp[size_t(y) * dst->pitch + x * 3 + c] = uint8_t(sum[c] / n);
            }
        }
        SDL_FreeSurface(src);
        return dst;
    }

    // Called before each present: every n-th frame is read back, unless it is the same picture as the last one sent
    // (a still screen costs nothing); one a second is sent anyway, so a new viewer gets a picture.
    void frame(SDL_Renderer* r, long f) {
        if (!live || f % every != 0) return;
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(r, &w, &h);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
        if (!s) return;
        if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGB24, s->pixels, s->pitch) != 0) { SDL_FreeSurface(s); return; }
        uint64_t hash = 1469598103934665603ull;
        const auto* p = static_cast<const uint8_t*>(s->pixels);
        for (int y = 0; y < h; y += 3)
            for (int x = 0; x < w * 3; x += 7) hash = (hash ^ p[size_t(y) * s->pitch + x]) * 1099511628211ull;
        if (hash == lastHash && f - lastSent < 60) { SDL_FreeSurface(s); return; }
        lastHash = hash;
        lastSent = f;
        {
            std::lock_guard<std::mutex> l(m);
            if (pending) SDL_FreeSurface(pending);   // the sender is behind: that frame is dropped
            pending = s;
            pendingAt = now();
        }
        cv.notify_one();
    }

    void audio(const Uint8* data, int len) {
        if (!live) return;
        static int rate = 0, channels = 0;
        if (!rate) {
            Uint16 fmt = 0;
            if (!Mix_QuerySpec(&rate, &fmt, &channels)) { rate = 48000; channels = 2; }
        }
        std::vector<uint8_t> head;
        put32(head, uint32_t(rate));
        head.push_back(uint8_t(channels));
        auto v = message(2, now(), data, size_t(len), head);
        {
            std::lock_guard<std::mutex> l(m);
            if (sound.size() > 64) sound.pop_front();   // the server stopped reading: keep the newest second or so
            sound.push_back(std::move(v));
        }
        cv.notify_one();
    }

    // Real time: each frame waits for its 1/60 s (a hidden window's vsync does not pace it)
    void pace() {
        if (!live) return;
        using namespace std::chrono;
        const auto step = duration_cast<steady_clock::duration>(duration<double>(1.0 / 60));
        next += step;
        auto t = steady_clock::now();
        if (t > next + step * 4) next = t;   // far behind (a load, a breakpoint): start again from now
        while ((t = steady_clock::now()) < next) {
            const auto left = duration_cast<milliseconds>(next - t).count();
            if (left > 1) Sleep(DWORD(left - 1));
        }
    }
};

Stream& st() {
    static Stream s;
    return s;
}

}  // namespace

// ---- what hostshim.cpp calls

bool streamStart() {
    static std::once_flag once;   // the main thread and the audio thread both ask
    std::call_once(once, [] {
        if (const char* w = getenv("GF_STREAM"); w && *w) {
            st().open(w);
            atexit([] { st().close(); });   // every thread ends before the process does
        }
    });
    return st().live;
}
void streamFrame(SDL_Renderer* r, long frame) { st().frame(r, frame); }
void streamPace() { st().pace(); }
void streamAudio(const Uint8* data, int len) { st().audio(data, len); }
uint64_t streamButtons() { return st().buttons; }
void streamStick(float& x, float& y) {
    if (!st().live) return;
    const float a = st().sx, b = st().sy;
    if (a != 0 || b != 0) { x = a; y = b; }
}
bool streamQuit() { return st().quit; }
bool streamKeyboard(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    return st().keyboard(header, guide, initial, out, size);
}
