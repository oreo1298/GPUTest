// Sampler feedback (feature.sampler_feedback, Direct3D 12 only): a MinMip feedback map paired with
// a mipmapped texture records, per mip region, the finest mip level shaders asked for — what
// texture-streaming engines use to decide which tiles to load. The test writes a known pattern
// (one sample per region at a chosen mip, some regions untouched), decodes the map with
// ResolveSubresourceRegion and compares it region by region, then measures what writing
// feedback costs on top of the same texture samples (shaders/src/features/sampler_feedback.slang).
// Sampler feedback has no Vulkan equivalent, so the test records native D3D12 commands.

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"
#include "gt/shaders.h"

#if GT_HAS_D3D12
#include "d3d12/d3d12_device.h"
#endif

#include <bit>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

#if GT_HAS_D3D12
constexpr std::uint32_t kRegions = 16; // regions per axis in the verification pattern (csPattern)
constexpr const char* kModule = "features/sampler_feedback";

struct FeedbackParams {
    std::uint32_t textureSize, regionSize, maxLod, samples;
};

/// Mirror of expectedLod() in sampler_feedback.slang; 0xFF = not sampled.
std::uint8_t expectedLod(std::uint32_t x, std::uint32_t y, std::uint32_t maxLod) {
    if ((x + y) % 7u == 0u) return 0xFF;
    return static_cast<std::uint8_t>((x * 3u + y * 5u) % (maxLod + 1u));
}

namespace d3d = rhi::d3d12;
using d3d::Com;

Com<ID3D12Device8> device8(d3d::D3D12Device& dev) {
    Com<ID3D12Device8> d8;
    dev.d3dDevice()->QueryInterface(__uuidof(ID3D12Device8), d8.putVoid());
    return d8;
}

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                         D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

/// Shader-visible and CPU-only descriptors borrowed from the device heaps for the test's lifetime.
struct Descriptors {
    d3d::D3D12Device& dev;
    std::uint32_t first = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE clearCpu{};
    explicit Descriptors(d3d::D3D12Device& d) : dev(d) {
        first = dev.viewHeap().allocRange(2);
        clearCpu = dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).alloc();
    }
    ~Descriptors() {
        try {
            dev.waitIdle();
        } catch (const std::exception&) {
        }
        dev.viewHeap().freeRange(first, 2);
        dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).free(clearCpu);
    }
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

void runD3D12(TestContext& ctx) {
    auto& dev = static_cast<d3d::D3D12Device&>(ctx.device());
    const bool software = dev.adapter().isSoftware();
    const Com<ID3D12Device8> dev8 = device8(dev);
    const std::uint32_t size = software ? 256u : 2048u;
    const std::uint32_t mips = static_cast<std::uint32_t>(std::bit_width(size));
    const std::uint32_t region = size / kRegions;
    const std::uint32_t maxLod = static_cast<std::uint32_t>(std::bit_width(region)) - 1u; // log2(region)
    auto& d = ctx.details();
    d["texture"] = std::format("{0}x{0}, {1} mips", size, mips);
    d["mip_region"] = std::format("{0}x{0}", region);

    // Paired texture (contents are irrelevant to feedback) and the MinMip feedback map.
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = td.height = size;
    td.mipLevels = mips;
    td.usage = rhi::TextureUsage::Sampled;
    td.name = "feedback-paired";
    auto texture = dev.createTexture(td);
    ID3D12Resource* texRes = static_cast<d3d::D3D12Texture*>(texture.get())->resource();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 fd{};
    fd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fd.Width = size;
    fd.Height = size;
    fd.DepthOrArraySize = 1;
    fd.MipLevels = static_cast<UINT16>(mips);
    fd.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    fd.SampleDesc = {1, 0};
    fd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    fd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    fd.SamplerFeedbackMipRegion = {region, region, 1};
    Com<ID3D12Resource> feedback;
    dev.check(dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &fd,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                             __uuidof(ID3D12Resource), feedback.putVoid()),
              "CreateCommittedResource2(feedback map)");
    dev.setName(feedback.get(), "feedback-map");

    // Decoded map: one R8_UINT texel per region, and a readback buffer for it.
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = kRegions;
    dd.Height = kRegions;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R8_UINT;
    dd.SampleDesc = {1, 0};
    Com<ID3D12Resource> decoded;
    dev.check(dev.d3dDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
                                                       D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                       __uuidof(ID3D12Resource), decoded.putVoid()),
              "CreateCommittedResource(decoded feedback)");
    dev.setName(decoded.get(), "feedback-decoded");
    constexpr std::uint32_t kPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    rhi::Device& rdev = dev;
    auto readback =
        rdev.createBuffer(rhi::BufferDesc{std::uint64_t(kPitch) * kRegions, rhi::BufferUsage::CopyDst,
                                          rhi::MemoryType::Readback, 0, "feedback-readback"});
    auto sink = rdev.createBuffer(
        rhi::BufferDesc{16, rhi::BufferUsage::Storage, rhi::MemoryType::Device, 16, "feedback-sink"});
    const D3D12_GPU_VIRTUAL_ADDRESS sinkAddress = static_cast<d3d::D3D12Buffer*>(sink.get())->address();

    // Descriptors: [first] paired texture SRV, [first + 1] feedback UAV; a CPU copy of the UAV for clears.
    Descriptors desc(dev);
    auto& heap = dev.viewHeap();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = mips;
    dev.d3dDevice()->CreateShaderResourceView(texRes, &sv, heap.cpu(desc.first));
    dev8->CreateSamplerFeedbackUnorderedAccessView(texRes, feedback.get(), heap.cpu(desc.first + 1));
    dev8->CreateSamplerFeedbackUnorderedAccessView(texRes, feedback.get(), desc.clearCpu);

    // Root signature: b0 constants, (t0, u0) table, u1 root UAV, s0 static point sampler.
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 1};
    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, sizeof(FeedbackParams) / 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    for (auto& p : params)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1 = {3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const Com<ID3D12RootSignature> rootSig = dev.createRootSignature(rsd);

    auto pipeline = [&](const char* entry) {
        const shaders::Blob blob = shaders::find(kModule, entry, shaders::Format::Dxil);
        if (!blob)
            throw rhi::Error(std::format("shader {}:{} is not embedded in this build", kModule, entry));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig.get();
        pd.CS = {blob.data, blob.size};
        Com<ID3D12PipelineState> pso;
        dev.check(
            dev.d3dDevice()->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), pso.putVoid()),
            std::format("CreateComputePipelineState({})", entry).c_str());
        return pso;
    };
    const auto pattern = pipeline("csPattern");
    const auto plain = pipeline("csSample");
    const auto withFeedback = pipeline("csSampleFeedback");

    auto dispatch = [&](rhi::CommandList& cmd, ID3D12PipelineState* pso, std::uint32_t groups,
                        const FeedbackParams& p) {
        cmd.transition(texture.get(), rhi::ResourceState::ShaderRead);
        cmd.transition(sink.get(), rhi::ResourceState::UnorderedAccess);
        auto& list = static_cast<d3d::D3D12CommandList&>(cmd);
        ID3D12GraphicsCommandList* l = list.beginNative();
        l->SetComputeRootSignature(rootSig.get());
        l->SetPipelineState(pso);
        l->SetComputeRoot32BitConstants(0, sizeof(FeedbackParams) / 4, &p, 0);
        l->SetComputeRootDescriptorTable(1, heap.gpu(desc.first));
        l->SetComputeRootUnorderedAccessView(2, sinkAddress);
        l->Dispatch(groups, 1, 1);
        list.endNative();
    };

    // --- correctness: clear, write the pattern, decode, read back
    ctx.progress(0.1, "writing and decoding a known feedback pattern");
    const FeedbackParams patternParams{size, region, maxLod, 1};
    ctx.timeGpu([&](rhi::CommandList& cmd) {
        auto& list = static_cast<d3d::D3D12CommandList&>(cmd);
        ID3D12GraphicsCommandList* l = list.beginNative();
        const UINT zero[4] = {};
        l->ClearUnorderedAccessViewUint(heap.gpu(desc.first + 1), desc.clearCpu, feedback.get(), zero, 0,
                                        nullptr);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = feedback.get();
        l->ResourceBarrier(1, &uav);
        list.endNative();
        dispatch(cmd, pattern.get(), (kRegions * kRegions + 63) / 64, patternParams);
        l = list.beginNative();
        Com<ID3D12GraphicsCommandList1> l1;
        l->QueryInterface(__uuidof(ID3D12GraphicsCommandList1), l1.putVoid());
        if (!l1) throw rhi::Error("ID3D12GraphicsCommandList1 is not available");
        D3D12_RESOURCE_BARRIER toResolve = transitionBarrier(
            feedback.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        l->ResourceBarrier(1, &toResolve);
        l1->ResolveSubresourceRegion(decoded.get(), 0, 0, 0, feedback.get(), UINT_MAX, nullptr,
                                     DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
        D3D12_RESOURCE_BARRIER toCopy[2] = {
            transitionBarrier(decoded.get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                              D3D12_RESOURCE_STATE_COPY_SOURCE),
            transitionBarrier(feedback.get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        l->ResourceBarrier(2, toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = static_cast<d3d::D3D12Buffer*>(readback.get())->resource();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, kRegions, kRegions, 1, kPitch};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = decoded.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER back = transitionBarrier(decoded.get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                        D3D12_RESOURCE_STATE_RESOLVE_DEST);
        l->ResourceBarrier(1, &back);
        list.endNative();
    });
    const auto* rows = static_cast<const std::uint8_t*>(readback->mapped());
    std::uint32_t exact = 0, finer = 0, wrong = 0;
    std::string firstMismatch;
    for (std::uint32_t y = 0; y < kRegions; ++y)
        for (std::uint32_t x = 0; x < kRegions; ++x) {
            const std::uint8_t got = rows[std::size_t(y) * kPitch + x];
            const std::uint8_t want = expectedLod(x, y, maxLod);
            if (got == want) {
                ++exact;
                continue;
            }
            // A finer mip than requested (or a region marked that was not sampled) is conservative:
            // a streaming engine loads more than needed but never shows missing detail.
            if (got < want)
                ++finer;
            else
                ++wrong;
            if (firstMismatch.empty())
                firstMismatch = std::format("region ({}, {}): expected {}, got {}", x, y,
                                            want == 0xFF ? std::string("not sampled") : std::to_string(want),
                                            got == 0xFF ? std::string("not sampled") : std::to_string(got));
        }
    const std::uint32_t total = kRegions * kRegions;
    d["regions_exact"] = exact;
    d["regions_conservative"] = finer;
    d["regions_wrong"] = wrong;

    // --- cost of writing feedback on top of the same samples
    const std::uint32_t threads = software ? 4096u : 1u << 20;
    const FeedbackParams workParams{size, region, maxLod, software ? 16u : 64u};
    const double samples = double(threads) * workParams.samples;
    ctx.progress(0.4, "texture samples without feedback");
    const MicroResult base = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
        dispatch(cmd, plain.get(), threads / 64, workParams);
    });
    ctx.progress(0.7, "texture samples with feedback writes");
    const MicroResult fb = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
        dispatch(cmd, withFeedback.get(), threads / 64, workParams);
    });
    const double baseRate = base.msPerRep > 0 ? samples / (base.msPerRep * 1e-3) * 1e-9 : 0.0;
    const double fbRate = fb.msPerRep > 0 ? samples / (fb.msPerRep * 1e-3) * 1e-9 : 0.0;
    const double overhead = base.msPerRep > 0 ? 100.0 * (fb.msPerRep - base.msPerRep) / base.msPerRep : 0.0;
    ctx.addMetric("feedback_overhead_pct", "Feedback write overhead", overhead, "%", false);
    ctx.addMetric("feedback_gsamples_s", "Samples with feedback", fbRate, "Gsamples/s");
    ctx.addMetric("plain_gsamples_s", "Samples without feedback", baseRate, "Gsamples/s");
    d["ms_plain"] = base.msPerRep;
    d["ms_feedback"] = fb.msPerRep;

    if (wrong)
        ctx.setVerdict(
            results::Verdict::Fail,
            std::format("{} of {} feedback regions report a coarser mip than was sampled (first: {}).", wrong,
                        total, firstMismatch));
    else if (finer)
        ctx.setVerdict(
            results::Verdict::Warn,
            std::format("Feedback is conservative: {} of {} regions report a finer mip than was sampled "
                        "(first: {}).",
                        finer, total, firstMismatch));
    else
        ctx.setVerdict(
            results::Verdict::Pass,
            std::format("All {} feedback regions decode to the mip level that was sampled.", total));
}
#endif

class SamplerFeedbackTest : public Test {
public:
    SamplerFeedbackTest() {
        m_info.id = "feature.sampler_feedback";
        m_info.name = "Sampler feedback";
        m_info.description =
            "Records which mip levels shaders sample in a D3D12 MinMip feedback map, checks the decoded "
            "map region by region, and measures the cost of writing feedback.";
        m_info.category = Category::Feature;
        m_info.group = "Resources";
        m_info.frameBased = false;
        m_info.primaryMetric = "feedback_overhead_pct";
        m_info.primaryUnit = "%";
        m_info.higherIsBetter = false;
        m_info.reference = 0.0; // a ratio: shown, not scored
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 75;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
#if GT_HAS_D3D12
        if (ctx.api() != rhi::Api::D3D12)
            return SupportResult::no("sampler feedback is a Direct3D 12 feature");
        auto& dev = static_cast<d3d::D3D12Device&>(ctx.device());
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (FAILED(dev.d3dDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) ||
            o7.SamplerFeedbackTier == D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED)
            return SupportResult::no("no sampler feedback (D3D12 sampler feedback tier 0.9 or later)");
        if (dev.shaderModel() < D3D_SHADER_MODEL_6_5)
            return SupportResult::no("sampler feedback needs shader model 6.5");
        if (!device8(dev))
            return SupportResult::no("ID3D12Device8 is not available (update Windows or the Agility SDK)");
        if (!shaders::find(kModule, "csPattern", shaders::Format::Dxil))
            return SupportResult::no("sampler feedback shaders are not embedded in this build");
        return SupportResult::yes();
#else
        (void)ctx;
        return SupportResult::no("sampler feedback is a Direct3D 12 feature (not built on this platform)");
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

GT_REGISTER_TEST(gt::bench::SamplerFeedbackTest)
