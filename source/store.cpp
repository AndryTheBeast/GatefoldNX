/*
 * Gatefold NX · store.cpp
 */
#include "store.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

#include <deque>
#include <mutex>
#include <thread>

#include "cores.hpp"

namespace {

/**
 * Files are written by a thread of their own: a save touched five files on the SD card, tens of
 * milliseconds on the console, and it ran every 20 s while playing, a hitch in whatever was moving.
 * A file queued again before it was written is replaced, and a file whose contents did not change
 * since it was last written is skipped.
 */
struct Writer {
    struct Job { std::string path, data; bool atomic = false; };
    std::mutex m;
    std::condition_variable wake, done;
    std::deque<Job> jobs;
    std::unordered_map<std::string, std::string> written;
    bool busy = false, started = false, quit = false;
    std::thread thread;   // kept, never detached: libnx has no pthread_detach (detach() throws and aborts)

    void put(const std::string& path, std::string data, bool atomic) {
        std::lock_guard<std::mutex> g(m);
        const bool queued = std::any_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.path == path; });
        if (auto w = written.find(path); !queued && w != written.end() && w->second == data) return;
        if (queued) { for (Job& j : jobs) if (j.path == path) j.data = std::move(data); }
        else jobs.push_back({ path, std::move(data), atomic });
        if (!started) {
            started = true;
            thread = std::thread([this] { cores::moveHere(cores::Files); run(); });
        }
        wake.notify_one();
    }
    void flush() {
        std::unique_lock<std::mutex> g(m);
        done.wait(g, [&] { return jobs.empty() && !busy; });
    }
    /** Writes what is queued and ends the thread. A thread left running when Gatefold exits keeps hbloader from
     *  unloading it: loading the next .nro (an update's Restart now) then aborted with 0xD401. */
    void stop() {
        {
            std::lock_guard<std::mutex> g(m);
            if (!started) return;
            quit = true;
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        std::lock_guard<std::mutex> g(m);
        started = false;
        quit = false;
    }
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> g(m);
                busy = false;
                done.notify_all();
                wake.wait(g, [&] { return !jobs.empty() || quit; });
                if (jobs.empty()) return;   // quit, with everything written
                j = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }
            // Atomic ones are written next to the old file and renamed over it, so a power cut mid-write loses nothing.
            // A write that did not go through whole (a full SD card) never replaces the old file, and is not remembered
            // as written, so the next save tries again.
            const std::string tmp = j.atomic ? j.path + ".tmp" : j.path;
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                bool ok = fwrite(j.data.data(), 1, j.data.size(), f) == j.data.size();
                ok = fclose(f) == 0 && ok;
                if (j.atomic) {
                    if (ok) {
                        remove(j.path.c_str());
                        ok = rename(tmp.c_str(), j.path.c_str()) == 0;
                    } else {
                        remove(tmp.c_str());
                    }
                }
                if (ok) {
                    std::lock_guard<std::mutex> g(m);
                    written[j.path] = std::move(j.data);
                }
            }
        }
    }
};
Writer& writer() {
    static Writer* w = new Writer;   // never destroyed; its thread ends with Store::stopWriter (App::shutdown)
    return *w;
}

void appendf(std::string& s, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t at = s.size();
        s.resize(at + n + 1);
        vsnprintf(&s[at], n + 1, fmt, ap2);
        s.resize(at + n);
    }
    va_end(ap2);
}

void makeDirs() {
    static bool made = false;   // the tag cache is opened in the folder straight after the first save
    if (made) return;
    mkdir("sdmc:/config", 0777);
    mkdir(Store::Dir, 0777);
    made = true;
}

std::vector<std::string> readLines(const std::string& file) {
    std::vector<std::string> out;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return out;
    char buf[2048];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (n) out.emplace_back(buf);
    }
    fclose(f);
    return out;
}

}  // namespace

void Store::load() {
    bool filesTabKnown = false;   // a settings.txt from before Files: its hiddenTabs knows nothing of bit 6
    bool linesKnown = false;      // ... and one from before placed lines: Files had a line of its own above it
    for (const auto& l : readLines(path("settings.txt"))) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "volume") volume = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "order") order = Order(std::clamp(atoi(v.c_str()), 0, 3));
        else if (k == "sidebarCollapsed") sidebarCollapsed = v == "1";
        else if (k == "motion") motion = v == "1";
        else if (k == "spectrum") showSpectrum = v == "1";
        else if (k == "canvasLayout") canvasLayout = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "canvasLyrics") canvasLyrics = v == "1";
        else if (k == "lyricTiming") lyricTiming = v == "1";
        else if (k == "lyricTimingFor") lyricTimingFor = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "resume") resume = v == "1";
        else if (k == "background") background = v == "1";
        else if (k == "viewPlaylist") viewPlaylist = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewFolders") viewFolders = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewTracks") viewTracks = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewGenre") viewGenre = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "libTab") libTab = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "tracksSort") tracksSort = std::clamp(atoi(v.c_str()), 0, 5);
        else if (k == "tracksDesc") tracksDesc = v == "1";
        else if (k == "hiddenTabs") hiddenTabs = unsigned(atoi(v.c_str())) & 127u;
        else if (k == "theme") { theme = std::clamp(atoi(v.c_str()), 0, 4); if (theme == 2) theme = 1; }   // Grey is no longer offered: Dark
        else if (k == "brightness") brightness = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "saturation") saturation = std::clamp(float(atof(v.c_str())), 0.f, 1.f);   // was up to 150%, which changed nothing past 100%
        else if (k == "colourTransition") colourTransition = std::clamp(float(atof(v.c_str())), 0.f, 3.f);
        else if (k == "customBg") customBg = atoi(v.c_str());
        else if (k == "customAccent") customAccent = atoi(v.c_str());
        else if (k == "customAccent2") customAccent2 = atoi(v.c_str());
        else if (k == "customMode") customMode = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "canvasAutoMinimal") canvasAutoMinimal = v == "1";
        else if (k == "canvasFloat") canvasFloat = v == "1";
        else if (k == "canvasZoom") canvasZoom = v == "1";
        else if (k == "vizSensitivity") vizSensitivity = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "vizSmoothing") vizSmoothing = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "vizBands") vizBands = std::clamp(atoi(v.c_str()), 16, 96);
        else if (k == "vizPeaks") vizPeaks = v == "1";
        else if (k == "canvasPulse") canvasPulse = v == "1";
        else if (k == "canvasBars") canvasBars = std::clamp(atoi(v.c_str()), 16, 128);
        else if (k == "canvasIdleAfter") canvasIdleAfter = std::clamp(float(atof(v.c_str())), 1.f, 30.f);
        else if (k == "canvasVizTime") canvasVizTime = std::clamp(float(atof(v.c_str())), 0.1f, 4.f);
        else if (k == "coverPulse") coverPulse = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "pointerSpeed") pointerSpeed = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "pointerAccel") pointerAccel = v == "1";
        else if (k == "frameTimes") frameTimes = v == "1";
        else if (k == "keepAwake") keepAwake = v == "1";
        else if (k == "levelMode") levelMode = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "levelTarget") levelTarget = std::clamp(atoi(v.c_str()), -23, -6);
        else if (k == "batteryPercent") batteryPercent = v == "1";
        else if (k == "batteryEstimate") batteryEstimate = v == "1";
        else if (k == "dimAfter") dimAfter = std::clamp(atoi(v.c_str()), 0, 60);
        else if (k == "sleepConsole") sleepConsole = std::clamp(atoi(v.c_str()), 0, 300);
        else if (k == "stopCovers") stopCovers = v == "1";
        else if (k == "stopThumbs") stopThumbs = v == "1";
        else if (k == "stopLevels") stopLevels = v == "1";
        else if (k == "crossfade") crossfade = std::clamp(float(atof(v.c_str())), 0.f, 12.f);
        else if (k == "xfadeNotInAlbum") xfadeNotInAlbum = v == "1";
        else if (k == "eqOn") this->eq.on = uint8_t(v == "1");
        else if (k == "eqAdvanced") this->eq.advanced = uint8_t(v == "1");
        else if (k == "eqPreset") this->eq.preset = int8_t(std::clamp(atoi(v.c_str()), -1, GFEQ_PRESETS + GFEQ_USER_MAX - 1));
        else if (k == "eqPreamp") this->eq.preamp = float(atof(v.c_str()));
        else if (k == "eqBass") this->eq.bass = float(atof(v.c_str()));
        else if (k == "eqTreble") this->eq.treble = float(atof(v.c_str()));
        else if (k == "eqEdited") this->eq.edited = uint8_t(v == "1");
        else if (k == "eqBands") gfEqParseBands(v.c_str(), this->eq.bands);
        else if (k == "eqCustom") { gfEqParseBands(v.c_str(), this->eq.custom); this->eq.hasCustom = 1; }
        else if (k == "musicRoot") musicRoot = v;
        else if (k == "showHidden") showHidden = v == "1";
        else if (k == "viewPlaylists") viewPlaylists = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "tabOrder") {
            // ids that exist, once each; any missing (a tab newer than the file) go at the end in their own order
            std::vector<int> o;
            for (size_t i = 0; i < v.size();) {
                const int t = atoi(v.c_str() + i);
                if (((t >= 0 && t < TabCount) || isLine(t)) && std::find(o.begin(), o.end(), t) == o.end()) o.push_back(t);
                const size_t c = v.find(',', i);
                if (c == std::string::npos) break;
                i = c + 1;
            }
            for (int t = 0; t < TabCount; t++) if (std::find(o.begin(), o.end(), t) == o.end()) o.push_back(t);
            tabOrder = o;
        }
        else if (k == "filesTab") filesTabKnown = true;
        else if (k == "tabLines") linesKnown = true;
        else if (k == "filesDir") filesDir = v;
        else if (k == "importDir") importDir = v;
        else if (k == "exportDir") exportDir = v;
        else if (k == "coverDir") coverDir = v;
        else if (k == "backupDir") backupDir = v;
        else if (k == "updates") updates = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "updateAtStart") updateAtStart = v == "1";
        else if (k == "updateToasted") updateToasted = v;
    }
    if (!filesTabKnown) hiddenTabs |= 64u;   // Files stays hidden until Settings → Appearance shows it
    if (!linesKnown && std::none_of(tabOrder.begin(), tabOrder.end(), isLine)) {   // the line Files had, now one of yours
        auto f = std::find(tabOrder.begin(), tabOrder.end(), int(TabFiles));
        tabOrder.insert(f, TabLine);
    }
    gfEqClamp(&eq);
    playlists.clear();
    for (const auto& l : readLines(path("playlists.txt"))) {
        const size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        playlists.push_back({ l.substr(0, tab), l.substr(tab + 1), {} });
    }
    // Icons chosen: "icon<TAB>name<TAB>path" (a file of their own, so playlists.txt stays as older versions read it)
    for (const auto& l : readLines(path("playlist-icons.tsv"))) {
        const size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::string name = l.substr(a + 1, b - a - 1), where = l.substr(b + 1);
        for (auto& p : playlists) if (p.name == name && p.path == where) p.icon = l.substr(0, a);
    }
    // Your own playlists' tracks: "#name" then one path a line (lists.txt).
    FolderPlaylist* cur = nullptr;
    for (const auto& l : readLines(path("lists.txt"))) {
        if (!l.empty() && l[0] == '#') {
            cur = nullptr;
            for (auto& p : playlists) if (p.own() && p.name == l.substr(1)) cur = &p;
        } else if (cur && !l.empty()) {
            cur->tracks.push_back(l);
        }
    }
    liked = readLines(path("liked.txt"));
    eqUser.clear();
    for (const auto& l : readLines(path("eq-presets.txt"))) {
        GfEqUser u{};
        if (gfEqParseUser(l.c_str(), &u) && eqUser.size() < GFEQ_USER_MAX) eqUser.push_back(u);
    }
    if (eq.preset >= GFEQ_PRESETS + int(eqUser.size())) gfEqMakeCustom(&eq);   // a preset deleted meanwhile: what it set stays, as Custom
    edits.clear();
    for (const auto& l : readLines(path("edits.tsv"))) {
        // path \t field=value \t field=value …
        size_t a = l.find('\t');
        if (a == std::string::npos) continue;
        auto& e = edits[l.substr(0, a)];
        while (a != std::string::npos) {
            const size_t b = l.find('\t', a + 1);
            const std::string kv = l.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) e[kv.substr(0, eq)] = kv.substr(eq + 1);
            a = b;
        }
    }
    covers.clear();
    for (const auto& l : readLines(path("covers.tsv"))) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) covers[l.substr(0, tab)] = l.substr(tab + 1);
    }
    lyricNudge.clear();
    for (const auto& l : readLines(path("lyric-offsets.tsv"))) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) lyricNudge[l.substr(0, tab)] = std::clamp(float(atof(l.c_str() + tab + 1)), -10.f, 10.f);
    }
}

/** lyric-offsets.tsv: "path<TAB>seconds", the lyrics nudge per track (written by the writer thread). */
void Store::saveLyricNudges() const {
    std::string out;
    for (const auto& [p, s] : lyricNudge) appendf(out, "%s\t%.1f\n", p.c_str(), double(s));
    writeFile("lyric-offsets.tsv", std::move(out));
}

/** eq-presets.txt (GFEQ_USER): your saved equalizer presets, as gfEqParseUser reads them. */
void Store::saveEqPresets() const {
    std::string out;
    for (const auto& u : eqUser) {
        appendf(out, "%s\t%.1f\t%.1f\t%.1f\t", u.name, double(u.preamp), double(u.bass), double(u.treble));
        for (int b = 0; b < GFEQ_BANDS; b++) appendf(out, b ? " %.1f" : "%.1f", double(u.bands[b]));
        out += '\n';
    }
    writeFile("eq-presets.txt", std::move(out));
}

void Store::saveEdits() const {
    mkdir("sdmc:/config", 0777);
    mkdir(Dir, 0777);
    auto clean = [](std::string s) {
        for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    if (FILE* f = fopen(path("edits.tsv").c_str(), "wb")) {
        for (const auto& [p, e] : edits) {
            if (e.empty()) continue;
            fputs(p.c_str(), f);
            for (const auto& [k, v] : e) fprintf(f, "\t%s=%s", k.c_str(), clean(v).c_str());
            fputc('\n', f);
        }
        fclose(f);
    }
    if (FILE* f = fopen(path("covers.tsv").c_str(), "wb")) {
        for (const auto& [p, file] : covers) fprintf(f, "%s\t%s\n", p.c_str(), file.c_str());
        fclose(f);
    }
}

void Store::save() const {
    makeDirs();
    std::string f;
    appendf(f, "volume=%.3f\norder=%d\nsidebarCollapsed=%d\nmotion=%d\nspectrum=%d\ncanvasLayout=%d\ncanvasLyrics=%d\n", volume, int(order),
            sidebarCollapsed, motion, showSpectrum, canvasLayout, canvasLyrics);
    appendf(f, "vizSensitivity=%.3f\nvizSmoothing=%.3f\nvizBands=%d\nvizPeaks=%d\ncoverPulse=%.3f\n", vizSensitivity, vizSmoothing, vizBands,
            vizPeaks, coverPulse);
    appendf(f, "canvasPulse=%d\nhiddenTabs=%u\n", canvasPulse, hiddenTabs);
    appendf(f, "canvasBars=%d\ncanvasIdleAfter=%.2f\ncanvasVizTime=%.2f\n", canvasBars, double(canvasIdleAfter), double(canvasVizTime));
    appendf(f, "canvasFloat=%d\ncanvasZoom=%d\nresume=%d\ntheme=%d\ncanvasAutoMinimal=%d\n", canvasFloat, canvasZoom, resume, theme,
            canvasAutoMinimal);
    appendf(f, "background=%d\n", background);
    appendf(f, "brightness=%d\nsaturation=%.2f\ncolourTransition=%.2f\ncustomBg=%d\ncustomAccent=%d\ncustomAccent2=%d\n", brightness, double(saturation),
            double(colourTransition), customBg, customAccent, customAccent2);
    appendf(f, "customMode=%d\n", customMode);
    appendf(f, "viewPlaylist=%d\nviewFolders=%d\nviewTracks=%d\nlibTab=%d\ntracksSort=%d\ntracksDesc=%d\n", viewPlaylist, viewFolders, viewTracks, libTab,
            tracksSort, tracksDesc);
    appendf(f, "pointerSpeed=%.2f\npointerAccel=%d\nframeTimes=%d\nviewGenre=%d\n", double(pointerSpeed), pointerAccel, frameTimes, viewGenre);
    appendf(f, "keepAwake=%d\ndimAfter=%d\nsleepConsole=%d\n", keepAwake, dimAfter, sleepConsole);
    appendf(f, "batteryPercent=%d\nbatteryEstimate=%d\n", batteryPercent, batteryEstimate);
    appendf(f, "stopCovers=%d\nstopThumbs=%d\nstopLevels=%d\n", stopCovers, stopThumbs, stopLevels);
    appendf(f, "lyricTiming=%d\nlyricTimingFor=%d\n", lyricTiming, lyricTimingFor);
    appendf(f, "levelMode=%d\nlevelTarget=%d\n", levelMode, levelTarget);
    appendf(f, "crossfade=%.1f\nxfadeNotInAlbum=%d\n", double(crossfade), xfadeNotInAlbum);
    appendf(f, "eqEdited=%d\n", eq.edited);
    appendf(f, "eqOn=%d\neqAdvanced=%d\neqPreset=%d\neqPreamp=%.1f\neqBass=%.1f\neqTreble=%.1f\neqBands=", eq.on, eq.advanced, eq.preset,
            double(eq.preamp), double(eq.bass), double(eq.treble));
    for (int b = 0; b < GFEQ_BANDS; b++) appendf(f, b ? " %.1f" : "%.1f", double(eq.bands[b]));
    appendf(f, "\n");
    if (eq.hasCustom) {
        appendf(f, "eqCustom=");
        for (int b = 0; b < GFEQ_BANDS; b++) appendf(f, b ? " %.1f" : "%.1f", double(eq.custom[b]));
        appendf(f, "\n");
    }
    if (!musicRoot.empty()) appendf(f, "musicRoot=%s\n", musicRoot.c_str());
    appendf(f, "tabOrder=");
    for (size_t i = 0; i < tabOrder.size(); i++) appendf(f, i ? ",%d" : "%d", tabOrder[i]);
    appendf(f, "\ntabLines=1");   // knows placed lines (Store::load)
    appendf(f, "\n");
    appendf(f, "showHidden=%d\nfilesTab=1\nviewPlaylists=%d\n", showHidden, viewPlaylists);   // filesTab: hiddenTabs already knows the Files tab (bit 6)
    for (const auto& [k, v] : { std::pair<const char*, const std::string*>{ "filesDir", &filesDir }, { "importDir", &importDir },
                                { "exportDir", &exportDir }, { "coverDir", &coverDir }, { "backupDir", &backupDir } })
        if (!v->empty()) appendf(f, "%s=%s\n", k, v->c_str());
    appendf(f, "updates=%d\nupdateAtStart=%d\n", updates, updateAtStart);
    if (!updateToasted.empty()) appendf(f, "updateToasted=%s\n", updateToasted.c_str());
    writer().put(path("settings.txt"), std::move(f), true);

    std::string pl;
    for (const auto& p : playlists) appendf(pl, "%s\t%s\n", p.name.c_str(), p.path.c_str());
    writer().put(path("playlists.txt"), std::move(pl), true);
    std::string icons;
    for (const auto& p : playlists) if (!p.icon.empty()) appendf(icons, "%s\t%s\t%s\n", p.icon.c_str(), p.name.c_str(), p.path.c_str());
    writer().put(path("playlist-icons.tsv"), std::move(icons), true);
    std::string own;
    for (const auto& p : playlists) {
        if (!p.own()) continue;
        appendf(own, "#%s\n", p.name.c_str());
        for (const auto& t : p.tracks) appendf(own, "%s\n", t.c_str());
    }
    writer().put(path("lists.txt"), std::move(own), true);
    std::string lk;
    for (const auto& p : liked) appendf(lk, "%s\n", p.c_str());
    writer().put(path("liked.txt"), std::move(lk), true);
}

void Store::flush() { writer().flush(); }
void Store::stopWriter() { writer().stop(); }

void Store::writeFile(const char* file, std::string data) const {
    makeDirs();
    writer().put(path(file), std::move(data), true);
}

bool Store::loadSession(Session& s) const {
    s = Session{};
    for (const auto& l : readLines(path("session.txt"))) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "track") s.tracks.push_back(v);
        else if (k == "name") s.name = v;
        else if (k == "pos") s.pos = atoi(v.c_str());
        else if (k == "position") s.position = std::max(0.0, atof(v.c_str()));
        else if (k == "order") {
            const char* p = v.c_str();
            char* end = nullptr;
            for (long n = strtol(p, &end, 10); end != p; n = strtol(p, &end, 10)) { s.order.push_back(int(n)); p = end; }
        }
    }
    return !s.tracks.empty();
}

void Store::saveSession(const Session& s) const {
    makeDirs();
    std::string f;
    appendf(f, "name=%s\npos=%d\nposition=%.2f\norder=", s.name.c_str(), s.pos, s.position);
    for (size_t i = 0; i < s.order.size(); i++) appendf(f, i ? " %d" : "%d", s.order[i]);
    f += "\n";
    for (const auto& t : s.tracks) appendf(f, "track=%s\n", t.c_str());
    writer().put(path("session.txt"), std::move(f), true);
}

/** name, or "name 2", "name 3" …: playlists are known by name (lists.txt, the game actions' lists, the overlay). */
std::string Store::freePlaylistName(std::string name) const {
    if (name.empty()) name = "Playlist";
    const std::string base = name;
    for (int n = 2;; n++) {
        bool taken = name == "Liked";   // the background player and the overlay know Liked by that name
        for (const auto& p : playlists) taken |= p.name == name;
        if (!taken) return name;
        name = base + " " + std::to_string(n);
    }
}

int Store::newPlaylist(std::string name) {
    playlists.push_back({ freePlaylistName(std::move(name)), "", {} });
    return int(playlists.size()) - 1;
}

bool Store::isLiked(const std::string& p) const { return std::find(liked.begin(), liked.end(), p) != liked.end(); }

void Store::toggleLiked(const std::string& p) {
    auto it = std::find(liked.begin(), liked.end(), p);
    if (it == liked.end()) liked.push_back(p);
    else liked.erase(it);
}

int Store::playlistByPath(const std::string& p) const {
    for (int i = 0; i < int(playlists.size()); i++)
        if (strcasecmp(playlists[i].path.c_str(), p.c_str()) == 0) return i;
    return -1;
}
