/*
 * Songate · mood.cpp
 * See mood.hpp. The numbers were tuned on the user's library (tools: mixtrain mood, then a script that sorted and listed it).
 */
#include "mood.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>


namespace mood {

const char* name(int m) {
    static const char* n[Count] = { "Happy", "Party", "Relaxed", "Sad", "Aggressive" };
    return m >= 0 && m < Count ? n[m] : "";
}

const char* desc(int m) {
    static const char* d[Count] = { "Bright, upbeat, in a major key", "A strong beat to dance to", "Soft and unhurried",
                                    "Slow, quiet and in a minor key", "Loud, harsh and driving" };
    return m >= 0 && m < Count ? d[m] : "";
}

namespace {

/** Each value's rank among the others, 0..1: the library's own spread. */
std::vector<float> ranks(const std::vector<float>& v) {
    std::vector<int> order(v.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return v[size_t(a)] < v[size_t(b)]; });   // (ties keep their order)
    std::vector<float> r(v.size());
    const float d = v.size() > 1 ? float(v.size() - 1) : 1.f;
    for (size_t p = 0; p < order.size(); p++) r[size_t(order[p])] = float(p) / d;
    return r;
}

/** How near a tempo is to a dancing one (100-136, best 118), folded by octaves; 0 without a steady beat. */
float tempoFit(float b) {
    if (b <= 0) return 0;
    while (b < 85) b *= 2;
    while (b > 170) b /= 2;
    return std::max(0.f, 1 - std::fabs(b - 118) / 40);
}

}  // namespace

void Index::build(const std::vector<const mixan::Mood*>& tracks) {
    best.assign(tracks.size(), -1);
    score.assign(tracks.size(), 0.f);
    known = 0;
    std::vector<int> idx;   // the tracks measured
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i] && tracks[i]->valid) idx.push_back(int(i));
    known = int(idx.size());
    if (idx.size() < 2) return;
    auto col = [&](auto get) {
        std::vector<float> v;
        v.reserve(idx.size());
        for (int i : idx) v.push_back(get(*tracks[size_t(i)]));
        return ranks(v);
    };
    const auto rLoud = col([](const mixan::Mood& m) { return m.loud; }), rDyn = col([](const mixan::Mood& m) { return m.dyn; }),
               rCalm = col([](const mixan::Mood& m) { return m.calm; }), rBright = col([](const mixan::Mood& m) { return m.bright; }),
               rHf = col([](const mixan::Mood& m) { return m.hf; }), rOnsets = col([](const mixan::Mood& m) { return m.onsets; }),
               rPerc = col([](const mixan::Mood& m) { return m.perc; }), rFlux = col([](const mixan::Mood& m) { return m.flux; }),
               rMinor = col([](const mixan::Mood& m) { return m.minor; }), rBass = col([](const mixan::Mood& m) { return m.bass; }),
               rBpm = col([](const mixan::Mood& m) { return m.bpm; }), rPulse = col([](const mixan::Mood& m) { return m.pulse; }),
               rKick = col([](const mixan::Mood& m) { return m.kickPulse; });
    // the five moods' raw scores, then each as a z-score over the library
    std::vector<float> raw[Count];
    for (auto& v : raw) v.resize(idx.size());
    for (size_t k = 0; k < idx.size(); k++) {
        const mixan::Mood& m = *tracks[size_t(idx[k])];
        const float fast = m.beat > 0.3f ? rBpm[k] : 0.3f;
        const float E = 0.22f * rOnsets[k] + 0.18f * rPerc[k] + 0.16f * rBright[k] + 0.12f * rFlux[k] + 0.12f * rLoud[k] +
                        0.12f * (1 - rCalm[k]) + 0.08f * fast;
        const float D = 0.40f * rPulse[k] + 0.25f * rKick[k] + 0.20f * tempoFit(m.bpm) + 0.15f * rBass[k];
        const float V = 0.50f * (1 - rMinor[k]) + 0.20f * rBright[k] + 0.30f * D;
        const float harsh = 0.35f * rHf[k] + 0.25f * rBright[k] + 0.2f * rFlux[k] + 0.2f * (1 - rDyn[k]);
        raw[Happy][k] = V * 0.55f + (1 - std::fabs(E - 0.6f)) * 0.3f + D * 0.15f;
        raw[Party][k] = D * 0.55f + E * 0.30f + rBass[k] * 0.15f;
        raw[Relaxed][k] = (1 - E) * 0.6f + V * 0.4f;
        raw[Sad][k] = (1 - E) * 0.45f + (1 - V) * 0.55f;
        raw[Aggressive][k] = E * 0.4f + harsh * 0.25f + (1 - V) * 0.25f + (1 - D) * 0.10f;
    }
    float mu[Count], sd[Count];
    for (int c = 0; c < Count; c++) {
        double s = 0, s2 = 0;
        for (float x : raw[c]) { s += x; s2 += double(x) * x; }
        mu[c] = float(s / double(idx.size()));
        sd[c] = float(std::sqrt(std::max(1e-12, s2 / double(idx.size()) - double(mu[c]) * mu[c])));
    }
    for (size_t k = 0; k < idx.size(); k++) {
        int arg = 0;
        float z = -1e9f;
        for (int c = 0; c < Count; c++) {
            const float zc = (raw[c][k] - mu[c]) / sd[c];
            if (zc > z) { z = zc; arg = c; }
        }
        best[size_t(idx[k])] = static_cast<signed char>(arg);
        score[size_t(idx[k])] = z;
    }
}

}  // namespace mood
