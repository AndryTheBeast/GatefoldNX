/*
 * Gatefold NX · stats.cpp
 */
#include "stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "cores.hpp"

namespace {

std::string readFile(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

/** Calls line(fields, count) for each tab-separated line, up to 4 fields. */
template <class F> void eachLine(const std::string& text, F line) {
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string_view l(text.data() + at, eol - at);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        std::string_view f[4];
        int n = 0;
        while (n < 4) {
            const size_t tab = l.find('\t');
            f[n++] = l.substr(0, tab);
            if (tab == std::string_view::npos) break;
            l.remove_prefix(tab + 1);
        }
        line(f, n);
        at = eol + 1;
    }
}

int64_t num(std::string_view s) { return strtoll(std::string(s).c_str(), nullptr, 10); }

}  // namespace

void Stats::start(const std::string& dir) {
    dir_ = dir;
    thread_ = std::thread([this] { cores::moveHere(cores::Files); run(); });
}

void Stats::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

const PlayStat* Stats::find(const std::string& path) const {
    const auto it = map_.find(path);
    return it == map_.end() ? nullptr : &it->second;
}

void Stats::countPlay(const std::string& path, int64_t when) {
    PlayStat& s = map_[path];
    s.plays++;
    s.last = std::max(s.last, when);
    gen_++;
    dirty_ = true;
}

void Stats::wantDates(const std::vector<std::string>& paths) {
    std::vector<std::string> want;
    for (const auto& p : paths) {
        const auto it = map_.find(p);
        if (it == map_.end() || !it->second.added) want.push_back(p);
    }
    if (want.empty()) return;
    {
        std::lock_guard<std::mutex> g(m_);
        wantDates_.insert(wantDates_.end(), want.begin(), want.end());
    }
    wake_.notify_one();
}

void Stats::setBackground(bool on) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (background_ == on) return;
        background_ = on;
    }
    wake_.notify_one();
}

void Stats::forget(const std::string& path) {
    if (map_.erase(path)) { gen_++; dirty_ = true; }
}

bool Stats::poll() {
    Found f;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!found_.loaded && found_.dates.empty() && found_.plays.empty()) return false;
        f = std::move(found_);
        found_ = Found{};
    }
    if (f.loaded) {
        // What was counted before the file was read is added to it.
        for (auto& [p, s] : f.file) {
            auto it = map_.find(p);
            if (it == map_.end()) { map_.emplace(p, s); continue; }
            it->second.plays += s.plays;
            it->second.last = std::max(it->second.last, s.last);
            if (!it->second.added) it->second.added = s.added;
        }
        loaded_ = true;
    }
    for (const auto& [p, t] : f.dates) {
        PlayStat& s = map_[p];
        if (!s.added) { s.added = t; dirty_ = true; }
    }
    for (const auto& [p, t] : f.plays) {
        PlayStat& s = map_[p];
        s.plays++;
        s.last = std::max(s.last, t);
        dirty_ = true;
    }
    gen_++;
    return true;
}

std::string Stats::serialise() {
    dirty_ = false;
    std::string out;
    out.reserve(map_.size() * 96);
    char nums[64];
    for (const auto& [p, s] : map_) {
        snprintf(nums, sizeof nums, "\t%d\t%lld\t%lld\n", s.plays, (long long)s.last, (long long)s.added);
        out += p;
        out += nums;
    }
    return out;
}

void Stats::run() {
    // stats.tsv: path, plays, last played, date added
    {
        Found f;
        f.loaded = true;
        eachLine(readFile(dir_ + "/stats.tsv"), [&](std::string_view* c, int n) {
            if (n < 4 || c[0].empty()) return;
            f.file[std::string(c[0])] = { int(num(c[1])), num(c[2]), num(c[3]) };
        });
        std::lock_guard<std::mutex> g(m_);
        found_.loaded = true;
        found_.file = std::move(f.file);
    }
    const std::string plays = dir_ + "/bg-plays.txt", taken = dir_ + "/bg-plays.take";
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        if (!wantDates_.empty()) {
            std::vector<std::string> want = std::move(wantDates_);
            wantDates_.clear();
            g.unlock();
            // The file's date on the card: usually when it was copied there. Now for a file without one.
            const int64_t now = int64_t(std::time(nullptr));
            std::vector<std::pair<std::string, int64_t>> dates;
            dates.reserve(want.size());
            for (auto& p : want) {
                struct stat st {};
                const int64_t t = stat(p.c_str(), &st) == 0 && st.st_mtime > 0 ? int64_t(st.st_mtime) : now;
                dates.emplace_back(std::move(p), t);
            }
            g.lock();
            found_.dates.insert(found_.dates.end(), dates.begin(), dates.end());
            continue;
        }
        if (background_) {
            g.unlock();
            // The player appends "time<TAB>path" lines. Renamed first, so a play it writes meanwhile
            // starts a new file instead of being lost when this one is removed.
            std::vector<std::pair<std::string, int64_t>> got;
            remove(taken.c_str());
            if (rename(plays.c_str(), taken.c_str()) == 0) {
                eachLine(readFile(taken), [&](std::string_view* c, int n) {
                    // A time of 0: the player had no clock, so it is dated now.
                    if (n >= 2 && !c[1].empty()) got.emplace_back(std::string(c[1]), num(c[0]) > 0 ? num(c[0]) : int64_t(std::time(nullptr)));
                });
                remove(taken.c_str());
            }
            g.lock();
            found_.plays.insert(found_.plays.end(), got.begin(), got.end());
        }
        wake_.wait_for(g, std::chrono::seconds(5), [&] { return quit_ || !wantDates_.empty(); });
    }
}
