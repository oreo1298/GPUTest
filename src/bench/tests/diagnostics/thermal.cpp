// Thermal soak: sustained heavy load for minutes, with periodic bit-exact reference
// checks, then an analysis of how the card coped: performance at the start vs the end,
// where the temperature levelled off and how fast, clock drop, and why it throttled
// (power limit = normal under full load; thermal / hardware slowdown = cooling problem).

#include "gt/bench/context.h"
#include "gt/bench/test.h"
#include "gt/diagnostics.h"
#include "gt/duration.h"
#include "stress_common.h"

#include <format>

namespace gt::bench {
namespace {

class ThermalSoakTest : public Test {
public:
    ThermalSoakTest() {
        m_info.id = "diag.thermal";
        m_info.name = "Thermal soak";
        m_info.description = "Keeps the GPU under heavy load for several minutes (5 at Standard, 20 at Extreme), checks "
                             "rendering stays exact, and analyses temperatures, clocks, throttling and how much "
                             "performance drops once the card is hot.";
        m_info.category = Category::Diagnostic;
        m_info.order = 50;
        m_info.group = "Stability";
        m_info.primaryMetric = "perf_drop_pct";
        m_info.primaryUnit = "%";
        m_info.higherIsBetter = false;
        m_info.tags = {"stress", "thermal"};
        m_info.suites = {"diagnostics", "full"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        const auto& s = ctx.settings();
        double soakS = 300.0;
        switch (s.duration) {
        case DurationPreset::Smoke: soakS = 3.0; break;
        case DurationPreset::Quick: soakS = 60.0; break;
        case DurationPreset::Standard: soakS = 300.0; break;
        case DurationPreset::Extreme: soakS = 1200.0; break;
        }
        if (const std::string p = s.param("diag.soak_s"); !p.empty()) soakS = std::stod(p);

        diagnostics::StressRenderer stress(ctx, s.width, s.height);
        stress.captureReference();
        std::uint32_t frames = 0;
        FrameLoopDesc fl;
        fl.framesPerPass = 60; // short passes so the soak ends close to the requested time
        fl.simDuration = 3.0;
        fl.output = stress.output();
        fl.minSeconds = soakS;
        fl.maxSeconds = soakS + 120.0;
        fl.record = [&](FrameContext& f) {
            stress.recordFrame(f.cmd, static_cast<float>(f.simTime));
            if (!f.warmup && ++frames % 30 == 0) stress.recordCheck(f.cmd);
        };
        const FrameLoopResult r = ctx.runFrames(fl);
        ctx.setFrameResult(r);
        const auto checks = stress.readChecks();
        const auto samples = ctx.telemetrySamples ? ctx.telemetrySamples() : std::vector<results::TelemetrySample>{};
        const diag::SoakAnalysis a = diag::analyzeSoak(r.frameMs, samples);

        auto& d = ctx.details();
        d["soak_s"] = a.durationS;
        d["first_fps"] = a.firstFps;
        d["last_fps"] = a.lastFps;
        d["checks"] = checks.checks;
        d["failed_checks"] = checks.failedChecks;
        d["sensors"] = a.hasSensors;
        ctx.addMetric("perf_drop_pct", "Performance drop (start → end)", a.perfDropPct, "%", false);
        ctx.addMetric("failed_checks", "Reference frames with errors", checks.failedChecks, "frames", false);
        if (a.hasSensors) {
            if (a.maxTempC > 0) ctx.addMetric("max_temp_c", "Peak GPU temperature", a.maxTempC, "°C", false);
            if (a.maxHotspotC > 0) ctx.addMetric("max_hotspot_c", "Peak hotspot temperature", a.maxHotspotC, "°C", false);
            if (a.plateauTempC > 0) ctx.addMetric("plateau_temp_c", "Settled GPU temperature", a.plateauTempC, "°C", false);
            if (a.timeToPlateauS >= 0) ctx.addMetric("time_to_plateau_s", "Time to settle", a.timeToPlateauS, "s", false);
            if (a.firstClockMhz > 0) ctx.addMetric("clock_drop_pct", "GPU clock drop (start → end)", a.clockDropPct, "%", false);
            for (const auto& [reason, pct] : a.throttlePct)
                ctx.addMetric("throttle_" + reason + "_pct", "Throttled: " + reason, pct, "% of time", false);
        }

        // Verdict.
        auto pct = [&](const char* k) {
            auto it = a.throttlePct.find(k);
            return it == a.throttlePct.end() ? 0.0 : it->second;
        };
        const double thermalPct = pct("thermal") + pct("memory_thermal");
        std::vector<std::string> findings;
        results::Verdict v = results::Verdict::Pass;
        auto worse = [&](results::Verdict nv) {
            if (static_cast<int>(nv) > static_cast<int>(v)) v = nv;
        };
        if (checks.failedChecks) {
            worse(results::Verdict::Fail);
            findings.push_back(std::format("{} of {} reference frames rendered wrong under heat: the GPU makes errors when hot",
                                           checks.failedChecks, checks.checks));
        }
        if (pct("hw_slowdown") > 0) {
            worse(results::Verdict::Fail);
            findings.push_back(std::format("hardware slowdown was active {:.0f} % of the time (emergency clock cut: power "
                                           "delivery or cooling fault)",
                                           pct("hw_slowdown")));
        }
        if (a.perfDropPct > 10.0) {
            worse(results::Verdict::Warn);
            findings.push_back(std::format("performance fell {:.0f} % once hot", a.perfDropPct));
        }
        if (thermalPct > 5.0) {
            worse(results::Verdict::Warn);
            findings.push_back(std::format("thermal throttling {:.0f} % of the time — check fans, airflow, dust and paste",
                                           thermalPct));
        }
        if (a.maxHotspotC >= 100.0 || a.maxTempC >= 87.0) {
            worse(results::Verdict::Warn);
            findings.push_back(std::format("running hot (peak {:.0f} °C, hotspot {:.0f} °C)", a.maxTempC, a.maxHotspotC));
        }
        std::string summary = std::format("{} soak: {:.0f} fps at the start, {:.0f} fps at the end ({:+.1f} %)",
                                          formatDuration(a.durationS), a.firstFps, a.lastFps, -a.perfDropPct);
        if (a.hasSensors && a.maxTempC > 0) {
            summary += a.plateaued ? std::format("; temperature settled at {:.0f} °C after {}", a.plateauTempC,
                                                 formatDuration(std::max(0.0, a.timeToPlateauS)))
                                   : std::format("; temperature still rising at the end (peak {:.0f} °C)", a.maxTempC);
            if (pct("power") > 0) summary += std::format("; at the power limit {:.0f} % of the time (normal under full load)", pct("power"));
        } else if (!a.hasSensors) {
            summary += "; no sensors available, so only performance and rendering were checked";
        }
        summary += checks.checks ? std::format("; {} reference frame{} checked", checks.checks, checks.checks == 1 ? "" : "s")
                                 : std::string();
        std::string msg = summary + ".";
        if (!findings.empty()) {
            msg += " Problems: ";
            for (std::size_t i = 0; i < findings.size(); ++i) msg += (i ? "; " : "") + findings[i];
            msg += ".";
        }
        ctx.setVerdict(v, msg);
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::ThermalSoakTest)
