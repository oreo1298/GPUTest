// Variable-rate shading (feature.vrs): an ALU-heavy full-screen shader drawn at per-draw
// shading rates 1x1, 2x2 and (when supported) 4x4. Reports the time saved by each coarse
// rate, and checks with a per-pixel probe shader that the driver really shades one
// invocation per 2x2 block — some drivers accept the rate and silently ignore it. With
// shading-rate images (D3D12 VRS tier 2) a foveated image (full rate in the centre, coarser
// towards the edges) is timed too, and the probe checks the rate tile by tile.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <cmath>
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
        m_info.description =
            "Draws a shading-heavy full-screen effect at 1x1, 2x2 and 4x4 shading rates (and with a "
            "foveated shading-rate image where supported) and measures the time saved; verifies that "
            "coarse rates are really applied.";
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
                   : SupportResult::no("no per-draw variable-rate shading (VK_KHR_fragment_shading_rate / "
                                       "D3D12 VRS tier 1)");
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

        // Foveated shading-rate image: 1x1 inside 25 % of the half-diagonal, 2x2 up to 55 %, the
        // coarsest supported rate outside.
        const bool imageRates = dev.features().shadingRateImage && dev.features().shadingRateTileSize > 0;
        const std::uint32_t maxRate = dev.features().maxShadingRate;
        const std::uint32_t tile = dev.features().shadingRateTileSize;
        const std::uint32_t tilesX = imageRates ? (width + tile - 1) / tile : 0;
        const std::uint32_t tilesY = imageRates ? (height + tile - 1) / tile : 0;
        std::vector<std::uint8_t> tileRates(std::size_t(tilesX) * tilesY, 1);
        std::unique_ptr<rhi::Texture> rateImage;
        if (imageRates) {
            for (std::uint32_t ty = 0; ty < tilesY; ++ty)
                for (std::uint32_t tx = 0; tx < tilesX; ++tx) {
                    const double dx = ((tx + 0.5) * tile - width * 0.5) / (width * 0.5);
                    const double dy = ((ty + 0.5) * tile - height * 0.5) / (height * 0.5);
                    const double r = std::sqrt(dx * dx + dy * dy) / std::sqrt(2.0);
                    tileRates[std::size_t(ty) * tilesX + tx] =
                        static_cast<std::uint8_t>(r < 0.25   ? 1
                                                  : r < 0.55 ? 2
                                                             : maxRate);
                }
            std::vector<std::uint8_t> codes(tileRates.size());
            for (std::size_t i = 0; i < codes.size(); ++i)
                codes[i] = rhi::shadingRateCode(tileRates[i], tileRates[i]);
            rhi::TextureDesc rd;
            rd.format = rhi::Format::R8_UINT;
            rd.width = tilesX;
            rd.height = tilesY;
            rd.usage = rhi::TextureUsage::ShadingRate | rhi::TextureUsage::CopyDst;
            rd.name = "vrs-rate-image";
            rateImage = dev.createTexture(rd);
            dev.uploadTexture(rateImage.get(), 0, 0, codes.data(), tilesX);
        }

        auto record = [&](rhi::CommandList& cmd, rhi::Pipeline* pipe, std::uint32_t rate,
                          bool useImage = false) {
            rhi::RenderPassDesc rp;
            rp.colors.push_back({color.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            if (useImage) rp.shadingRateImage = rateImage.get();
            cmd.beginRenderPass(rp);
            cmd.setPipeline(pipe);
            cmd.setShadingRate(rate, rate);
            cmd.pushConstants(&params, sizeof(params));
            cmd.draw(3);
            cmd.endRenderPass();
        };

        auto& d = ctx.details();
        double baseMs = 0.0;
        d["max_rate"] = std::format("{0}x{0}", maxRate);
        d["shading_rate_image"] =
            imageRates ? std::format("yes ({0}x{0} px tiles)", tile) : std::string("no");
        for (std::uint32_t rate = 1; rate <= maxRate; rate *= 2) {
            ctx.progress(0.1 + 0.25 * double(rate / 2), std::format("{0}x{0} shading rate", rate));
            const MicroResult m = measureMicro(
                ctx, [&](rhi::CommandList& cmd, std::uint32_t) { record(cmd, heavy.get(), rate); });
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
                writePng(std::format("{0}/feature.vrs_{1}x{1}.png", s.dumpDir, rate), width, height,
                         px.data());
            }
        }
        if (imageRates) {
            ctx.progress(0.8, "foveated shading-rate image");
            const MicroResult m = measureMicro(
                ctx, [&](rhi::CommandList& cmd, std::uint32_t) { record(cmd, heavy.get(), 1, true); });
            d["ms_image"] = m.msPerRep;
            ctx.addMetric("speedup_image", "Speed-up with a foveated rate image",
                          m.msPerRep > 0 ? baseMs / m.msPerRep : 0.0, "x");
        }

        // Probe: at 2x2, every aligned 2x2 block must hold a single colour.
        ctx.progress(0.9, "checking coarse shading");
        // Percentage of aligned 2x2 blocks holding a single colour, split by the rate of the
        // shading-rate tile the block lies in (index 0: 1x1 tiles, 1: coarser tiles).
        struct BlockStats {
            std::uint64_t blocks[2]{}, uniform[2]{};
            double pct(int i) const {
                return blocks[i] ? 100.0 * double(uniform[i]) / double(blocks[i]) : 0.0;
            }
        };
        auto uniformBlocks = [&](std::uint32_t rate, bool useImage) {
            ctx.timeGpu([&](rhi::CommandList& cmd) { record(cmd, probe.get(), rate, useImage); });
            const std::vector<std::uint8_t> px = dev.readbackTexture(color.get());
            BlockStats st;
            for (std::uint32_t y = 0; y + 1 < height; y += 2)
                for (std::uint32_t x = 0; x + 1 < width; x += 2) {
                    auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                        std::uint32_t v;
                        std::memcpy(&v, &px[(std::size_t(yy) * width + xx) * 4], 4);
                        return v;
                    };
                    const int coarseTile =
                        useImage && tileRates[std::size_t(y / tile) * tilesX + x / tile] > 1 ? 1 : 0;
                    const std::uint32_t a = at(x, y);
                    st.uniform[coarseTile] += a == at(x + 1, y) && a == at(x, y + 1) && a == at(x + 1, y + 1);
                    ++st.blocks[coarseTile];
                }
            return st;
        };
        const double fine = uniformBlocks(1, false).pct(0), coarse = uniformBlocks(2, false).pct(0);
        d["uniform_blocks_pct_1x1"] = fine;
        d["uniform_blocks_pct_2x2"] = coarse;
        std::string imageNote;
        bool imageOk = true;
        if (imageRates) {
            const BlockStats st = uniformBlocks(1, true);
            d["image_uniform_blocks_pct_fine_tiles"] = st.pct(0);
            d["image_uniform_blocks_pct_coarse_tiles"] = st.pct(1);
            imageOk = st.pct(1) >= 95.0 && st.pct(0) <= 5.0;
            imageNote = std::format(
                " Rate image: {:.1f} % of blocks in coarse tiles and {:.1f} % in 1x1 tiles shaded once.",
                st.pct(1), st.pct(0));
        }
        if (coarse < 95.0)
            ctx.setVerdict(
                results::Verdict::Warn,
                std::format("The 2x2 rate was accepted but only {:.1f} % of 2x2 blocks were shaded once: the "
                            "driver appears to ignore the shading rate.{}",
                            coarse, imageNote));
        else if (!imageOk)
            ctx.setVerdict(
                results::Verdict::Warn,
                std::format("Per-draw rates work, but the shading-rate image is not applied tile by tile.{}",
                            imageNote));
        else
            ctx.setVerdict(
                results::Verdict::Pass,
                std::format("Coarse shading verified ({:.1f} % of 2x2 blocks shaded once at 2x2, {:.1f} % at "
                            "1x1).{}",
                            coarse, fine, imageNote));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::VrsTest)
