#include "gt/stats.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;

TEST_CASE("percentileSorted interpolates between ranks", "[stats]") {
    std::vector<double> v{1, 2, 3, 4, 5};
    CHECK(gt::percentileSorted(v, 0) == Approx(1));
    CHECK(gt::percentileSorted(v, 50) == Approx(3));
    CHECK(gt::percentileSorted(v, 100) == Approx(5));
    CHECK(gt::percentileSorted(v, 25) == Approx(2));
    CHECK(gt::percentileSorted(v, 90) == Approx(4.6));
    CHECK(gt::percentileSorted(std::vector<double>{}, 50) == 0.0);
    CHECK(gt::percentileSorted(std::vector<double>{7.0}, 99) == 7.0);
}

TEST_CASE("frame stats on a constant sequence", "[stats]") {
    std::vector<double> v(1000, 10.0);
    auto s = gt::computeFrameStats(v);
    CHECK(s.frames == 1000);
    CHECK(s.avgFps == Approx(100.0));
    CHECK(s.minFps == Approx(100.0));
    CHECK(s.maxFps == Approx(100.0));
    CHECK(s.low1Fps == Approx(100.0));
    CHECK(s.low01Fps == Approx(100.0));
    CHECK(s.stddevMs == Approx(0.0).margin(1e-12));
    CHECK(s.p99Ms == Approx(10.0));
    CHECK(s.stutterCount == 0);
}

TEST_CASE("1% and 0.1% lows use the slowest frames", "[stats]") {
    std::vector<double> v(1000, 10.0);
    v[100] = 50.0; // slowest single frame
    for (int i = 0; i < 9; ++i) v[200 + i * 10] = 20.0;
    auto s = gt::computeFrameStats(v);
    // slowest 1% = 10 frames: one 50 ms + nine 20 ms -> mean 23 ms
    CHECK(s.low1Fps == Approx(1000.0 / 23.0));
    // slowest 0.1% = 1 frame
    CHECK(s.low01Fps == Approx(20.0));
    CHECK(s.minFps == Approx(20.0));
    CHECK(s.maxMs == Approx(50.0));
    CHECK(s.avgFps == Approx(1000.0 * 1000.0 / (990 * 10.0 + 50.0 + 9 * 20.0)));
}

TEST_CASE("stutter detection needs both ratio and absolute margin", "[stats]") {
    std::vector<double> v(200, 10.0);
    v[100] = 30.0;  // 3x median and +20 ms -> stutter
    v[150] = 13.0;  // not 2x
    CHECK(gt::countStutters(v) == 1);
    std::vector<double> fast(200, 1.0);
    fast[100] = 3.0; // 3x but only +2 ms -> not a stutter
    CHECK(gt::countStutters(fast) == 0);
}

TEST_CASE("running stats match batch stats", "[stats]") {
    gt::RunningStats rs;
    std::vector<double> v{3, 1, 4, 1, 5, 9, 2, 6};
    for (double x : v) rs.add(x);
    CHECK(rs.count() == v.size());
    CHECK(rs.min() == 1);
    CHECK(rs.max() == 9);
    CHECK(rs.mean() == Approx(31.0 / 8.0));
    auto s = gt::computeFrameStats(v);
    CHECK(rs.stddev() == Approx(s.stddevMs));
}

TEST_CASE("geometric mean and slope", "[stats]") {
    CHECK(gt::geometricMean(std::vector<double>{1, 100}) == Approx(10));
    CHECK(gt::geometricMean(std::vector<double>{2, 0, -1, 8}) == Approx(4)); // non-positive ignored
    CHECK(gt::geometricMean(std::vector<double>{}) == 0);
    std::vector<double> x{0, 1, 2, 3}, y{1, 3, 5, 7};
    CHECK(gt::linearSlope(x, y) == Approx(2.0));
}

TEST_CASE("histogram counts every finite sample once", "[stats]") {
    const std::vector<double> v{0.5, 1.0, 1.5, 2.0, 2.99, 3.0, 10.0, -1.0, std::nan("")};
    const auto h = gt::histogram(v, 1.0, 1.0, 2);
    REQUIRE(h.counts.size() == 2);
    CHECK(h.counts[0] == 2); // 1.0, 1.5
    CHECK(h.counts[1] == 2); // 2.0, 2.99
    CHECK(h.under == 2);     // 0.5, -1.0
    CHECK(h.over == 2);      // 3.0, 10.0
    CHECK(h.total == 8);
}

TEST_CASE("frame time histogram picks a nice bin width", "[stats]") {
    std::vector<double> v;
    for (int i = 0; i < 1000; ++i) v.push_back(8.0 + 0.004 * i); // 8..12 ms
    const auto h = gt::frameTimeHistogram(v, 40);
    CHECK(h.binWidth == Catch::Approx(0.1));
    CHECK(h.lo <= 8.02);
    std::size_t sum = h.under + h.over;
    for (auto c : h.counts) sum += c;
    CHECK(sum == v.size());
    CHECK(h.counts.size() >= 38);
    CHECK(h.counts.size() <= 42);
}
