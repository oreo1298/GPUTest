#include "gt/stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gt {

double percentileSorted(std::span<const double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    p = std::clamp(p, 0.0, 100.0);
    const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

namespace {

// Mean of the slowest `fraction` of samples (at least one sample).
double meanOfSlowest(std::span<const double> sortedAsc, double fraction) {
    const std::size_t n = sortedAsc.size();
    auto k = static_cast<std::size_t>(std::ceil(static_cast<double>(n) * fraction));
    k = std::clamp<std::size_t>(k, 1, n);
    double sum = 0.0;
    for (std::size_t i = n - k; i < n; ++i) sum += sortedAsc[i];
    return sum / static_cast<double>(k);
}

double fpsFromMs(double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; }

} // namespace

FrameStats computeFrameStats(std::span<const double> frameTimesMs) {
    FrameStats s;
    s.frames = frameTimesMs.size();
    if (frameTimesMs.empty()) return s;

    std::vector<double> sorted(frameTimesMs.begin(), frameTimesMs.end());
    std::sort(sorted.begin(), sorted.end());

    s.totalMs = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    s.meanMs = s.totalMs / static_cast<double>(s.frames);
    s.minMs = sorted.front();
    s.maxMs = sorted.back();
    double var = 0.0;
    for (double v : sorted) var += (v - s.meanMs) * (v - s.meanMs);
    s.stddevMs = s.frames > 1 ? std::sqrt(var / static_cast<double>(s.frames - 1)) : 0.0;

    s.avgFps = s.totalMs > 0.0 ? 1000.0 * static_cast<double>(s.frames) / s.totalMs : 0.0;
    s.minFps = fpsFromMs(s.maxMs);
    s.maxFps = fpsFromMs(s.minMs);
    s.low1Fps = fpsFromMs(meanOfSlowest(sorted, 0.01));
    s.low01Fps = fpsFromMs(meanOfSlowest(sorted, 0.001));
    s.p50Ms = percentileSorted(sorted, 50.0);
    s.p90Ms = percentileSorted(sorted, 90.0);
    s.p95Ms = percentileSorted(sorted, 95.0);
    s.p99Ms = percentileSorted(sorted, 99.0);
    s.p999Ms = percentileSorted(sorted, 99.9);
    s.stutterCount = countStutters(frameTimesMs);
    return s;
}

std::size_t countStutters(std::span<const double> frameTimesMs, std::size_t window, double ratio,
                          double minExtraMs) {
    if (window == 0 || frameTimesMs.size() <= window) return 0;
    std::size_t count = 0;
    std::vector<double> buf(window);
    for (std::size_t i = window; i < frameTimesMs.size(); ++i) {
        std::copy(frameTimesMs.begin() + static_cast<std::ptrdiff_t>(i - window),
                  frameTimesMs.begin() + static_cast<std::ptrdiff_t>(i), buf.begin());
        auto mid = buf.begin() + static_cast<std::ptrdiff_t>(window / 2);
        std::nth_element(buf.begin(), mid, buf.end());
        const double median = *mid;
        const double t = frameTimesMs[i];
        if (t > median * ratio && t > median + minExtraMs) ++count;
    }
    return count;
}

void RunningStats::add(double v) {
    ++m_n;
    if (m_n == 1) {
        m_min = m_max = m_mean = v;
        m_m2 = 0.0;
        return;
    }
    m_min = std::min(m_min, v);
    m_max = std::max(m_max, v);
    const double delta = v - m_mean;
    m_mean += delta / static_cast<double>(m_n);
    m_m2 += delta * (v - m_mean);
}

double RunningStats::stddev() const { return std::sqrt(variance()); }

double geometricMean(std::span<const double> values) {
    double logSum = 0.0;
    std::size_t n = 0;
    for (double v : values) {
        if (v > 0.0 && std::isfinite(v)) {
            logSum += std::log(v);
            ++n;
        }
    }
    return n ? std::exp(logSum / static_cast<double>(n)) : 0.0;
}

double linearSlope(std::span<const double> x, std::span<const double> y) {
    const std::size_t n = std::min(x.size(), y.size());
    if (n < 2) return 0.0;
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        num += (x[i] - mx) * (y[i] - my);
        den += (x[i] - mx) * (x[i] - mx);
    }
    return den > 0.0 ? num / den : 0.0;
}

} // namespace gt
