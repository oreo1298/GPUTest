// BVH build and refit (feature.rt.bvh): how fast the driver builds bottom-level
// acceleration structures for a million-triangle height field (fast-trace and fast-build
// flavours), refits one after the vertices move, and builds / updates a top-level
// structure over many instances. Probe rays traced into the structures after the build
// and after the refit are checked against a CPU reference computed from the GPU's own
// vertex data, so a driver that refits incorrectly fails the test.

#include "rt_common.h"

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <cmath>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

struct GridParams {
    std::uint32_t cells;
    float size;
    float time;
    float amplitude;
    float probeHeight;
    std::uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(GridParams) == 32);

constexpr float kGridSize = 64.0f;
constexpr float kAmplitude = 1.5f;
constexpr float kProbeHeight = 10.0f;

class BvhTest : public Test {
public:
    BvhTest() {
        m_info.id = "feature.rt.bvh";
        m_info.name = "BVH build and refit";
        m_info.description = "Builds ray-tracing acceleration structures for a million-triangle mesh, refits them after "
                             "the mesh deforms, and builds a scene of many instances; probe rays check the results.";
        m_info.category = Category::Feature;
        m_info.group = "Ray tracing";
        m_info.frameBased = false;
        m_info.primaryMetric = "build_mtris";
        m_info.primaryUnit = "Mtri/s";
        m_info.reference = 1000.0; // provisional (docs/SCORING.md)
        m_info.minVramMB = 512;
        m_info.tags = {"rt"};
        m_info.suites = {"standard", "extreme", "raytracing"};
        m_info.order = 10;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().rayQuery ? SupportResult::yes()
                                                : SupportResult::no("no ray queries (VK_KHR_ray_query / DXR 1.1)");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const bool smoke = s.duration == DurationPreset::Smoke;
        const std::uint32_t cells = software ? (smoke ? 32u : 48u) : (smoke ? 256u : 724u);
        const std::uint32_t instanceCount = software ? 256u : (smoke ? 4096u : 65536u);
        const rt::Mesh mesh = rt::grid(cells, kGridSize);
        rt::GpuMesh gpu = rt::upload(dev, mesh, "bvh-grid");
        const std::uint32_t tris = mesh.triangleCount();
        const std::uint32_t probeCount = cells * cells;

        using U = rhi::BufferUsage;
        const std::uint64_t vertexBytes = mesh.positions.size() * sizeof(float);
        const std::uint64_t probeBytes = std::uint64_t(probeCount) * 8;
        auto probes = dev.createBuffer({probeBytes, U::Storage | U::CopySrc, rhi::MemoryType::Device, 0, "bvh-probes"});
        auto readback = dev.createBuffer({vertexBytes + probeBytes, U::CopyDst, rhi::MemoryType::Readback, 0, "bvh-readback"});

        auto computePipe = [&](const char* entry, std::vector<rhi::Binding> bindings) {
            rhi::ComputePipelineDesc pd;
            pd.name = entry;
            pd.compute = {"features/bvh", entry};
            pd.layout.bindings = std::move(bindings);
            pd.layout.pushConstantBytes = sizeof(GridParams);
            return dev.createComputePipeline(pd);
        };
        auto displace = computePipe("csDisplace", {{rhi::BindingType::StorageBufferRW, 0}});
        auto probe = computePipe("csProbe", {{rhi::BindingType::StorageBufferRW, 1}, {rhi::BindingType::AccelerationStructure, 0}});

        rhi::BlasDesc staticDesc;
        staticDesc.geometries = {gpu.triangles()};
        staticDesc.name = "blas-fast-trace";
        rhi::BlasDesc dynamicDesc = staticDesc;
        dynamicDesc.allowUpdate = true;
        dynamicDesc.fastBuild = true;
        dynamicDesc.name = "blas-dynamic";
        auto staticBlas = dev.createBlas(staticDesc);
        auto dynamicBlas = dev.createBlas(dynamicDesc);
        auto probeTlas = dev.createTlas(1);

        auto runDisplace = [&](rhi::CommandList& cmd, float time) {
            const GridParams p{cells, kGridSize, time, kAmplitude, kProbeHeight, 0, 0, 0};
            cmd.transition(gpu.vertices.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(displace.get());
            cmd.setStorageBuffer(0, gpu.vertices.get());
            cmd.pushConstants(&p, sizeof(p));
            cmd.dispatch(((cells + 1) * (cells + 1) + 63) / 64);
            cmd.uavBarrier();
        };

        // --- build timings -----------------------------------------------------------------------
        ctx.progress(0.05, "building (fast trace)");
        ctx.timeGpu([&](rhi::CommandList& cmd) { runDisplace(cmd, 0.0f); });
        const MicroResult buildStatic =
            measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.buildBlas(staticBlas.get(), staticDesc); });
        ctx.progress(0.25, "building (fast build)");
        const MicroResult buildDynamic =
            measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.buildBlas(dynamicBlas.get(), dynamicDesc); });

        // --- correctness after a full build -------------------------------------------------------
        ctx.progress(0.4, "checking probe rays");
        const ProbeCheck afterBuild = check(ctx, dev, staticBlas.get(), probeTlas.get(), probe.get(), gpu, probes.get(),
                                            readback.get(), cells);

        // --- refit -------------------------------------------------------------------------------
        ctx.progress(0.5, "refitting");
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            runDisplace(cmd, 1.7f);
            cmd.buildBlas(dynamicBlas.get(), dynamicDesc, true);
        });
        const MicroResult refit = measureMicro(
            ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.buildBlas(dynamicBlas.get(), dynamicDesc, true); });
        // Testing hook: move the vertices again without refitting, so the probes must fail.
        if (s.param("feature.rt.bvh.inject") == "1") ctx.timeGpu([&](rhi::CommandList& cmd) { runDisplace(cmd, 2.9f); });
        const ProbeCheck afterRefit = check(ctx, dev, dynamicBlas.get(), probeTlas.get(), probe.get(), gpu,
                                            probes.get(), readback.get(), cells);

        // --- top level ---------------------------------------------------------------------------
        ctx.progress(0.7, "building the top level");
        auto tlas = dev.createTlas(instanceCount, true);
        std::vector<rhi::AccelInstance> instances(instanceCount);
        const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(double(instanceCount))));
        auto place = [&](float phase) {
            for (std::uint32_t i = 0; i < instanceCount; ++i) {
                rhi::AccelInstance& in = instances[i];
                const float x = float(i % side) * kGridSize * 1.1f, z = float(i / side) * kGridSize * 1.1f;
                rt::scaleTranslate(in.transform, 1.0f, x, std::sin(phase + float(i) * 0.37f), z);
                in.id = i;
                in.blas = (i & 1) ? dynamicBlas.get() : staticBlas.get();
            }
        };
        place(0.0f);
        // Instance records go through per-list upload memory: bound the repetitions per submission.
        const auto tlasReps = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (16ull << 20) / (std::uint64_t(instanceCount) * 64)));
        const MicroResult tlasBuild = measureMicro(
            ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.buildTlas(tlas.get(), instances); }, 10.0,
            rhi::QueueType::Graphics, tlasReps);
        place(0.5f);
        const MicroResult tlasUpdate = measureMicro(
            ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.buildTlas(tlas.get(), instances, true); }, 10.0,
            rhi::QueueType::Graphics, tlasReps);

        // --- results -----------------------------------------------------------------------------
        auto rate = [](double count, double ms) { return ms > 0 ? count / (ms / 1000.0) / 1e6 : 0.0; };
        ctx.addMetric("build_mtris", "BLAS build (fast trace)", rate(tris, buildStatic.msPerRep), "Mtri/s");
        ctx.addMetric("build_fast_mtris", "BLAS build (fast build)", rate(tris, buildDynamic.msPerRep), "Mtri/s");
        ctx.addMetric("refit_mtris", "BLAS refit", rate(tris, refit.msPerRep), "Mtri/s");
        ctx.addMetric("refit_speedup", "Refit vs fast-build rebuild",
                      refit.msPerRep > 0 ? buildDynamic.msPerRep / refit.msPerRep : 0.0, "x");
        ctx.addMetric("tlas_minst", "TLAS build", rate(instanceCount, tlasBuild.msPerRep), "M instances/s");
        ctx.addMetric("tlas_update_minst", "TLAS update", rate(instanceCount, tlasUpdate.msPerRep), "M instances/s");
        ctx.addMetric("blas_bytes_per_tri", "BLAS size (fast trace)", double(staticBlas->sizeBytes()) / tris, "B/tri", false);
        ctx.addMetric("blas_dynamic_bytes_per_tri", "BLAS size (fast build, updatable)",
                      double(dynamicBlas->sizeBytes()) / tris, "B/tri", false);
        auto& d = ctx.details();
        d["triangles"] = tris;
        d["instances"] = instanceCount;
        d["build_ms"] = buildStatic.msPerRep;
        d["build_fast_ms"] = buildDynamic.msPerRep;
        d["refit_ms"] = refit.msPerRep;
        d["tlas_build_ms"] = tlasBuild.msPerRep;
        d["tlas_update_ms"] = tlasUpdate.msPerRep;
        d["blas_mb"] = double(staticBlas->sizeBytes()) / (1 << 20);
        d["blas_dynamic_mb"] = double(dynamicBlas->sizeBytes()) / (1 << 20);
        d["scratch_mb"] = double(staticBlas->scratchBytes()) / (1 << 20);
        d["tlas_mb"] = double(tlas->sizeBytes()) / (1 << 20);
        d["probe_rays"] = probeCount;
        d["probe_mismatches_build"] = afterBuild.mismatches;
        d["probe_mismatches_refit"] = afterRefit.mismatches;
        if (!afterBuild.example.empty()) d["probe_example_build"] = afterBuild.example;
        if (!afterRefit.example.empty()) d["probe_example_refit"] = afterRefit.example;
        if (software) d["note"] = "software renderer: reduced problem size";

        if (afterBuild.mismatches || afterRefit.mismatches)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("Probe rays hit the wrong place: {} of {} after the build, {} after the refit.",
                                       afterBuild.mismatches, probeCount, afterRefit.mismatches));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("All {} probe rays hit the right triangle at the right distance after both the "
                                       "build and the refit.",
                                       probeCount));
    }

private:
    struct ProbeCheck {
        std::uint32_t mismatches = 0;
        std::string example;
    };

    /// Traces one ray down onto every cell through a one-instance TLAS over `blas` and compares
    /// with the analytic hit on the vertices currently in the GPU buffer.
    static ProbeCheck check(TestContext& ctx, rhi::Device&, rhi::AccelerationStructure* blas,
                            rhi::AccelerationStructure* tlas, rhi::Pipeline* probe, rt::GpuMesh& gpu, rhi::Buffer* probes,
                            rhi::Buffer* readback, std::uint32_t cells) {
        const std::uint64_t vertexBytes = std::uint64_t(gpu.vertexCount) * 12;
        const std::uint32_t count = cells * cells;
        rhi::AccelInstance inst;
        inst.blas = blas;
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            cmd.buildTlas(tlas, {&inst, 1});
            cmd.accelBarrier();
            const GridParams p{cells, kGridSize, 0.0f, kAmplitude, kProbeHeight, 0, 0, 0};
            cmd.transition(probes, rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(probe);
            cmd.setStorageBuffer(1, probes);
            cmd.setAccelerationStructure(0, tlas);
            cmd.pushConstants(&p, sizeof(p));
            cmd.dispatch((count + 63) / 64);
            cmd.copyBuffer(readback, 0, gpu.vertices.get(), 0, vertexBytes);
            cmd.copyBuffer(readback, vertexBytes, probes, 0, std::uint64_t(count) * 8);
            cmd.transition(readback, rhi::ResourceState::HostRead);
        });
        const auto* bytes = static_cast<const std::uint8_t*>(readback->mapped());
        std::vector<float> v(gpu.vertexCount * 3);
        std::memcpy(v.data(), bytes, vertexBytes);
        std::vector<std::uint32_t> hit(std::size_t(count) * 2);
        std::memcpy(hit.data(), bytes + vertexBytes, std::size_t(count) * 8);

        ProbeCheck r;
        const std::uint32_t n = cells + 1;
        for (std::uint32_t c = 0; c < count; ++c) {
            const std::uint32_t cx = c % cells, cz = c / cells;
            // The probe at (0.37, 0.61) inside the cell lies in triangle 2c+1 = (v00, v01, v11):
            // barycentrics (1 - w, w - u, u) for local coordinates u = 0.37 (x), w = 0.61 (z).
            const float u = 0.37f, w = 0.61f;
            const std::uint32_t v00 = cz * n + cx, v01 = v00 + n, v11 = v01 + 1;
            const float y = (1 - w) * v[v00 * 3 + 1] + (w - u) * v[v01 * 3 + 1] + u * v[v11 * 3 + 1];
            const float expectT = kProbeHeight - y;
            float t;
            std::memcpy(&t, &hit[c * 2], 4);
            const std::uint32_t prim = hit[c * 2 + 1];
            const bool ok = prim == 2 * c + 1 && std::fabs(t - expectT) <= 1e-3f * kProbeHeight;
            if (!ok) {
                if (r.example.empty())
                    r.example = std::format("cell {}: expected triangle {} at t={:.4f}, got {} at t={:.4f}", c, 2 * c + 1,
                                            expectT, prim == 0xFFFFFFFFu ? std::string("miss") : std::to_string(prim), t);
                ++r.mismatches;
            }
        }
        return r;
    }

    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::BvhTest)
