#include "subsonic.hpp"

#if defined(__SWITCH__) || defined(GF_SUBSONIC)
#include <curl/curl.h>
#include <libxml/parser.h>
extern "C" {
#include <libavutil/md5.h>
#include <libavutil/random_seed.h>
}
#ifdef __SWITCH__
#include <switch.h>
#include "net.hpp"
#endif
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <unordered_set>

namespace {
std::string hex(const unsigned char* data, size_t n) {
    const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}
std::string md5(const std::string& value) {
    unsigned char hash[16];
    av_md5_sum(hash, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    return hex(hash, sizeof hash);
}
std::string encode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { out += '%'; out += hex(&c, 1); }
    }
    return out;
}
std::string attr(xmlNode* node, const char* name) {
    xmlChar* value = xmlGetProp(node, BAD_CAST name);
    std::string out = value ? reinterpret_cast<const char*>(value) : "";
    if (value) xmlFree(value);
    return out;
}
double number(xmlNode* node, const char* name) {
    const std::string text = attr(node, name);
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    return end != text.c_str() && !*end && std::isfinite(value) ? std::clamp(value, 0.0, double(INT_MAX)) : 0;
}
xmlNode* child(xmlNode* parent, const char* name) {
    for (xmlNode* n = parent ? parent->children : nullptr; n; n = n->next)
        if (n->type == XML_ELEMENT_NODE && xmlStrEqual(n->name, BAD_CAST name)) return n;
    return nullptr;
}
using Doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
Doc parse(const std::string& body, std::string& error) {
    Doc doc(xmlReadMemory(body.data(), int(body.size()), "response.xml", nullptr,
                         XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    xmlNode* root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
    if (!root || doc->intSubset || doc->extSubset || !xmlStrEqual(root->name, BAD_CAST "subsonic-response")) {
        error = "Invalid Subsonic response";
        return { nullptr, xmlFreeDoc };
    }
    if (attr(root, "status") != "ok") {
        // Only show the code: a server's free-form message could echo credentials.
        xmlNode* failure = child(root, "error");
        std::string code = failure ? attr(failure, "code") : "";
        if (code.empty() || code.size() > 4 || code.find_first_not_of("0123456789") != std::string::npos) code = "unknown";
        error = "Subsonic error " + code + (code == "40" ? ": check username and password" : "");
        return { nullptr, xmlFreeDoc };
    }
    return doc;
}
struct Sink {
    std::string data;
    FILE* file = nullptr;
    size_t received = 0, limit = 8 * 1024 * 1024;
};
size_t write(char* p, size_t size, size_t count, void* context) {
    auto& sink = *static_cast<Sink*>(context);
    const size_t bytes = size * count;
    if (bytes > sink.limit - sink.received) return 0;
    sink.received += bytes;
    if (sink.file) {
        if (sink.data.size() < 512) sink.data.append(p, std::min(bytes, 512 - sink.data.size()));
        return fwrite(p, 1, bytes, sink.file);
    }
    sink.data.append(p, bytes);
    return bytes;
}
int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(context)->load() ? 1 : 0;
}
bool request(const SubsonicConfig& config, const char* method, const std::string& args, Sink& sink,
             std::string& error, const std::atomic<bool>& cancel) {
    if (!subsonic::validUrl(config.url) || config.user.empty() || config.password.empty()) {
        error = "Set a server URL, username and password first"; return false;
    }
    if (cancel) return false;
#ifdef __SWITCH__
    if (!net::begin(error)) return false;
#endif
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Could not initialise networking"; return false; }
    std::string base = config.url;
    while (base.back() == '/') base.pop_back();
    uint32_t random[4];
#ifdef __SWITCH__
    // FFmpeg's entropy fallback reads a performance counter that Horizon does not permit.
    randomGet(random, sizeof random);
#else
    for (auto& value : random) value = av_get_random_seed();
#endif
    const std::string salt = hex(reinterpret_cast<unsigned char*>(random), sizeof random);
    const std::string url = base + "/rest/" + method + ".view?u=" + encode(config.user) + "&t=" + md5(config.password + salt)
        + "&s=" + salt + "&v=1.16.1&c=Songate&f=xml" + args;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    // Redirects stay off: authentication query parameters must never reach another origin.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, sink.file ? 300L : 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200 || !sink.received) {
        error = result == CURLE_OK ? "Subsonic HTTP error " + std::to_string(status) : "Subsonic transfer failed: " + std::string(curl_easy_strerror(result));
        return false;
    }
    return true;
}
Doc api(const SubsonicConfig& config, const char* method, const std::string& args, std::string& error,
        const std::atomic<bool>& cancel) {
    Sink sink;
    if (!request(config, method, args, sink, error, cancel)) return { nullptr, xmlFreeDoc };
    return parse(sink.data, error);
}
}

bool subsonic::init() {
    xmlInitParser();
#ifdef __SWITCH__
    return true;   // net::begin starts the shared sockets/curl only when a request is made.
#else
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
#endif
}
void subsonic::shutdown() {
#ifndef __SWITCH__
    curl_global_cleanup();
#endif
    xmlCleanupParser();
}
bool subsonic::validUrl(const std::string& url) {
    const size_t start = url.rfind("https://", 0) == 0 ? 8 : url.rfind("http://", 0) == 0 ? 7 : 0;
    if (!start || url.size() == start || url.find_first_of("?#\\\r\n\t ") != std::string::npos) return false;
    const size_t end = url.find('/', start);
    const std::string authority = url.substr(start, end - start);
    return !authority.empty() && authority.find('@') == std::string::npos;
}
bool subsonic::scan(const SubsonicConfig& config, std::vector<Track>& tracks, std::string& error, const std::atomic<bool>& cancel) {
    std::vector<Track> result;
    std::unordered_set<std::string> albums, songs;
    const std::string prefix = "subsonic:" + md5(config.url + "\n" + config.user) + ":";
    for (int offset = 0; ; offset += 500) {
        auto doc = api(config, "getAlbumList2", "&type=alphabeticalByName&size=500&offset=" + std::to_string(offset), error, cancel);
        if (!doc) return false;
        xmlNode* list = child(xmlDocGetRootElement(doc.get()), "albumList2");
        if (!list) { error = "Server returned no album list"; return false; }
        int count = 0;
        for (xmlNode* album = list->children; album; album = album->next) {
            if (album->type != XML_ELEMENT_NODE || !xmlStrEqual(album->name, BAD_CAST "album")) continue;
            const std::string id = attr(album, "id");
            if (id.empty() || !albums.insert(id).second) { error = "Invalid or repeated album ID"; return false; }
            ++count;
            auto detail = api(config, "getAlbum", "&id=" + encode(id), error, cancel);
            if (!detail) return false;
            xmlNode* parent = child(xmlDocGetRootElement(detail.get()), "album");
            if (!parent) { error = "Server returned no album"; return false; }
            for (xmlNode* song = parent->children; song; song = song->next) {
                if (song->type != XML_ELEMENT_NODE || !xmlStrEqual(song->name, BAD_CAST "song") || attr(song, "isVideo") == "true") continue;
                Track t;
                t.subsonicId = attr(song, "id");
                if (t.subsonicId.empty()) { error = "Server returned an empty song ID"; return false; }
                if (!songs.insert(t.subsonicId).second) continue;
                t.subsonicCoverId = attr(song, "coverArt");
                if (t.subsonicCoverId.empty()) t.subsonicCoverId = attr(parent, "coverArt");
                t.path = prefix + encode(t.subsonicId);
                t.title = attr(song, "title"); t.artist = attr(song, "artist"); t.album = attr(song, "album");
                t.albumArtist = attr(parent, "artist"); t.genre = attr(song, "genre"); t.date = attr(song, "year");
                t.duration = number(song, "duration");
                t.trackNo = int(number(song, "track"));
                t.tagged = true;
                result.push_back(std::move(t));
                if (result.size() > 100000) { error = "Server library exceeds 100,000 songs"; return false; }
            }
        }
        if (count < 500) break;
    }
    tracks = std::move(result);
    return true;
}
bool subsonic::buffer(const SubsonicConfig& config, const Track& track, const std::string& file,
                      std::string& error, const std::atomic<bool>& cancel) {
    Sink sink;
    sink.file = fopen(file.c_str(), "wb");
    sink.limit = 128 * 1024 * 1024;
    if (!sink.file) { error = "Could not create the song buffer on the SD card"; return false; }
    // shortcut: buffer one complete MP3 before playing; use progressive IO when the decoders support network input.
    bool ok = request(config, "stream", "&id=" + encode(track.subsonicId) + "&format=mp3&maxBitRate=192", sink, error, cancel);
    if (fclose(sink.file) != 0) { error = "Could not finish writing the song buffer"; ok = false; }
    const size_t first = sink.data.find_first_not_of(" \t\r\n");
    if (ok && first != std::string::npos && (sink.data[first] == '<' || sink.data[first] == '{')) {
        error = "Server returned an error instead of audio"; ok = false;
    }
    if (!ok || cancel) { remove(file.c_str()); return false; }
    if (!track.subsonicCoverId.empty()) {
        Sink cover;
        const std::string image = file + ".jpg";
        cover.file = fopen(image.c_str(), "wb");
        if (cover.file) {
            std::string ignored;
            bool saved = request(config, "getCoverArt", "&id=" + encode(track.subsonicCoverId) + "&size=512", cover, ignored, cancel);
            if (fclose(cover.file) != 0) saved = false;
            if (!saved) remove(image.c_str());
        }
    }
    if (cancel) { remove(file.c_str()); remove((file + ".jpg").c_str()); return false; }
    return true;
}
#else
bool subsonic::init() { return false; }
void subsonic::shutdown() {}
bool subsonic::validUrl(const std::string&) { return false; }
bool subsonic::scan(const SubsonicConfig&, std::vector<Track>&, std::string& error, const std::atomic<bool>&) {
    error = "Subsonic is unavailable in this preview build"; return false;
}
bool subsonic::buffer(const SubsonicConfig&, const Track&, const std::string&, std::string& error, const std::atomic<bool>&) {
    error = "Subsonic is unavailable in this preview build"; return false;
}
#endif
