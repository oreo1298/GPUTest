// Variable-rate shading (feature.vrs): an ALU-heavy full-screen shader drawn at per-draw
// shading rates 1x1, 2x2 and (when supported) 4x4. Reports the time saved by each coarse
// rate, and checks with a per-pixel probe shader that the driver really shades one
// invocation per 2x2 block — some drivers accept the rate and silently ignore it.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <cstring>
#include <format>

namespace gt::bench {
namespace {

struct VrsParams {
    std::uint32_t width, height, iterations;
    float time;
};

class VrsTest : public Test {
public:
    VrsTest() {
        m_info.id = "feature.vrs";
        m_info.name = "Variable-rate shading";
        m_info.description = "Draws a shading-heavy full-screen effect at 1x1, 2x2 and 4x4 shading rates and measures "
                             "the time saved; verifies that coarse rates are really applied.";
        m_info.category = Category::Feature;
        m_info.group = "Rasterization";
        m_info.frameBased = false;
        m_info.primaryMetric = "speedup_2x2";
        m_info.primaryUnit = "x";
        m_info.reference = 0.0; // a ratio: shown, not scored
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 40;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().variableRateShading
                   ? SupportResult::yes()
                   : SupportResult::no("no per-draw variable-rate shading (VK_KHR_fragment_shading_rate / D3D12 VRS tier 1)");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t width = software ? 320 : s.width, height = software ? 180 : s.height;
        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA8_UNORM;
        cd.width = width;
        cd.height = height;
        cd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        cd.name = "vrs-color";
        auto color = dev.createTexture(cd);

        auto pipeline = [&](const char* fragment) {
            rhi::GraphicsPipelineDesc pd;
            pd.name = fragment;
            pd.vertex = {"features/vrs", "vsFull"};
            pd.fragment = {"features/vrs", fragment};
            pd.colorFormats = {cd.format};
            pd.variableRate = true;
            pd.layout.pushConstantBytes = sizeof(VrsParams);
            return dev.createGraphicsPipeline(pd);
        };
        auto heavy = pipeline("psHeavy");
        auto probe = pipeline("psRateProbe");
        const VrsParams params{width, height, software ? 64u : 512u, 0.7f};

        auto record = [&](rhi::CommandList& cmd, rhi::Pipeline* pipe, std::uint32_t rate) {
            rhi::RenderPassDesc rp;
            rp.colors.push_back({color.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            cmd.beginRenderPass(rp);
            cmd.setPipeline(pipe);
            cmd.setShadingRate(rate, rate);
            cmd.pushConstants(&params, sizeof(params));
            cmd.draw(3);
            cmd.endRenderPass();
        };

        auto& d = ctx.details();
        double baseMs = 0.0;
        const std::uint32_t maxRate = dev.features().maxShadingRate;
        d["max_rate"] = std::format("{0}x{0}", maxRate);
        for (std::uint32_t rate = 1; rate <= maxRate; rate *= 2) {
            ctx.progress(0.1 + 0.25 * double(rate / 2), std::format("{0}x{0} shading rate", rate));
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { record(cmd, heavy.get(), rate); });
            d[std::format("ms_{0}x{0}", rate)] = m.msPerRep;
            if (rate == 1) {
                baseMs = m.msPerRep;
                ctx.addMetric("ms_1x1", "Frame time at 1x1", m.msPerRep, "ms", false);
            } else {
                ctx.addMetric(std::format("speedup_{0}x{0}", rate), std::format("Speed-up at {0}x{0}", rate),
                              m.msPerRep > 0 ? baseMs / m.msPerRep : 0.0, "x");
            }
            if (s.dumpFrames) {
                std::vector<std::uint8_t> px = dev.readbackTexture(color.get());
                writePng(std::format("{0}/feature.vrs_{1}x{1}.png", s.dumpDir, rate), width, height, px.data());
            }
        }

        // Probe: at 2x2, every aligned 2x2 block must hold a single colour.
        ctx.progress(0.9, "checking coarse shading");
        auto uniformBlocks = [&](std::uint32_t rate) {
            ctx.timeGpu([&](rhi::CommandList& cmd) { record(cmd, probe.get(), rate); });
            const std::vector<std::uint8_t> px = dev.readbackTexture(color.get());
            std::uint64_t blocks = 0, uniform = 0;
            for (std::uint32_t y = 0; y + 1 < height; y += 2)
                for (std::uint32_t x = 0; x + 1 < width; x += 2) {
                    auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                        std::uint32_t v;
                        std::memcpy(&v, &px[(std::size_t(yy) * width + xx) * 4], 4);
                        return v;
                    };
                    const std::uint32_t a = at(x, y);
                    uniform += a == at(x + 1, y) && a == at(x, y + 1) && a == at(x + 1, y + 1);
                    ++blocks;
                }
            return blocks ? 100.0 * double(uniform) / double(blocks) : 0.0;
        };
        const double fine = uniformBlocks(1), coarse = uniformBlocks(2);
        d["uniform_blocks_pct_1x1"] = fine;
        d["uniform_blocks_pct_2x2"] = coarse;
        if (coarse < 95.0)
            ctx.setVerdict(results::Verdict::Warn,
                           std::format("The 2x2 rate was accepted but only {:.1f} % of 2x2 blocks were shaded once: the "
                                       "driver appears to ignore the shading rate.",
                                       coarse));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Coarse shading verified ({:.1f} % of 2x2 blocks shaded once at 2x2, {:.1f} % at 1x1).",
                                       coarse, fine));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::VrsTest)
