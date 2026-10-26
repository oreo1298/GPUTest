// Debug-only test: renders a few frames, then reports a device loss the way the
// Vulkan/D3D backends do (rhi::DeviceLostError). Exercises the runner's device-lost
// handling (partial results, fault capture, --on-device-lost recover) in CI, where
// no real driver reset can be provoked. Tagged "debug": not in any suite and hidden
// in the GUI; run it explicitly with --test debug.device_lost.

#include "gt/bench/context.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <string>

namespace gt::bench {
namespace {

class DebugDeviceLost : public Test {
public:
    DebugDeviceLost() {
        m_info.id = "debug.device_lost";
        m_info.name = "Simulated device loss";
        m_info.description = "Renders a few frames and then reports a lost device, to test how a run handles a driver "
                             "reset. For development only.";
        m_info.category = Category::Diagnostic;
        m_info.group = "Debug";
        m_info.tags = {"debug"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        const auto frames = static_cast<std::uint32_t>(std::max(1, std::stoi(ctx.settings().param("debug.frames", "8"))));
        rhi::TextureDesc td;
        td.width = 64;
        td.height = 64;
        td.format = rhi::Format::RGBA8_UNORM;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
        td.name = "debug.device_lost target";
        auto target = ctx.device().createTexture(td);
        FrameLoopDesc fl;
        fl.framesPerPass = frames;
        fl.warmupFrames = 0;
        fl.simDuration = 1.0;
        fl.output = target.get();
        fl.record = [&](FrameContext& f) {
            rhi::RenderPassDesc rp;
            const float v = static_cast<float>(f.frame) / static_cast<float>(frames);
            rp.colors.push_back({target.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {v, 0.2f, 0.4f, 1.0f}});
            f.cmd.beginRenderPass(rp);
            f.cmd.endRenderPass();
        };
        // The loss is reported after the frame loop, so the result carries partial frame data.
        ctx.setFrameResult(ctx.runFrames(fl));
        ctx.device().waitIdle();
        throw rhi::DeviceLostError("simulated device loss (debug.device_lost)");
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::DebugDeviceLost)
