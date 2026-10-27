#include "gt/results.h"
#include "gt/scoring.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

using Catch::Approx;
using namespace gt;

namespace {

results::RunResult sampleRun() {
    results::RunResult r;
    r.appName = "Test";
    r.appVersion = "1.0";
    r.createdUtc = "2026-01-01T00:00:00Z";
    r.config.suite = "standard";
    r.config.api = "vulkan";
    r.config.width = 1920;
    r.config.height = 1080;
    r.config.quality = "high";
    r.config.duration = "standard";
    r.system.gpu.name = "Example GPU";
    r.system.gpu.vendorId = 0x10DE;
    r.system.gpu.deviceId = 0x2684;
    r.system.host.cpu = "Example CPU";
    r.status = results::RunStatus::Completed;
    for (int loop = 0; loop < 2; ++loop) {
        results::TestResult t;
        t.id = "scene.example";
        t.name = "Example";
        t.category = "scene";
        t.loop = loop;
        t.scored = true;
        t.score = loop == 0 ? 1000.0 : 900.0;
        t.primaryMetric = "avg_fps";
        t.metrics.push_back({"avg_fps", "Average FPS", 120.0, "fps", true});
        t.frameTimesMs = {8.0f, 8.5f, 9.0f};
        t.frameStats = computeFrameStats(std::vector<double>{8.0, 8.5, 9.0});
        t.startS = loop * 10.0;
        t.durationS = 9.0;
        t.details["instances"] = 42;
        r.tests.push_back(t);
        r.loops.push_back({loop, loop * 10.0, 10.0, 0.0, true});
    }
    results::TelemetrySample s;
    s.t = 1.0;
    s.gpuClockMhz = 2500;
    s.tempEdgeC = 60;
    s.powerW = 300;
    s.throttle = results::throttle::kPower;
    r.telemetry.samples.push_back(s);
    s.t = 1.1;
    r.telemetry.samples.push_back(s);
    s.t = 1.2;
    s.throttle = 0;
    r.telemetry.samples.push_back(s);
    r.telemetry.intervalS = 0.1;
    return r;
}

} // namespace

TEST_CASE("results JSON round trip", "[results]") {
    results::RunResult r = sampleRun();
    scoring::computeScores(r);
    const auto j = results::toJson(r);
    CHECK(j["schema"] == results::kSchemaId);
    CHECK(j["schema_version"] == results::kSchemaVersion);
    const results::RunResult back = results::fromJson(j);
    CHECK(back.config.suite == "standard");
    CHECK(back.system.gpu.vendorId == 0x10DE);
    CHECK(back.system.gpu.deviceId == 0x2684);
    REQUIRE(back.tests.size() == 2);
    CHECK(back.tests[0].id == "scene.example");
    CHECK(back.tests[0].scored);
    CHECK(back.tests[0].score == Approx(1000.0));
    REQUIRE(back.tests[0].frameStats);
    CHECK(back.tests[0].frameStats->frames == 3);
    CHECK(back.tests[0].frameTimesMs.size() == 3);
    CHECK(back.tests[0].details["instances"] == 42);
    CHECK(back.telemetry.samples.size() == 3);
    CHECK(back.telemetry.samples[0].gpuClockMhz == Approx(2500));
    CHECK(std::isnan(back.telemetry.samples[0].fanRpm));
    CHECK(back.telemetry.samples[0].throttle == results::throttle::kPower);
    CHECK(back.status == results::RunStatus::Completed);
}

TEST_CASE("results reader rejects foreign files", "[results]") {
    CHECK_THROWS(results::fromJson(nlohmann::json{{"hello", 1}}));
    nlohmann::json j = results::toJson(sampleRun());
    j["schema_version"] = 999;
    CHECK_THROWS(results::fromJson(j));
}

TEST_CASE("atomic results file write and read", "[results]") {
    const auto dir = std::filesystem::temp_directory_path() / "gt-results-test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "r.json").string();
    std::string err;
    REQUIRE(results::writeFile(sampleRun(), path, &err));
    auto back = results::readFile(path, &err);
    REQUIRE(back);
    CHECK(back->tests.size() == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("telemetry summary and throttle events", "[results]") {
    results::RunResult r = sampleRun();
    r.telemetry.computeSummary();
    REQUIRE(r.telemetry.summary.count("gpu_clock_mhz"));
    CHECK(r.telemetry.summary["gpu_clock_mhz"].avg == Approx(2500));
    CHECK(r.telemetry.summary.count("fan_rpm") == 0);
    REQUIRE(r.telemetry.throttleEvents.size() == 1);
    CHECK(r.telemetry.throttleEvents[0].start == Approx(1.0));
    CHECK(r.telemetry.throttleEvents[0].end == Approx(1.2));
    CHECK(r.telemetry.throttleEvents[0].reasons == results::throttle::kPower);
}

TEST_CASE("scoring", "[scoring]") {
    CHECK(scoring::testScore(120, 60, true) == Approx(2000));
    CHECK(scoring::testScore(5, 10, false) == Approx(2000));
    CHECK(scoring::testScore(0, 10, true) == 0);
    results::RunResult r = sampleRun();
    scoring::computeScores(r, 0.95);
    // one test averaged over loops: (1000 + 900) / 2
    CHECK(r.scores.subscores["scene"] == Approx(950));
    CHECK(r.scores.overall == Approx(950));
    REQUIRE(r.loops.size() == 2);
    CHECK(r.loops[0].score == Approx(1000));
    CHECK(r.loops[1].score == Approx(900));
    CHECK(r.stability.valid);
    CHECK(r.stability.ratio == Approx(0.9));
    CHECK_FALSE(r.stability.passed);
    CHECK(r.scores.comparableKey == "standard|vulkan|1920x1080|high|standard|off|r2");
    CHECK(r.scores.perWatt == Approx(950.0 / 300.0));
}

TEST_CASE("throttle names round trip", "[results]") {
    const std::uint32_t m = results::throttle::kThermal | results::throttle::kPower;
    CHECK(results::throttle::fromNames(results::throttle::names(m)) == m);
}

TEST_CASE("device losses round trip", "[results]") {
    results::RunResult r = sampleRun();
    results::DeviceLoss d;
    d.test = "scene.lattice";
    d.loop = 2;
    d.t = 12.5;
    d.message = "vkQueueSubmit: VK_ERROR_DEVICE_LOST";
    d.recovered = true;
    d.faultReportSupported = true;
    d.faultDescription = "page fault";
    d.faultDetails = {"invalid read at 0x1000 (±0x0)"};
    results::TelemetrySample s;
    s.t = 12.4;
    s.tempEdgeC = 83.0f;
    s.throttle = results::throttle::kThermal;
    d.lastSample = s;
    r.deviceLosses.push_back(d);
    const results::RunResult back = results::fromJson(results::toJson(r));
    REQUIRE(back.deviceLosses.size() == 1);
    const auto& b = back.deviceLosses.front();
    CHECK(b.test == d.test);
    CHECK(b.loop == 2);
    CHECK(b.t == Approx(12.5));
    CHECK(b.recovered);
    CHECK(b.faultReportSupported);
    CHECK(b.faultDescription == "page fault");
    REQUIRE(b.faultDetails.size() == 1);
    REQUIRE(b.lastSample.has_value());
    CHECK(b.lastSample->tempEdgeC == Approx(83.0));
    CHECK(b.lastSample->throttle == results::throttle::kThermal);
    // Files written before the field existed still load.
    auto j = results::toJson(sampleRun());
    j.erase("device_losses");
    CHECK(results::fromJson(j).deviceLosses.empty());
}
