#pragma once
// Shared by the artifact-detection and thermal-soak diagnostics: a deterministic,
// ALU-heavy ray-marched render (shaders/src/diagnostics/stress.slang) plus bit-exact
// reference checks. A reference frame is rendered once; under load the same frame is
// re-rendered at intervals and compared on the GPU, pixel by pixel.

#include "gt/bench/context.h"

#include <cstdint>
#include <memory>

namespace gt::bench::diagnostics {

class StressRenderer {
public:
    StressRenderer(TestContext& ctx, std::uint32_t width, std::uint32_t height);
    /// Final image of each load frame (shown in the window, dumped as golden frames).
    rhi::Texture* output() const { return m_color.get(); }
    /// Records one load frame at `time` (seconds of simulated time).
    void recordFrame(rhi::CommandList& cmd, float time);
    /// Renders the reference frame (synchronously). Call once before any check.
    void captureReference();
    /// Records a re-render of the reference frame and its comparison. `perturb`
    /// renders a slightly different frame instead (testing hook for the detection).
    void recordCheck(rhi::CommandList& cmd, bool perturb = false);

    struct CheckSummary {
        std::uint32_t checks = 0;
        std::uint32_t failedChecks = 0;
        std::uint64_t mismatchedPixels = 0;
        std::uint32_t firstX = 0, firstY = 0;
    };
    /// Reads the comparison counters back (waits for the GPU).
    CheckSummary readChecks();

private:
    void render(rhi::CommandList& cmd, rhi::Texture* target, float time);
    TestContext& m_ctx;
    rhi::Device& m_dev;
    std::uint32_t m_width, m_height;
    std::uint32_t m_steps = 64, m_shadowSteps = 24;
    std::unique_ptr<rhi::Texture> m_color, m_reference, m_check;
    std::unique_ptr<rhi::Pipeline> m_draw, m_compare;
    std::unique_ptr<rhi::Buffer> m_counters, m_readback;
    std::uint32_t m_checks = 0;
    static constexpr float kReferenceTime = 3.25f;
    static constexpr std::uint32_t kSlots = 4096;
};

} // namespace gt::bench::diagnostics
