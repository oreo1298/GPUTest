// Upscaling (feature.upscale): the cost of AMD FSR 1 (EASU upscaling + RCAS sharpening) at each
// quality mode, and its image quality against a bilinear baseline. An analytic test image is rendered
// with 4x4 supersampling at the output resolution (ground truth) and at each render resolution; the
// upscaled results are compared with the ground truth (PSNR). EASU, being edge-directed, must
// reconstruct anti-aliased edges at least as well as bilinear filtering, so a broken port fails.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"
#include "gt/bench/upscale.h"

#include <cmath>
#include <format>

namespace gt::bench {
namespace {

struct PatternParams {
    std::uint32_t width, height, pad0, pad1;
};

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double se = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = double(a[i + c]) - double(b[i + c]);
            se += d * d;
            ++n;
        }
    if (!n) return 0.0;
    const double mse = se / double(n);
    return mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}

class UpscaleTest : public Test {
public:
    UpscaleTest() {
        m_info.id = "feature.upscale";
        m_info.name = "Upscaling (FSR 1)";
        m_info.description = "Measures AMD FidelityFX Super Resolution 1 (edge-adaptive upscaling and sharpening) at "
                             "each quality mode and checks its image quality against bilinear upscaling.";
        m_info.category = Category::Feature;
        m_info.group = "Upscaling";
        m_info.frameBased = false;
        m_info.primaryMetric = "fsr1_quality_gpix";
        m_info.primaryUnit = "Gpixel/s";
        m_info.reference = 0.0; // shown, not scored (docs/SCORING.md)
        m_info.tags = {"upscaling"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 95;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& f = ctx.device().features();
        if (f.maxComputeWorkgroupInvocations == 0) return SupportResult::no("no compute shaders");
        return SupportResult::yes();
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        // Upscaling to 4K is the common case on real GPUs; software renderers use a small image.
        const std::uint32_t outW = software ? 480u : 3840u, outH = software ? 270u : 2160u;

        rhi::ComputePipelineDesc pd;
        pd.name = "upscale-pattern";
        pd.compute = {"features/upscale", "csPattern"};
        pd.layout.bindings = {{rhi::BindingType::StorageTexture, 0}};
        pd.layout.pushConstantBytes = sizeof(PatternParams);
        auto pattern = dev.createComputePipeline(pd);
        auto image = [&](std::uint32_t w, std::uint32_t h, const char* name) {
            rhi::TextureDesc td;
            td.format = rhi::Format::RGBA8_UNORM;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
            td.name = name;
            auto t = dev.createTexture(td);
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                const PatternParams p{w, h, 0, 0};
                cmd.transition(t.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pattern.get());
                cmd.setStorageTexture(0, t.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch((w + 7) / 8, (h + 7) / 8);
                cmd.transition(t.get(), rhi::ResourceState::ShaderRead);
            });
            return t;
        };
        ctx.progress(0.05, "rendering the reference image");
        auto truthTex = image(outW, outH, "upscale-truth");
        const std::vector<std::uint8_t> truth = dev.readbackTexture(truthTex.get());
        auto dump = [&](rhi::Texture* t, const std::string& name) {
            if (!s.dumpFrames || s.dumpDir.empty()) return;
            const std::vector<std::uint8_t> px = dev.readbackTexture(t);
            writePng(std::format("{}/feature.upscale_{}.png", s.dumpDir, name), outW, outH, px.data());
        };
        dump(truthTex.get(), "truth");

        auto& d = ctx.details();
        d["output_resolution"] = std::format("{}x{}", outW, outH);
        const UpscaleMode modes[] = {UpscaleMode::UltraQuality, UpscaleMode::Quality, UpscaleMode::Balanced,
                                     UpscaleMode::Performance};
        std::string failures;
        double qualityGpix = 0.0;
        int step = 0;
        for (UpscaleMode mode : modes) {
            const std::string name(upscaleModeName(mode));
            ctx.progress(0.1 + 0.2 * step++, std::format("FSR 1 {}", name));
            const double ratio = upscaleRatio(mode);
            const std::uint32_t inW = renderExtent(outW, ratio), inH = renderExtent(outH, ratio);
            auto input = image(inW, inH, "upscale-input");
            UpscalePass fsr(dev, Upscaler::Fsr1, inW, inH, outW, outH, s.sharpness);
            UpscalePass bilinear(dev, Upscaler::Bilinear, inW, inH, outW, outH);

            const MicroResult t = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                fsr.record(cmd, input.get());
            });
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                fsr.record(cmd, input.get());
                bilinear.record(cmd, input.get());
            });
            const double pFsr = psnr(truth, dev.readbackTexture(fsr.output()));
            const double pEasu = psnr(truth, dev.readbackTexture(fsr.intermediate()));
            const double pBilinear = psnr(truth, dev.readbackTexture(bilinear.output()));
            const double gpix = t.msPerRep > 0 ? double(outW) * outH / (t.msPerRep / 1000.0) / 1e9 : 0.0;
            if (mode == UpscaleMode::Quality) {
                qualityGpix = gpix;
                dump(fsr.output(), "fsr1_quality");
                dump(bilinear.output(), "bilinear_quality");
            }
            std::string key = name;
            for (char& c : key)
                if (c == '-') c = '_';
            ctx.addMetric(std::format("fsr1_{}_ms", key), std::format("FSR 1 {} ({}x{} -> {}x{})", name, inW, inH,
                                                                      outW, outH),
                          t.msPerRep, "ms", false);
            ctx.addMetric(std::format("fsr1_{}_psnr", key), std::format("FSR 1 {} PSNR", name), pFsr, "dB");
            d[std::format("easu_{}_psnr", key)] = pEasu;
            d[std::format("bilinear_{}_psnr", key)] = pBilinear;
            d[std::format("render_{}", key)] = std::format("{}x{}", inW, inH);
            // EASU must reconstruct the supersampled edges at least as well as bilinear filtering.
            if (pEasu < pBilinear - 0.5 || pFsr < 20.0)
                failures += std::format(" {}: EASU {:.2f} dB, FSR {:.2f} dB, bilinear {:.2f} dB.", name, pEasu, pFsr,
                                        pBilinear);
        }
        ctx.addMetric("fsr1_quality_gpix", "FSR 1 quality mode throughput", qualityGpix, "Gpixel/s");
        if (software) d["note"] = "software renderer: 480x270 output";
        if (!failures.empty())
            ctx.setVerdict(results::Verdict::Fail,
                           "FSR 1 reconstructed the test image worse than bilinear upscaling:" + failures);
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("FSR 1 beats or matches bilinear upscaling at every mode ({:.2f} vs {:.2f} dB "
                                       "at quality, EASU only).",
                                       double(d["easu_quality_psnr"]), double(d["bilinear_quality_psnr"])));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::UpscaleTest)
