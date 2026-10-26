// VRAM tester: fills as much free graphics memory as the duration preset allows with
// test patterns, reads it back on the GPU and reports every mismatch. Patterns follow
// classic memory testers: walking ones / zeros (stuck and coupled bits), moving
// inversions over a checkerboard (MATS+ style, ascending then descending), random data
// and address-in-address (addressing faults). All chunks are written before any is
// verified, so data has to survive a while in memory.

#include "gt/bench/context.h"
#include "gt/bench/test.h"
#include "gt/diagnostics.h"
#include "gt/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

struct MemtestParams {
    std::uint32_t mode, pattern, seed, invert, wordCount, baseWord, chunk, flags, threads;
};
static_assert(sizeof(MemtestParams) == 36);

constexpr std::uint32_t kMaxRecords = 1024;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

struct Step {
    std::uint32_t mode;
    std::uint32_t invert;
    bool descending;
};
struct Pattern {
    const char* name;
    std::uint32_t id;
    std::vector<Step> steps; ///< each step runs over every chunk
};

class VramTest : public Test {
public:
    VramTest() {
        m_info.id = "diag.vram";
        m_info.name = "VRAM test";
        m_info.description = "Fills free graphics memory with test patterns (walking bits, moving inversions, random "
                             "data, address-in-address) and checks every word, to find bad memory cells, unstable "
                             "memory overclocks and addressing faults.";
        m_info.category = Category::Diagnostic;
        m_info.order = 20;
        m_info.group = "Memory";
        m_info.frameBased = false;
        m_info.primaryMetric = "errors";
        m_info.primaryUnit = "errors";
        m_info.higherIsBetter = false;
        m_info.tags = {"stress", "memory"};
        m_info.suites = {"diagnostics", "full"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        const rhi::AdapterInfo& ad = dev.adapter();
        const bool sharedMemory = ad.type != rhi::AdapterType::Discrete;

        // How much to test: free memory (budget - usage) scaled by the duration preset.
        const rhi::MemoryBudget mb = dev.memoryBudget();
        std::uint64_t freeBytes = mb.valid && mb.localBudget > mb.localUsage ? mb.localBudget - mb.localUsage
                                  : ad.dedicatedMemory                       ? ad.dedicatedMemory * 8 / 10
                                                                             : 512 * kMiB;
        double fraction = 0.9;
        std::uint64_t cap = ~0ull;
        switch (s.duration) {
        case DurationPreset::Smoke: cap = 64 * kMiB; break;
        case DurationPreset::Quick: fraction = 0.5; cap = 4096 * kMiB; break;
        case DurationPreset::Standard: fraction = 0.9; break;
        case DurationPreset::Extreme: fraction = 0.92; break;
        }
        if (sharedMemory) cap = std::min<std::uint64_t>(cap, std::min<std::uint64_t>(freeBytes / 4, 2048 * kMiB));
        std::uint64_t target = std::min<std::uint64_t>(static_cast<std::uint64_t>(static_cast<double>(freeBytes) * fraction), cap);
        if (const std::string p = s.param("diag.vram.mb"); !p.empty()) target = std::stoull(p) * kMiB;
        const std::uint64_t chunkBytes = s.duration == DurationPreset::Smoke ? 32 * kMiB : 256 * kMiB;
        target = std::max(target, chunkBytes);

        // Allocate chunks until the target or the first allocation failure.
        std::vector<std::unique_ptr<rhi::Buffer>> chunks;
        std::string allocNote;
        for (std::uint64_t got = 0; got + chunkBytes <= target; got += chunkBytes) {
            try {
                chunks.push_back(dev.createBuffer({chunkBytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                                   rhi::MemoryType::Device, 0, "vram-test-chunk"}));
            } catch (const rhi::Error& e) {
                allocNote = std::format("allocation stopped after {} MiB: {}", got / kMiB, e.what());
                break;
            }
            ctx.checkStop();
        }
        if (chunks.empty()) throw rhi::Error("could not allocate any test memory: " + allocNote);
        const std::uint64_t testedBytes = chunks.size() * chunkBytes;
        const auto wordsPerChunk = static_cast<std::uint32_t>(chunkBytes / 4);
        const std::uint32_t threads = std::min<std::uint32_t>(wordsPerChunk / 16, 65535u * 256u) / 256u * 256u;
        const std::uint32_t groups = threads / 256u;

        const std::uint64_t errorBytes = (2 + kMaxRecords * 4) * 4;
        auto errors = dev.createBuffer({errorBytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
                                        rhi::MemoryType::Device, 0, "vram-test-errors"});
        auto readback = dev.createBuffer({errorBytes, rhi::BufferUsage::CopyDst, rhi::MemoryType::Readback, 0, "vram-test-readback"});

        rhi::ComputePipelineDesc pd;
        pd.name = "memtest";
        pd.compute = {"diagnostics/memtest", "csMemtest"};
        pd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0}, {rhi::BindingType::StorageBufferRW, 1}};
        pd.layout.pushConstantBytes = sizeof(MemtestParams);
        auto pipe = dev.createComputePipeline(pd);

        const std::vector<Pattern> patterns = {
            {"walking ones", 0, {{0, 0, false}, {1, 0, false}}},
            {"walking zeros", 1, {{0, 0, false}, {1, 0, false}}},
            {"moving inversions", 4, {{0, 0, false}, {2, 0, false}, {2, ~0u, true}, {1, 0, false}}},
            {"random data", 2, {{0, 0, false}, {1, 0, false}}},
            {"address in address", 3, {{0, 0, false}, {1, 0, false}}},
        };

        const bool inject = s.param("diag.vram.inject") == "1";
        ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.fillBuffer(errors.get(), 0, errorBytes, 0); });
        std::vector<std::uint64_t> perPattern(patterns.size(), 0);
        std::uint64_t knownErrors = 0;
        double gpuMs = 0.0;
        std::uint64_t bytesTouched = 0;
        std::uint32_t passes = 0;
        const double minS = std::max(0.0, s.minSeconds());
        const double maxS = s.maxSeconds();
        const std::size_t stepsPerPass = [&] {
            std::size_t n = 0;
            for (const auto& p : patterns) n += p.steps.size();
            return n * chunks.size();
        }();
        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        auto readErrors = [&](std::vector<std::uint32_t>& out) {
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.copyBuffer(readback.get(), 0, errors.get(), 0, errorBytes);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
            out.resize(errorBytes / 4);
            std::memcpy(out.data(), readback->mapped(), errorBytes);
        };
        std::vector<std::uint32_t> err;
        bool timeLimited = false;
        do {
            const std::uint32_t seed = passes * 0x9E3779B9u + 1u;
            std::size_t step = 0;
            for (std::size_t pi = 0; pi < patterns.size(); ++pi) {
                const Pattern& pat = patterns[pi];
                for (const Step& st : pat.steps) {
                    for (std::uint32_t c = 0; c < chunks.size(); ++c) {
                        ctx.checkStop();
                        MemtestParams p{st.mode, pat.id, seed, st.invert, wordsPerChunk,
                                        static_cast<std::uint32_t>(c * static_cast<std::uint64_t>(wordsPerChunk)), c,
                                        st.descending ? 1u : 0u, threads};
                        gpuMs += ctx.timeGpu([&](rhi::CommandList& cmd) {
                            cmd.transition(chunks[c].get(), rhi::ResourceState::UnorderedAccess);
                            cmd.transition(errors.get(), rhi::ResourceState::UnorderedAccess);
                            cmd.setPipeline(pipe.get());
                            cmd.setStorageBuffer(0, chunks[c].get());
                            cmd.setStorageBuffer(1, errors.get());
                            cmd.pushConstants(&p, sizeof(p));
                            cmd.dispatch(groups);
                        });
                        bytesTouched += chunkBytes * (st.mode == 2 ? 2 : 1);
                        // Testing hook: corrupt 4 words after the random-data write to prove detection works.
                        if (inject && pat.id == 2 && st.mode == 0 && c == 0 && passes == 0)
                            ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.fillBuffer(chunks[0].get(), 1 << 20, 16, 0xDEADBEEFu); });
                        ++step;
                        // Passes repeat until the preset's minimum time; estimate how many that will be.
                        const double done = passes + static_cast<double>(step) / static_cast<double>(stepsPerPass);
                        const double expected = std::max(1.0, std::ceil(minS * done / std::max(elapsed(), 1e-3)));
                        ctx.progress(std::min(0.999, done / expected),
                                     std::format("pass {}: {}", passes + 1, pat.name));
                    }
                }
                readErrors(err);
                perPattern[pi] += err[0] - knownErrors;
                knownErrors = err[0];
            }
            ++passes;
            if (elapsed() > maxS) {
                timeLimited = true;
                break;
            }
        } while (elapsed() < minS);

        // Results.
        const std::uint32_t total = err.empty() ? 0 : err[0];
        const std::uint32_t flipped = err.empty() ? 0 : err[1];
        std::vector<diag::MemError> records;
        for (std::uint32_t i = 0; i < std::min(total, kMaxRecords); ++i)
            records.push_back({err[2 + i * 4], err[3 + i * 4], err[4 + i * 4], err[5 + i * 4]});
        const auto ranges = diag::errorRanges(records);
        auto& d = ctx.details();
        d["tested_mb"] = testedBytes / kMiB;
        d["free_mb"] = freeBytes / kMiB;
        d["chunk_mb"] = chunkBytes / kMiB;
        d["chunks"] = chunks.size();
        d["passes"] = passes;
        d["patterns"] = [&] {
            nlohmann::json a = nlohmann::json::array();
            for (std::size_t i = 0; i < patterns.size(); ++i) a.push_back({{"name", patterns[i].name}, {"errors", perPattern[i]}});
            return a;
        }();
        if (!allocNote.empty()) d["allocation"] = allocNote;
        if (sharedMemory) d["note"] = "integrated / shared-memory GPU: this exercises system memory through the GPU";
        nlohmann::json jr = nlohmann::json::array();
        for (std::size_t i = 0; i < std::min<std::size_t>(ranges.size(), 32); ++i) {
            const auto& r = ranges[i];
            jr.push_back({{"chunk", r.chunk},
                          {"first_byte", r.firstByte},
                          {"last_byte", r.lastByte},
                          {"errors", r.count},
                          {"bits", diag::describeBits(r.flippedBits)}});
        }
        d["failing_ranges"] = jr;
        if (total > kMaxRecords) d["records_truncated"] = true;

        ctx.addMetric("errors", "Memory errors", total, "errors", false);
        ctx.addMetric("tested_mb", "Memory tested", static_cast<double>(testedBytes / kMiB), "MiB");
        ctx.addMetric("passes", "Passes", passes, "passes");
        if (gpuMs > 0) ctx.addMetric("throughput", "Test throughput", static_cast<double>(bytesTouched) / (gpuMs / 1000.0) / 1e9, "GB/s");

        const double share = 100.0 * static_cast<double>(testedBytes) / static_cast<double>(std::max<std::uint64_t>(freeBytes, 1));
        const std::string scope = std::format("{} MiB ({} of free memory, {} chunk{}) with {} patterns × {} pass{}",
                                              testedBytes / kMiB, share < 1.0 ? std::string("< 1 %") : std::format("{:.0f} %", share),
                                              chunks.size(), chunks.size() == 1 ? "" : "s", patterns.size(), passes,
                                              passes == 1 ? "" : "es");
        if (total == 0) {
            ctx.setVerdict(timeLimited ? results::Verdict::Warn : results::Verdict::Pass,
                           std::format("No errors in {}.{}", scope, timeLimited ? " The time limit cut the test short." : ""));
        } else {
            std::string where;
            for (std::size_t i = 0; i < std::min<std::size_t>(ranges.size(), 3); ++i)
                where += std::format("{}chunk {} offset 0x{:x}–0x{:x} ({} error{})", i ? "; " : "", ranges[i].chunk,
                                     ranges[i].firstByte, ranges[i].lastByte, ranges[i].count, ranges[i].count == 1 ? "" : "s");
            std::string which;
            for (std::size_t i = 0; i < patterns.size(); ++i)
                if (perPattern[i]) which += std::format("{}{} ({})", which.empty() ? "" : ", ", patterns[i].name, perPattern[i]);
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("{} memory error{} in {}: {}{}. Flipped {}. Found by: {}. This points to an unstable "
                                       "memory overclock, overheating memory or failing VRAM.",
                                       total, total == 1 ? "" : "s", scope, where, ranges.size() > 3 ? "; …" : "",
                                       diag::describeBits(flipped), which));
        }
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::VramTest)
