/*
 * Gatefold NX · bgclient.cpp
 * The IPC calls to the background player. The PC preview has no sysmodule: there, connect()
 * simply fails and Gatefold plays on its own, as it does on a console without the sysmodule.
 */
#include "bgclient.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <SDL2/SDL.h>
#include <switch.h>

#include "gfbg.h"

namespace {
Service srv;

/**
 * One clock for every timestamp here. Callers passed two: the app's frame clock (polls) and SDL's
 * (seeks, play/pause from Player). The app's starts at launch's end and stands still while Gatefold
 * waits on the HOME menu, so a seek's 0.3 s "trust what was asked for" lasted seconds, even minutes,
 * and the position read frozen: the lyrics stopped following after a tap on a line.
 */
double bgClock() { return SDL_GetTicks64() / 1000.0; }
}

bool BgClient::connect() {
    if (connected_) return true;
    connected_ = R_SUCCEEDED(gfbgOpen(&srv, GfbgClient_Gatefold));
    pcm_.assign(GFBG_PCM, 0.f);
    return connected_;
}

void BgClient::disconnect() {
    if (connected_) serviceClose(&srv);
    connected_ = false;
}

bool BgClient::poll(double) {
    const double now = bgClock();
    if (!connected_) return false;
    GfbgState s;
    if (R_FAILED(gfbgGetState(&srv, &s))) { disconnect(); return false; }
    st_.status = int(s.status);
    st_.pos = s.pos;
    st_.track = s.track;
    st_.trackGen = s.trackGen;
    st_.queueGen = s.queueGen;
    st_.errorGen = s.errorGen;
    st_.trackBy = s.trackBy;
    st_.queueBy = s.queueBy;
    st_.queueCount = s.queueCount;
    st_.queueBytes = s.queueBytes;
    st_.mode = int(s.mode);
    st_.stopAfter = s.stopAfter != 0;
    st_.autoPaused = s.autoPaused != 0;
    st_.position = s.position;
    st_.duration = s.duration;
    st_.volume = s.volume;
    st_.gain = s.gain;
    st_.sampleRate = int(s.sampleRate);
    st_.channels = int(s.channels);
    st_.codec = int(s.codec);
    st_.bitrate = int(s.bitrate);
    st_.appState = int(s.appState);
    st_.heapFree = s.heapFree;
    st_.sleepLeft = s.sleepLeft;
    st_.sleepGen = s.sleepGen;
    st_.eqGen = s.eqGen;
    st_.joinGen = s.joinGen;
    st_.joining = int(s.joining);
    st_.joinAt = s.joinAt;
    st_.joinLength = s.joinLength;
    st_.joinBpm = s.joinBpm;
    st_.joinFlags = s.joinFlags;
    if (now < expectUntil_) {
        if (expectStatus_ >= 0) st_.status = expectStatus_;
        if (expectPos_ >= 0) st_.position = expectPos_;
    } else {
        expectStatus_ = -1;
        expectPos_ = -1;
    }
    polledAt_ = now;
    return true;
}

double BgClient::position(double) const {
    const double now = bgClock();
    double p = st_.position;
    if (st_.status == BgState::Playing) p += std::clamp(now - polledAt_, 0.0, 0.25);
    return st_.duration > 0 ? std::min(p, st_.duration) : p;
}

bool BgClient::setQueue(const BgQueue& q) {
    if (!connected_) return false;
    size_t pathBytes = 0;
    for (const auto& p : q.paths) pathBytes += p.size() + 1;
    std::vector<u8> buf(sizeof(GfbgQueueHeader) + q.order.size() * 4 + pathBytes);
    GfbgQueueHeader h = {};
    h.count = u32(q.paths.size());
    h.pos = q.pos;
    h.mode = u32(q.mode);
    h.flags = GfbgQueue_KeepCurrent | (q.automix ? GfbgQueue_Automix : 0);
    h.pathBytes = u32(pathBytes);
    memcpy(h.name, q.name.c_str(), std::min(q.name.size(), sizeof h.name - 1));
    memcpy(buf.data(), &h, sizeof h);
    for (size_t i = 0; i < q.order.size(); i++) {
        const s32 o = q.order[i];
        memcpy(buf.data() + sizeof h + i * 4, &o, 4);
    }
    u8* p = buf.data() + sizeof h + q.order.size() * 4;
    for (const auto& s : q.paths) { memcpy(p, s.c_str(), s.size() + 1); p += s.size() + 1; }
    refused_ = false;
    if (const Result rc = gfbgSetQueue(&srv, buf.data(), buf.size()); R_FAILED(rc)) {
        if (rc == GFBG_QUEUE_TOO_BIG) refused_ = true;   // it kept its old queue and stays connected
        else disconnect();
        return false;
    }
    // What the player has now, until the next poll says so itself (Player::loaded reads these).
    st_.queueCount = h.count;
    st_.pos = q.pos;
    return true;
}

bool BgClient::getQueue(BgQueue& q) {
    if (!connected_ || st_.queueBytes < sizeof(GfbgQueueHeader)) return false;
    std::vector<u8> buf(st_.queueBytes + 4096);   // room in case it grew since the poll
    if (R_FAILED(gfbgGetQueue(&srv, buf.data(), buf.size()))) return false;
    GfbgQueueHeader h;
    memcpy(&h, buf.data(), sizeof h);
    if (sizeof h + size_t(h.count) * 4 + h.pathBytes > buf.size()) return false;
    q.name.assign(h.name, strnlen(h.name, sizeof h.name));
    q.pos = h.pos;
    q.mode = int(h.mode);
    q.order.resize(h.count);
    for (u32 i = 0; i < h.count; i++) {
        s32 o;
        memcpy(&o, buf.data() + sizeof h + i * 4, 4);
        q.order[i] = o;
    }
    q.paths.clear();
    const char* p = reinterpret_cast<const char*>(buf.data() + sizeof h + size_t(h.count) * 4);
    const char* end = p + h.pathBytes;
    while (p < end && q.paths.size() < h.count) {
        const size_t n = strnlen(p, size_t(end - p));
        q.paths.emplace_back(p, n);
        p += n + 1;
    }
    return q.paths.size() == h.count;
}

void BgClient::playAt(int pos, bool paused, double start) {
    if (!connected_) return;
    gfbgPlayAt(&srv, pos, paused, start);
    st_.pos = pos;
    expectStatus_ = paused ? BgState::Paused : BgState::Playing;
    expectPos_ = start;
    expectUntil_ = polledAt_ + 0.3;
}

void BgClient::toggle(double) {
    const double now = bgClock();
    if (!connected_) return;
    gfbgTransport(&srv, GfbgOp_Toggle);
    expectStatus_ = st_.status == BgState::Playing ? BgState::Paused : BgState::Playing;
    expectUntil_ = now + 0.3;
}

void BgClient::pause(double) {
    const double now = bgClock();
    if (!connected_) return;
    gfbgTransport(&srv, GfbgOp_Pause);
    expectStatus_ = BgState::Paused;
    expectUntil_ = now + 0.3;
}

void BgClient::stop() {
    if (connected_) gfbgTransport(&srv, GfbgOp_Stop);
}

void BgClient::seek(double t, double) {
    const double now = bgClock();
    if (!connected_) return;
    gfbgSeek(&srv, t);
    expectPos_ = t;
    expectUntil_ = now + 0.3;
    st_.position = t;
    polledAt_ = now;
}

void BgClient::setVolume(float v) {
    if (!connected_ || std::fabs(v - sentVolume_) < 0.0005f) return;
    sentVolume_ = v;
    gfbgSetVolume(&srv, v);
}

void BgClient::setStopAfter(bool on) {
    if (connected_) gfbgSetStopAfter(&srv, on);
}

void BgClient::setLevelMode(int mode) {
    if (!connected_) return;
    GfbgConfig c = {};
    if (R_FAILED(gfbgGetConfig(&srv, &c)) || c.reserved[0] == u32(mode)) return;
    c.reserved[0] = u32(mode);
    gfbgSetConfig(&srv, &c);
}

void BgClient::setSleep(float seconds) {
    if (connected_) gfbgSetSleep(&srv, seconds);
}

void BgClient::setCrossfade(float seconds, bool notInAlbum) {
    if (!connected_) return;
    GfbgConfig c = {};
    if (R_FAILED(gfbgGetConfig(&srv, &c))) return;
    const u32 flags = notInAlbum ? GfbgXfade_NotInAlbum : 0;
    if (c.crossfade == seconds && c.xfadeFlags == flags) return;
    c.crossfade = seconds;
    c.xfadeFlags = flags;
    gfbgSetConfig(&srv, &c);
}

void BgClient::setAutomixModel(int model, const std::string& file) {
    if (!connected_) return;
    GfbgConfig c = {};
    if (R_FAILED(gfbgGetConfig(&srv, &c))) return;
    if (c.automixModel == u32(model) && file == c.automixFile) return;
    c.automixModel = u32(model);
    snprintf(c.automixFile, sizeof c.automixFile, "%s", file.c_str());
    gfbgSetConfig(&srv, &c);
}

bool BgClient::eq(GfbgEq& out) { return connected_ && R_SUCCEEDED(gfbgGetEq(&srv, &out)); }

void BgClient::setEq(const GfbgEq& e) {
    if (connected_) gfbgSetEq(&srv, &e);
}

bool BgClient::pcm(float* out, int n) {
    if (!connected_ || n > GFBG_PCM) return false;
    u32 serial = 0;
    if (R_FAILED(gfbgGetPcm(&srv, pcm_.data(), &serial)) || serial == pcmSerial_) return false;
    pcmSerial_ = serial;
    memcpy(out, pcm_.data() + (GFBG_PCM - n), size_t(n) * sizeof(float));
    return true;
}

bool BgClient::config(BgConfig& out) {
    GfbgConfig c;
    if (!connected_ || R_FAILED(gfbgGetConfig(&srv, &c))) return false;
    out.onOpen = c.onOpen;
    out.onHome = c.onHome;
    out.onClose = c.onClose;
    out.onReturn = c.onReturn;
    out.fadeOut = c.fadeOut;
    out.fadeIn = c.fadeIn;
    for (int i = 0; i < 4; i++) out.lists[i] = c.lists[i];
    out.listStart = int(c.listStart);
    return true;
}

void BgClient::setConfig(const BgConfig& in) {
    if (!connected_) return;
    GfbgConfig c = {};
    if (R_FAILED(gfbgGetConfig(&srv, &c))) return;   // keeps what Gatefold does not show (the game volume)
    c.onOpen = u8(in.onOpen);
    c.onHome = u8(in.onHome);
    c.onClose = u8(in.onClose);
    c.onReturn = u8(in.onReturn);
    c.fadeOut = in.fadeOut;
    c.fadeIn = in.fadeIn;
    for (int i = 0; i < 4; i++) c.lists[i] = in.lists[i];
    c.listStart = u32(in.listStart);
    gfbgSetConfig(&srv, &c);
}

