// Compute correctness under load: two long dependent chains per element, one in
// integer arithmetic (checked bit for bit against the CPU for a sample of elements)
// and one in floating point (checked against the GPU's own first round: same GPU, same
// shader, same inputs must give the same bits). Rounds repeat until the preset's time;
// any difference means the GPU computed something wrong.

#include "gt/bench/context.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

struct ChainParams {
    std::uint32_t count, iterations, seed, pad;
};

std::uint32_t rotl(std::uint32_t x, std::uint32_t r) { return (x << r) | (x >> (32u - r)); }

// Must match csIntChain in shaders/src/diagnostics/compute.slang.
std::uint32_t intChain(std::uint32_t i, std::uint32_t iterations, std::uint32_t seed) {
    std::uint32_t x = i * 2654435761u + seed;
    std::uint32_t y = x ^ 0x6A09E667u;
    for (std::uint32_t k = 0; k < iterations; ++k) {
        x = x * 1664525u + 1013904223u;
        y ^= rotl(x, 7u) + (y >> 3u);
        x ^= y * 0x9E3779B9u;
    }
    return x ^ y;
}

class ComputeCheckTest : public Test {
public:
    ComputeCheckTest() {
        m_info.id = "diag.compute";
        m_info.name = "Compute error check";
        m_info.description = "Runs long integer and floating-point calculation chains on every shader core and checks "
                             "the results against the CPU and against the GPU's own earlier rounds. Any difference is a "
                             "calculation error.";
        m_info.category = Category::Diagnostic;
        m_info.order = 30;
        m_info.group = "Stability";
        m_info.frameBased = false;
        m_info.primaryMetric = "mismatches";
        m_info.primaryUnit = "values";
        m_info.higherIsBetter = false;
        m_info.tags = {"stress", "compute"};
        m_info.suites = {"diagnostics", "full"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        std::uint32_t count = 1u << 20;
        switch (s.quality) {
        case Quality::Low: count = 1u << 18; break;
        case Quality::Medium: count = 1u << 19; break;
        case Quality::High: count = 1u << 20; break;
        case Quality::Ultra: count = 1u << 21; break;
        }
        if (s.duration == DurationPreset::Smoke) count = 1u << 14;
        const std::uint64_t bytes = std::uint64_t(count) * 4;
        auto makeOut = [&](const char* name) {
            return dev.createBuffer({bytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
                                     rhi::MemoryType::Device, 0, name});
        };
        auto intOut = makeOut("compute-check-int");
        auto floatOut = makeOut("compute-check-float");
        auto readback = dev.createBuffer({bytes * 2, rhi::BufferUsage::CopyDst, rhi::MemoryType::Readback, 0, "compute-check-readback"});
        auto pipeline = [&](const char* entry) {
            rhi::ComputePipelineDesc pd;
            pd.name = entry;
            pd.compute = {"diagnostics/compute", entry};
            pd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0}};
            pd.layout.pushConstantBytes = sizeof(ChainParams);
            return dev.createComputePipeline(pd);
        };
        auto intPipe = pipeline("csIntChain");
        auto floatPipe = pipeline("csFloatChain");
        const std::uint32_t seed = 0x5EEDu;
        const bool inject = s.param("diag.compute.inject") == "1";

        auto round = [&](std::uint32_t iterations, bool corrupt) {
            const ChainParams p{count, iterations, seed, 0};
            return ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.transition(intOut.get(), rhi::ResourceState::UnorderedAccess);
                cmd.transition(floatOut.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(intPipe.get());
                cmd.setStorageBuffer(0, intOut.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch((count + 255) / 256);
                cmd.setPipeline(floatPipe.get());
                cmd.setStorageBuffer(0, floatOut.get());
                cmd.pushConstants(&p, sizeof(p));
                cmd.dispatch((count + 255) / 256);
                if (corrupt) cmd.fillBuffer(intOut.get(), 4096, 4, 0x12345678u); // testing hook
                cmd.copyBuffer(readback.get(), 0, intOut.get(), 0, bytes);
                cmd.copyBuffer(readback.get(), bytes, floatOut.get(), 0, bytes);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
        };

        // Calibrate the chain length so one round takes ~40 ms (keeps far from OS GPU watchdogs).
        std::uint32_t iterations = 256;
        const double calMs = std::max(0.01, round(iterations, false));
        const std::uint32_t maxIter = s.duration == DurationPreset::Smoke ? 512 : 1u << 16;
        iterations = static_cast<std::uint32_t>(std::clamp(iterations * 40.0 / calMs, 64.0, static_cast<double>(maxIter)));
        iterations = iterations / 64 * 64;

        // Reference round.
        round(iterations, false);
        std::vector<std::uint32_t> ref(count * 2), cur(count * 2);
        std::memcpy(ref.data(), readback->mapped(), bytes * 2);
        // CPU check of the integer chain on a sample of elements.
        const std::uint32_t sample = std::min<std::uint32_t>(count, s.duration == DurationPreset::Smoke ? 256 : 2048);
        std::uint32_t cpuMismatch = 0;
        for (std::uint32_t k = 0; k < sample; ++k) {
            const std::uint32_t i = static_cast<std::uint32_t>((std::uint64_t(k) * count) / sample);
            if (ref[i] != intChain(i, iterations, seed)) ++cpuMismatch;
        }

        const double minS = std::max(0.0, s.minSeconds());
        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        std::uint32_t rounds = 0, badRounds = 0;
        std::uint64_t intMismatch = 0, floatMismatch = 0;
        double gpuMs = 0.0;
        do {
            ctx.checkStop();
            gpuMs += round(iterations, inject && rounds == 0);
            std::memcpy(cur.data(), readback->mapped(), bytes * 2);
            std::uint64_t ib = 0, fb = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                ib += cur[i] != ref[i];
                fb += cur[count + i] != ref[count + i];
            }
            intMismatch += ib;
            floatMismatch += fb;
            badRounds += (ib + fb) > 0;
            ++rounds;
            ctx.progress(minS > 0 ? std::min(0.999, elapsed() / minS) : 0.999, std::format("round {}", rounds));
        } while (elapsed() < minS);

        // ~10 integer ops per iteration and element in each kernel, roughly.
        const double ops = static_cast<double>(count) * iterations * 20.0 * rounds;
        auto& d = ctx.details();
        d["elements"] = count;
        d["iterations"] = iterations;
        d["rounds"] = rounds;
        d["bad_rounds"] = badRounds;
        d["cpu_checked"] = sample;
        const std::uint64_t total = intMismatch + floatMismatch + cpuMismatch;
        ctx.addMetric("mismatches", "Wrong results", static_cast<double>(total), "values", false);
        ctx.addMetric("rounds", "Rounds", rounds, "rounds");
        if (gpuMs > 0) ctx.addMetric("throughput", "Approximate throughput", ops / (gpuMs / 1000.0) / 1e9, "Gop/s");
        if (total == 0) {
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("{} round{} × {} values × {} steps: every result matched (integer chain also "
                                       "verified on the CPU for {} values).",
                                       rounds, rounds == 1 ? "" : "s", count, iterations, sample));
        } else {
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("{} wrong results in {} of {} rounds (integer {}, floating point {}, vs CPU {}). The "
                                       "GPU's arithmetic is not reliable under load: unstable overclock or undervolt, "
                                       "overheating, or failing hardware.",
                                       total, badRounds, rounds, intMismatch, floatMismatch, cpuMismatch));
        }
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::ComputeCheckTest)
