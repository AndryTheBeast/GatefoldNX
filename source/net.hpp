/*
 * Gatefold NX · net.hpp
 * The updater's only way online: HTTPS GETs to GitHub, never at launch unless Settings → Updates allows it; and Settings ›
 * Support's report, one POST when you send it.
 * On the console it is libcurl over the Switch's own ssl service (its certificates, peers verified); in the PC
 * preview a stand-in serves a local folder (GF_NET=<folder>: releases.json and <tag>/<asset>), so the whole
 * flow can be tried without the internet.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace net {

/** How far a transfer is (bytes), and a way to stop it from another thread. */
struct Progress {
    std::atomic<int64_t> done{ 0 }, total{ 0 };
    std::atomic<bool> cancel{ false };
};

/** Sockets and curl, once; false (and why) when the console has no network. Safe to call again. */
bool begin(std::string& err);
/** Undoes begin() (at exit). */
void end();
/** The body of a GET (the GitHub API wants a User-Agent; one is always sent). */
bool get(const std::string& url, std::string& out, std::string& err, Progress* p = nullptr);
/** A GET written to a file (replaced only when complete: written next to it, then renamed). */
bool download(const std::string& url, const std::string& file, std::string& err, Progress* p);
/** A POST of JSON (Settings › Support's reports): `secret` a header line ("Authorization: …") wiped, here and in curl's
 *  copy, once the request is done; the answer's body in out. */
bool post(const std::string& url, const std::string& json, char* secret, std::string& out, std::string& err);

}  // namespace net
