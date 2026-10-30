/*
 * Gatefold NX · levels.cpp
 */
#include "levels.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cores.hpp"
#include "loudness.hpp"

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

}  // namespace

void Levels::start(const std::string& dir) {
    dir_ = dir;
    threads_[0] = std::thread([this] { cores::moveHere(cores::Loud); loadFile(); run(0); });
    threads_[1] = std::thread([this] { cores::moveHere(cores::Loud2); run(1); });
}

void Levels::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

const Level* Levels::find(const std::string& path) const {
    const auto it = map_.find(path);
    return it == map_.end() ? nullptr : &it->second;
}

void Levels::wantMood(bool on) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (moodWanted_ == on) return;
        moodWanted_ = on;
        allAt_ = 0;   // the library again: the ones without their mood
    }
    wake_.notify_all();
}

void Levels::want(std::vector<std::string> first, std::vector<std::string> all) {
    {
        std::lock_guard<std::mutex> g(m_);
        first_ = std::move(first);
        if (all.size() != all_.size() || all != all_) { all_ = std::move(all); allAt_ = 0; }
    }
    wake_.notify_all();
}

bool Levels::poll() {
    std::vector<std::pair<std::string, Level>> got;
    bool read = false;
    std::unordered_map<std::string, Level> file;
    {
        std::lock_guard<std::mutex> g(m_);
        if (found_.empty() && !(fileRead_ && !loaded_)) return false;
        got.swap(found_);
        if (fileRead_ && !loaded_) { file.swap(fromFile_); read = true; }
        for (double s : took_) {
            if (lastDone_ >= 0 && s - lastDone_ < 30) { measuredS_ += s - lastDone_; measuredN_++; }
            lastDone_ = std::max(lastDone_, s);
        }
        took_.clear();
    }
    if (read) {
        for (auto& [p, l] : file) map_.emplace(p, l);   // what was measured before the file was read stays
        loaded_ = true;
    }
    for (auto& [p, l] : got) map_[p] = l;
    if (read || !got.empty()) {
        done_ = moodDone_ = 0;
        for (const auto& [p, l] : map_) {
            if (l.mix.seconds != 0) done_++;
            if (l.moodTried) moodDone_++;
        }
    }
    if (!got.empty()) dirty_ = true;
    gen_++;
    return true;
}

void Levels::forget() {
    {
        std::lock_guard<std::mutex> g(m_);
        known_.clear();
        moodKnown_.clear();
        found_.clear();
        allAt_ = 0;
    }
    map_.clear();
    done_ = moodDone_ = 0;
    dirty_ = true;
    gen_++;
    wake_.notify_all();
}

std::string Levels::serialise() {
    dirty_ = false;
    std::string out;
    out.reserve(map_.size() * 96);
    char b[80];
    for (const auto& [p, l] : map_) {
        if (std::isnan(l.lufs)) snprintf(b, sizeof b, "\tx\t0\t0\n");   // tried, could not be measured
        else snprintf(b, sizeof b, "\t%.2f\t%.6f\t%.0f\n", double(l.lufs), double(l.peak), double(l.blocks));
        out += p;
        out += b;
    }
    return out;
}

std::string Levels::serialiseMix() const {
    std::string out;
    out.reserve(map_.size() * 140);
    for (const auto& [p, l] : map_) {
        if (l.mix.seconds == 0) continue;   // from before Automix: analysed when decoded again (-1: tried, unreadable)
        out += p;
        out += '\t';
        out += l.mix.pack();
        out += '\n';
    }
    return out;
}

std::string Levels::serialiseMood() const {
    std::string out;
    out.reserve(map_.size() * 110);
    for (const auto& [p, l] : map_) {
        if (!l.moodTried) continue;
        out += p;
        out += '\t';
        out += l.mix.mood.valid ? l.mix.mood.pack() : std::to_string(mixan::Mood::Version) + ",x";   // x: tried, nothing to measure
        out += '\n';
    }
    return out;
}

/**
 * loudness.tsv: path, LUFS (x: could not be measured), true peak, blocks; mix.tsv: path, Automix's analysis
 * (mixan::Info::pack). The first worker, before it measures. A track counts as done only with both (and the
 * analysis of this version): one measured before Automix is decoded once more.
 */
void Levels::loadFile() {
    {
        std::unordered_map<std::string, Level> file;
        const std::string text = readFile(dir_ + "/loudness.tsv");
        size_t at = 0;
        while (at < text.size()) {
            size_t eol = text.find('\n', at);
            if (eol == std::string::npos) eol = text.size();
            const std::string line = text.substr(at, eol - at);
            at = eol + 1;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos || t1 == 0) continue;
            const char* c = line.c_str() + t1 + 1;
            Level l;
            char* e = nullptr;
            if (*c == 'x') { e = const_cast<char*>(c + 1); }
            else { l.lufs = strtof(c, &e); }
            if (*e == '\t') l.peak = strtof(e + 1, &e);
            if (*e == '\t') l.blocks = strtof(e + 1, &e);
            file[line.substr(0, t1)] = l;
        }
        std::unordered_set<std::string> mixed;
        const std::string mix = readFile(dir_ + "/mix.tsv");
        at = 0;
        while (at < mix.size()) {
            size_t eol = mix.find('\n', at);
            if (eol == std::string::npos) eol = mix.size();
            const std::string line = mix.substr(at, eol - at);
            at = eol + 1;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos || t1 == 0) continue;
            const auto it = file.find(line.substr(0, t1));
            if (it != file.end() && mixan::Info::unpack(line.substr(t1 + 1), it->second.mix)) mixed.insert(it->first);
        }
        // mood.tsv: path, the mood's features (mixan::Mood::pack) or "<version>,x" (tried); another version's are measured again
        const std::string mood = readFile(dir_ + "/mood.tsv");
        at = 0;
        while (at < mood.size()) {
            size_t eol = mood.find('\n', at);
            if (eol == std::string::npos) eol = mood.size();
            const std::string line = mood.substr(at, eol - at);
            at = eol + 1;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos || t1 == 0) continue;
            const auto it = file.find(line.substr(0, t1));
            if (it == file.end()) continue;
            const std::string v = line.substr(t1 + 1);
            if (mixan::Mood::unpack(v, it->second.mix.mood)) it->second.moodTried = true;
            else if (v == std::to_string(mixan::Mood::Version) + ",x") it->second.moodTried = true;
        }
        std::lock_guard<std::mutex> g(m_);
        for (const auto& [p, l] : file) {
            if (mixed.count(p)) known_.insert(p);
            if (l.moodTried) moodKnown_.insert(p);
        }
        fromFile_ = std::move(file);
        fileRead_ = true;
    }
    wake_.notify_all();
}

void Levels::pause(bool p) {
    {
        std::lock_guard<std::mutex> g(m_);
        paused_ = p;
    }
    wake_.notify_all();
}

void Levels::run(int) {
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        if (!fileRead_) { wake_.wait(g); continue; }   // what was measured before is not measured again
        // The next path not measured yet: the ones asked for first, then the library in order.
        std::string path;
        bool background = false;
        // (with Mood playlists, a track measured before without its mood is decoded again for it, in the library's turn)
        auto todo = [&](const std::string& p) { return !known_.count(p) || (moodWanted_ && !moodKnown_.count(p)); };
        for (const auto& p : first_)
            if (!known_.count(p)) { path = p; break; }
        while (path.empty() && !paused_ && allAt_ < all_.size()) {
            const std::string& p = all_[allAt_++];
            if (todo(p)) { path = p; background = true; }
        }
        if (path.empty()) {
            wake_.wait(g);
            continue;
        }
        known_.insert(path);
        moodKnown_.insert(path);
        if (background) busy_++;
        g.unlock();
        loud::Meter::Result r;
        mixan::Analyzer an;
        const bool ok = loud::measureFile(path, r, &quit_, &an);
        g.lock();
        if (background) busy_--;
        if (quit_) break;
        Level l;
        if (ok) {
            g.unlock();   // the analysis's last steps take a moment: not under the lock
            l.mix = an.finish();
            g.lock();
            if (quit_) break;
        } else {
            l.mix.seconds = -1;   // tried: not decoded again at every launch
        }
        l.moodTried = true;   // (measured with it, or tried)
        if (ok && !std::isnan(r.lufs)) {
            l.lufs = float(r.lufs);
            l.peak = float(r.truePeak);
            // the blocks that passed both gates: those at or above the relative gate
            double n = 0, p = 0;
            for (int i = 0; i < loud::Bins; i++) { n += r.hist.n[size_t(i)]; p += r.hist.p[size_t(i)]; }
            double kept = 0;
            if (n > 0) {
                const double rel = -0.691 + 10 * std::log10(p / n) - 10;
                const int from = std::max(0, int(std::ceil((rel - loud::Lo) / loud::Bin - 1e-9)));
                for (int i = from; i < loud::Bins; i++) kept += r.hist.n[size_t(i)];
            }
            l.blocks = float(kept);
            if (background) took_.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        found_.emplace_back(path, l);
    }
}
