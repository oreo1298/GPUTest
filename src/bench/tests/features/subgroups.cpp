// Subgroup operations (feature.subgroups): checks wave intrinsics (sum, prefix sum, ballot
// count, max) against a CPU reference built from the lane layout the GPU reports, then
// compares three ways of reducing 256 values per workgroup: shared-memory atomics, a
// shared-memory tree, and subgroup sums (shaders/src/features/subgroups.slang).

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

namespace gt::bench {
namespace {

struct SubgroupParams {
    std::uint32_t count, iterations, pad0, pad1;
};

std::uint32_t pcgHash(std::uint32_t v) { // same as gtHash in common/bindings.slang
    const std::uint32_t state = v * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

class SubgroupTest : public Test {
public:
    SubgroupTest() {
        m_info.id = "feature.subgroups";
        m_info.name = "Subgroup operations";
        m_info.description = "Verifies wave/subgroup intrinsics against a CPU reference and measures how much faster a "
                             "workgroup reduction gets with them than with shared memory or atomics.";
        m_info.category = Category::Feature;
        m_info.group = "Compute";
        m_info.frameBased = false;
        m_info.primaryMetric = "wave_greductions";
        m_info.primaryUnit = "G values/s";
        m_info.reference = 2000.0; // provisional (docs/SCORING.md)
        m_info.tags = {"compute"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 60;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().subgroupOps ? SupportResult::yes()
                                                   : SupportResult::no("no subgroup arithmetic in compute shaders");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();
        using U = rhi::BufferUsage;

        // --- correctness -------------------------------------------------------------------------
        const std::uint32_t count = 1u << 14;
        auto lanes = dev.createBuffer({count * 16ull, U::Storage | U::CopySrc, rhi::MemoryType::Device, 16, "sg-lanes"});
        auto ops = dev.createBuffer({count * 16ull, U::Storage | U::CopySrc, rhi::MemoryType::Device, 16, "sg-ops"});
        auto sink = dev.createBuffer({4096, U::Storage | U::CopySrc, rhi::MemoryType::Device, 4, "sg-sink"});
        auto readback = dev.createBuffer({count * 32ull, U::CopyDst, rhi::MemoryType::Readback, 0, "sg-readback"});
        auto pipe = [&](const char* entry) {
            rhi::ComputePipelineDesc pd;
            pd.name = entry;
            pd.compute = {"features/subgroups", entry};
            pd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0},
                                  {rhi::BindingType::StorageBufferRW, 1},
                                  {rhi::BindingType::StorageBufferRW, 2}};
            pd.layout.pushConstantBytes = sizeof(SubgroupParams);
            return dev.createComputePipeline(pd);
        };
        auto check = pipe("csWaveCheck");
        auto bind = [&](rhi::CommandList& cmd, rhi::Pipeline* p, const SubgroupParams& params) {
            for (rhi::Buffer* b : {lanes.get(), ops.get(), sink.get()}) cmd.transition(b, rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(p);
            cmd.setStorageBuffer(0, lanes.get());
            cmd.setStorageBuffer(1, ops.get());
            cmd.setStorageBuffer(2, sink.get());
            cmd.pushConstants(&params, sizeof(params));
        };
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            bind(cmd, check.get(), {count, 0, 0, 0});
            cmd.dispatch(count / 64);
            cmd.copyBuffer(readback.get(), 0, lanes.get(), 0, count * 16ull);
            cmd.copyBuffer(readback.get(), count * 16ull, ops.get(), 0, count * 16ull);
            cmd.transition(readback.get(), rhi::ResourceState::HostRead);
        });
        std::vector<std::uint32_t> lane(count * 4), op(count * 4);
        std::memcpy(lane.data(), readback->mapped(), count * 16ull);
        std::memcpy(op.data(), static_cast<const std::uint8_t*>(readback->mapped()) + count * 16ull, count * 16ull);

        // Group threads by subgroup (identified by the first thread's index), order by lane.
        std::map<std::uint32_t, std::vector<std::uint32_t>> groups;
        for (std::uint32_t i = 0; i < count; ++i) groups[lane[i * 4 + 1]].push_back(i);
        std::uint64_t errors = 0;
        bool contiguous = true;
        std::uint32_t laneCount = lane[2];
        std::string example;
        for (auto& [first, members] : groups) {
            std::sort(members.begin(), members.end(), [&](std::uint32_t a, std::uint32_t b) { return lane[a * 4] < lane[b * 4]; });
            std::uint32_t sum = 0, ballot = 0, mx = 0;
            for (std::uint32_t i : members) {
                const std::uint32_t x = pcgHash(i) & 0xFFFFu;
                sum += x;
                ballot += x & 1u;
                mx = std::max(mx, x);
            }
            std::uint32_t prefix = 0;
            for (std::uint32_t i : members) {
                const std::uint32_t x = pcgHash(i) & 0xFFFFu;
                const std::uint32_t* o = &op[i * 4];
                const bool ok = o[0] == sum && o[1] == prefix && o[2] == ballot && o[3] == mx && lane[i * 4 + 2] == laneCount;
                if (!ok) {
                    if (example.empty())
                        example = std::format("thread {}: sum {} (want {}), prefix {} ({}), ballot {} ({}), max {} ({})", i,
                                              o[0], sum, o[1], prefix, o[2], ballot, o[3], mx);
                    ++errors;
                }
                if (lane[i * 4] != i - first) contiguous = false;
                prefix += x;
            }
        }

        // --- reductions --------------------------------------------------------------------------
        const std::uint32_t groupsPerDispatch = software ? 16u : 4096u;
        const std::uint32_t iterations = software ? 4u : 64u;
        struct Variant {
            const char* entry;
            const char* name;
            const char* label;
        };
        const Variant variants[] = {{"csReduceAtomic", "atomic", "Shared-memory atomics"},
                                    {"csReduceShared", "shared", "Shared-memory tree"},
                                    {"csReduceWave", "wave", "Subgroup operations"}};
        double rate[3] = {};
        std::vector<std::uint32_t> sums[3];
        for (int v = 0; v < 3; ++v) {
            ctx.progress(0.3 + 0.2 * v, variants[v].label);
            auto p = pipe(variants[v].entry);
            const SubgroupParams params{count, iterations, 0, 0};
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                bind(cmd, p.get(), params);
                cmd.dispatch(groupsPerDispatch);
            });
            const double values = double(groupsPerDispatch) * 256.0 * iterations;
            rate[v] = m.msPerRep > 0 ? values / (m.msPerRep / 1000.0) / 1e9 : 0.0;
            ctx.addMetric(std::format("{}_greductions", variants[v].name), variants[v].label, rate[v], "G values/s");
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.fillBuffer(sink.get(), 0, 4096, 0);
                bind(cmd, p.get(), params);
                cmd.dispatch(std::min(groupsPerDispatch, 1024u));
                cmd.copyBuffer(readback.get(), 0, sink.get(), 0, 4096);
                cmd.transition(readback.get(), rhi::ResourceState::HostRead);
            });
            sums[v].resize(1024);
            std::memcpy(sums[v].data(), readback->mapped(), 4096);
        }
        if (rate[1] > 0) ctx.addMetric("speedup_vs_shared", "Subgroup vs shared-memory tree", rate[2] / rate[1], "x");
        const bool reductionsAgree = sums[0] == sums[1] && sums[1] == sums[2];

        auto& d = ctx.details();
        d["subgroup_size"] = f.subgroupSize;
        d["subgroup_size_range"] = std::format("{}-{}", f.minSubgroupSize, f.maxSubgroupSize);
        d["observed_lane_count"] = laneCount;
        d["contiguous_lanes"] = contiguous;
        d["checked_threads"] = count;
        d["errors"] = errors;
        if (!example.empty()) d["error_example"] = example;
        d["reductions_agree"] = reductionsAgree;
        if (software) d["note"] = "software renderer: reduced problem size";

        if (errors)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("{} of {} threads got wrong subgroup results ({}).", errors, count, example));
        else if (!reductionsAgree)
            ctx.setVerdict(results::Verdict::Warn, "The three reduction methods produced different sums.");
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("Subgroup sum, prefix sum, ballot and max are correct ({} lanes{}).", laneCount,
                                       contiguous ? "" : ", non-contiguous lane layout"));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::SubgroupTest)
