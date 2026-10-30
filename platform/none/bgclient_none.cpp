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

bool BgClient::connect() { return false; }
void BgClient::disconnect() {}
bool BgClient::poll(double) { return false; }
double BgClient::position(double) const { return 0; }
bool BgClient::setQueue(const BgQueue&) { return false; }
void BgClient::setAutomixModel(int, const std::string&) {}
bool BgClient::getQueue(BgQueue&) { return false; }
void BgClient::playAt(int, bool, double) {}
void BgClient::toggle(double) {}
void BgClient::pause(double) {}
void BgClient::stop() {}
void BgClient::seek(double, double) {}
void BgClient::setVolume(float) {}
void BgClient::setStopAfter(bool) {}
void BgClient::setLevelMode(int) {}
void BgClient::setSleep(float) {}
void BgClient::setCrossfade(float, bool) {}
bool BgClient::eq(GfbgEq&) { return false; }
void BgClient::setEq(const GfbgEq&) {}
bool BgClient::pcm(float*, int) { return false; }
bool BgClient::config(BgConfig&) { return false; }
void BgClient::setConfig(const BgConfig&) {}
