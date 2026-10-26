// Shader execution reordering (feature.rt.ser): incoherent rays into the sphere field with
// eight closest-hit "materials" of different cost (shaders/src/features/ser.slang), traced
// once plainly and once with HitObject + ReorderThread grouping threads by material.
// Reports the speed-up and checks that both produce bit-identical shading.
// --param feature.rt.ser.baseline_only=1 runs only the plain variant (CI coverage on GPUs
// without SER).

#include "rt_common.h"

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <cstring>
#include <format>

namespace gt::bench {
namespace {

class SerTest : public Test {
public:
    SerTest() {
        m_info.id = "feature.rt.ser";
        m_info.name = "Shader execution reordering";
        m_info.description = "Shades incoherent ray hits with eight materials of different cost, with and without "
                             "reordering threads by material before shading, and checks both give the same image.";
        m_info.category = Category::Feature;
        m_info.group = "Ray tracing";
        m_info.frameBased = false;
        m_info.primaryMetric = "ser_speedup";
        m_info.primaryUnit = "x";
        m_info.reference = 0.0; // a ratio: shown, not scored
        m_info.tags = {"rt"};
        m_info.suites = {"standard", "extreme", "raytracing"};
        m_info.order = 30;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& f = ctx.device().features();
        if (!f.rayTracingPipeline) return SupportResult::no("no ray-tracing pipelines");
        if (f.shaderExecutionReordering || ctx.settings().param("feature.rt.ser.baseline_only") == "1")
            return SupportResult::yes();
        if (f.shaderExecutionReorderingNv)
            return SupportResult::no("only NVIDIA's own SER extension is available; the test uses the cross-vendor "
                                     "VK_EXT_ray_tracing_invocation_reorder (update the driver)");
        return SupportResult::no("no shader execution reordering (VK_EXT_ray_tracing_invocation_reorder)");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const bool ser = dev.features().shaderExecutionReordering;
        const std::uint32_t width = software ? 160 : s.width, height = software ? 90 : s.height;
        rt::SphereField field = rt::buildSphereField(dev, software ? 6 : 32, software ? 2 : 4, s.seed, width, height,
                                                     [&](const auto& record) { ctx.timeGpu(record); });
        rt::RayParams p = field.params;
        p.mode = 1; // incoherent rays: neighbouring threads hit different materials

        using U = rhi::BufferUsage;
        const std::uint64_t rays = std::uint64_t(width) * height;
        auto out = dev.createBuffer({rays * 4, U::Storage | U::CopySrc, rhi::MemoryType::Device, 0, "ser-out"});
        auto readback = dev.createBuffer({rays * 4, U::CopyDst, rhi::MemoryType::Readback, 0, "ser-readback"});
        auto pipeline = [&](const char* rayGen) {
            rhi::RayTracingPipelineDesc pd;
            pd.name = rayGen;
            pd.rayGen = {"features/ser", rayGen};
            pd.miss = {{"features/ser", "msShade"}};
            pd.hitGroups = {{{"features/ser", "chShade"}, {}, {}}};
            pd.layout.bindings = {{rhi::BindingType::AccelerationStructure, 0}, {rhi::BindingType::StorageBufferRW, 0}};
            pd.layout.pushConstantBytes = sizeof(rt::RayParams);
            return dev.createRayTracingPipeline(pd);
        };

        struct Result {
            double ms = 0.0;
            std::vector<std::uint32_t> values;
        };
        auto measure = [&](rhi::Pipeline* pipe, const char* label, double progress) {
            ctx.progress(progress, label);
            auto record = [&](rhi::CommandList& cmd, std::uint32_t) {
                cmd.transition(out.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pipe);
                cmd.setAccelerationStructure(0, field.tlas.get());
                cmd.setStorageBuffer(0, out.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.traceRays(width, height);
            };
            Result r;
            r.ms = measureMicro(ctx, record).msPerRep;
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                record(cmd, 0);
                cmd.copyBuffer(readback.get(), 0, out.get(), 0, rays * 4);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
            r.values.resize(rays);
            std::memcpy(r.values.data(), readback->mapped(), rays * 4);
            return r;
        };
        auto plainPipe = pipeline("rgShade");
        const Result plain = measure(plainPipe.get(), "shading without reordering", 0.2);
        auto rate = [&](double ms) { return ms > 0 ? double(rays) / (ms / 1000.0) / 1e9 : 0.0; };
        ctx.addMetric("plain_grays", "Without reordering", rate(plain.ms), "Grays/s");
        auto& d = ctx.details();
        d["rays"] = rays;
        d["materials"] = 8;
        if (software) d["note"] = "software renderer: reduced problem size";
        if (!ser) {
            ctx.setVerdict(results::Verdict::Pass, "Baseline only: this GPU has no shader execution reordering.");
            return;
        }

        auto serPipe = pipeline("rgShadeSer");
        const Result reordered = measure(serPipe.get(), "shading with reordering", 0.6);
        ctx.addMetric("ser_grays", "With reordering", rate(reordered.ms), "Grays/s");
        ctx.addMetric("ser_speedup", "Reordering speed-up", reordered.ms > 0 ? plain.ms / reordered.ms : 0.0, "x");
        std::uint64_t mismatches = 0;
        for (std::uint64_t i = 0; i < rays; ++i) mismatches += plain.values[i] != reordered.values[i];
        d["mismatches"] = mismatches;
        if (mismatches)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("Reordered shading differed from plain shading on {} of {} rays.", mismatches, rays));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Reordering changed the speed ({:.2f}x) but not a single shaded value.",
                                       reordered.ms > 0 ? plain.ms / reordered.ms : 0.0));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::SerTest)
