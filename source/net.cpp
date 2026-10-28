/*
 * Gatefold NX · net.cpp
 * See net.hpp. Console: libcurl (portlibs, built on libnx's ssl service: the console's own TLS and trusted
 * certificates, so no CA bundle travels with Gatefold). Preview: GF_NET=<folder>.
 */
#include "net.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef GF_VERSION
#define GF_VERSION "1.1.0"
#endif

namespace net {
namespace {

/** Written next to the file and renamed over it once whole. */
bool finish(const std::string& tmp, const std::string& file) {
    remove(file.c_str());
    return rename(tmp.c_str(), file.c_str()) == 0;
}

}  // namespace
}  // namespace net

#ifdef __SWITCH__
#include <curl/curl.h>
#include <switch.h>

namespace net {
namespace {

std::mutex gate;
bool started = false;

struct Sink {
    std::string* str = nullptr;
    FILE* file = nullptr;
    Progress* p = nullptr;
};

size_t onData(char* data, size_t size, size_t n, void* user) {
    Sink* s = static_cast<Sink*>(user);
    const size_t len = size * n;
    if (s->str) s->str->append(data, len);
    if (s->file && fwrite(data, 1, len, s->file) != len) return 0;   // the card is full: curl stops with a write error
    return len;
}

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    Progress* p = static_cast<Progress*>(user);
    if (!p) return 0;
    if (total > 0) p->total = int64_t(total);
    p->done = int64_t(now);
    return p->cancel ? 1 : 0;
}

/** What went wrong, in words the Settings row can show. */
std::string explain(CURLcode rc, long status) {
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT: return "No internet connection";
    case CURLE_OPERATION_TIMEDOUT: return "The connection timed out";
    case CURLE_ABORTED_BY_CALLBACK: return "Stopped";
    case CURLE_WRITE_ERROR: return "Could not write to the SD card (is it full?)";
    case CURLE_PEER_FAILED_VERIFICATION: return "The server's certificate could not be checked";
    case CURLE_HTTP_RETURNED_ERROR:
        if (status == 403 || status == 429) return "GitHub says to wait a while (too many checks this hour)";
        if (status == 404) return "Not found on GitHub";
        return "GitHub answered " + std::to_string(status);
    default: return std::string("Network error: ") + curl_easy_strerror(rc);
    }
}

bool run(const std::string& url, Sink& sink, std::string& err) {
    std::string e;
    if (!begin(e)) { err = e; return false; }
    CURL* c = curl_easy_init();
    if (!c) { err = "The network library did not start"; return false; }
    curl_slist* headers = nullptr;
    if (url.find("api.github.com") != std::string::npos) headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "GatefoldNX/" GF_VERSION);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // release assets redirect to GitHub's file host
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);   // under 64 bytes a second for 30 s: give up
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, sink.p);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) { err = explain(rc, status); return false; }
    return true;
}

}  // namespace

bool begin(std::string& err) {
    std::lock_guard<std::mutex> g(gate);
    if (started) return true;
    if (R_FAILED(socketInitializeDefault())) { err = "The console's network did not start"; return false; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socketExit();
        err = "The network library did not start";
        return false;
    }
    started = true;
    return true;
}

void end() {
    std::lock_guard<std::mutex> g(gate);
    if (!started) return;
    curl_global_cleanup();
    socketExit();
    started = false;
}

bool get(const std::string& url, std::string& out, std::string& err, Progress* p) {
    out.clear();
    Sink s;
    s.str = &out;
    s.p = p;
    return run(url, s, err);
}

bool download(const std::string& url, const std::string& file, std::string& err, Progress* p) {
    const std::string tmp = file + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "Could not write to the SD card"; return false; }
    setvbuf(f, nullptr, _IOFBF, 256 * 1024);
    Sink s;
    s.file = f;
    s.p = p;
    const bool ok = run(url, s, err);
    if (fclose(f) != 0 && ok) { err = "Could not write to the SD card (is it full?)"; remove(tmp.c_str()); return false; }
    if (!ok) { remove(tmp.c_str()); return false; }
    if (!finish(tmp, file)) { err = "Could not write to the SD card"; return false; }
    return true;
}

}  // namespace net

#else  // the PC preview: GF_NET=<folder> stands in for GitHub

#include <SDL2/SDL.h>

namespace net {
namespace {

/** api.github.com/repos/…/releases → <folder>/releases.json; …/download/<tag>/<name> → <folder>/<tag>/<name>. */
bool local(const std::string& url, std::string& path, std::string& err) {
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    if (url.find("api.github.com") != std::string::npos) { path = std::string(root) + "/releases.json"; return true; }
    const size_t b = url.find_last_of('/');
    const size_t a = b == std::string::npos || b == 0 ? std::string::npos : url.find_last_of('/', b - 1);
    if (a == std::string::npos) { err = "Not found on GitHub"; return false; }
    path = std::string(root) + "/" + url.substr(a + 1);
    return true;
}

}  // namespace

bool begin(std::string& err) {
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    return true;
}

void end() {}

bool get(const std::string& url, std::string& out, std::string& err, Progress* p) {
    std::string path;
    if (!local(url, path, err)) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Not found on GitHub"; return false; }
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (p) p->done = p->total = int64_t(out.size());
    SDL_Delay(150);   // a little of the wait a real request has
    return true;
}

bool download(const std::string& url, const std::string& file, std::string& err, Progress* p) {
    std::string path;
    if (!local(url, path, err)) return false;
    FILE* in = fopen(path.c_str(), "rb");
    if (!in) { err = "Not found on GitHub"; return false; }
    fseek(in, 0, SEEK_END);
    if (p) { p->total = int64_t(ftell(in)); p->done = 0; }
    fseek(in, 0, SEEK_SET);
    const std::string tmp = file + ".part";
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) { fclose(in); err = "Could not write to the SD card"; return false; }
    char buf[32768];
    size_t n;
    bool ok = true;
    // Slowed to about 2 MB a second, so the progress card and Stop can be seen.
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (p && p->cancel) { err = "Stopped"; ok = false; break; }
        fwrite(buf, 1, n, out);
        if (p) p->done += int64_t(n);
        SDL_Delay(15);
    }
    fclose(in);
    fclose(out);
    if (!ok) { remove(tmp.c_str()); return false; }
    if (!finish(tmp, file)) { err = "Could not write to the SD card"; return false; }
    return true;
}

}  // namespace net
#endif
