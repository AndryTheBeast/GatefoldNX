#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "library.hpp"

struct SubsonicConfig {
    bool enabled = false;
    std::string url, user, password;
};

namespace subsonic {
bool init();
void shutdown();
bool validUrl(const std::string& url);
bool scan(const SubsonicConfig& config, std::vector<Track>& tracks, std::string& error, const std::atomic<bool>& cancel);
bool buffer(const SubsonicConfig& config, const Track& track, const std::string& file, std::string& error,
            const std::atomic<bool>& cancel);
}
