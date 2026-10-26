// Artifact detection under load: renders the deterministic stress scene continuously
// and, every few frames, re-renders a fixed reference frame and compares it with the
// one rendered at the start, bit for bit, on the GPU. A healthy GPU reproduces it
// exactly; unstable core clocks/voltages show up as mismatching pixels long before
// they crash a game.

#include "gt/bench/context.h"
#include "gt/bench/test.h"
#include "stress_common.h"

#include <format>

namespace gt::bench {
namespace {

class ArtifactTest : public Test {
public:
    ArtifactTest() {
        m_info.id = "diag.artifacts";
        m_info.name = "Artifact detection";
        m_info.description = "Renders a demanding, fully deterministic scene and keeps re-rendering one reference frame "
                             "under load, comparing every pixel with the first rendering. Any difference means the GPU "
                             "made a calculation error (unstable overclock or undervolt, overheating, failing chip).";
        m_info.category = Category::Diagnostic;
        m_info.order = 40;
        m_info.group = "Stability";
        m_info.primaryMetric = "failed_checks";
        m_info.primaryUnit = "frames";
        m_info.higherIsBetter = false;
        m_info.tags = {"stress"};
        m_info.suites = {"diagnostics", "full"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        const auto& s = ctx.settings();
        diagnostics::StressRenderer stress(ctx, s.width, s.height);
        stress.captureReference();
        const std::uint32_t interval = s.duration == DurationPreset::Smoke ? 4 : 8; // frames between checks
        const bool inject = s.param("diag.artifacts.inject") == "1";
        std::uint32_t frames = 0;
        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(600);
        fl.simDuration = 30.0;
        fl.output = stress.output();
        fl.goldenFrames = {0};
        fl.record = [&](FrameContext& f) {
            stress.recordFrame(f.cmd, static_cast<float>(f.simTime));
            if (!f.warmup && ++frames % interval == 0) stress.recordCheck(f.cmd, inject && frames == interval);
        };
        const FrameLoopResult r = ctx.runFrames(fl);
        ctx.setFrameResult(r);
        const auto c = stress.readChecks();
        auto& d = ctx.details();
        d["checks"] = c.checks;
        d["failed_checks"] = c.failedChecks;
        d["mismatched_pixels"] = c.mismatchedPixels;
        d["check_interval_frames"] = interval;
        ctx.addMetric("failed_checks", "Reference frames with errors", c.failedChecks, "frames", false);
        ctx.addMetric("checks", "Reference frames checked", c.checks, "frames");
        if (c.checks == 0) {
            ctx.setVerdict(results::Verdict::Warn, "The run was too short to check any reference frame.");
        } else if (c.failedChecks == 0) {
            ctx.setVerdict(results::Verdict::Pass,
                           c.checks == 1 ? std::string("The reference frame re-rendered under load matched the original bit for bit.")
                                         : std::format("All {} reference frames re-rendered under load matched the original "
                                                       "bit for bit.", c.checks));
        } else {
            d["first_mismatch"] = {{"x", c.firstX}, {"y", c.firstY}};
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("{} of {} reference frames came out different ({} wrong pixels, first at {}, {}). "
                                       "The GPU produced calculation errors under load: typical of an unstable core "
                                       "overclock or undervolt, overheating, or a failing GPU.",
                                       c.failedChecks, c.checks, c.mismatchedPixels, c.firstX, c.firstY));
        }
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::ArtifactTest)
