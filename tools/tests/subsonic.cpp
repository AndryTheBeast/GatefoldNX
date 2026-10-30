#include "../../source/subsonic.hpp"
#include <cassert>
#include <climits>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(subsonic::init());
    assert(subsonic::validUrl("https://music.example.com/base"));
    for (const char* bad : { "", "https://", "file:///tmp/music", "http:///rest", "https://u:p@music", "https://music/?x=1", "https://music/#x", "https://music/\n" })
        assert(!subsonic::validUrl(bad));
    SubsonicConfig config{ true, std::string(argv[1]) + "/base", "user & é", "secret+&é" };
    std::atomic<bool> cancel{ false };
    std::string error;
    std::vector<Track> tracks;
    assert(subsonic::scan(config, tracks, error, cancel));
    assert(tracks.size() == 501); // Includes a second page; duplicate songs and videos are excluded.
    assert(tracks[0].title == "Song & café");
    assert(tracks[0].artist == "Artist" && tracks[0].albumArtist == "Album artist");
    assert(tracks[0].duration == 123 && tracks[0].trackNo == 2 && tracks[0].tagged);
    assert(tracks[0].path.find(config.password) == std::string::npos);
    assert(tracks[0].path.find("../") == std::string::npos);
    assert(tracks.back().duration == 0 && tracks.back().trackNo == INT_MAX);
    const std::string stable = tracks[0].path;
    const auto good = config;
    for (const char* endpoint : { "/bad", "/malformed", "/dtd", "/missing", "/repeated", "/redirect" }) {
        config.url = std::string(argv[1]) + endpoint;
        assert(!subsonic::scan(config, tracks, error, cancel));
        assert(tracks.size() == 501 && tracks[0].path == stable); // Failure never replaces a library.
        assert(error.find(config.password) == std::string::npos);
    }
    config = good;
    assert(subsonic::buffer(config, tracks[0], "song.mp3", error, cancel));
    assert(std::filesystem::file_size("song.mp3") == 8195);
    assert(std::filesystem::file_size("song.mp3.jpg") == 4);
    std::filesystem::remove("song.mp3.jpg");
    config.url = std::string(argv[1]) + "/cover-error";
    assert(subsonic::buffer(config, tracks[0], "song.mp3", error, cancel));
    assert(!std::filesystem::exists("song.mp3.jpg"));
    config.url = std::string(argv[1]) + "/audio-error";
    assert(!subsonic::buffer(config, tracks[0], "song.mp3", error, cancel));
    assert(!std::filesystem::exists("song.mp3"));
    config = good;
    assert(!subsonic::buffer(config, tracks[0], "missing/song.mp3", error, cancel));
    cancel = true;
    assert(!subsonic::scan(config, tracks, error, cancel));
    assert(!subsonic::buffer(config, tracks[0], "cancel.mp3", error, cancel));
    assert(!std::filesystem::exists("cancel.mp3"));
    cancel = false;
    config.url = std::string(argv[1]) + "/slow";
    bool ok = true;
    std::thread worker([&] { ok = subsonic::buffer(config, tracks[0], "cancel.mp3", error, cancel); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel = true;
    worker.join();
    assert(!ok && !std::filesystem::exists("cancel.mp3"));
    subsonic::shutdown();
    puts("Subsonic regression checks passed");
}
