// Cooperative matrices (feature.coopmat): matrix-unit throughput for every element type and
// shape this build compiles that the GPU lists (FP16, BF16, FP8 E4M3, INT8). Each variant is
// first checked with one multiply against a CPU reference (small exact values), then timed
// with many multiply-accumulates per subgroup (shaders/src/features/coopmat.slang).

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

namespace gt::bench {
namespace {

struct CoopParams {
    std::uint32_t iterations, pad0, pad1, pad2;
};

struct Variant {
    const char* name; ///< manifest variant
    const char* ab;   ///< element types as reported by Device::cooperativeMatrixConfigs
    const char* c;
    std::uint32_t m, n, k;
    std::uint32_t abBytes;
    const char* label;
};

const Variant kVariants[] = {
    {"f16_16x16x16", "f16", "f32", 16, 16, 16, 2, "FP16"},   {"f16_8x16x16", "f16", "f32", 8, 16, 16, 2, "FP16"},
    {"bf16_16x16x16", "bf16", "f32", 16, 16, 16, 2, "BF16"}, {"e4m3_16x16x32", "e4m3", "f32", 16, 16, 32, 1, "FP8"},
    {"e4m3_16x16x16", "e4m3", "f32", 16, 16, 16, 1, "FP8"},  {"s8_16x16x32", "s8", "s32", 16, 16, 32, 1, "INT8"},
    {"s8_16x16x16", "s8", "s32", 16, 16, 16, 1, "INT8"},
};

/// Element encodings for the exact test values 0, 0.25, 0.5, 0.75 (index 0..3).
std::uint32_t encode(const char* type, int q) {
    const std::string t = type;
    if (t == "f16") return std::array<std::uint32_t, 4>{0x0000, 0x3400, 0x3800, 0x3A00}[q];
    if (t == "bf16") return std::array<std::uint32_t, 4>{0x0000, 0x3E80, 0x3F00, 0x3F40}[q];
    if (t == "e4m3") return std::array<std::uint32_t, 4>{0x00, 0x28, 0x30, 0x34}[q];
    return static_cast<std::uint32_t>(q); // s8: the integers 0..3
}

class CoopMatTest : public Test {
public:
    CoopMatTest() {
        m_info.id = "feature.coopmat";
        m_info.name = "Cooperative matrices";
        m_info.description = "Measures the matrix (tensor) units with cooperative-matrix multiply-accumulate in FP16, "
                             "BF16, FP8 and INT8, after checking each against a CPU reference.";
        m_info.category = Category::Feature;
        m_info.group = "Matrix / ML";
        m_info.frameBased = false;
        m_info.primaryMetric = "fp16_tflops";
        m_info.primaryUnit = "TFLOPS";
        m_info.reference = 150.0; // provisional (docs/SCORING.md)
        m_info.tags = {"compute", "ml"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 100;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        if (!ctx.device().features().cooperativeMatrix) return SupportResult::no("no cooperative matrices (VK_KHR_cooperative_matrix)");
        for (const Variant& v : kVariants)
            if (listed(ctx.device(), v)) return SupportResult::yes();
        return SupportResult::no("none of the GPU's cooperative-matrix shapes is one this build compiles");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const bool software = dev.adapter().isSoftware();
        using U = rhi::BufferUsage;
        auto& d = ctx.details();
        nlohmann::json configs = nlohmann::json::array();
        for (const auto& c : dev.cooperativeMatrixConfigs())
            configs.push_back(std::format("{}x{}x{} {}*{}+{}->{}{}", c.m, c.n, c.k, c.a, c.b, c.c, c.result,
                                          c.saturating ? " (sat)" : ""));
        d["configs"] = configs;

        auto matA = dev.createBuffer({4096, U::Storage | U::CopyDst, rhi::MemoryType::Device, 0, "coop-a"});
        auto matB = dev.createBuffer({4096, U::Storage | U::CopyDst, rhi::MemoryType::Device, 0, "coop-b"});
        const std::uint64_t cBytes = 1024ull * 16 * 16 * 4;
        auto matC = dev.createBuffer({cBytes, U::Storage | U::CopySrc, rhi::MemoryType::Device, 0, "coop-c"});
        auto readback = dev.createBuffer({16 * 16 * 4, U::CopyDst, rhi::MemoryType::Readback, 0, "coop-readback"});
        auto upload = dev.createBuffer({8192, U::CopySrc, rhi::MemoryType::Upload, 0, "coop-upload"});

        std::uint32_t errors = 0;
        std::string firstError;
        std::map<std::string, double> best; // per label
        int done = 0;
        for (const Variant& v : kVariants) {
            if (!listed(dev, v) || best.count(v.label)) continue;
            ctx.progress(0.1 + 0.8 * done++ / 4.0, std::format("{} {}x{}x{}", v.label, v.m, v.n, v.k));
            // A (m x k) and B (k x n) with small exact values; C = A * B computed on the CPU.
            std::vector<int> qa(v.m * v.k), qb(v.k * v.n);
            for (std::uint32_t i = 0; i < v.m; ++i)
                for (std::uint32_t k = 0; k < v.k; ++k) qa[i * v.k + k] = int((i + k) % 4);
            for (std::uint32_t k = 0; k < v.k; ++k)
                for (std::uint32_t j = 0; j < v.n; ++j) qb[k * v.n + j] = int((k * 2 + j) % 4);
            auto* up = static_cast<std::uint8_t*>(upload->mapped());
            auto put = [&](std::uint8_t* dst, const std::vector<int>& q) {
                for (std::size_t i = 0; i < q.size(); ++i) {
                    const std::uint32_t e = encode(v.ab, q[i]);
                    std::memcpy(dst + i * v.abBytes, &e, v.abBytes);
                }
            };
            put(up, qa);
            put(up + 4096, qb);

            rhi::ComputePipelineDesc pd;
            pd.name = v.name;
            pd.compute = {"features/coopmat", std::string("csCoopMat@") + v.name};
            pd.layout.bindings = {{rhi::BindingType::StorageBufferRO, 0},
                                  {rhi::BindingType::StorageBufferRO, 1},
                                  {rhi::BindingType::StorageBufferRW, 0}};
            pd.layout.pushConstantBytes = sizeof(CoopParams);
            auto pipe = dev.createComputePipeline(pd);
            auto bind = [&](rhi::CommandList& cmd, const CoopParams& p) {
                cmd.transition(matA.get(), rhi::ResourceState::ShaderRead);
                cmd.transition(matB.get(), rhi::ResourceState::ShaderRead);
                cmd.transition(matC.get(), rhi::ResourceState::UnorderedAccess);
                cmd.setPipeline(pipe.get());
                cmd.setReadOnlyBuffer(0, matA.get());
                cmd.setReadOnlyBuffer(1, matB.get());
                cmd.setStorageBuffer(0, matC.get());
                cmd.pushConstants(&p, sizeof(p));
            };

            // Correctness: one multiply, one workgroup; subgroup 0's tile is at offset 0.
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.copyBuffer(matA.get(), 0, upload.get(), 0, 4096);
                cmd.copyBuffer(matB.get(), 0, upload.get(), 4096, 4096);
                bind(cmd, {1, 0, 0, 0});
                cmd.dispatch(1);
                cmd.copyBuffer(readback.get(), 0, matC.get(), 0, std::uint64_t(v.m) * v.n * 4);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
            const auto* out = static_cast<const std::uint8_t*>(readback->mapped());
            const bool isInt = std::string(v.c) == "s32";
            const double unit = isInt ? 1.0 : 0.0625; // 0.25 * 0.25 per product
            std::uint32_t wrong = 0;
            for (std::uint32_t i = 0; i < v.m; ++i)
                for (std::uint32_t j = 0; j < v.n; ++j) {
                    long expect = 0;
                    for (std::uint32_t k = 0; k < v.k; ++k) expect += long(qa[i * v.k + k]) * qb[k * v.n + j];
                    double got;
                    if (isInt) {
                        std::int32_t x;
                        std::memcpy(&x, out + (i * v.n + j) * 4, 4);
                        got = x;
                    } else {
                        float x;
                        std::memcpy(&x, out + (i * v.n + j) * 4, 4);
                        got = x;
                    }
                    if (std::fabs(got - double(expect) * unit) > 1e-3) {
                        if (firstError.empty())
                            firstError = std::format("{} {}x{}x{}: C[{}][{}] = {} (want {})", v.label, v.m, v.n, v.k, i, j,
                                                     got, double(expect) * unit);
                        ++wrong;
                    }
                }
            errors += wrong;

            // Throughput.
            const std::uint32_t groups = software ? 64u : 8192u;
            const CoopParams p{software ? 16u : 256u, 0, 0, 0};
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                bind(cmd, p);
                cmd.dispatch(groups);
            });
            const double subgroups = double(groups) * 64.0 / std::max(1u, dev.features().subgroupSize);
            const double ops = 2.0 * v.m * v.n * v.k * p.iterations * subgroups;
            const double rate = m.msPerRep > 0 ? ops / (m.msPerRep / 1000.0) / 1e12 : 0.0;
            best[v.label] = rate;
            const std::string key = std::string(v.label) == "INT8" ? "int8_tops" : std::format("{}_tflops", lower(v.label));
            ctx.addMetric(key, std::format("{} ({}x{}x{})", v.label, v.m, v.n, v.k), rate, isInt ? "TOPS" : "TFLOPS");
            d[std::format("{}_shape", lower(v.label))] = std::format("{}x{}x{}", v.m, v.n, v.k);
        }
        d["errors"] = errors;
        if (!firstError.empty()) d["error_example"] = firstError;
        if (software) d["note"] = "software renderer: reduced problem size";
        std::string tested;
        for (const auto& [label, rate] : best) tested += (tested.empty() ? "" : ", ") + label;
        if (errors)
            ctx.setVerdict(results::Verdict::Fail, std::format("Matrix results were wrong ({}).", firstError));
        else
            ctx.setVerdict(results::Verdict::Pass, std::format("Exact results for {}.", tested));
    }

private:
    static bool listed(rhi::Device& dev, const Variant& v) {
        for (const auto& c : dev.cooperativeMatrixConfigs())
            if (c.m == v.m && c.n == v.n && c.k == v.k && c.a == v.ab && c.b == v.ab && c.c == v.c && c.result == v.c &&
                !c.saturating)
                return true;
        return false;
    }
    static std::string lower(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::CoopMatTest)
