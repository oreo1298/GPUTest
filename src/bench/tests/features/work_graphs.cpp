// Work graphs (feature.work_graphs, Direct3D 12 only): a two-node graph launched from one CPU
// record — a broadcasting entry node that emits one record per thread to a thread-launch leaf
// node (shaders/src/features/work_graphs.hlsl). Checks every leaf result and the record count,
// then compares the graph's throughput with the same work issued as one ordinary dispatch, which
// shows the cost of the GPU's record scheduling. Vulkan has no ratified equivalent yet
// (VK_AMDX_shader_enqueue is provisional), so the test records native D3D12 commands.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"
#include "gt/shaders.h"

#if GT_HAS_D3D12
#include "d3d12/d3d12_device.h"
#endif

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace gt::bench {
namespace {

#if GT_HAS_D3D12
namespace d3d = rhi::d3d12;
using d3d::Com;

constexpr const char* kModule = "features/work_graphs";
constexpr const wchar_t* kProgram = L"KilnGraph";
constexpr std::uint32_t kCounters = 64;

struct GraphParams {
    std::uint32_t records, iterations, pad0, pad1;
};

/// Mirror of hashChain() in work_graphs.hlsl.
std::uint32_t hashChain(std::uint32_t v, std::uint32_t iterations) {
    for (std::uint32_t i = 0; i < iterations; ++i) {
        const std::uint32_t state = v * 747796405u + 2891336453u;
        const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        v = (word >> 22u) ^ word;
    }
    return v;
}

// Methods returning structs take an out-parameter in the non-MSVC Windows ABI of DirectX-Headers.
D3D12_PROGRAM_IDENTIFIER programIdentifier(ID3D12StateObjectProperties1* props, LPCWSTR name) {
#if defined(_MSC_VER) || !defined(_WIN32)
    return props->GetProgramIdentifier(name);
#else
    D3D12_PROGRAM_IDENTIFIER id{};
    props->GetProgramIdentifier(&id, name);
    return id;
#endif
}

D3D12_WORK_GRAPHS_TIER workGraphsTier(d3d::D3D12Device& dev) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    if (FAILED(dev.d3dDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21))))
        return D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
    return o21.WorkGraphsTier;
}

void runD3D12(TestContext& ctx) {
    auto& dev = static_cast<d3d::D3D12Device&>(ctx.device());
    rhi::Device& rdev = dev;
    const bool software = dev.adapter().isSoftware();
    const std::uint32_t records = software ? 16384u : 1u << 20;
    const GraphParams params{records, software ? 16u : 64u, 0, 0};
    const std::uint32_t groups = (records + 63) / 64;
    auto& d = ctx.details();
    d["records"] = records;
    d["iterations_per_record"] = params.iterations;

    // Root signature shared by the graph and the baseline: b0 constants, u0 results, u1 counters.
    D3D12_ROOT_PARAMETER1 rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = {0, 0, sizeof(GraphParams) / 4};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    for (auto& p : rp)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1 = {3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const Com<ID3D12RootSignature> rootSig = dev.createRootSignature(rsd);

    // The graph: every node of the library, entry point "entry".
    const shaders::Blob lib = shaders::find(kModule, "graph", shaders::Format::Dxil);
    const shaders::Blob baselineCs = shaders::find("features/work_graphs_baseline", "csBaseline", shaders::Format::Dxil);
    if (!lib || !baselineCs) throw rhi::Error("work-graph shaders are not embedded in this build");
    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary = {lib.data, lib.size};
    D3D12_WORK_GRAPH_DESC graphDesc{};
    graphDesc.ProgramName = kProgram;
    graphDesc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    D3D12_GLOBAL_ROOT_SIGNATURE globalRs{rootSig.get()};
    const D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc},
        {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graphDesc},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRs}};
    D3D12_STATE_OBJECT_DESC sod{D3D12_STATE_OBJECT_TYPE_EXECUTABLE, 3, subobjects};
    Com<ID3D12StateObject> stateObject;
    dev.check(dev.device5()->CreateStateObject(&sod, __uuidof(ID3D12StateObject), stateObject.putVoid()),
              "CreateStateObject(work graph)");
    const auto graphProps = stateObject.as<ID3D12WorkGraphProperties>();
    const auto soProps = stateObject.as<ID3D12StateObjectProperties1>();
    if (!graphProps || !soProps) throw rhi::Error("the work-graph state object has no work-graph properties");
    const UINT graphIndex = graphProps->GetWorkGraphIndex(kProgram);
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS req{};
    graphProps->GetWorkGraphMemoryRequirements(graphIndex, &req);
    const D3D12_PROGRAM_IDENTIFIER program = programIdentifier(soProps.get(), kProgram);
    // The largest useful backing memory when reasonable, otherwise the minimum.
    constexpr std::uint64_t kBackingCap = 256ull << 20;
    const std::uint64_t backingSize = std::max<std::uint64_t>(
        req.MaxSizeInBytes <= kBackingCap ? req.MaxSizeInBytes : req.MinSizeInBytes, 256);
    d["backing_memory_min_kib"] = double(req.MinSizeInBytes) / 1024.0;
    d["backing_memory_max_kib"] = double(req.MaxSizeInBytes) / 1024.0;
    d["backing_memory_used_kib"] = double(backingSize) / 1024.0;

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = rootSig.get();
    cpd.CS = {baselineCs.data, baselineCs.size};
    Com<ID3D12PipelineState> baseline;
    dev.check(
        dev.d3dDevice()->CreateComputePipelineState(&cpd, __uuidof(ID3D12PipelineState), baseline.putVoid()),
        "CreateComputePipelineState(csBaseline)");

    using U = rhi::BufferUsage;
    auto results = rdev.createBuffer(
        {records * 4ull, U::Storage | U::CopySrc, rhi::MemoryType::Device, 4, "wg-results"});
    auto counters = rdev.createBuffer(
        {kCounters * 4ull, U::Storage | U::CopySrc | U::CopyDst, rhi::MemoryType::Device, 0, "wg-counters"});
    auto backing = rdev.createBuffer({backingSize, U::Storage, rhi::MemoryType::Device, 0, "wg-backing"});
    auto readResults =
        rdev.createBuffer({records * 4ull, U::CopyDst, rhi::MemoryType::Readback, 0, "wg-readback"});
    auto readCounters =
        rdev.createBuffer({kCounters * 4ull, U::CopyDst, rhi::MemoryType::Readback, 0, "wg-readback-n"});
    auto address = [](rhi::Buffer* b) { return static_cast<d3d::D3D12Buffer*>(b)->address(); };

    bool initialized = false;
    auto bindRoot = [&](ID3D12GraphicsCommandList* l) {
        l->SetComputeRootSignature(rootSig.get());
        l->SetComputeRoot32BitConstants(0, sizeof(GraphParams) / 4, &params, 0);
        l->SetComputeRootUnorderedAccessView(1, address(results.get()));
        l->SetComputeRootUnorderedAccessView(2, address(counters.get()));
    };
    auto recordGraph = [&](rhi::CommandList& cmd) {
        cmd.transition(results.get(), rhi::ResourceState::UnorderedAccess);
        cmd.transition(counters.get(), rhi::ResourceState::UnorderedAccess);
        cmd.transition(backing.get(), rhi::ResourceState::UnorderedAccess);
        auto& list = static_cast<d3d::D3D12CommandList&>(cmd);
        ID3D12GraphicsCommandList* l = list.beginNative();
        Com<ID3D12GraphicsCommandList10> l10;
        l->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), l10.putVoid());
        if (!l10) throw rhi::Error("ID3D12GraphicsCommandList10 is not available");
        bindRoot(l);
        D3D12_SET_PROGRAM_DESC sp{};
        sp.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        sp.WorkGraph.ProgramIdentifier = program;
        sp.WorkGraph.Flags =
            initialized ? D3D12_SET_WORK_GRAPH_FLAG_NONE : D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        sp.WorkGraph.BackingMemory = {address(backing.get()), backingSize};
        l10->SetProgram(&sp);
        initialized = true;
        const std::uint32_t entryRecord = groups; // SV_DispatchGrid
        D3D12_DISPATCH_GRAPH_DESC dg{};
        dg.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dg.NodeCPUInput.EntrypointIndex = 0;
        dg.NodeCPUInput.NumRecords = 1;
        dg.NodeCPUInput.pRecords = &entryRecord;
        dg.NodeCPUInput.RecordStrideInBytes = sizeof(entryRecord);
        l10->DispatchGraph(&dg);
        list.endNative();
    };
    auto recordBaseline = [&](rhi::CommandList& cmd) {
        cmd.transition(results.get(), rhi::ResourceState::UnorderedAccess);
        cmd.transition(counters.get(), rhi::ResourceState::UnorderedAccess);
        auto& list = static_cast<d3d::D3D12CommandList&>(cmd);
        ID3D12GraphicsCommandList* l = list.beginNative();
        bindRoot(l);
        l->SetPipelineState(baseline.get());
        l->Dispatch(groups, 1, 1);
        list.endNative();
    };

    // --- correctness: one graph launch, every leaf result and the record count
    ctx.progress(0.1, "checking every record of one graph launch");
    ctx.timeGpu([&](rhi::CommandList& cmd) {
        cmd.fillBuffer(counters.get(), 0, kCounters * 4ull, 0);
        cmd.fillBuffer(results.get(), 0, records * 4ull, 0);
        recordGraph(cmd);
        cmd.copyBuffer(readResults.get(), 0, results.get(), 0, records * 4ull);
        cmd.copyBuffer(readCounters.get(), 0, counters.get(), 0, kCounters * 4ull);
        cmd.transition(readResults.get(), rhi::ResourceState::HostRead);
        cmd.transition(readCounters.get(), rhi::ResourceState::HostRead);
    });
    std::vector<std::uint32_t> got(records), counts(kCounters);
    std::memcpy(got.data(), readResults->mapped(), records * 4ull);
    std::memcpy(counts.data(), readCounters->mapped(), kCounters * 4ull);
    std::uint64_t launched = 0;
    for (std::uint32_t c : counts)
        launched += c;
    std::uint32_t wrong = 0;
    std::string example;
    for (std::uint32_t i = 0; i < records; ++i) {
        const std::uint32_t want = hashChain(i * 3u + 1u, params.iterations);
        if (got[i] == want) continue;
        if (example.empty())
            example = std::format("record {}: expected {:#010x}, got {:#010x}", i, want, got[i]);
        ++wrong;
    }
    d["leaf_records_executed"] = launched;
    d["wrong_results"] = wrong;

    // --- throughput: the graph against one plain dispatch doing the same work
    ctx.progress(0.4, "work graph throughput");
    const MicroResult graph =
        measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { recordGraph(cmd); });
    ctx.progress(0.7, "the same work as one dispatch");
    const MicroResult plain =
        measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { recordBaseline(cmd); });
    const double graphRate = graph.msPerRep > 0 ? records / (graph.msPerRep * 1e-3) * 1e-6 : 0.0;
    const double plainRate = plain.msPerRep > 0 ? records / (plain.msPerRep * 1e-3) * 1e-6 : 0.0;
    ctx.addMetric("graph_mrecords_s", "Work-graph records", graphRate, "Mrecords/s");
    ctx.addMetric("dispatch_mthreads_s", "Same work as one dispatch", plainRate, "Mthreads/s");
    ctx.addMetric("graph_vs_dispatch", "Graph speed relative to a dispatch",
                  graph.msPerRep > 0 ? plain.msPerRep / graph.msPerRep : 0.0, "x");
    d["ms_graph"] = graph.msPerRep;
    d["ms_dispatch"] = plain.msPerRep;

    if (wrong || launched != records)
        ctx.setVerdict(results::Verdict::Fail,
                       std::format("{} of {} leaf records ran ({} wrong results{}{}).", launched, records,
                                   wrong, example.empty() ? "" : "; first: ", example));
    else
        ctx.setVerdict(
            results::Verdict::Pass,
            std::format("All {} records of the graph ran exactly once with the right results.", records));
}
#endif

class WorkGraphTest : public Test {
public:
    WorkGraphTest() {
        m_info.id = "feature.work_graphs";
        m_info.name = "Work graphs";
        m_info.description =
            "Launches a two-node D3D12 work graph from one CPU record, checks every record it "
            "produced, and compares its throughput with the same work as an ordinary dispatch.";
        m_info.category = Category::Feature;
        m_info.group = "Compute";
        m_info.frameBased = false;
        m_info.primaryMetric = "graph_mrecords_s";
        m_info.primaryUnit = "Mrecords/s";
        m_info.reference = 0.0; // depends on the per-record work: shown, not scored
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 95;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
#if GT_HAS_D3D12
        if (ctx.api() != rhi::Api::D3D12)
            return SupportResult::no("work graphs are a Direct3D 12 feature (Vulkan has only the provisional "
                                     "VK_AMDX_shader_enqueue)");
        auto& dev = static_cast<d3d::D3D12Device&>(ctx.device());
        if (workGraphsTier(dev) < D3D12_WORK_GRAPHS_TIER_1_0)
            return SupportResult::no("no work graphs (D3D12 work graphs tier 1.0; needs a recent driver)");
        if (dev.shaderModel() < D3D_SHADER_MODEL_6_8)
            return SupportResult::no("work graphs need shader model 6.8");
        if (!dev.device5()) return SupportResult::no("ID3D12Device5 is not available");
        if (!shaders::find(kModule, "graph", shaders::Format::Dxil))
            return SupportResult::no(
                "work-graph shaders are not embedded in this build (needs DXC 1.8 or later)");
        return SupportResult::yes();
#else
        (void)ctx;
        return SupportResult::no("work graphs are a Direct3D 12 feature (not built on this platform)");
#endif
    }

    void run(TestContext& ctx) override {
#if GT_HAS_D3D12
        runD3D12(ctx);
#else
        (void)ctx;
#endif
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::WorkGraphTest)
