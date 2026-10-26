// Ray throughput (feature.rt.rays): a field of instanced spheres on a ground plane traced
// with coherent camera rays and with incoherent rays (random origins and directions, like
// diffuse bounces), once with inline ray queries from a compute shader and once with a
// ray-tracing pipeline. Both methods trace identical rays, so their per-ray results must
// agree; a disagreement points at a driver bug.

#include "rt_common.h"

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <random>

namespace gt::bench {
namespace {

constexpr std::uint32_t kMiss = 0xFFFFFFFFu;

class RaysTest : public Test {
public:
    RaysTest() {
        m_info.id = "feature.rt.rays";
        m_info.name = "Ray throughput";
        m_info.description = "Traces coherent camera rays and incoherent bounce-like rays through a scene of instanced "
                             "spheres, with inline ray queries and with a ray-tracing pipeline, and checks both agree.";
        m_info.category = Category::Feature;
        m_info.group = "Ray tracing";
        m_info.frameBased = false;
        m_info.primaryMetric = "incoherent";
        m_info.primaryUnit = "Grays/s";
        m_info.reference = 5.0; // provisional (docs/SCORING.md)
        m_info.tags = {"rt"};
        m_info.suites = {"quick", "standard", "extreme", "raytracing"};
        m_info.order = 20;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& f = ctx.device().features();
        return f.rayQuery || f.rayTracingPipeline ? SupportResult::yes() : SupportResult::no("no hardware ray tracing");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t width = software ? 160 : s.width, height = software ? 90 : s.height;
        const int side = software ? 6 : 32;  // spheres per row
        const int subdiv = software ? 2 : 4; // 320 / 5120 triangles per sphere
        rt::SphereField field = rt::buildSphereField(dev, side, subdiv, s.seed, width, height,
                                                     [&](const auto& record) { ctx.timeGpu(record); });
        rhi::AccelerationStructure* tlas = field.tlas.get();

        using U = rhi::BufferUsage;
        const std::uint64_t rays = std::uint64_t(width) * height;
        auto hits = dev.createBuffer({rays * 4, U::Storage | U::CopySrc, rhi::MemoryType::Device, 0, "rays-hits"});
        auto readback = dev.createBuffer({rays * 4, U::CopyDst, rhi::MemoryType::Readback, 0, "rays-readback"});

        rhi::LayoutDesc layout;
        layout.bindings = {{rhi::BindingType::AccelerationStructure, 0}, {rhi::BindingType::StorageBufferRW, 0}};
        layout.pushConstantBytes = sizeof(rt::RayParams);
        std::unique_ptr<rhi::Pipeline> queryPipe, rtPipe;
        if (f.rayQuery) {
            rhi::ComputePipelineDesc pd;
            pd.name = "csRayQuery";
            pd.compute = {"features/raytracing", "csRayQuery"};
            pd.layout = layout;
            queryPipe = dev.createComputePipeline(pd);
        }
        if (f.rayTracingPipeline) {
            rhi::RayTracingPipelineDesc pd;
            pd.name = "rays-pipeline";
            pd.rayGen = {"features/raytracing", "rgRays"};
            pd.miss = {{"features/raytracing", "msRays"}};
            pd.hitGroups = {{{"features/raytracing", "chRays"}, {}, {}}};
            pd.layout = layout;
            rtPipe = dev.createRayTracingPipeline(pd);
        }

        rt::RayParams p = field.params;

        struct Run {
            const char* method;
            std::uint32_t mode;
            double graysPerSec = 0.0;
            std::vector<std::uint32_t> result;
        };
        std::vector<Run> runs;
        for (const char* method : {"query", "pipeline"}) {
            rhi::Pipeline* pipe = method[0] == 'q' ? queryPipe.get() : rtPipe.get();
            if (!pipe) continue;
            for (std::uint32_t mode : {0u, 1u}) {
                ctx.progress(0.1 + 0.2 * double(runs.size()),
                             std::format("{} rays, {}", mode ? "incoherent" : "coherent",
                                         pipe == queryPipe.get() ? "ray query" : "pipeline"));
                p.mode = mode;
                auto record = [&](rhi::CommandList& cmd, std::uint32_t) {
                    cmd.transition(hits.get(), rhi::ResourceState::UnorderedAccess);
                    cmd.setPipeline(pipe);
                    cmd.setAccelerationStructure(0, tlas);
                    cmd.setStorageBuffer(0, hits.get());
                    cmd.pushConstants(&p, sizeof(p));
                    if (pipe == queryPipe.get())
                        cmd.dispatch((width + 7) / 8, (height + 7) / 8);
                    else
                        cmd.traceRays(width, height);
                };
                const MicroResult m = measureMicro(ctx, record);
                Run r{method, mode};
                r.graysPerSec = m.msPerRep > 0 ? double(rays) / (m.msPerRep / 1000.0) / 1e9 : 0.0;
                ctx.timeGpu([&](rhi::CommandList& cmd) {
                    record(cmd, 0);
                    cmd.copyBuffer(readback.get(), 0, hits.get(), 0, rays * 4);
                    cmd.transition(readback.get(), rhi::ResourceState::HostRead);
                });
                r.result.resize(rays);
                std::memcpy(r.result.data(), readback->mapped(), rays * 4);
                if (s.dumpFrames && mode == 0) dumpHits(s, r.method, width, height, r.result);
                runs.push_back(std::move(r));
            }
        }

        // --- results -----------------------------------------------------------------------------
        auto& d = ctx.details();
        double best[2] = {0.0, 0.0};
        for (const Run& r : runs) {
            const char* modeName = r.mode ? "incoherent" : "coherent";
            ctx.addMetric(std::format("{}_{}", r.method, modeName),
                          std::format("{} rays ({})", r.mode ? "Incoherent" : "Coherent",
                                      r.method[0] == 'q' ? "ray query" : "pipeline"),
                          r.graysPerSec, "Grays/s");
            best[r.mode] = std::max(best[r.mode], r.graysPerSec);
            std::uint64_t hitCount = 0;
            for (std::uint32_t v : r.result) hitCount += v != kMiss;
            d[std::format("hit_pct_{}_{}", r.method, modeName)] = 100.0 * double(hitCount) / double(rays);
        }
        ctx.addMetric("coherent", "Coherent rays (best)", best[0], "Grays/s");
        ctx.addMetric("incoherent", "Incoherent rays (best)", best[1], "Grays/s");
        ctx.addMetric("coherence_ratio", "Coherent vs incoherent", best[1] > 0 ? best[0] / best[1] : 0.0, "x", false);
        d["rays_per_launch"] = rays;
        d["width"] = width;
        d["height"] = height;
        d["instances"] = field.instances;
        d["triangles_per_sphere"] = field.trianglesPerSphere;
        d["methods"] = queryPipe && rtPipe ? "ray query + pipeline" : queryPipe ? "ray query" : "pipeline";
        if (software) d["note"] = "software renderer: reduced problem size";

        // Coherent camera rays must mostly hit something: the camera looks down onto the field.
        std::uint64_t mismatches = 0;
        std::string example;
        for (std::uint32_t mode : {0u, 1u}) {
            const Run* a = nullptr;
            const Run* b = nullptr;
            for (const Run& r : runs)
                if (r.mode == mode) (a ? b : a) = &r;
            if (!a || !b) continue;
            for (std::uint64_t i = 0; i < rays; ++i)
                if (a->result[i] != b->result[i]) {
                    if (example.empty())
                        example = std::format("{} ray {}: ray query {:08x}, pipeline {:08x}", mode ? "incoherent" : "coherent",
                                              i, a->result[i], b->result[i]);
                    ++mismatches;
                }
        }
        d["disagreements"] = mismatches;
        if (!example.empty()) d["disagreement_example"] = example;
        const double coherentHit = d.value(std::format("hit_pct_{}_coherent", runs.front().method), 0.0);
        const double allowed = 1e-4 * double(rays) * 2; // edge hits may legitimately differ in rare cases
        if (coherentHit < 50.0)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("Only {:.1f} % of camera rays hit the scene (expected most): the acceleration "
                                       "structure or traversal is broken.",
                                       coherentHit));
        else if (double(mismatches) > allowed)
            ctx.setVerdict(results::Verdict::Warn,
                           std::format("Ray queries and the ray-tracing pipeline disagreed on {} rays ({}).", mismatches, example));
        else if (queryPipe && rtPipe)
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Ray queries and the ray-tracing pipeline returned identical hits{}.",
                                       mismatches ? std::format(" (except {} edge rays)", mismatches) : std::string()));
        else
            ctx.setVerdict(results::Verdict::Pass, queryPipe ? "Ray queries only: this GPU has no ray-tracing pipelines."
                                                             : "Ray-tracing pipeline only: this GPU has no ray queries.");
    }

private:
    /// Writes the camera-ray hits as an image (one colour per instance, shaded by triangle).
    static void dumpHits(const RunSettings& s, const char* method, std::uint32_t w, std::uint32_t h,
                         const std::vector<std::uint32_t>& hits) {
        std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < hits.size(); ++i) {
            std::uint8_t* px = &rgba[i * 4];
            px[3] = 255;
            if (hits[i] == kMiss) {
                px[0] = 120, px[1] = 150, px[2] = 200;
                continue;
            }
            const std::uint32_t inst = hits[i] >> 20, prim = hits[i] & 0xFFFFFu;
            const std::uint32_t c = inst ? inst * 2654435761u : 0x808080u;
            const float shade = 0.75f + 0.25f * float(prim % 7) / 6.0f;
            for (int k = 0; k < 3; ++k) px[k] = static_cast<std::uint8_t>(float((c >> (8 * k)) & 0xFF) * shade);
        }
        writePng(std::format("{}/feature.rt.rays_{}.png", s.dumpDir, method), w, h, rgba.data());
    }

    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::RaysTest)
