#pragma once
// Direct3D 11 backend (feature level 11_0 and up). Command lists record closures that the device
// replays on the immediate context at submit (one ordered stream for every queue type). HLSL
// registers map directly to D3D11 slots per shader stage; push constants are a constant buffer at
// b13. D3D11 has no persistently mapped GPU memory: Upload and Readback buffers are CPU copies,
// synchronised when commands read them (uploads) or after the submission that filled them
// completes (readbacks, through staging resources). D3D11 views must match the shader's
// declaration, so pipelines reflect their DXBC and storage buffers take the structured or raw
// layout their first shader use asks for.

#include "command_arena.h"
#include "dxbc_reflect.h"
#include "gt/rhi/rhi.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gt::rhi::d3d11 {

// --- helpers -------------------------------------------------------------------------------------------
template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : m_p(o.m_p) {
        if (m_p) m_p->AddRef();
    }
    Com(Com&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
    ~Com() { reset(); }
    Com& operator=(Com o) noexcept {
        std::swap(m_p, o.m_p);
        return *this;
    }
    void reset() {
        if (m_p) std::exchange(m_p, nullptr)->Release();
    }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    T** put() {
        reset();
        return &m_p;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    template <typename U>
    Com<U> as() const {
        Com<U> out;
        if (m_p) m_p->QueryInterface(__uuidof(U), out.putVoid());
        return out;
    }

private:
    T* m_p = nullptr;
};

std::string narrow(const wchar_t* s);
std::wstring widen(const char* s);
std::string hresultName(HRESULT hr);
DXGI_FORMAT toDxgi(Format f);
DXGI_FORMAT toDxgiResource(Format f); ///< typeless for depth formats (sampled depth)
DXGI_FORMAT toDxgiSrv(Format f);
D3D11_COMPARISON_FUNC toD3d(CompareOp op);

class D3D11Device;
struct Executor;

std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

constexpr std::uint32_t kSlots = 16;    ///< t#, u# and s# registers tracked per stage
constexpr std::uint32_t kCbSlots = 14;  ///< b0..b13
constexpr std::uint32_t kPushSlot = 13; ///< push constants (common/bindings.slang)

// --- resources ---------------------------------------------------------------------------------------------
class D3D11Buffer final : public Buffer {
public:
    static constexpr std::uint32_t kAnyLayout = ~0u;
    D3D11Buffer(D3D11Device& dev, const BufferDesc& desc, const void* data);
    void* mapped() override { return m_shadow.empty() ? nullptr : m_shadow.data(); }
    std::byte* shadow() { return m_shadow.data(); }
    bool isUpload() const { return m_desc.memory == MemoryType::Upload || m_desc.memory == MemoryType::DeviceUpload; }
    bool isReadback() const { return m_desc.memory == MemoryType::Readback; }
    /// Storage buffer whose layout (structured with a stride, or raw) is not fixed by its usage.
    bool structurable() const { return m_structurable; }
    bool created() const { return static_cast<bool>(m_buffer); }
    /// The GPU buffer (null for Readback memory). `stride` asks for a structured layout (0 = raw);
    /// the buffer is created, or re-created with its contents, to match.
    ID3D11Buffer* gpu(std::uint32_t stride = kAnyLayout);
    ID3D11Buffer* staging() const { return m_staging.get(); } ///< Readback memory: staging copy
    std::uint32_t stride() const { return m_stride; }           ///< structured stride of the GPU buffer
    /// Upload memory read by the GPU: copy the CPU data to the GPU buffer once per submission.
    void syncForSubmission(std::uint64_t submission);
    void syncRange(std::uint64_t submission, std::uint64_t bytes); ///< transient chunks: only the used part
    void forceResync() { m_syncedSubmission = 0; }
    /// Writes before the GPU buffer exists go to its initial contents (no GPU work). False if it exists.
    bool writePending(std::uint64_t offset, const void* data, std::uint64_t size);
    bool fillPending(std::uint64_t offset, std::uint64_t size, std::uint32_t value);
    ID3D11ShaderResourceView* srv(std::uint64_t offset, std::uint64_t size, std::uint32_t stride);
    ID3D11UnorderedAccessView* uav(std::uint64_t offset, std::uint64_t size, std::uint32_t stride);

private:
    void create(std::uint32_t stride);
    D3D11Device& m_dev;
    Com<ID3D11Buffer> m_buffer, m_staging;
    std::vector<std::byte> m_shadow;  ///< Upload / Readback memory
    std::vector<std::byte> m_initial; ///< Device memory not created yet
    bool m_structurable = false;
    std::uint32_t m_stride = 0;
    UINT m_width = 0;
    std::uint64_t m_syncedSubmission = 0;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Com<ID3D11ShaderResourceView>> m_srvs;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Com<ID3D11UnorderedAccessView>> m_uavs;
};

class D3D11Texture final : public Texture {
public:
    D3D11Texture(D3D11Device& dev, const TextureDesc& desc);
    D3D11Texture(D3D11Device& dev, const TextureDesc& desc, Com<ID3D11Texture2D> existing);
    ID3D11Resource* resource() const { return m_resource.get(); }
    ID3D11ShaderResourceView* srv(Subresource sub);
    ID3D11UnorderedAccessView* uav(std::uint32_t mip);
    ID3D11RenderTargetView* rtv(std::uint32_t mip, std::uint32_t layer);
    ID3D11DepthStencilView* dsv(bool readOnly);
    UINT subresource(std::uint32_t mip, std::uint32_t layer) const;
    std::uint32_t layers() const;

private:
    D3D11Device& m_dev;
    Com<ID3D11Resource> m_resource;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, Com<ID3D11ShaderResourceView>> m_srvs;
    std::map<std::uint32_t, Com<ID3D11UnorderedAccessView>> m_uavs;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Com<ID3D11RenderTargetView>> m_rtvs;
    std::array<Com<ID3D11DepthStencilView>, 2> m_dsvs;
};

class D3D11Sampler final : public Sampler {
public:
    D3D11Sampler(D3D11Device& dev, const SamplerDesc& desc);
    ID3D11SamplerState* state() const { return m_state.get(); }

private:
    Com<ID3D11SamplerState> m_state;
};

class D3D11QueryPool final : public QueryPool {
public:
    D3D11QueryPool(D3D11Device& dev, QueryType type, std::uint32_t count);
    bool read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) override;
    ID3D11Query* query(std::uint32_t index) const { return m_queries[index].get(); }

private:
    D3D11Device& m_dev;
    std::vector<Com<ID3D11Query>> m_queries;
};

// --- pipelines ------------------------------------------------------------------------------------------------
enum StageIndex : std::uint32_t { kVS, kHS, kDS, kGS, kPS, kCS, kStages };

/// What one register holds, from the shaders' reflection.
struct SlotInfo {
    dxbc::ResourceKind kind = dxbc::ResourceKind::Other;
    std::uint32_t stride = 0; ///< structured buffers
};

/// Registers one stage uses (bit masks).
struct StageSlots {
    std::uint32_t cb = 0, srv = 0, uav = 0, sampler = 0;
};

class D3D11Pipeline final : public Pipeline {
public:
    D3D11Pipeline(D3D11Device& dev, const GraphicsPipelineDesc& desc);
    D3D11Pipeline(D3D11Device& dev, const ComputePipelineDesc& desc);
    void bindState(ID3D11DeviceContext* ctx, bool depthReadOnly) const;
    bool hasStage(StageIndex s) const { return m_present[s]; }
    const StageSlots& slots(StageIndex s) const { return m_slots[s]; }
    const StageSlots& used() const { return m_used; } ///< union over stages
    const SlotInfo& srvInfo(std::uint32_t slot) const { return m_srv[slot]; }
    const SlotInfo& uavInfo(std::uint32_t slot) const { return m_uav[slot]; }
    std::uint32_t vertexStride(std::uint32_t binding) const { return m_strides[binding]; }
    std::uint32_t vertexBindings() const { return m_vertexBindings; } ///< bit mask
    const char* name() const { return m_name.c_str(); }

private:
    void reflect(StageIndex stage, std::span<const std::uint8_t> code);
    D3D11Device& m_dev;
    std::string m_name;
    std::array<bool, kStages> m_present{};
    std::array<StageSlots, kStages> m_slots{};
    StageSlots m_used{};
    std::array<SlotInfo, kSlots> m_srv{}, m_uav{};
    std::array<std::uint32_t, 16> m_strides{};
    std::uint32_t m_vertexBindings = 0;
    Com<ID3D11VertexShader> m_vs;
    Com<ID3D11HullShader> m_hs;
    Com<ID3D11DomainShader> m_ds;
    Com<ID3D11GeometryShader> m_gs;
    Com<ID3D11PixelShader> m_ps;
    Com<ID3D11ComputeShader> m_cs;
    Com<ID3D11InputLayout> m_inputLayout;
    Com<ID3D11RasterizerState> m_raster;
    Com<ID3D11DepthStencilState> m_depth, m_depthReadOnly;
    Com<ID3D11BlendState> m_blend;
    D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

// --- execution ----------------------------------------------------------------------------------------------------
struct BoundBuffer {
    D3D11Buffer* buffer = nullptr;
    std::uint64_t offset = 0, size = 0;
    ID3D11Buffer* dynamic = nullptr; ///< constant data uploaded at replay (instead of `buffer`)
};
struct BoundTexture {
    D3D11Texture* texture = nullptr;
    Subresource sub{};
    std::uint32_t mip = 0;
};

/// Replay state: what recorded commands have bound, applied to the context lazily at draws and
/// dispatches.
struct Executor {
    D3D11Device& dev;
    ID3D11DeviceContext1* ctx;
    Executor(D3D11Device& d, ID3D11DeviceContext1* c) : dev(d), ctx(c) {}

    std::array<BoundBuffer, kCbSlots> cb{};
    std::array<BoundBuffer, kSlots> srvBuf{}, uavBuf{};
    std::array<BoundTexture, kSlots> srvTex{}, uavTex{};
    std::array<D3D11Sampler*, kSlots> samplers{};
    struct Vertex {
        D3D11Buffer* buffer = nullptr;
        std::uint64_t offset = 0;
    };
    std::array<Vertex, 16> vertex{};
    D3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
    std::uint64_t indexOffset = 0;
    D3D11Pipeline* pipeline = nullptr;
    D3D11Pipeline* appliedGraphics = nullptr;
    D3D11Pipeline* appliedCompute = nullptr;
    bool depthReadOnly = false, appliedDepthReadOnly = false;
    bool graphicsDirty = true, computeDirty = true, vertexDirty = true, indexDirty = true;
    std::uint32_t renderTargets = 0;
    std::array<std::uint32_t, kStages> boundSrvs{}; ///< SRV slots set per stage (to clear later)
    std::uint32_t boundComputeUavs = 0, boundGraphicsUavs = 0;
    std::uint64_t bufferEpoch = 0;

    void reset(); ///< start of a submission: forget everything
    void bindingsChanged() { graphicsDirty = computeDirty = true; }
    void prepareDraw();
    void prepareDispatch();
    /// Before render targets change: no SRV or UAV may alias them.
    void unbindForPass();
    /// Constant data for b`slot`: written to a dynamic buffer (Map with discard) and bound.
    void uploadConstants(std::uint32_t slot, const void* data, std::uint64_t size);

private:
    void syncEpoch();
    void bindStage(StageIndex s);
    void clearSrvs(StageIndex s);
    ID3D11ShaderResourceView* srvFor(std::uint32_t slot, const SlotInfo& info);
    ID3D11UnorderedAccessView* uavFor(std::uint32_t slot, const SlotInfo& info);
    std::array<std::array<Com<ID3D11Buffer>, 5>, kCbSlots> m_constants{};
    struct DynamicStructured {
        Com<ID3D11Buffer> buffer;
        Com<ID3D11ShaderResourceView> view;
        std::uint32_t stride = 0, capacity = 0, viewElements = 0;
    };
    std::array<DynamicStructured, kSlots> m_dynamicSrvs{};
};

// --- command list ----------------------------------------------------------------------------------------------------
class D3D11CommandList final : public CommandList {
public:
    D3D11CommandList(D3D11Device& dev, QueueType queue);
    ~D3D11CommandList() override;
    void setLastSubmit(std::uint64_t v) { m_lastSubmit = v; }
    const CommandArena<Executor>& commands() const { return m_cmds; }
    /// Copies the CPU-written transient memory to its GPU buffers (before replay).
    void uploadTransient(std::uint64_t submission);

    void begin() override;
    void end() override;
    void transition(Texture*, ResourceState) override { requireRecording("transition"); }
    void transition(Buffer*, ResourceState) override { requireRecording("transition"); }
    void uavBarrier() override { requireRecording("uavBarrier"); } // D3D11 orders UAV accesses itself
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) override;
    void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) override;
    void setPipeline(Pipeline* pipeline) override;
    void setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) override;
    void setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) override;
    void setUniformBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset, std::uint64_t size) override;
    void setUniformData(std::uint32_t slot, const void* data, std::uint64_t size) override;
    void setTexture(std::uint32_t slot, Texture* texture, Subresource sub) override;
    void setStorageTexture(std::uint32_t slot, Texture* texture, std::uint32_t mip) override;
    void setStorageBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset, std::uint64_t size) override;
    void setReadOnlyBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset, std::uint64_t size) override;
    void setSampler(std::uint32_t slot, Sampler* sampler) override;
    void pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) override;
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                     std::int32_t vertexOffset, std::uint32_t firstInstance) override;
    void drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount, std::uint32_t stride) override;
    void drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                             std::uint32_t stride) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void dispatchIndirect(Buffer* args, std::uint64_t offset) override;
    void copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                    std::uint64_t size) override;
    void copyTexture(Texture* dst, Texture* src) override;
    void copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                             std::uint32_t mip, std::uint32_t layer) override;
    void copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                             std::uint64_t srcOffset, std::uint32_t rowPitch) override;
    void fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override;
    void resolveTexture(Texture* dst, Texture* src) override;
    void resetQueries(QueryPool*, std::uint32_t, std::uint32_t) override { requireRecording("resetQueries"); }
    void writeTimestamp(QueryPool* pool, std::uint32_t index) override;
    void beginQuery(QueryPool* pool, std::uint32_t index) override;
    void endQuery(QueryPool* pool, std::uint32_t index) override;
    void resolveQueries(QueryPool*, std::uint32_t, std::uint32_t) override { requireRecording("resolveQueries"); }
    void beginMarker(const char* name) override;
    void endMarker() override;
    TransientAllocation allocTransient(std::uint64_t size, std::uint64_t alignment) override;

private:
    void requireRecording(const char* what) const;
    void beforeWork(const char* what);
    const std::byte* stash(const void* data, std::size_t size);
    D3D11Device& m_dev;
    CommandArena<Executor> m_cmds;
    bool m_recording = false, m_inRenderPass = false;
    std::uint64_t m_lastSubmit = 0;
    std::vector<std::pair<D3D11Texture*, D3D11Texture*>> m_pendingResolves;
    D3D11Pipeline* m_pipe = nullptr;
    std::uint32_t m_cbMask = 0, m_srvTexMask = 0, m_srvBufMask = 0, m_uavTexMask = 0, m_uavBufMask = 0,
                  m_samplerMask = 0;
    std::array<std::uint32_t, 32> m_push{};
    bool m_pushDirty = true;
    struct Chunk {
        std::unique_ptr<D3D11Buffer> buffer;
        std::uint64_t used = 0;
    };
    std::vector<Chunk> m_chunks;
    std::size_t m_chunk = 0;
    std::vector<std::unique_ptr<std::byte[]>> m_data; ///< CPU copies of uniform data (64 KiB blocks)
    std::size_t m_dataBlock = 0, m_dataUsed = 0;
};

// --- swapchain ------------------------------------------------------------------------------------------------------
class D3D11Swapchain final : public Swapchain {
public:
    D3D11Swapchain(D3D11Device& dev, const SwapchainDesc& desc);
    ~D3D11Swapchain() override;
    Texture* acquire() override { return m_image.get(); }
    void resize(std::uint32_t width, std::uint32_t height) override;
    Format format() const override { return m_format; }
    bool isHdr() const override { return m_hdr; }
    std::string presentModeName() const override;
    void present();

private:
    void wrap();
    D3D11Device& m_dev;
    SwapchainDesc m_desc;
    Com<IDXGISwapChain1> m_swapchain;
    std::unique_ptr<D3D11Texture> m_image;
    Format m_format = Format::BGRA8_UNORM;
    DXGI_SWAP_EFFECT m_effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT m_flags = 0;
    bool m_tearing = false, m_hdr = false;
};

// --- device -------------------------------------------------------------------------------------------------------------
class D3D11Device final : public Device {
public:
    explicit D3D11Device(const DeviceCreateInfo& info);
    ~D3D11Device() override;

    std::vector<Capability> capabilities() const override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    std::unique_ptr<Sampler> createSampler(const SamplerDesc& desc) override;
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override;
    std::unique_ptr<QueryPool> createQueryPool(QueryType type, std::uint32_t count) override;
    std::unique_ptr<CommandList> createCommandList(QueueType queue) override;
    std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
    std::uint64_t submit(QueueType queue, const SubmitInfo& info) override;
    void present(Swapchain* swapchain) override;
    void wait(QueueType queue, std::uint64_t value) override;
    bool isComplete(QueueType queue, std::uint64_t value) override;
    void waitIdle() override;
    void uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) override;
    void uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                       std::uint32_t rowPitch) override;
    std::vector<std::uint8_t> readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) override;
    std::uint32_t textureCopyPitchAlignment() const override { return 4; }
    MemoryBudget memoryBudget() const override;
    ValidationCounts validationCounts() const override { return m_validationCounts; }
    std::uint32_t renderTargetSampleCounts(Format format) const override;
    void setDebugName(Buffer* buffer, const char* name) override;
    void setDebugName(Texture* texture, const char* name) override;

    // --- internal
    ID3D11Device1* d3d() const { return m_device.get(); }
    ID3D11DeviceContext1* context() const { return m_context.get(); }
    IDXGIFactory2* dxgiFactory() const { return m_factory.get(); }
    ID3DUserDefinedAnnotation* annotation() const { return m_annotation.get(); }
    D3D_FEATURE_LEVEL featureLevel() const { return m_featureLevel; }
    bool tearingSupported() const { return m_tearing; }
    bool constantBufferOffsets() const { return m_cbOffsets; }
    double timestampFrequency() const { return m_timestampFrequency; }
    std::span<const std::uint8_t> shaderBytecode(const ShaderRef& ref) const;
    void setName(ID3D11DeviceChild* object, const char* name) const;
    /// Throws for a failed HRESULT; device removal is reported with its reason.
    void check(HRESULT hr, const char* what) const;
    /// During replay: runs `fn` once the submission being replayed has completed (readback copies
    /// into CPU memory).
    void afterReplay(std::function<void()> fn);
    void drainMessages() const;
    std::uint64_t replaying() const { return m_submitted + 1; } ///< submission value during replay
    /// Bumped when a buffer's GPU resource is re-created: bound views are stale.
    std::uint64_t bufferEpoch() const { return m_bufferEpoch; }
    void bufferRecreated() { ++m_bufferEpoch; }

private:
    void fillFeatures();
    void retire(std::uint64_t upTo);

    Com<IDXGIFactory2> m_factory;
    Com<IDXGIAdapter1> m_adapterDxgi;
    Com<ID3D11Device1> m_device;
    Com<ID3D11DeviceContext1> m_context;
    Com<ID3D11InfoQueue> m_infoQueue;
    Com<ID3DUserDefinedAnnotation> m_annotation;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    bool m_tearing = false, m_cbOffsets = false;
    double m_timestampFrequency = 1e9;
    mutable ValidationCounts m_validationCounts;
    std::unique_ptr<Executor> m_exec;
    struct Pending {
        std::uint64_t value;
        Com<ID3D11Query> event, disjoint;
        std::vector<std::function<void()>> after;
    };
    std::vector<Pending> m_pending;
    std::vector<std::function<void()>> m_replayAfter;
    std::uint64_t m_submitted = 0, m_completed = 0, m_bufferEpoch = 0;
};

} // namespace gt::rhi::d3d11
