#include "gt/diagnostics.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace gt;

TEST_CASE("VRAM errors are grouped into ranges per chunk", "[diag]") {
    std::vector<diag::MemError> e{
        {1, 1000, 0x1, 0x3}, {0, 10, 0xFF, 0xFE}, {0, 12, 0xF0, 0xF8}, {0, 100000, 0x0, 0x80000000}, {1, 1001, 0x1, 0x5}};
    const auto r = diag::errorRanges(e, 4096);
    REQUIRE(r.size() == 3);
    CHECK(r[0].chunk == 0);
    CHECK(r[0].firstByte == 40);
    CHECK(r[0].lastByte == 51);
    CHECK(r[0].count == 2);
    CHECK(r[0].flippedBits == (0x1u | 0x8u));
    CHECK(r[1].firstByte == 400000);
    CHECK(r[2].chunk == 1);
    CHECK(r[2].count == 2);
    CHECK(r[2].flippedBits == (0x2u | 0x4u));
}

TEST_CASE("flipped bits are described readably", "[diag]") {
    CHECK(diag::describeBits(0) == "no bits");
    CHECK(diag::describeBits(1u << 3) == "bit 3");
    CHECK(diag::describeBits((1u << 3) | (1u << 17)) == "bits 3, 17");
    CHECK(diag::describeBits(0xFFFF) == "16 different bits");
}

TEST_CASE("PCIe expected bandwidth", "[diag]") {
    CHECK(diag::pcieExpectedGBps(3, 16) == Approx(15.75).epsilon(0.01));
    CHECK(diag::pcieExpectedGBps(4, 16) == Approx(31.5).epsilon(0.01));
    CHECK(diag::pcieExpectedGBps(4, 8) == Approx(15.75).epsilon(0.01));
    CHECK(diag::pcieExpectedGBps(0, 16) == 0.0);
}

TEST_CASE("thermal soak analysis", "[diag]") {
    // 100 s: 10 ms frames for 50 s, then 12.5 ms frames (100 -> 80 fps).
    std::vector<double> frames;
    for (int i = 0; i < 5000; ++i) frames.push_back(10.0);
    for (int i = 0; i < 4000; ++i) frames.push_back(12.5);
    std::vector<results::TelemetrySample> samples;
    for (int i = 0; i <= 1000; ++i) {
        results::TelemetrySample s;
        s.t = 20.0 + i * 0.1; // telemetry time base differs from the frame base
        s.tempEdgeC = static_cast<float>(i < 400 ? 50.0 + i * 0.075 : 80.0);
        s.gpuClockMhz = i < 500 ? 2500.0f : 2250.0f;
        if (i >= 500) s.throttle = results::throttle::kThermal;
        samples.push_back(s);
    }
    const auto a = diag::analyzeSoak(frames, samples);
    CHECK(a.durationS == Approx(100.0));
    CHECK(a.firstFps == Approx(100.0));
    CHECK(a.lastFps == Approx(80.0));
    CHECK(a.perfDropPct == Approx(20.0));
    CHECK(a.hasSensors);
    CHECK(a.maxTempC == Approx(80.0));
    CHECK(a.plateaued);
    CHECK(a.plateauTempC == Approx(80.0));
    CHECK(a.timeToPlateauS == Approx(38.7).margin(0.2)); // 50 + 0.075 i reaches 79 °C at i = 387
    CHECK(a.clockDropPct == Approx(10.0));
    CHECK(a.throttlePct.at("thermal") == Approx(50.0).margin(0.2));
}

TEST_CASE("soak without sensors still reports performance", "[diag]") {
    std::vector<double> frames(1000, 16.0);
    const auto a = diag::analyzeSoak(frames, {});
    CHECK_FALSE(a.hasSensors);
    CHECK(a.perfDropPct == Approx(0.0).margin(1e-9));
}
