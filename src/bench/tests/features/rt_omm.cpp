// Opacity micromaps (feature.rt.omm): camera rays through thousands of alpha-tested "leaf"
// cards (shaders/src/features/omm.slang; omm_dxr.hlsl on D3D12). Without micromaps every
// candidate triangle is alpha-tested in the shader; with a per-triangle opacity micromap
// (VK_EXT_opacity_micromap, DXR 1.2) the hardware resolves triangles entirely inside or
// outside the leaf, and only the ones on its edge are tested. Reports both rates, the alpha tests per ray, and checks that both
// return the same hits. --param feature.rt.omm.baseline_only=1 runs the alpha-tested part
// alone (CI coverage on GPUs without micromaps).

#include "rt_common.h"

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"
#include "gt/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <random>

namespace gt::bench {
namespace {

constexpr float kRadius = 0.45f; // leafOpaque() in omm.slang
constexpr float kMargin = 1e-3f; // keeps classification away from float rounding at the edge

/// Classifies a triangle (uv coordinates) against the opaque disc around (0.5, 0.5).
rhi::TriangleOpacity classify(const float (&p)[3][2]) {
    auto dist = [](float x, float y) { return std::sqrt((x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f)); };
    bool allInside = true;
    for (const auto& v : p) allInside = allInside && dist(v[0], v[1]) < kRadius - kMargin;
    if (allInside) return rhi::TriangleOpacity::Opaque; // the disc is convex
    // Distance from the disc centre to the triangle: 0 if inside it, else to the nearest edge.
    const float cx = 0.5f, cy = 0.5f;
    auto cross = [&](int a, int b) { return (p[b][0] - p[a][0]) * (cy - p[a][1]) - (p[b][1] - p[a][1]) * (cx - p[a][0]); };
    const float c0 = cross(0, 1), c1 = cross(1, 2), c2 = cross(2, 0);
    if ((c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0)) return rhi::TriangleOpacity::Unknown;
    float best = 1e9f;
    for (int e = 0; e < 3; ++e) {
        const float* a = p[e];
        const float* b = p[(e + 1) % 3];
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const float t = std::clamp(((cx - a[0]) * dx + (cy - a[1]) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
        best = std::min(best, std::sqrt((a[0] + t * dx - cx) * (a[0] + t * dx - cx) + (a[1] + t * dy - cy) * (a[1] + t * dy - cy)));
    }
    return best > kRadius + kMargin ? rhi::TriangleOpacity::Transparent : rhi::TriangleOpacity::Unknown;
}

class OmmTest : public Test {
public:
    OmmTest() {
        m_info.id = "feature.rt.omm";
        m_info.name = "Opacity micromaps";
        m_info.description = "Traces rays through thousands of alpha-tested leaves, with every candidate tested in the "
                             "shader and with opacity micromaps resolving most of them in hardware; results must match.";
        m_info.category = Category::Feature;
        m_info.group = "Ray tracing";
        m_info.frameBased = false;
        m_info.primaryMetric = "omm_speedup";
        m_info.primaryUnit = "x";
        m_info.reference = 0.0; // a ratio: shown, not scored
        m_info.tags = {"rt"};
        m_info.suites = {"standard", "extreme", "raytracing"};
        m_info.order = 40;
    }
    const TestInfo& info() const override { return m_info; }

    static bool dxrShaderEmbedded() {
        return static_cast<bool>(shaders::find("features/omm_dxr", "csAlphaTrace", shaders::Format::Dxil));
    }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& f = ctx.device().features();
        if (!f.rayQuery) return SupportResult::no("no ray queries");
        if (ctx.settings().param("feature.rt.omm.baseline_only") == "1") return SupportResult::yes();
        if (f.opacityMicromap && ctx.api() == rhi::Api::D3D12 && !dxrShaderEmbedded())
            return SupportResult::no("the DXR 1.2 shader variant is not embedded in this build (needs DXC 1.9)");
        if (f.opacityMicromap) return SupportResult::yes();
        return SupportResult::no("no opacity micromaps (VK_EXT_opacity_micromap / D3D12 raytracing tier 1.2)");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const bool omm = dev.features().opacityMicromap && (ctx.api() != rhi::Api::D3D12 || dxrShaderEmbedded());
        const std::uint32_t width = software ? 160 : s.width, height = software ? 90 : s.height;
        const std::uint32_t cells = software ? 4 : 8;
        const std::uint32_t cards = software ? 300 : 20000;

        // Card mesh: the unit square in the XY plane (uv = position.xy), cells x cells quads.
        rt::Mesh card = rt::grid(cells, 1.0f);
        for (std::size_t i = 0; i < card.positions.size(); i += 3) {
            const float x = card.positions[i] + 0.5f, z = card.positions[i + 2] + 0.5f;
            card.positions[i] = x, card.positions[i + 1] = z, card.positions[i + 2] = 0.0f;
        }
        std::vector<rhi::TriangleOpacity> opacity(card.triangleCount());
        std::uint32_t states[3] = {};
        for (std::uint32_t t = 0; t < card.triangleCount(); ++t) {
            float p[3][2];
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t v = card.indices[t * 3 + k];
                p[k][0] = card.positions[v * 3];
                p[k][1] = card.positions[v * 3 + 1];
            }
            opacity[t] = classify(p);
            ++states[static_cast<int>(opacity[t])];
        }
        rt::GpuMesh gpu = rt::upload(dev, card, "omm-card");
        rhi::BlasDesc plainDesc;
        plainDesc.geometries = {gpu.triangles()};
        plainDesc.geometries[0].opaque = false;
        plainDesc.name = "omm-card-plain";
        rhi::BlasDesc ommDesc = plainDesc;
        ommDesc.geometries[0].opacity = opacity;
        ommDesc.name = "omm-card-micromap";

        // Cards scattered in a box in front of the camera, random size and rotation about Y.
        const float boxW = software ? 6.0f : 40.0f, boxH = software ? 4.0f : 12.0f, boxD = software ? 12.0f : 120.0f;
        std::mt19937 rng(static_cast<std::uint32_t>(s.seed));
        std::uniform_real_distribution<float> ux(-boxW, boxW), uy(0.0f, boxH), uz(2.0f, 2.0f + boxD), ua(0.0f, 6.2831853f),
            us(1.0f, 1.6f);
        std::vector<rhi::AccelInstance> instances(cards);
        for (std::uint32_t i = 0; i < cards; ++i) {
            const float a = ua(rng), sc = us(rng), c = std::cos(a) * sc, sn = std::sin(a) * sc;
            const float m[3][4] = {{c, 0, sn, ux(rng)}, {0, sc, 0, uy(rng)}, {-sn, 0, c, uz(rng)}};
            std::memcpy(instances[i].transform, m, sizeof(m));
            instances[i].id = i;
        }
        auto buildScene = [&](rhi::BlasDesc& desc, std::unique_ptr<rhi::AccelerationStructure>& blas,
                              std::unique_ptr<rhi::AccelerationStructure>& tlas) {
            blas = dev.createBlas(desc);
            for (auto& in : instances) in.blas = blas.get();
            tlas = dev.createTlas(cards);
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.buildBlas(blas.get(), desc);
                cmd.accelBarrier();
                cmd.buildTlas(tlas.get(), instances);
                cmd.accelBarrier();
            });
        };

        rt::RayParams p{};
        const float tanHalf = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
        const float eye[4] = {0.0f, boxH * 0.5f, -1.0f, tanHalf}, fwd[4] = {0, 0, 1, float(width) / float(height)},
                    right[4] = {1, 0, 0, 0}, up[4] = {0, 1, 0, 0};
        std::memcpy(p.camPos, eye, 16);
        std::memcpy(p.camFwd, fwd, 16);
        std::memcpy(p.camRight, right, 16);
        std::memcpy(p.camUp, up, 16);
        p.width = width;
        p.height = height;
        p.mode = 0;

        using U = rhi::BufferUsage;
        const std::uint64_t rays = std::uint64_t(width) * height;
        auto hits = dev.createBuffer({rays * 4, U::Storage | U::CopySrc, rhi::MemoryType::Device, 0, "omm-hits"});
        auto counters = dev.createBuffer({256, U::Storage | U::CopySrc | U::CopyDst, rhi::MemoryType::Device, 4, "omm-counters"});
        auto readback = dev.createBuffer({rays * 4 + 256, U::CopyDst, rhi::MemoryType::Readback, 0, "omm-readback"});
        rhi::ComputePipelineDesc pd;
        pd.name = "csAlphaTrace";
        // D3D12 ignores micromaps unless the ray query allows them, which needs the HLSL variant; it
        // runs both passes so they share one shader.
        if (ctx.api() == rhi::Api::D3D12 && omm) pd.compute = {"features/omm_dxr", "csAlphaTrace"};
        else pd.compute = {"features/omm", "csAlphaTrace"};
        pd.layout.bindings = {{rhi::BindingType::AccelerationStructure, 0},
                              {rhi::BindingType::StorageBufferRW, 0},
                              {rhi::BindingType::StorageBufferRW, 1}};
        pd.layout.pushConstantBytes = sizeof(rt::RayParams);
        auto pipe = dev.createComputePipeline(pd);

        struct Result {
            double ms = 0.0;
            double testsPerRay = 0.0;
            std::vector<std::uint32_t> hits;
        };
        auto measure = [&](rhi::AccelerationStructure* tlas, const char* label, double progress) {
            ctx.progress(progress, label);
            auto record = [&](rhi::CommandList& cmd, std::uint32_t) {
                cmd.transition(hits.get(), rhi::ResourceState::UnorderedAccess);
                cmd.transition(counters.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pipe.get());
                cmd.setAccelerationStructure(0, tlas);
                cmd.setStorageBuffer(0, hits.get());
                cmd.setStorageBuffer(1, counters.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch((width + 7) / 8, (height + 7) / 8);
            };
            Result r;
            r.ms = measureMicro(ctx, record).msPerRep;
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.fillBuffer(counters.get(), 0, 4, 0);
                record(cmd, 0);
                cmd.copyBuffer(readback.get(), 0, hits.get(), 0, rays * 4);
                cmd.copyBuffer(readback.get(), rays * 4, counters.get(), 0, 4);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
            const auto* bytes = static_cast<const std::uint8_t*>(readback->mapped());
            r.hits.resize(rays);
            std::memcpy(r.hits.data(), bytes, rays * 4);
            std::uint32_t tests = 0;
            std::memcpy(&tests, bytes + rays * 4, 4);
            r.testsPerRay = double(tests) / double(rays);
            return r;
        };
        auto rate = [&](double ms) { return ms > 0 ? double(rays) / (ms / 1000.0) / 1e9 : 0.0; };

        std::unique_ptr<rhi::AccelerationStructure> plainBlas, plainTlas, ommBlas, ommTlas;
        buildScene(plainDesc, plainBlas, plainTlas);
        const Result plain = measure(plainTlas.get(), "alpha testing every candidate", 0.2);
        ctx.addMetric("plain_grays", "Alpha-tested rays", rate(plain.ms), "Grays/s");
        ctx.addMetric("plain_tests_per_ray", "Alpha tests per ray", plain.testsPerRay, "", false);
        auto& d = ctx.details();
        const std::uint32_t tris = card.triangleCount();
        d["cards"] = cards;
        d["triangles_per_card"] = tris;
        d["opaque_triangles_pct"] = 100.0 * states[1] / tris;
        d["transparent_triangles_pct"] = 100.0 * states[0] / tris;
        d["edge_triangles_pct"] = 100.0 * states[2] / tris;
        std::uint64_t hitCount = 0;
        for (std::uint32_t v : plain.hits) hitCount += v != 0xFFFFFFFFu;
        d["hit_pct"] = 100.0 * double(hitCount) / double(rays);
        if (software) d["note"] = "software renderer: reduced problem size";
        if (!omm) {
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Alpha testing only ({:.1f} tests per ray): this GPU has no opacity micromaps.",
                                       plain.testsPerRay));
            return;
        }

        buildScene(ommDesc, ommBlas, ommTlas);
        const Result withOmm = measure(ommTlas.get(), "with opacity micromaps", 0.6);
        ctx.addMetric("omm_grays", "With opacity micromaps", rate(withOmm.ms), "Grays/s");
        ctx.addMetric("omm_tests_per_ray", "Alpha tests per ray with micromaps", withOmm.testsPerRay, "", false);
        ctx.addMetric("omm_speedup", "Micromap speed-up", withOmm.ms > 0 ? plain.ms / withOmm.ms : 0.0, "x");
        std::uint64_t mismatches = 0;
        for (std::uint64_t i = 0; i < rays; ++i) mismatches += plain.hits[i] != withOmm.hits[i];
        d["mismatches"] = mismatches;
        if (mismatches)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("With opacity micromaps {} of {} rays hit something else.", mismatches, rays));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Identical hits; alpha tests per ray dropped from {:.1f} to {:.1f}.",
                                       plain.testsPerRay, withOmm.testsPerRay));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::OmmTest)
