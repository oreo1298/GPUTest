// Fixed-function throughput (shaders/src/synthetic/raster.slang):
//  - synth.fill.pixel: full-screen layers into RGBA8 / RGBA16F, opaque and blended (ROPs)
//  - synth.fill.texel: texture reads per pixel with point / bilinear / trilinear / 16x
//    anisotropic filtering (texture units)
//  - synth.geometry: tiny triangles (setup + rasterizer) and tessellation
//  - synth.msaa: cost of 2x / 4x / 8x multisampling relative to no MSAA

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <array>
#include <format>
#include <vector>

namespace gt::bench {
namespace {

struct RasterParams {
    float color[4];
    std::uint32_t samples;
    float tessFactor;
    float triSize;
    std::uint32_t gridW;
    float texScale[2];
    float lodBias;
    std::uint32_t pad;
};
static_assert(sizeof(RasterParams) == 48);

std::unique_ptr<rhi::Texture> target(rhi::Device& dev, rhi::Format f, std::uint32_t w, std::uint32_t h, std::uint32_t samples = 1,
                                     const char* name = "raster-target") {
    rhi::TextureDesc td;
    td.format = f;
    td.width = w;
    td.height = h;
    td.sampleCount = samples;
    td.usage = rhi::TextureUsage::RenderTarget | (samples == 1 ? rhi::TextureUsage::Sampled : rhi::TextureUsage::None);
    td.name = name;
    return dev.createTexture(td);
}

rhi::GraphicsPipelineDesc rasterPipeline(const char* vs, const char* ps, rhi::Format f) {
    rhi::GraphicsPipelineDesc gd;
    gd.name = ps;
    gd.vertex = {"synthetic/raster", vs};
    gd.fragment = {"synthetic/raster", ps};
    gd.colorFormats = {f};
    gd.raster.cull = rhi::CullMode::None;
    gd.depth.test = false;
    gd.depth.write = false;
    gd.layout.pushConstantBytes = sizeof(RasterParams);
    return gd;
}

/// Render size for fill tests: the requested resolution, at least 1080p-sized on real GPUs.
std::pair<std::uint32_t, std::uint32_t> fillSize(TestContext& ctx) {
    const auto& s = ctx.settings();
    if (ctx.device().adapter().isSoftware()) return {std::min(s.width, 640u), std::min(s.height, 360u)};
    return {std::max(s.width, 1920u), std::max(s.height, 1080u)};
}

// --- pixel fill ------------------------------------------------------------------------------
class PixelFillTest : public Test {
public:
    PixelFillTest() {
        m_info.id = "synth.fill.pixel";
        m_info.name = "Pixel fill rate";
        m_info.description = "Draws full-screen layers with a trivial shader to measure how many pixels per second the "
                             "render back ends write: 8-bit and 16-bit-float targets, opaque and alpha-blended.";
        m_info.category = Category::Synthetic;
        m_info.group = "Rasterization";
        m_info.frameBased = false;
        m_info.primaryMetric = "rgba8_gpix";
        m_info.primaryUnit = "Gpixel/s";
        m_info.reference = 250.0;
        m_info.tags = {"raster"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 40;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto [w, h] = fillSize(ctx);
        const std::uint32_t layers = 16;
        struct Variant {
            rhi::Format format;
            bool blend;
            const char* metric;
            const char* label;
        };
        const Variant variants[] = {{rhi::Format::RGBA8_UNORM, false, "rgba8_gpix", "RGBA8 opaque"},
                                    {rhi::Format::RGBA8_UNORM, true, "rgba8_blend_gpix", "RGBA8 blended"},
                                    {rhi::Format::RGBA16_FLOAT, false, "rgba16f_gpix", "RGBA16F opaque"},
                                    {rhi::Format::RGBA16_FLOAT, true, "rgba16f_blend_gpix", "RGBA16F blended"}};
        int i = 0;
        for (const Variant& v : variants) {
            auto rt = target(dev, v.format, w, h);
            auto gd = rasterPipeline("vsFull", "psSolid", v.format);
            if (v.blend) {
                rhi::BlendAttachment b;
                b.enable = true;
                b.srcColor = rhi::BlendFactor::SrcAlpha;
                b.dstColor = rhi::BlendFactor::InvSrcAlpha;
                gd.blend = {b};
            }
            auto pipe = dev.createGraphicsPipeline(gd);
            RasterParams p{};
            p.color[0] = 0.2f, p.color[1] = 0.5f, p.color[2] = 0.8f, p.color[3] = 0.5f;
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                rhi::RenderPassDesc rp;
                rp.colors.push_back({rt.get(), 0, 0, rhi::LoadOp::Load, rhi::StoreOp::Store, {}});
                cmd.beginRenderPass(rp);
                cmd.setPipeline(pipe.get());
                cmd.pushConstants(&p, sizeof(p));
                for (std::uint32_t l = 0; l < layers; ++l) cmd.draw(3);
                cmd.endRenderPass();
            });
            const double gpix = m.msPerRep > 0 ? double(w) * h * layers / (m.msPerRep / 1000.0) / 1e9 : 0;
            ctx.addMetric(v.metric, v.label, gpix, "Gpixel/s");
            ctx.progress(++i / 4.0, v.label);
        }
        ctx.details()["target"] = std::format("{}x{}", w, h);
    }

private:
    TestInfo m_info;
};

// --- texel fill ------------------------------------------------------------------------------
class TexelFillTest : public Test {
public:
    TexelFillTest() {
        m_info.id = "synth.fill.texel";
        m_info.name = "Texture fill rate";
        m_info.description = "Reads a mip-mapped texture 8 times per pixel with point, bilinear, trilinear and 16x "
                             "anisotropic filtering, in 8-bit and 16-bit-float formats, to measure texture-unit throughput.";
        m_info.category = Category::Synthetic;
        m_info.group = "Rasterization";
        m_info.frameBased = false;
        m_info.primaryMetric = "rgba8_bilinear_gtex";
        m_info.primaryUnit = "Gtexel/s";
        m_info.reference = 800.0;
        m_info.tags = {"raster"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 41;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto [w, h] = fillSize(ctx);
        const std::uint32_t samples = 8;
        auto rt = target(dev, rhi::Format::RGBA8_UNORM, w, h);
        auto pipe = dev.createGraphicsPipeline([&] {
            auto gd = rasterPipeline("vsFull", "psTexel", rhi::Format::RGBA8_UNORM);
            gd.layout.bindings = {{rhi::BindingType::SampledTexture, 0}, {rhi::BindingType::Sampler, 0}};
            return gd;
        }());
        struct Filter {
            const char* name;
            rhi::Filter min, mag, mip;
            float aniso;
            float uvScale; // > 1 minifies, so mips (and anisotropy, with a squashed axis) come into play
            float squash;
        };
        const Filter filters[] = {{"point", rhi::Filter::Nearest, rhi::Filter::Nearest, rhi::Filter::Nearest, 1, 1, 1},
                                  {"bilinear", rhi::Filter::Linear, rhi::Filter::Linear, rhi::Filter::Nearest, 1, 1, 1},
                                  {"trilinear", rhi::Filter::Linear, rhi::Filter::Linear, rhi::Filter::Linear, 1, 1.7f, 1},
                                  {"aniso16x", rhi::Filter::Linear, rhi::Filter::Linear, rhi::Filter::Linear, 16, 1.7f, 8}};
        const rhi::Format formats[] = {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA16_FLOAT};
        int step = 0;
        for (rhi::Format f : formats) {
            // 2048² mip-mapped texture with noise in every level.
            rhi::TextureDesc td;
            td.format = f;
            td.width = td.height = 2048;
            td.mipLevels = 12;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.name = "texel-source";
            auto tex = dev.createTexture(td);
            const std::uint32_t bpp = f == rhi::Format::RGBA8_UNORM ? 4 : 8;
            for (std::uint32_t mip = 0; mip < td.mipLevels; ++mip) {
                const std::uint32_t mw = std::max(1u, td.width >> mip);
                std::vector<std::uint8_t> data(std::size_t(mw) * mw * bpp);
                std::uint32_t x = 2463534242u + mip;
                for (auto& b : data) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    b = static_cast<std::uint8_t>(x);
                }
                if (bpp == 8) // keep half floats finite: clear the exponent's top bit
                    for (std::size_t k = 1; k < data.size(); k += 2) data[k] &= 0x3B;
                dev.uploadTexture(tex.get(), mip, 0, data.data(), mw * bpp);
            }
            for (const Filter& fl : filters) {
                if (fl.aniso > 1 && !dev.features().anisotropicFiltering) continue;
                rhi::SamplerDesc sd;
                sd.minFilter = fl.min;
                sd.magFilter = fl.mag;
                sd.mipFilter = fl.mip;
                sd.maxAnisotropy = fl.aniso;
                auto sampler = dev.createSampler(sd);
                RasterParams p{};
                p.samples = samples;
                p.texScale[0] = fl.uvScale * fl.squash;
                p.texScale[1] = fl.uvScale;
                const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                    cmd.transition(tex.get(), rhi::ResourceState::ShaderRead);
                    rhi::RenderPassDesc rp;
                    rp.colors.push_back({rt.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
                    cmd.beginRenderPass(rp);
                    cmd.setPipeline(pipe.get());
                    cmd.setTexture(0, tex.get());
                    cmd.setSampler(0, sampler.get());
                    cmd.pushConstants(&p, sizeof(p));
                    cmd.draw(3);
                    cmd.endRenderPass();
                });
                const double gtex = m.msPerRep > 0 ? double(w) * h * samples / (m.msPerRep / 1000.0) / 1e9 : 0;
                const std::string fmt = f == rhi::Format::RGBA8_UNORM ? "rgba8" : "rgba16f";
                ctx.addMetric(std::format("{}_{}_gtex", fmt, fl.name),
                              std::format("{} {}", f == rhi::Format::RGBA8_UNORM ? "RGBA8" : "RGBA16F", fl.name), gtex,
                              "Gtexel/s");
                ctx.progress(++step / 8.0, fl.name);
            }
        }
        ctx.details()["target"] = std::format("{}x{}", w, h);
        ctx.details()["reads_per_pixel"] = samples;
    }

private:
    TestInfo m_info;
};

// --- geometry --------------------------------------------------------------------------------
class GeometryTest : public Test {
public:
    GeometryTest() {
        m_info.id = "synth.geometry";
        m_info.name = "Geometry throughput";
        m_info.description = "Millions of tiny triangles to measure triangle setup and rasterization, and the same "
                             "patches tessellated at factors 8 and 32 to measure the tessellator.";
        m_info.category = Category::Synthetic;
        m_info.group = "Rasterization";
        m_info.frameBased = false;
        m_info.primaryMetric = "triangles_gtris";
        m_info.primaryUnit = "Gtris/s";
        m_info.reference = 8.0;
        m_info.tags = {"raster"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 42;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t w = software ? 640 : 1920, h = software ? 360 : 1080;
        auto rt = target(dev, rhi::Format::RGBA8_UNORM, w, h);
        const std::uint32_t instances = software ? 1 : 16; // × 65536 triangles each
        const std::uint32_t tris = 65536 * instances;
        RasterParams p{};
        p.color[0] = p.color[1] = p.color[2] = 0.7f;
        p.gridW = 1024;
        p.triSize = 2.0f / 1024.0f * 0.6f; // about one pixel at 1080p
        auto draw = [&](rhi::Pipeline* pipe, std::uint32_t vertices, std::uint32_t inst) {
            return measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                rhi::RenderPassDesc rp;
                rp.colors.push_back({rt.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}});
                cmd.beginRenderPass(rp);
                cmd.setPipeline(pipe);
                cmd.pushConstants(&p, sizeof(p));
                cmd.draw(vertices, inst);
                cmd.endRenderPass();
            });
        };
        auto tiny = dev.createGraphicsPipeline(rasterPipeline("vsTiny", "psFlat", rhi::Format::RGBA8_UNORM));
        const MicroResult m = draw(tiny.get(), 65536 * 3, instances);
        ctx.addMetric("triangles_gtris", "Small triangles", m.msPerRep > 0 ? tris / (m.msPerRep / 1000.0) / 1e9 : 0, "Gtris/s");
        if (!dev.features().tessellation) {
            ctx.details()["tessellation"] = "not supported";
            return;
        }
        auto gd = rasterPipeline("vsPatch", "psFlat", rhi::Format::RGBA8_UNORM);
        gd.hull = {"synthetic/raster", "hsTess"};
        gd.domain = {"synthetic/raster", "dsTess"};
        gd.topology = rhi::PrimitiveTopology::PatchList;
        gd.patchControlPoints = 3;
        auto tess = dev.createGraphicsPipeline(gd);
        // Software rasterizers tessellate very slowly: a token workload there (correctness only).
        const std::uint32_t patches = software ? 16 : 65536;
        p.triSize = 2.0f / 1024.0f * 6.0f;
        for (float factor : software ? std::vector<float>{8.0f} : std::vector<float>{8.0f, 32.0f}) {
            p.tessFactor = factor;
            const MicroResult t = draw(tess.get(), patches * 3, 1);
            // An integer-partitioned triangle patch with factor n yields n² triangles.
            const double out = double(patches) * factor * factor;
            ctx.addMetric(std::format("tess{}_gtris", static_cast<int>(factor)), std::format("Tessellation factor {}", factor),
                          t.msPerRep > 0 ? out / (t.msPerRep / 1000.0) / 1e9 : 0, "Gtris/s");
        }
    }

private:
    TestInfo m_info;
};

// --- MSAA ------------------------------------------------------------------------------------
class MsaaTest : public Test {
public:
    MsaaTest() {
        m_info.id = "synth.msaa";
        m_info.name = "MSAA cost";
        m_info.description = "Renders the same dense geometry without anti-aliasing and with 2x, 4x and 8x MSAA (plus "
                             "resolve) and reports how much slower each level is.";
        m_info.category = Category::Synthetic;
        m_info.group = "Rasterization";
        m_info.frameBased = false;
        m_info.primaryMetric = "msaa4_cost_pct";
        m_info.primaryUnit = "%";
        m_info.higherIsBetter = false;
        m_info.tags = {"raster"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 43;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto [w, h] = fillSize(ctx);
        auto resolved = target(dev, rhi::Format::RGBA8_UNORM, w, h, 1, "msaa-resolved");
        RasterParams p{};
        p.color[0] = 0.3f, p.color[1] = 0.6f, p.color[2] = 0.9f;
        p.gridW = 256;
        p.triSize = 2.0f / 256.0f * 0.9f;
        const std::uint32_t instances = dev.adapter().isSoftware() ? 1 : 4;
        double base = 0.0;
        for (std::uint32_t samples : {1u, 2u, 4u, 8u}) {
            if (!(dev.renderTargetSampleCounts(rhi::Format::RGBA8_UNORM) & samples)) {
                ctx.details()[std::format("msaa{}", samples)] = "not supported";
                continue;
            }
            auto rt = samples > 1 ? target(dev, rhi::Format::RGBA8_UNORM, w, h, samples, "msaa-target") : nullptr;
            auto gd = rasterPipeline("vsTiny", "psShade", rhi::Format::RGBA8_UNORM);
            gd.sampleCount = samples;
            auto pipe = dev.createGraphicsPipeline(gd);
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                rhi::RenderPassDesc rp;
                rp.colors.push_back({samples > 1 ? rt.get() : resolved.get(), 0, 0, rhi::LoadOp::Clear,
                                     rhi::StoreOp::Store, {0, 0, 0, 1}});
                cmd.beginRenderPass(rp);
                cmd.setPipeline(pipe.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.draw(65536 * 3, instances);
                cmd.endRenderPass();
                if (samples > 1) cmd.resolveTexture(resolved.get(), rt.get());
            });
            if (samples == 1) {
                base = m.msPerRep;
                ctx.addMetric("msaa1_ms", "No MSAA", m.msPerRep, "ms", false);
            } else if (base > 0) {
                ctx.addMetric(std::format("msaa{}_cost_pct", samples), std::format("{}x MSAA extra time", samples),
                              (m.msPerRep / base - 1.0) * 100.0, "%", false);
            }
        }
        ctx.details()["target"] = std::format("{}x{}", w, h);
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::PixelFillTest)
GT_REGISTER_TEST(gt::bench::TexelFillTest)
GT_REGISTER_TEST(gt::bench::GeometryTest)
GT_REGISTER_TEST(gt::bench::MsaaTest)
