// Multi-queue overlap (feature.async): how much a GPU gains from running independent work
// on separate queues at the same time.
//  - Async compute: a fill-rate-bound graphics workload (blended full-screen layers) and an
//    ALU-bound compute workload, run back to back and then concurrently on the graphics and
//    compute queues.
//  - Copy overlap: a large host-to-GPU upload on the copy queue concurrently with compute.
// Each resource is only ever used on one queue (no ownership transfers needed).
// Gain = serial time / concurrent time (1.0 = no overlap, 2.0 = perfect overlap of equal halves).

#include "gt/bench/context.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>

namespace gt::bench {
namespace {

struct ThroughputParams {
    std::uint32_t iterations;
    float mulF, addF;
    std::uint32_t mulI, addI, packedB, pad0, pad1;
};

struct RasterParams {
    float color[4];
    std::uint32_t samples;
    float tessFactor, triSize;
    std::uint32_t gridW;
    float texScale[2];
    float lodBias;
    std::uint32_t pad;
};

using Recorder = std::function<void(rhi::CommandList&)>;

class AsyncTest : public Test {
public:
    AsyncTest() {
        m_info.id = "feature.async";
        m_info.name = "Async compute and copy overlap";
        m_info.description = "Runs graphics and compute work, and an upload and compute work, first one after the other "
                             "and then at the same time on separate queues, and reports how much time the overlap saves.";
        m_info.category = Category::Feature;
        m_info.group = "Queues";
        m_info.frameBased = false;
        m_info.primaryMetric = "async_gain";
        m_info.primaryUnit = "x";
        m_info.reference = 0.0; // a ratio: shown, not scored
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 50;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();
        const double targetMs = software ? 15.0 : 25.0;
        using U = rhi::BufferUsage;

        // --- workloads ---------------------------------------------------------------------------
        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA16_FLOAT;
        cd.width = software ? 320 : s.width;
        cd.height = software ? 180 : s.height;
        cd.usage = rhi::TextureUsage::RenderTarget;
        cd.name = "async-color";
        auto color = dev.createTexture(cd);
        rhi::GraphicsPipelineDesc gpd;
        gpd.name = "async-fill";
        gpd.vertex = {"synthetic/raster", "vsFull"};
        gpd.fragment = {"synthetic/raster", "psSolid"};
        gpd.colorFormats = {cd.format};
        gpd.blend = {rhi::BlendAttachment::alpha()};
        gpd.layout.pushConstantBytes = sizeof(RasterParams);
        auto fill = dev.createGraphicsPipeline(gpd);
        const RasterParams rp{{0.02f, 0.03f, 0.04f, 0.1f}, 0, 0, 0, 0, {1, 1}, 0, 0};
        std::uint32_t layers = 8;
        auto graphics = [&](rhi::CommandList& cmd) {
            rhi::RenderPassDesc pass;
            pass.colors.push_back({color.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}});
            cmd.beginRenderPass(pass);
            cmd.setPipeline(fill.get());
            cmd.pushConstants(&rp, sizeof(rp));
            for (std::uint32_t i = 0; i < layers; ++i) cmd.draw(3);
            cmd.endRenderPass();
        };

        auto sink = dev.createBuffer({4096, U::Storage, rhi::MemoryType::Device, 0, "async-sink"});
        rhi::ComputePipelineDesc cpd;
        cpd.name = "async-alu";
        cpd.compute = {"synthetic/throughput", "csFp32"};
        cpd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0}};
        cpd.layout.pushConstantBytes = sizeof(ThroughputParams);
        auto alu = dev.createComputePipeline(cpd);
        const std::uint32_t threads = software ? (1u << 12) : (1u << 20);
        ThroughputParams tp{64, 0.9999f, 0.0001f, 0, 0, 0, 0, 0};
        auto compute = [&](rhi::CommandList& cmd) {
            cmd.transition(sink.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(alu.get());
            cmd.setStorageBuffer(0, sink.get());
            cmd.pushConstants(&tp, sizeof(tp));
            cmd.dispatch(threads / 256);
        };

        const std::uint64_t copyBytes = software ? (16ull << 20) : (256ull << 20);
        auto staging = dev.createBuffer({copyBytes, U::CopySrc, rhi::MemoryType::Upload, 0, "async-staging"});
        auto target = dev.createBuffer({copyBytes, U::CopyDst, rhi::MemoryType::Device, 0, "async-target"});
        std::uint32_t copies = 1;
        auto upload = [&](rhi::CommandList& cmd) {
            for (std::uint32_t i = 0; i < copies; ++i) cmd.copyBuffer(target.get(), 0, staging.get(), 0, copyBytes);
        };

        // --- calibration: each workload alone takes about targetMs -------------------------------
        ctx.progress(0.05, "calibrating");
        auto scale = [&](double ms, std::uint32_t current, std::uint32_t maxValue) {
            return static_cast<std::uint32_t>(std::clamp(double(current) * targetMs / std::max(ms, 0.01), 1.0, double(maxValue)));
        };
        layers = scale(ctx.timeGpu(graphics), layers, 4096);
        tp.iterations = scale(ctx.timeGpu(compute, rhi::QueueType::Compute), tp.iterations, 1u << 20);
        copies = scale(ctx.timeGpu(upload, rhi::QueueType::Copy), copies, 64);
        auto& d = ctx.details();
        d["graphics_layers"] = layers;
        d["compute_iterations"] = tp.iterations;
        d["upload_mb"] = double(copyBytes) * copies / (1 << 20);
        d["async_compute_queue"] = f.asyncComputeQueue;
        d["copy_queue"] = f.copyQueue;

        // --- measurements ------------------------------------------------------------------------
        const std::uint32_t samples = s.duration == DurationPreset::Smoke ? 2 : 5;
        auto median = [&](const std::function<double()>& measure) {
            std::vector<double> t;
            measure(); // warm-up
            for (std::uint32_t i = 0; i < samples; ++i) {
                ctx.checkStop();
                t.push_back(measure());
            }
            std::sort(t.begin(), t.end());
            return t[t.size() / 2];
        };
        struct Pair {
            const char* name;
            rhi::QueueType qa, qb;
            Recorder a, b;
        };
        const Pair pairs[] = {{"async", rhi::QueueType::Graphics, rhi::QueueType::Compute, graphics, compute},
                              {"copy", rhi::QueueType::Copy, rhi::QueueType::Compute, upload, compute}};
        int step = 0;
        for (const Pair& p : pairs) {
            ctx.progress(0.2 + 0.4 * step++, std::format("{} overlap", p.name));
            const double serial = median([&] { return runOn(dev, {{p.qa, p.a}}) + runOn(dev, {{p.qb, p.b}}); });
            const double concurrent = median([&] { return runOn(dev, {{p.qa, p.a}, {p.qb, p.b}}); });
            const double gain = concurrent > 0 ? serial / concurrent : 0.0;
            d[std::format("{}_serial_ms", p.name)] = serial;
            d[std::format("{}_concurrent_ms", p.name)] = concurrent;
            ctx.addMetric(std::format("{}_gain", p.name),
                          p.name[0] == 'a' ? "Async compute overlap gain" : "Copy queue overlap gain", gain, "x");
        }

        std::string msg;
        if (!f.asyncComputeQueue)
            msg = "No separate compute queue: graphics and compute share one queue, so overlap depends on the driver.";
        else
            msg = std::format("Overlap saved {:.0f} % of the serial time for graphics + compute.",
                              100.0 * (1.0 - double(d["async_concurrent_ms"]) / double(d["async_serial_ms"])));
        if (!f.copyQueue) msg += " No dedicated copy queue.";
        ctx.setVerdict(results::Verdict::Pass, msg);
    }

private:
    /// Records each workload into its own list, submits them all, waits; returns the wall time.
    double runOn(rhi::Device& dev, std::vector<std::pair<rhi::QueueType, Recorder>> work) {
        std::vector<std::pair<rhi::QueueType, std::uint64_t>> pending;
        for (std::size_t i = 0; i < work.size(); ++i) {
            auto& list = m_lists[i][static_cast<int>(work[i].first)];
            if (!list) list = dev.createCommandList(work[i].first);
            list->begin();
            work[i].second(*list);
            list->end();
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < work.size(); ++i) {
            rhi::CommandList* l[] = {m_lists[i][static_cast<int>(work[i].first)].get()};
            pending.emplace_back(work[i].first, dev.submit(work[i].first, rhi::SubmitInfo{l}));
        }
        for (const auto& [q, v] : pending) dev.wait(q, v);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    TestInfo m_info;
    std::unique_ptr<rhi::CommandList> m_lists[2][3];
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::AsyncTest)
