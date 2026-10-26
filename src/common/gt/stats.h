#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace gt {

/// Percentile of an already *sorted* sample using linear interpolation between
/// closest ranks (the "R-7" / Excel PERCENTILE.INC definition). p in [0, 100].
double percentileSorted(std::span<const double> sorted, double p);

/// Summary statistics over a sequence of frame times (milliseconds).
/// Definitions are documented in docs/SCORING.md.
struct FrameStats {
    std::size_t frames = 0;
    double totalMs = 0.0;
    double avgFps = 0.0;      ///< frames / total time
    double minFps = 0.0;      ///< from the slowest frame
    double maxFps = 0.0;      ///< from the fastest frame
    double low1Fps = 0.0;     ///< 1000 / mean of the slowest 1% of frame times
    double low01Fps = 0.0;    ///< 1000 / mean of the slowest 0.1% of frame times
    double meanMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double stddevMs = 0.0;
    double p50Ms = 0.0;
    double p90Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double p999Ms = 0.0;
    std::size_t stutterCount = 0; ///< see countStutters()
};

FrameStats computeFrameStats(std::span<const double> frameTimesMs);

/// A frame counts as a stutter when it is slower than both `ratio` x the median
/// of the preceding `window` frames and that median + `minExtraMs`.
std::size_t countStutters(std::span<const double> frameTimesMs, std::size_t window = 32, double ratio = 2.0,
                          double minExtraMs = 4.0);

/// Running mean / min / max / count without storing samples (Welford variance).
class RunningStats {
public:
    void add(double v);
    void reset() { *this = RunningStats{}; }
    std::size_t count() const { return m_n; }
    double mean() const { return m_n ? m_mean : 0.0; }
    double min() const { return m_n ? m_min : 0.0; }
    double max() const { return m_n ? m_max : 0.0; }
    double variance() const { return m_n > 1 ? m_m2 / static_cast<double>(m_n - 1) : 0.0; }
    double stddev() const;

private:
    std::size_t m_n = 0;
    double m_mean = 0.0;
    double m_m2 = 0.0;
    double m_min = 0.0;
    double m_max = 0.0;
};

/// Geometric mean of strictly positive values (non-positive values are ignored).
double geometricMean(std::span<const double> values);

/// Least-squares slope of y over x (0 when fewer than two points).
double linearSlope(std::span<const double> x, std::span<const double> y);

/// Fixed-width histogram. Values below `lo` / at or above `lo + width * bins` are
/// counted in `under` / `over` so no sample is silently dropped.
struct Histogram {
    double lo = 0.0;
    double binWidth = 1.0;
    std::vector<std::size_t> counts;
    std::size_t under = 0;
    std::size_t over = 0;
    std::size_t total = 0;
    double binStart(std::size_t i) const { return lo + binWidth * static_cast<double>(i); }
};

Histogram histogram(std::span<const double> values, double lo, double binWidth, std::size_t bins);

/// Histogram of frame times with a "nice" bin width (1/2/2.5/5 x 10^n) chosen so
/// the 0.5th..99.9th percentile range spans about `targetBins` bins. Outliers land
/// in `over`/`under`.
Histogram frameTimeHistogram(std::span<const double> frameTimesMs, std::size_t targetBins = 40);

} // namespace gt
