// Draw-call overhead (feature.drawcalls): tens of thousands of one-triangle draws into a
// small target, so issuing draws is the bottleneck. Measures the CPU time to record them
// and the GPU time to execute them for: push constants per draw, a uniform buffer per draw
// (a descriptor update each time), alternating pipelines, and one multi-draw indirect call.

#include "gt/bench/context.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace gt::bench {
namespace {

struct DrawParams {
    std::uint32_t drawIndex, mode, gridW, pad;
};

struct IndirectArgs { // VkDrawIndirectCommand / D3D12_DRAW_ARGUMENTS
    std::uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
};

class DrawCallTest : public Test {
public:
    DrawCallTest() {
        m_info.id = "feature.drawcalls";
        m_info.name = "Draw-call overhead";
        m_info.description = "Issues tens of thousands of tiny draws with different per-draw state changes and reports "
                             "how many draws per second the CPU can record and the GPU can execute.";
        m_info.category = Category::Feature;
        m_info.group = "API overhead";
        m_info.frameBased = false;
        m_info.primaryMetric = "push_cpu_mdraws";
        m_info.primaryUnit = "M draws/s";
        m_info.reference = 0.0; // CPU bound: shown, not scored
        m_info.tags = {"cpu"};
        m_info.suites = {"standard", "extreme", "legacy"};
        m_info.order = 90;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t draws = software ? 2000u : (s.duration == DurationPreset::Smoke ? 10000u : 50000u);
        constexpr std::uint32_t kGrid = 256;

        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA8_UNORM;
        cd.width = cd.height = kGrid;
        cd.usage = rhi::TextureUsage::RenderTarget;
        cd.name = "drawcalls-target";
        auto target = dev.createTexture(cd);

        auto pipeline = [&](const char* vs, bool uniform, const char* name) {
            rhi::GraphicsPipelineDesc pd;
            pd.name = name;
            pd.vertex = {"features/drawcalls", vs};
            pd.fragment = {"features/drawcalls", "psDraw"};
            pd.colorFormats = {cd.format};
            pd.layout.pushConstantBytes = sizeof(DrawParams);
            if (uniform) pd.layout.bindings = {{rhi::BindingType::UniformBuffer, 0}};
            return dev.createGraphicsPipeline(pd);
        };
        auto pushPipe = pipeline("vsPush", false, "draws-push");
        auto pushPipe2 = pipeline("vsPush", false, "draws-push-b");
        auto uniformPipe = pipeline("vsUniform", true, "draws-uniform");
        auto indirectPipe = pipeline("vsIndirect", false, "draws-indirect");

        std::unique_ptr<rhi::Buffer> args;
        if (f.multiDrawIndirect) {
            std::vector<IndirectArgs> a(draws);
            for (std::uint32_t i = 0; i < draws; ++i) a[i] = {3, 1, 3 * i, 0};
            args = dev.createBuffer({a.size() * sizeof(IndirectArgs), rhi::BufferUsage::Indirect | rhi::BufferUsage::CopyDst,
                                     rhi::MemoryType::Device, 0, "drawcalls-args"},
                                    a.data());
        }

        enum Variant { Push, Uniform, Pipelines, Indirect };
        const char* names[] = {"push", "uniform", "pipelines", "indirect"};
        const char* labels[] = {"Push constants per draw", "Uniform buffer per draw", "Alternating pipelines",
                                "Multi-draw indirect"};
        auto record = [&](rhi::CommandList& cmd, Variant v) {
            rhi::RenderPassDesc rp;
            rp.colors.push_back({target.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}});
            if (v == Indirect) cmd.transition(args.get(), rhi::ResourceState::IndirectArgument);
            cmd.beginRenderPass(rp);
            DrawParams p{0, 0, kGrid, 0};
            switch (v) {
            case Push:
                cmd.setPipeline(pushPipe.get());
                for (std::uint32_t i = 0; i < draws; ++i) {
                    p.drawIndex = i;
                    cmd.pushConstants(&p, sizeof(p));
                    cmd.draw(3);
                }
                break;
            case Uniform:
                cmd.setPipeline(uniformPipe.get());
                cmd.pushConstants(&p, sizeof(p));
                for (std::uint32_t i = 0; i < draws; ++i) {
                    const std::uint32_t perDraw[4] = {i, 0, 0, 0};
                    cmd.setUniformData(0, perDraw, sizeof(perDraw));
                    cmd.draw(3);
                }
                break;
            case Pipelines:
                for (std::uint32_t i = 0; i < draws; ++i) {
                    cmd.setPipeline((i & 1) ? pushPipe2.get() : pushPipe.get());
                    p.drawIndex = i;
                    cmd.pushConstants(&p, sizeof(p));
                    cmd.draw(3);
                }
                break;
            case Indirect:
                cmd.setPipeline(indirectPipe.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.drawIndirect(args.get(), 0, draws, sizeof(IndirectArgs));
                break;
            }
            cmd.endRenderPass();
        };

        auto& d = ctx.details();
        const std::uint32_t samples = s.duration == DurationPreset::Smoke ? 2 : 5;
        for (Variant v : {Push, Uniform, Pipelines, Indirect}) {
            if (v == Indirect && !args) continue;
            ctx.progress(0.1 + 0.22 * static_cast<int>(v), labels[v]);
            std::vector<double> cpu, gpu;
            for (std::uint32_t i = 0; i <= samples; ++i) { // first round warms up
                ctx.checkStop();
                double cpuMs = 0.0;
                const double gpuMs = ctx.timeGpu([&](rhi::CommandList& cmd) {
                    const auto t0 = std::chrono::steady_clock::now();
                    record(cmd, v);
                    cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                });
                if (i == 0) continue;
                cpu.push_back(cpuMs);
                gpu.push_back(gpuMs);
            }
            std::sort(cpu.begin(), cpu.end());
            std::sort(gpu.begin(), gpu.end());
            const double cpuMs = cpu[cpu.size() / 2], gpuMs = gpu[gpu.size() / 2];
            auto rate = [&](double ms) { return ms > 0 ? double(draws) / (ms / 1000.0) / 1e6 : 0.0; };
            ctx.addMetric(std::format("{}_cpu_mdraws", names[v]), std::format("{} (CPU)", labels[v]), rate(cpuMs), "M draws/s");
            ctx.addMetric(std::format("{}_gpu_mdraws", names[v]), std::format("{} (GPU)", labels[v]), rate(gpuMs), "M draws/s");
            d[std::format("{}_cpu_ms", names[v])] = cpuMs;
            d[std::format("{}_gpu_ms", names[v])] = gpuMs;
        }
        d["draws"] = draws;
        if (!args) d["note_indirect"] = "no multi-draw indirect on this GPU";
        if (software) d["note"] = "software renderer: reduced problem size";
        ctx.setVerdict(results::Verdict::Pass,
                       std::format("{} draws recorded in {:.2f} ms with push constants ({:.2f} ms with a uniform buffer "
                                   "per draw).",
                                   draws, double(d["push_cpu_ms"]), double(d["uniform_cpu_ms"])));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::DrawCallTest)
