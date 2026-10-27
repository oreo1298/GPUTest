// Upscaling (feature.upscale): the cost of AMD FSR 1 (EASU upscaling + RCAS sharpening) at each
// quality mode, and its image quality against a bilinear baseline. An analytic test image is rendered
// with 4x4 supersampling at the output resolution (ground truth) and at each render resolution; the
// upscaled results are compared with the ground truth (PSNR). EASU, being edge-directed, must
// reconstruct anti-aliased edges at least as well as bilinear filtering, so a broken port fails.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/plugins.h"
#include "gt/bench/test.h"
#include "gt/bench/upscale.h"

#include <cctype>
#include <cmath>
#include <format>

namespace gt::bench {
namespace {

struct PatternParams {
    std::uint32_t width, height;
    float offsetX = 0, offsetY = 0; ///< scene pan, in image heights
    float jitterX = 0, jitterY = 0; ///< sample offset from pixel centres, pixels
    float motionX = 0, motionY = 0; ///< csMotion output
};

/// Halton sequence (base 2 / 3), the usual temporal-upscaler jitter.
float halton(std::uint32_t index, std::uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (std::uint32_t i = index; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

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
        m_pattern = pattern.get();
        auto image = [&](std::uint32_t w, std::uint32_t h, const char* name) {
            auto t = texture(dev, rhi::Format::RGBA8_UNORM, w, h, name);
            ctx.timeGpu([&](rhi::CommandList& cmd) { drawPattern(cmd, t.get(), {w, h}); });
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
        if (s.upscalerPlugins) {
            ctx.progress(0.9, "upscaler plugins");
            testPlugins(ctx, outW, outH, failures);
        }
        if (software) d["note"] = "software renderer: 480x270 output";
        if (!failures.empty())
            ctx.setVerdict(results::Verdict::Fail, "Upscaling failed its quality check:" + failures);
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("FSR 1 beats or matches bilinear upscaling at every mode ({:.2f} vs {:.2f} dB "
                                       "at quality, EASU only).",
                                       double(d["easu_quality_psnr"]), double(d["bilinear_quality_psnr"])));
    }

private:
    static std::unique_ptr<rhi::Texture> texture(rhi::Device& dev, rhi::Format f, std::uint32_t w, std::uint32_t h,
                                                 const char* name) {
        rhi::TextureDesc td;
        td.format = f;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
        td.name = name;
        return dev.createTexture(td);
    }

    void drawPattern(rhi::CommandList& cmd, rhi::Texture* t, const PatternParams& p) {
        cmd.transition(t, rhi::ResourceState::UnorderedAccess);
        cmd.setPipeline(m_pattern);
        cmd.setStorageTexture(0, t);
        cmd.pushConstants(&p, sizeof(p));
        cmd.dispatch((p.width + 7) / 8, (p.height + 7) / 8);
        cmd.transition(t, rhi::ResourceState::ShaderRead);
    }

    /// Every usable plugin upscales a panning, jittered sequence at quality mode; the last frame is compared
    /// with the ground truth at the same pan.
    void testPlugins(TestContext& ctx, std::uint32_t outW, std::uint32_t outH, std::string& failures) {
        rhi::Device& dev = ctx.device();
        auto& d = ctx.details();
        nlohmann::json list = nlohmann::json::array();
        const auto& plugins = upscalerPlugins();
        if (plugins.empty()) d["plugins_note"] = "no upscaler plugins in the plugins folder";
        rhi::ComputePipelineDesc pd;
        pd.name = "upscale-motion";
        pd.compute = {"features/upscale", "csMotion"};
        pd.layout.bindings = {{rhi::BindingType::StorageTexture, 1}, {rhi::BindingType::StorageTexture, 2}};
        pd.layout.pushConstantBytes = sizeof(PatternParams);
        auto motionPipe = dev.createComputePipeline(pd);
        for (const UpscalerPlugin& plugin : plugins) {
            nlohmann::json e{{"file", plugin.path}};
            if (!plugin.api) {
                e["status"] = "not loaded: " + plugin.error;
                list.push_back(e);
                continue;
            }
            const gt_upscaler_plugin& api = *plugin.api;
            e["id"] = api.id;
            e["name"] = api.name;
            if (api.version) e["version"] = api.version;
            if (const std::string why = upscalerPluginSupport(dev, api); !why.empty()) {
                e["status"] = "unsupported: " + why;
                list.push_back(e);
                continue;
            }
            try {
                const UpscaleMode mode = UpscaleMode::Quality;
                const double ratio = upscaleRatio(mode);
                const std::uint32_t inW = renderExtent(outW, ratio), inH = renderExtent(outH, ratio);
                const bool smoke = ctx.settings().duration == DurationPreset::Smoke;
                const std::uint32_t frames = smoke ? 8u : 32u;
                // The scene pans by (2.5, 1.25) output pixels per frame.
                const float vx = 2.5f, vy = 1.25f;
                auto color = texture(dev, rhi::Format::RGBA8_UNORM, inW, inH, "plugin-color");
                auto motion = texture(dev, rhi::Format::RG16_FLOAT, inW, inH, "plugin-motion");
                auto depth = texture(dev, rhi::Format::R32_FLOAT, inW, inH, "plugin-depth");
                PluginUpscalePass pass(dev, api, inW, inH, outW, outH, mode);
                PatternParams mp{inW, inH};
                mp.motionX = -vx / float(ratio); // to the previous frame's position, render pixels
                mp.motionY = -vy / float(ratio);
                std::vector<double> psnrs;
                PatternParams last{};
                for (std::uint32_t i = 0; i < frames; ++i) {
                    PatternParams p{inW, inH};
                    p.offsetX = vx * float(i) / float(outH);
                    p.offsetY = vy * float(i) / float(outH);
                    p.jitterX = halton(i + 1, 2) - 0.5f;
                    p.jitterY = halton(i + 1, 3) - 0.5f;
                    last = p;
                    ctx.timeGpu([&](rhi::CommandList& cmd) {
                        drawPattern(cmd, color.get(), p);
                        if (i == 0) {
                            cmd.transition(motion.get(), rhi::ResourceState::UnorderedAccess);
                            cmd.transition(depth.get(), rhi::ResourceState::UnorderedAccess);
                            cmd.setPipeline(motionPipe.get());
                            cmd.setStorageTexture(1, motion.get());
                            cmd.setStorageTexture(2, depth.get());
                            cmd.pushConstants(&mp, sizeof(mp));
                            cmd.dispatch((inW + 7) / 8, (inH + 7) / 8);
                            cmd.transition(motion.get(), rhi::ResourceState::ShaderRead);
                            cmd.transition(depth.get(), rhi::ResourceState::ShaderRead);
                        }
                        pass.record(cmd, color.get(), depth.get(), motion.get(), p.jitterX, p.jitterY, i == 0);
                    });
                    if (i == 0 || i + 1 == frames) {
                        // Ground truth at this frame's pan (no jitter).
                        auto truth = texture(dev, rhi::Format::RGBA8_UNORM, outW, outH, "plugin-truth");
                        PatternParams tp{outW, outH};
                        tp.offsetX = p.offsetX;
                        tp.offsetY = p.offsetY;
                        ctx.timeGpu([&](rhi::CommandList& cmd) { drawPattern(cmd, truth.get(), tp); });
                        psnrs.push_back(psnr(dev.readbackTexture(truth.get()), dev.readbackTexture(pass.output())));
                    }
                }
                const MicroResult t = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                    pass.record(cmd, color.get(), depth.get(), motion.get(), last.jitterX, last.jitterY, false);
                });
                std::string key = api.id;
                for (char& c : key)
                    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                ctx.addMetric(std::format("plugin_{}_ms", key),
                              std::format("{} quality ({}x{} -> {}x{})", api.name, inW, inH, outW, outH), t.msPerRep,
                              "ms", false);
                ctx.addMetric(std::format("plugin_{}_psnr", key), std::format("{} PSNR after {} frames", api.name,
                                                                              frames),
                              psnrs.back(), "dB");
                e["status"] = "measured";
                e["psnr_first_frame"] = psnrs.front();
                e["psnr_last_frame"] = psnrs.back();
                e["temporal"] = api.temporal != 0;
                if (psnrs.back() < 20.0)
                    failures += std::format(" {}: only {:.2f} dB after {} frames.", api.name, psnrs.back(), frames);
            } catch (const std::exception& ex) {
                e["status"] = std::string("error: ") + ex.what();
                failures += std::format(" {}: {}.", api.name, ex.what());
            }
            list.push_back(e);
        }
        d["plugins"] = list;
    }

    TestInfo m_info;
    rhi::Pipeline* m_pattern = nullptr;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::UpscaleTest)
