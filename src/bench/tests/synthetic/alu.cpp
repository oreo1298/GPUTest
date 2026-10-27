// ALU throughput: FP32 / FP16 / FP64 FMA, INT32 multiply-add and packed INT8 dot
// products (shaders/src/synthetic/throughput.slang). Results in TFLOPS / TOPS, counting
// an FMA or multiply-add as two operations and a 4-wide int8 dot product as eight.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <format>

namespace gt::bench {
namespace {

struct ThroughputParams {
    std::uint32_t iterations;
    float mulF, addF;
    std::uint32_t mulI, addI, packedB, pad0, pad1;
};
static_assert(sizeof(ThroughputParams) == 32);

enum class Alu { Fp32, Fp16, Fp64, Int32, Dot8 };

struct AluSpec {
    Alu kind;
    const char* id;
    const char* name;
    const char* entry;
    const char* unit;
    double opsPerIteration; ///< per thread
    double reference;       ///< provisional: value worth 1000 points (docs/SCORING.md)
    const char* description;
};

const AluSpec kSpecs[] = {
    {Alu::Fp32, "synth.alu.fp32", "FP32 throughput", "csFp32", "TFLOPS", 64, 30.0,
     "Single-precision fused multiply-add rate of all shader cores."},
    {Alu::Fp16, "synth.alu.fp16", "FP16 throughput", "csFp16", "TFLOPS", 64, 60.0,
     "Half-precision multiply-add rate (packed math doubles it on many GPUs)."},
    {Alu::Fp64, "synth.alu.fp64", "FP64 throughput", "csFp64", "TFLOPS", 16, 1.0,
     "Double-precision multiply-add rate (consumer GPUs run it at 1/16–1/64 of FP32)."},
    {Alu::Int32, "synth.alu.int32", "INT32 throughput", "csInt32", "TOPS", 64, 15.0,
     "32-bit integer multiply-add rate."},
    {Alu::Dot8, "synth.alu.int8dot", "INT8 dot-product throughput", "csDot8", "TOPS", 64, 120.0,
     "Packed 4×int8 dot products with accumulate (DP4a), the building block of quantized inference."},
};

class AluTest : public Test {
public:
    explicit AluTest(const AluSpec& spec) : m_spec(spec) {
        m_info.id = spec.id;
        m_info.name = spec.name;
        m_info.description = spec.description;
        m_info.category = Category::Synthetic;
        m_info.group = "Compute throughput";
        m_info.frameBased = false;
        m_info.primaryMetric = "throughput";
        m_info.primaryUnit = spec.unit;
        m_info.reference = spec.reference;
        m_info.tags = {"compute"};
        m_info.suites = {"standard", "extreme", "compute"};
        if (spec.kind == Alu::Fp32) m_info.suites.insert(m_info.suites.end(), {"quick", "legacy"});
        m_info.order = 10 + static_cast<int>(spec.kind);
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& f = ctx.device().features();
        switch (m_spec.kind) {
        case Alu::Fp16: return f.shaderFloat16 ? SupportResult::yes() : SupportResult::no("no FP16 shader arithmetic");
        case Alu::Fp64: return f.shaderFloat64 ? SupportResult::yes() : SupportResult::no("no FP64 shader arithmetic");
        case Alu::Dot8:
            return f.integerDotProduct ? SupportResult::yes() : SupportResult::no("no shader integer dot products");
        default: return SupportResult::yes();
        }
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t threads = software ? (1u << 14) : (1u << 20);
        // Long dispatches (about 1 ms of FP32 on an RTX 4090) keep launch gaps and the last partial wave small.
        const std::uint32_t iterations = ctx.settings().duration == DurationPreset::Smoke ? 32 : 1024;
        auto sink = dev.createBuffer({4096, rhi::BufferUsage::Storage, rhi::MemoryType::Device, 0, "alu-sink"});
        rhi::ComputePipelineDesc pd;
        pd.name = m_spec.entry;
        pd.compute = {"synthetic/throughput", m_spec.entry};
        pd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0}};
        pd.layout.pushConstantBytes = sizeof(ThroughputParams);
        auto pipe = dev.createComputePipeline(pd);
        const ThroughputParams p{iterations, 0.9999f, 0.0001f, 1664525u, 1013904223u, 0x01FF7F80u, 0, 0};
        const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
            cmd.transition(sink.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(pipe.get());
            cmd.setStorageBuffer(0, sink.get());
            cmd.pushConstants(&p, sizeof(p));
            cmd.dispatch(threads / 256);
        });
        const double ops = static_cast<double>(threads) * iterations * m_spec.opsPerIteration;
        const double rate = m.msPerRep > 0 ? ops / (m.msPerRep / 1000.0) / 1e12 : 0.0;
        ctx.addMetric("throughput", m_spec.name, rate, m_spec.unit);
        ctx.addMetric("ms_per_dispatch", "Time per dispatch", m.msPerRep, "ms", false);
        auto& d = ctx.details();
        d["threads"] = threads;
        d["iterations"] = iterations;
        d["dispatches_per_sample"] = m.reps;
        d["samples"] = m.samples;
        d["spread_pct"] = m.spreadPct;
        if (software) d["note"] = "software renderer: reduced problem size";
    }

private:
    AluSpec m_spec;
    TestInfo m_info;
};

[[maybe_unused]] const bool kRegistered = [] {
    for (const AluSpec& s : kSpecs) Registry::instance().add([s] { return std::make_unique<AluTest>(s); });
    return true;
}();

} // namespace
} // namespace gt::bench
