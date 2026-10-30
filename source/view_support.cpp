/*
 * Songate · view_support.cpp
 * Settings › Support's sheets (the user, 2026-10-30): the report's form, filled with the options the app gives, sent from the
 * console to Songate's reports on GitHub (report.hpp: a repository of its own); and Atmosphère's crash reports. The card's
 * reads (the crash reports, the console's versions) and the sending run on workers; nothing of it on the main thread.
 */
#include <algorithm>
#include <ctime>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {

const char* const kAreas[] = { "Now playing", "Library and playlists", "Automix", "Mood playlists", "Canvas and lyrics",
                               "Sound and equalizer", "Background player and overlay", "Updates and settings", "Something else" };
constexpr int kAreaCount = int(std::size(kAreas));
const char* const kOften[] = { "Always", "Sometimes", "Once" };

std::string thousands(size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

}  // namespace

/** Reports are off until allowed here: what a report sends, where it goes and who can see it, said before anything is sent
 *  (the user, 2026-10-30: "a toggle like Updates for Support, with the privacy prompt too"). */
void App::allowReports() {
    confirm(ICON_HELP, "Support", "Send reports from Songate?", "GitHub · AndryTheBeast/songate-reports, a private repository",
            "Allow Songate to send reports?",
            "Songate sends a report only when you fill in the form and press Send, to a private repository on GitHub that only "
            "Songate's developer can read. With what you write it sends Songate's version, the firmware and Atmosphère's, docked "
            "or handheld, the background player, the playing file's format, how many tracks you have, and, if you leave them on, "
            "Atmosphère's crash reports and Songate's last messages. Never your files' names. GitHub sees your console's IP "
            "address. You can turn this off at any time.",
            "Allow", [this] {
                store.reports = true;
                persistSoon(0.3f);
                toast("Reports allowed: sent only when you press Send");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

void App::openReport(int kind) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    const Id back = ui.focusedId();
    sheet = Sheet{};
    sheet.page = Sheet::Report;
    sheet.repKind = kind;
    sheet.returnFocus = back;
    ui.focusDefault();
    supportScan();
}

void App::supportScan() {
    if (supportAsked_ || supportThread_.joinable()) return;
    supportAsked_ = true;
    supportReady_ = false;
    supportThread_ = std::thread([this] {
        cores::moveHere(cores::FileOps);
        supportSys_ = report::system();
        supportCrashes_ = report::crashes(20);
        supportReady_ = true;
    });
}

void App::supportTick() {
    if (supportThread_.joinable() && supportReady_) supportThread_.join();
    if (reportThread_.joinable() && reportDone_) {
        reportThread_.join();
        if (reportOk_) {
            reportSentAt_ = time;
            toast(reportNumber_ > 0 ? "Sent, thank you: report #" + std::to_string(reportNumber_) : std::string("Sent, thank you"));
            if (sheet.page == Sheet::Report) closeSheet();
        } else {
            toast("Not sent: " + reportErr_);
        }
    }
    // the app's log, saved now and then by the writer thread (store.writeFile), never from here
    if (logDirty_ && time - logSavedAt_ > 30) {
        std::string t;
        for (const auto& l : logLines_) { t += l; t += '\n'; }
        store.writeFile("log.txt", std::move(t));
        logDirty_ = false;
        logSavedAt_ = time;
    }
}

std::string App::reportBody() const {
    const bool bug = sheet.repKind == 0;
    std::string b = std::string("**") + (bug ? "Bug" : "Idea") + "** · " + (sheet.repArea >= 0 ? kAreas[sheet.repArea] : "?");
    if (bug) b += std::string(" · ") + kOften[std::clamp(sheet.repOften, 0, 2)];
    b += "\n\n### " + std::string(bug ? "What happened" : "The idea") + "\n\n" + (sheet.repWhat.empty() ? "(not given)" : sheet.repWhat) + "\n";
    if (bug) b += "\n### Steps\n\n" + (sheet.repSteps.empty() ? std::string("(not given)") : sheet.repSteps) + "\n";
    // the app's part: nothing of the user's files but the playing one's format
    b += "\n### This console\n\n| | |\n|---|---|\n| Songate | " + Updater::thisVersion() + " · protocol " + std::to_string(Updater::thisProtocol()) + " |\n";
    b += "{{SYSTEM}}";
    b += "| Background player | " + std::string(player.remote() ? "connected" : "not connected") + " |\n";
    if (now.track >= 0 && !now.info.codec.empty()) {
        std::string f = now.info.codec;
        if (now.info.sampleRate > 0) { char r[24]; snprintf(r, sizeof r, " · %.1f kHz", now.info.sampleRate / 1000.0); f += r; }
        if (now.info.bitsPerSample > 0) f += " · " + std::to_string(now.info.bitsPerSample) + " bit";
        else if (now.info.bitrateKbps > 0) f += " · " + std::to_string(now.info.bitrateKbps) + " kbps";
        b += "| Playing | " + f + " |\n";
    }
    b += "| Library | " + thousands(lib.tracks.size()) + " tracks |\n";
    b += "{{CRASHES}}";
    if (sheet.repLog && !logLines_.empty()) {
        const size_t n = std::min<size_t>(40, logLines_.size());
        b += "\n### Songate's log (" + (n == 1 ? std::string("its last message") : "the last " + std::to_string(n) + " messages") + ")\n\n```\n";
        for (size_t i = logLines_.size() > 40 ? logLines_.size() - 40 : 0; i < logLines_.size(); i++) b += logLines_[i] + "\n";
        b += "```\n";
    }
    b += "\n*Sent from Songate's Settings › Support.*\n";
    return b;
}

void App::reportHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const bool bug = sheet.repKind == 0;
    icon = bug ? ICON_BUG : PLI_BULB;
    kick = "Support";
    title = bug ? "Report a bug" : "Suggest a feature";
    sub = report::available() ? "Sent to Songate's reports on GitHub, with what is listed below" : "This build cannot send reports";
}

void App::reportRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    const bool bug = sheet.repKind == 0;
    Row kind{ Row::Setting, nullptr, "It is" };
    kind.opts = { "A bug", "An idea" };
    kind.cur = sheet.repKind;
    kind.pick = [this](int p) { sheet.repKind = p; };
    kind.h = 56;
    rows.push_back(kind);
    rows.push_back({ Row::Name, nullptr, "Title", "", sheet.repTitle.empty() ? "Add a title" : sheet.repTitle, [this] {
                         std::string t;
                         if (keyboard("A short title", sheet.repTitle, t)) {
                             while (!t.empty() && t.back() == ' ') t.pop_back();
                             sheet.repTitle = t;
                         }
                     } });
    rows.back().h = 48;
    rows.push_back({ Row::Name, nullptr, bug ? "What happened" : "The idea", "", sheet.repWhat.empty() ? "Tell us" : sheet.repWhat, [this, bug] {
                         std::string t;
                         if (keyboard(bug ? "What happened, and what you expected" : "Your idea", sheet.repWhat, t)) sheet.repWhat = t;
                     } });
    rows.back().h = 48;
    if (bug) {
        rows.push_back({ Row::Name, nullptr, "Steps", "", sheet.repSteps.empty() ? "How to make it happen" : sheet.repSteps, [this] {
                             std::string t;
                             if (keyboard("The steps that make it happen", sheet.repSteps, t)) sheet.repSteps = t;
                         } });
        rows.back().h = 48;
        Row often{ Row::Setting, nullptr, "How often" };
        often.opts = { kOften[0], kOften[1], kOften[2] };
        often.cur = sheet.repOften;
        often.pick = [this](int p) { sheet.repOften = p; };
        often.h = 56;
        rows.push_back(often);
    }
    rows.push_back({ Row::Group, nullptr, "Which part of Songate" });
    rows.back().h = 38;
    for (int a = 0; a < kAreaCount; a++) {
        rows.push_back({ Row::Choice, nullptr, kAreas[a], "", "", [this, a] { sheet.repArea = a; } });
        rows.back().h = 42;
        rows.back().on = sheet.repArea == a;
    }
    rows.push_back({ Row::Group, nullptr, "Sent with it" });
    rows.back().h = 38;
    // the crash reports of the last 30 days (none: nothing to send)
    int recent = 0;
    if (supportAsked_ && !supportThread_.joinable())
        for (const auto& c : supportCrashes_) recent += c.at > (long long)std::time(nullptr) - 30LL * 86400 ? 1 : 0;
    Row crashes{ Row::Toggle, nullptr, "Crash reports",
                 recent ? std::to_string(recent) + " from Atmosphère in the last 30 days: when, which program, the error" : "None from Atmosphère in the last 30 days",
                 "", [this] { sheet.repCrashes = !sheet.repCrashes; } };
    crashes.on = sheet.repCrashes && recent > 0;
    if (!recent) crashes.act = nullptr;
    crashes.h = 62;
    rows.push_back(crashes);
    Row log{ Row::Toggle, nullptr, "Songate's log", "Its last 40 messages, the ones shown at the bottom of the screen", "",
             [this] { sheet.repLog = !sheet.repLog; } };
    log.on = sheet.repLog;
    log.h = 62;
    rows.push_back(log);
    Row text{ Row::Text, ICON_INFO, "", "Always: Songate's version, the firmware and Atmosphère's, the background player, the playing file's format "
                                       "and how many tracks you have. Never your files' names." };
    text.h = 2 * 19.f + 14;
    rows.push_back(text);
}

void App::reportFooter(float x, float w, float fy) {
    const Font bf{ css::Sm, Weight::SemiBold };
    const bool sending = reportThread_.joinable();
    const char* label = sending ? "Sending…" : "Send";
    const float sw = g.measure(label, bf) + 32 + 24, kw = g.measure("Cancel", bf) + 32;
    bool c = false;
    button(uid("rp.send"), ICON_CHECK, label, x + w - 24 - sw, fy + 7, true, &c, false, false, DefaultFocus, sw);
    if (c && !sending) {
        const double wait = 300 - (time - reportSentAt_);
        if (!report::available()) toast("This build cannot send reports");
        else if (!store.reports) { closeSheet(); allowReports(); }   // (switched off meanwhile: asked again)
        else if (sheet.repTitle.empty()) toast("Add a title first");
        else if (sheet.repArea < 0) toast("Choose the part of Songate it is about");
        else if (wait > 0) toast("One report every five minutes: try again in " + std::to_string(int(wait / 60) + 1) + " min");
        else {
            const bool bug = sheet.repKind == 0;
            const std::string title = std::string(bug ? "[Bug] " : "[Idea] ") + sheet.repTitle;
            const std::string body = reportBody();
            const bool withCrashes = sheet.repCrashes;
            reportDone_ = false;
            reportThread_ = std::thread([this, title, body, withCrashes, bug] {
                cores::moveHere(cores::Net);
                const report::System s = report::system();
                std::string b = body;
                const std::string sys = "| Firmware | " + s.firmware + " |\n| Atmosphère | " + s.atmosphere + " |\n| Mode | " +
                                        (s.docked ? "docked" : "handheld") + " |\n";
                b.replace(b.find("{{SYSTEM}}"), 10, sys);
                std::string cr;
                if (withCrashes) {
                    const long long since = (long long)std::time(nullptr) - 30LL * 86400;
                    for (const auto& c : report::crashes(10))
                        if (c.at > since) cr += "- " + c.when + " · " + c.program + " · " + c.result + "\n";
                    if (!cr.empty()) cr = "\n### Crash reports (Atmosphère, the last 30 days)\n\n" + cr;
                }
                b.replace(b.find("{{CRASHES}}"), 11, cr);
                reportOk_ = report::send(title, b, bug ? "bug" : "enhancement", reportNumber_, reportErr_);
                reportDone_ = true;
            });
        }
    }
    button(uid("rp.cancel"), nullptr, "Cancel", x + w - 24 - sw - 8 - kw, fy + 7, false, &c);
    if (c) closeSheet();
}

void App::crashRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    if (!supportAsked_ || supportThread_.joinable()) {
        rows.push_back({ Row::Text, ICON_HOURGLASS, "", "Reading them…" });
        rows.back().h = 19 + 14;
        return;
    }
    if (supportCrashes_.empty()) {
        rows.push_back({ Row::Text, ICON_CHECK, "", "None on this console: Atmosphère has not recorded a crash." });
        rows.back().h = 19 + 14;
        return;
    }
    for (const auto& c : supportCrashes_) {
        rows.push_back({ Row::Action, c.fatal ? ICON_X : ICON_BUG, c.program, c.when + "  ·  " + c.result, "", nullptr });
        rows.back().h = 58;
    }
}
