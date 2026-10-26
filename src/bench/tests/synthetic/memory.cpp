// Memory-system micro-benchmarks (shaders/src/synthetic/memory.slang):
//  - synth.mem.bandwidth: streaming read / write / copy over a buffer larger than any
//    GPU cache (copy counts bytes read + written).
//  - synth.mem.latency: single-thread pointer chase over working sets from 4 KiB up;
//    the latency curve's plateaus approximate the cache hierarchy.
//  - synth.mem.atomics: 32-bit atomic adds on one hot address and spread over 64 Ki
//    addresses, plus 64-bit atomics where supported.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"
#include "gt/diagnostics.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <random>

namespace gt::bench {
namespace {

struct MemoryParams {
    std::uint32_t elements, threads, start, spread;
};
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::unique_ptr<rhi::Pipeline> memoryPipeline(rhi::Device& dev, const char* entry, std::vector<rhi::Binding> bindings) {
    rhi::ComputePipelineDesc pd;
    pd.name = entry;
    pd.compute = {"synthetic/memory", entry};
    pd.layout.bindings = std::move(bindings);
    pd.layout.pushConstantBytes = sizeof(MemoryParams);
    return dev.createComputePipeline(pd);
}

std::string formatSize(std::uint64_t bytes) {
    if (bytes >= kMiB) return std::format("{} MiB", bytes / kMiB);
    return std::format("{} KiB", bytes / 1024);
}

// --- bandwidth -----------------------------------------------------------------------------
class BandwidthTest : public Test {
public:
    BandwidthTest() {
        m_info.id = "synth.mem.bandwidth";
        m_info.name = "Memory bandwidth";
        m_info.description = "Streams through a buffer larger than the GPU's caches to measure graphics-memory read, "
                             "write and copy bandwidth.";
        m_info.category = Category::Synthetic;
        m_info.group = "Memory";
        m_info.frameBased = false;
        m_info.primaryMetric = "read_gbs";
        m_info.primaryUnit = "GB/s";
        m_info.reference = 700.0;
        m_info.tags = {"memory"};
        m_info.suites = {"quick", "standard", "extreme", "compute"};
        m_info.order = 20;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        std::uint64_t bytes = 512 * kMiB;
        if (software || s.duration == DurationPreset::Smoke) bytes = 32 * kMiB;
        else if (dev.adapter().type == rhi::AdapterType::Discrete && dev.adapter().dedicatedMemory < 4096 * kMiB) bytes = 256 * kMiB;
        const auto elements = static_cast<std::uint32_t>(bytes / 16);
        const std::uint32_t threads = std::clamp<std::uint32_t>(elements / 16, 256, 65535u * 256u) / 256u * 256u;
        auto a = dev.createBuffer({bytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "bw-a"});
        auto b = dev.createBuffer({bytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "bw-b"});
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            cmd.fillBuffer(a.get(), 0, bytes, 0x3F800000u);
            cmd.fillBuffer(b.get(), 0, bytes, 0);
        });
        const std::vector<rhi::Binding> bind{{rhi::BindingType::StorageBufferRW, 0}, {rhi::BindingType::StorageBufferRW, 1}};
        struct Kernel {
            const char* entry;
            const char* metric;
            const char* label;
            double bytesMoved;
        };
        const Kernel kernels[] = {{"csRead", "read_gbs", "Read bandwidth", 1.0},
                                  {"csWrite", "write_gbs", "Write bandwidth", 1.0},
                                  {"csCopy", "copy_gbs", "Copy bandwidth (read + write)", 2.0}};
        int done = 0;
        for (const Kernel& k : kernels) {
            auto pipe = memoryPipeline(dev, k.entry, bind);
            const MemoryParams p{elements, threads, 0, 0};
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                cmd.transition(a.get(), rhi::ResourceState::UnorderedAccess);
                cmd.transition(b.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pipe.get());
                cmd.setStorageBuffer(0, b.get()); // dst
                cmd.setStorageBuffer(1, a.get()); // src
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch(threads / 256);
            });
            const double gbs = m.msPerRep > 0 ? static_cast<double>(bytes) * k.bytesMoved / (m.msPerRep / 1000.0) / 1e9 : 0;
            ctx.addMetric(k.metric, k.label, gbs, "GB/s");
            ctx.progress(++done / 3.0, k.label);
        }
        ctx.details()["buffer_mb"] = bytes / kMiB;
        if (software) ctx.details()["note"] = "software renderer: system memory, reduced buffer size";
    }

private:
    TestInfo m_info;
};

// --- latency ---------------------------------------------------------------------------------
class LatencyTest : public Test {
public:
    LatencyTest() {
        m_info.id = "synth.mem.latency";
        m_info.name = "Memory latency & caches";
        m_info.description = "Follows a chain of dependent loads through working sets from 4 KiB to hundreds of MiB. "
                             "Latency steps up at each cache boundary, revealing the size and speed of the L1, L2 and "
                             "last-level caches and of graphics memory.";
        m_info.category = Category::Synthetic;
        m_info.group = "Memory";
        m_info.frameBased = false;
        m_info.primaryMetric = "dram_latency_ns";
        m_info.primaryUnit = "ns";
        m_info.higherIsBetter = false;
        m_info.tags = {"memory"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 21;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        const bool smoke = s.duration == DurationPreset::Smoke;
        const std::uint64_t maxBytes = smoke || dev.adapter().isSoftware() ? 16 * kMiB : 512 * kMiB;
        const std::uint32_t strideBytes = 128; // one node per 128 B: defeats adjacent-line prefetch
        const std::uint32_t steps = smoke ? 1u << 12 : 1u << 16;
        auto chain = dev.createBuffer({maxBytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "chase-chain"});
        auto result = dev.createBuffer({256, rhi::BufferUsage::Storage, rhi::MemoryType::Device, 0, "chase-result"});
        auto upload = dev.createBuffer({maxBytes, rhi::BufferUsage::CopySrc, rhi::MemoryType::Upload, 0, "chase-upload"});
        auto pipe = memoryPipeline(dev, "csChase", {{rhi::BindingType::StorageBufferRW, 2}, {rhi::BindingType::StorageBufferRW, 3}});

        std::vector<std::uint64_t> sizes;
        std::vector<double> latency;
        std::mt19937 rng(1234);
        nlohmann::json curve = nlohmann::json::array();
        for (std::uint64_t ws = 4096; ws <= maxBytes; ws *= 2) {
            ctx.checkStop();
            // Random cyclic order over the nodes of this working set.
            const auto nodes = static_cast<std::uint32_t>(ws / strideBytes);
            std::vector<std::uint32_t> order(nodes);
            std::iota(order.begin(), order.end(), 0u);
            std::shuffle(order.begin() + 1, order.end(), rng);
            auto* words = static_cast<std::uint32_t*>(upload->mapped());
            const std::uint32_t wordsPerNode = strideBytes / 4;
            for (std::uint32_t i = 0; i < nodes; ++i)
                words[order[i] * wordsPerNode] = order[(i + 1) % nodes] * wordsPerNode;
            ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.copyBuffer(chain.get(), 0, upload.get(), 0, ws); });
            const MemoryParams warm{std::min(nodes, steps), 1, 0, 0};
            const MemoryParams p{steps, 1, 0, 0};
            auto chase = [&](const MemoryParams& mp) {
                return ctx.timeGpu([&](rhi::CommandList& cmd) {
                    cmd.transition(chain.get(), rhi::ResourceState::UnorderedAccess);
                    cmd.transition(result.get(), rhi::ResourceState::UnorderedAccess);
                    cmd.setPipeline(pipe.get());
                    cmd.setStorageBuffer(2, chain.get());
                    cmd.setStorageBuffer(3, result.get());
                    cmd.pushConstants(&mp, sizeof(mp));
                    cmd.dispatch(1);
                });
            };
            chase(warm); // bring the working set into the caches it fits in
            const double ms = std::min(chase(p), chase(p));
            const double ns = ms * 1e6 / steps;
            sizes.push_back(ws);
            latency.push_back(ns);
            curve.push_back({{"bytes", ws}, {"ns", ns}});
            ctx.progress(std::log2(static_cast<double>(ws) / 4096.0) / std::log2(static_cast<double>(maxBytes) / 4096.0),
                         formatSize(ws));
        }
        const auto levels = diag::latencyLevels(sizes, latency);
        auto& d = ctx.details();
        d["curve"] = curve;
        nlohmann::json lv = nlohmann::json::array();
        for (const auto& l : levels) lv.push_back({{"up_to_bytes", l.upToBytes}, {"latency_ns", l.latencyNs}});
        d["levels"] = lv;
        if (!latency.empty()) {
            ctx.addMetric("l1_latency_ns", "Smallest-working-set latency", latency.front(), "ns", false);
            ctx.addMetric("dram_latency_ns", std::format("Latency at {}", formatSize(sizes.back())), latency.back(), "ns", false);
        }
        std::string summary;
        for (std::size_t i = 0; i + 1 < levels.size(); ++i)
            summary += std::format("{}{:.0f} ns up to ≈{}", i ? ", " : "", levels[i].latencyNs, formatSize(levels[i].upToBytes));
        if (!levels.empty()) summary += std::format("{}{:.0f} ns beyond", summary.empty() ? "" : ", ", levels.back().latencyNs);
        d["summary"] = summary;
    }

private:
    TestInfo m_info;
};

// --- atomics ---------------------------------------------------------------------------------
class AtomicsTest : public Test {
public:
    AtomicsTest() {
        m_info.id = "synth.mem.atomics";
        m_info.name = "Atomics throughput";
        m_info.description = "Atomic additions from many threads: all on one hot address (worst-case contention), "
                             "spread over 64 Ki addresses, and 64-bit atomics where supported.";
        m_info.category = Category::Synthetic;
        m_info.group = "Memory";
        m_info.frameBased = false;
        m_info.primaryMetric = "spread_gops";
        m_info.primaryUnit = "G/s";
        m_info.reference = 150.0;
        m_info.tags = {"compute"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 22;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t threads = software ? (1u << 12) : (1u << 18);
        const std::uint32_t iterations = ctx.settings().duration == DurationPreset::Smoke ? 8 : 64;
        const std::uint32_t spread = 65536;
        auto counters = dev.createBuffer({spread * 4ull, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "atomics-32"});
        auto counters64 = dev.createBuffer({spread * 8ull, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "atomics-64"});
        auto run = [&](const char* entry, std::uint32_t slot, rhi::Buffer* buf, std::uint32_t addresses) {
            auto pipe = memoryPipeline(dev, entry, {{rhi::BindingType::StorageBufferRW, slot}});
            const MemoryParams p{iterations, threads, 0, addresses};
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                cmd.transition(buf, rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pipe.get());
                cmd.setStorageBuffer(slot, buf);
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch(threads / 256);
            });
            return m.msPerRep > 0 ? static_cast<double>(threads) * iterations / (m.msPerRep / 1000.0) / 1e9 : 0.0;
        };
        ctx.addMetric("spread_gops", "32-bit atomics, 64 Ki addresses", run("csAtomicAdd", 4, counters.get(), spread), "G/s");
        ctx.progress(0.4, "one address");
        ctx.addMetric("hot_gops", "32-bit atomics, one address", run("csAtomicAdd", 4, counters.get(), 1), "G/s");
        const auto& f = dev.features();
        if (f.atomicInt64 && f.shaderInt64) {
            ctx.progress(0.7, "64-bit");
            ctx.addMetric("spread64_gops", "64-bit atomics, 64 Ki addresses", run("csAtomicAdd64", 5, counters64.get(), spread), "G/s");
        } else {
            ctx.details()["atomics64"] = "not supported";
        }
    }

private:
    TestInfo m_info;
};

GT_REGISTER_TEST(BandwidthTest)
GT_REGISTER_TEST(LatencyTest)
GT_REGISTER_TEST(AtomicsTest)

} // namespace
} // namespace gt::bench
