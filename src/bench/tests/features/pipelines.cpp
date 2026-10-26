// Pipeline compilation (feature.pipelines) — the cause of shader-compilation stutter.
// Every pipeline gets a fresh 32-bit salt patched into its SPIR-V, so the driver and its
// on-disk shader caches have never seen it (salts are random per run, not per seed).
//  - cold: unique pipelines created one at a time (median, p95 and worst = a stutter hitch)
//  - warm: the same pipelines created again (driver caches hit)
//  - parallel: unique pipelines created from several threads (how well compilation scales)

#include "gt/bench/context.h"
#include "gt/bench/test.h"
#include "gt/shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <random>
#include <span>
#include <thread>

namespace gt::bench {
namespace {

constexpr std::uint32_t kSalt = 0x5A17C0DEu;

struct Variant {
    std::vector<std::uint8_t> vertex, fragment;
};

class PipelineTest : public Test {
public:
    PipelineTest() {
        m_info.id = "feature.pipelines";
        m_info.name = "Pipeline compilation";
        m_info.description = "Compiles graphics pipelines the driver has never seen, then the same ones again, and "
                             "from several threads: compile time, worst hitch, cache benefit and parallel scaling.";
        m_info.category = Category::Feature;
        m_info.group = "API overhead";
        m_info.frameBased = false;
        m_info.primaryMetric = "cold_ms";
        m_info.primaryUnit = "ms";
        m_info.higherIsBetter = false;
        m_info.reference = 0.0; // CPU + driver bound: shown, not scored
        m_info.tags = {"cpu"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 80;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.api() == rhi::Api::Vulkan ? SupportResult::yes()
                                             : SupportResult::no("salted shader variants are implemented for SPIR-V only");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const bool smoke = s.duration == DurationPreset::Smoke;
        const std::uint32_t count = software || smoke ? 8u : 48u;
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);

        const auto vs = shaders::find("features/pipelines", "vsMaterial", shaders::Format::Spirv);
        const auto fs = shaders::find("features/pipelines", "psMaterial", shaders::Format::Spirv);
        if (!vs || !fs) throw rhi::Error("pipeline test shaders are not embedded in this build");
        const std::size_t saltOffset = findSalt(fs.bytes());
        std::mt19937 rng(std::random_device{}());
        auto makeVariant = [&] {
            Variant v;
            v.vertex.assign(vs.data, vs.data + vs.size);
            v.fragment.assign(fs.data, fs.data + fs.size);
            std::uint32_t salt = rng();
            if (salt == kSalt) salt ^= 1u;
            std::memcpy(v.fragment.data() + saltOffset, &salt, 4);
            return v;
        };

        rhi::GraphicsPipelineDesc base;
        base.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}, {1, rhi::Format::RGB32_FLOAT, 12}, {2, rhi::Format::RG32_FLOAT, 24}};
        base.vertexBindings = {{0, 32}};
        base.colorFormats = {rhi::Format::RGBA8_UNORM};
        base.depthFormat = rhi::Format::D32_FLOAT;
        base.layout.pushConstantBytes = 96;
        base.name = "salted-material";
        auto create = [&](const Variant& v) {
            rhi::GraphicsPipelineDesc d = base;
            d.vertex.bytecode = v.vertex;
            d.fragment.bytecode = v.fragment;
            return dev.createGraphicsPipeline(d);
        };

        // Warm up the render-pass cache and the driver's own first-use costs.
        create(makeVariant());

        // --- cold, one at a time -----------------------------------------------------------------
        std::vector<Variant> variants;
        for (std::uint32_t i = 0; i < count; ++i) variants.push_back(makeVariant());
        std::vector<double> cold, warm;
        for (std::uint32_t i = 0; i < count; ++i) {
            ctx.checkStop();
            ctx.progress(0.05 + 0.4 * i / count, "compiling new pipelines");
            cold.push_back(create(variants[i])->creationMs());
        }
        // --- warm: the same pipelines again --------------------------------------------------------
        for (std::uint32_t i = 0; i < count; ++i) {
            ctx.checkStop();
            ctx.progress(0.45 + 0.2 * i / count, "re-creating the same pipelines");
            warm.push_back(create(variants[i])->creationMs());
        }
        // --- parallel: new pipelines from several threads -----------------------------------------
        ctx.progress(0.7, std::format("compiling on {} threads", threads));
        std::vector<Variant> parallelVariants;
        for (std::uint32_t i = 0; i < count; ++i) parallelVariants.push_back(makeVariant());
        std::vector<std::unique_ptr<rhi::Pipeline>> made(count);
        std::atomic<std::uint32_t> next{0};
        std::exception_ptr failure;
        std::mutex failureMutex;
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> pool;
            for (unsigned t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    for (std::uint32_t i; (i = next++) < count;) {
                        try {
                            made[i] = create(parallelVariants[i]);
                        } catch (...) {
                            std::lock_guard lock(failureMutex);
                            failure = std::current_exception();
                        }
                    }
                });
        }
        const double parallelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        made.clear();
        if (failure) std::rethrow_exception(failure);

        auto pct = [](std::vector<double> v, double p) {
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, static_cast<std::size_t>(p * double(v.size() - 1) + 0.5))];
        };
        const double coldMed = pct(cold, 0.5), warmMed = pct(warm, 0.5);
        const double coldSum = [&] {
            double t = 0;
            for (double c : cold) t += c;
            return t;
        }();
        ctx.addMetric("cold_ms", "New pipeline (median)", coldMed, "ms", false);
        ctx.addMetric("cold_p95_ms", "New pipeline (95th percentile)", pct(cold, 0.95), "ms", false);
        ctx.addMetric("worst_ms", "Worst hitch", *std::max_element(cold.begin(), cold.end()), "ms", false);
        ctx.addMetric("warm_ms", "Cached pipeline (median)", warmMed, "ms", false);
        ctx.addMetric("cache_speedup", "Cache speed-up", warmMed > 0 ? coldMed / warmMed : 0.0, "x");
        ctx.addMetric("parallel_speedup", std::format("Speed-up on {} threads", threads),
                      parallelMs > 0 ? coldSum / parallelMs : 0.0, "x");
        auto& d = ctx.details();
        d["pipelines"] = count;
        d["threads"] = threads;
        d["parallel_total_ms"] = parallelMs;
        d["serial_total_ms"] = coldSum;
        d["fragment_spirv_bytes"] = fs.size;
        ctx.setVerdict(results::Verdict::Pass,
                       std::format("New pipelines took {:.1f} ms each (worst {:.1f} ms); cached ones {:.2f} ms.", coldMed,
                                   *std::max_element(cold.begin(), cold.end()), warmMed));
    }

private:
    static std::size_t findSalt(std::span<const std::uint8_t> spirv) {
        for (std::size_t i = 0; i + 4 <= spirv.size(); i += 4) {
            std::uint32_t w;
            std::memcpy(&w, spirv.data() + i, 4);
            if (w == kSalt) return i;
        }
        throw rhi::Error("salt constant not found in the pipeline test shader");
    }

    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::PipelineTest)
