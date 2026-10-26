#include "gt/diagnostics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace gt::diag {

std::vector<ErrorRange> errorRanges(std::vector<MemError> errors, std::uint64_t gapBytes) {
    std::sort(errors.begin(), errors.end(), [](const MemError& a, const MemError& b) {
        return a.chunk != b.chunk ? a.chunk < b.chunk : a.word < b.word;
    });
    std::vector<ErrorRange> out;
    for (const MemError& e : errors) {
        const std::uint64_t byte = e.word * 4;
        if (!out.empty() && out.back().chunk == e.chunk && byte <= out.back().lastByte + gapBytes) {
            ErrorRange& r = out.back();
            r.lastByte = std::max(r.lastByte, byte + 3);
            ++r.count;
            r.flippedBits |= e.expected ^ e.actual;
        } else {
            out.push_back({e.chunk, byte, byte + 3, 1, e.expected ^ e.actual});
        }
    }
    return out;
}

std::string describeBits(std::uint32_t mask) {
    const int n = std::popcount(mask);
    if (n == 0) return "no bits";
    if (n > 4) return std::format("{} different bits", n);
    std::string s = n == 1 ? "bit " : "bits ";
    bool first = true;
    for (int b = 0; b < 32; ++b)
        if (mask & (1u << b)) {
            s += (first ? "" : ", ") + std::to_string(b);
            first = false;
        }
    return s;
}

double pcieLaneGBps(int gen) {
    switch (gen) {
    case 1: return 2.5 * 0.8 / 8.0;            // 8b/10b
    case 2: return 5.0 * 0.8 / 8.0;
    case 3: return 8.0 * 128.0 / 130.0 / 8.0;  // 128b/130b
    case 4: return 16.0 * 128.0 / 130.0 / 8.0;
    case 5: return 32.0 * 128.0 / 130.0 / 8.0;
    case 6: return 64.0 * 242.0 / 256.0 / 8.0; // PAM4 + FLIT (FEC/CRC overhead)
    case 7: return 128.0 * 242.0 / 256.0 / 8.0;
    default: return 0.0;
    }
}

double pcieExpectedGBps(int gen, int width) { return pcieLaneGBps(gen) * std::max(0, width); }

SoakAnalysis analyzeSoak(std::span<const double> frameMs, std::span<const results::TelemetrySample> samples) {
    SoakAnalysis a;
    double total = 0.0;
    for (double f : frameMs) total += f;
    a.durationS = total / 1000.0;
    // FPS over a time window [from, to) seconds into the soak.
    auto fpsBetween = [&](double from, double to) {
        double t = 0.0, sum = 0.0;
        std::size_t n = 0;
        for (double f : frameMs) {
            const double mid = t + f / 2000.0;
            t += f / 1000.0;
            if (mid < from || mid >= to) continue;
            sum += f;
            ++n;
        }
        return sum > 0.0 ? 1000.0 * static_cast<double>(n) / sum : 0.0;
    };
    const double head = std::min(a.durationS * 0.5, std::max(a.durationS * 0.1, 5.0));
    const double tail = std::min(a.durationS * 0.5, std::max(a.durationS * 0.3, 5.0));
    a.firstFps = fpsBetween(0.0, head);
    a.lastFps = fpsBetween(a.durationS - tail, a.durationS + 1.0);
    if (a.firstFps > 0.0) a.perfDropPct = (a.firstFps - a.lastFps) / a.firstFps * 100.0;

    std::vector<const results::TelemetrySample*> withTemp;
    for (const auto& s : samples)
        if (std::isfinite(s.tempEdgeC)) withTemp.push_back(&s);
    if (samples.empty()) return a;
    const double t0 = samples.front().t, t1 = samples.back().t;
    const double span = std::max(t1 - t0, 1e-9);
    // Throttle shares (idle is not a limitation).
    std::map<std::string, std::size_t> counts;
    for (const auto& s : samples)
        for (const auto& n : results::throttle::names(s.throttle & ~results::throttle::kIdle)) ++counts[n];
    for (const auto& [n, c] : counts) a.throttlePct[n] = 100.0 * static_cast<double>(c) / static_cast<double>(samples.size());
    // Clocks: first 10 % vs last 30 % of the samples.
    auto avgOver = [&](double from, double to, auto get) {
        double sum = 0.0;
        std::size_t n = 0;
        for (const auto& s : samples) {
            const double v = get(s);
            if (s.t < from || s.t > to || !std::isfinite(v)) continue;
            sum += v;
            ++n;
        }
        return n ? sum / static_cast<double>(n) : std::nan("");
    };
    auto clock = [](const results::TelemetrySample& s) { return static_cast<double>(s.gpuClockMhz); };
    const double c0 = avgOver(t0, t0 + span * 0.1, clock), c1 = avgOver(t1 - span * 0.3, t1, clock);
    if (std::isfinite(c0) && std::isfinite(c1) && c0 > 0) {
        a.firstClockMhz = c0;
        a.lastClockMhz = c1;
        a.clockDropPct = (c0 - c1) / c0 * 100.0;
        a.hasSensors = true;
    }
    if (withTemp.empty()) return a;
    a.hasSensors = true;
    for (const auto* s : withTemp) {
        a.maxTempC = std::max(a.maxTempC, static_cast<double>(s->tempEdgeC));
        if (std::isfinite(s->tempHotspotC)) a.maxHotspotC = std::max(a.maxHotspotC, static_cast<double>(s->tempHotspotC));
    }
    a.plateauTempC = avgOver(t1 - span * 0.3, t1, [](const results::TelemetrySample& s) { return static_cast<double>(s.tempEdgeC); });
    // A plateau needs the last 30 % to stay within 2 °C; otherwise the GPU was still heating up.
    double lo = 1e9, hi = -1e9;
    for (const auto* s : withTemp)
        if (s->t >= t1 - span * 0.3) {
            lo = std::min(lo, static_cast<double>(s->tempEdgeC));
            hi = std::max(hi, static_cast<double>(s->tempEdgeC));
        }
    a.plateaued = hi - lo <= 2.0;
    if (a.plateaued)
        for (const auto* s : withTemp)
            if (std::abs(s->tempEdgeC - a.plateauTempC) <= 1.0) {
                a.timeToPlateauS = s->t - t0;
                break;
            }
    return a;
}

std::vector<LatencyLevel> latencyLevels(std::span<const std::uint64_t> sizes, std::span<const double> ns, double jump) {
    std::vector<LatencyLevel> out;
    const std::size_t n = std::min(sizes.size(), ns.size());
    std::vector<double> plateau;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (!plateau.empty() && ns[i] > median(plateau) * (1.0 + jump)) {
            out.push_back({sizes[i - 1], median(plateau)});
            plateau.clear();
        }
        plateau.push_back(ns[i]);
    }
    if (!plateau.empty()) out.push_back({sizes[n - 1], median(plateau)});
    // A level made of a single transitional point between two plateaus is not a cache.
    std::vector<LatencyLevel> merged;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const bool single = i + 1 < out.size() && i > 0 && out[i].upToBytes == out[i - 1].upToBytes * 2;
        if (single && out.size() > 2) continue;
        merged.push_back(out[i]);
    }
    return merged;
}

} // namespace gt::diag
