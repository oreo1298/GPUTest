#pragma once
// Direct3D 12 backend. Public so feature tests can reach native objects:
//   auto* dx = dynamic_cast<gt::rhi::d3d12::D3D12Device*>(&device);
//
// Binding model: each pipeline layout becomes a root signature with the push constants as
// root constants (b13), buffers as root descriptors, and one descriptor table each for the
// texture views and the samplers (descriptors are copied into shader-visible heaps at draw
// time). Resource states are tracked per command list for resources that decay to COMMON
// after every submission (buffers, textures created with concurrentQueues) and globally for
// the other textures; every binding transitions its resource to a state that allows the use.

#include "d3d12_common.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace gt::rhi::d3d12 {

class D3D12Device;

// --- descriptor heaps ------------------------------------------------------------------------
/// Non-shader-visible heap for views created with the resources (copied to the GPU heap at bind).
class CpuDescriptorHeap {
public:
    CpuDescriptorHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t perHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE alloc();
    void free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

private:
    ID3D12Device* m_dev;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type;
    std::uint32_t m_perHeap;
    std::uint32_t m_increment;
    std::uint32_t m_used = 0; ///< in the newest heap
    std::vector<Com<ID3D12DescriptorHeap>> m_heaps;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_free;
    std::mutex m_mutex;
};

/// Shader-visible heap split into fixed-size blocks that command lists own while recording, plus
/// persistent ranges (texture tables) taken from the top.
class GpuDescriptorHeap {
public:
    GpuDescriptorHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity, std::uint32_t blockSize);
    ID3D12DescriptorHeap* heap() const { return m_heap.get(); }
    std::uint32_t blockSize() const { return m_blockSize; }
    std::uint32_t allocBlock();
    void freeBlock(std::uint32_t first);
    std::uint32_t allocRange(std::uint32_t count);
    void freeRange(std::uint32_t first, std::uint32_t count);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(std::uint32_t index) const;

private:
    Com<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
    std::uint32_t m_increment = 0;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_blockSize = 0;
    std::uint32_t m_blockTop = 0;  ///< blocks grow upwards from 0
    std::uint32_t m_rangeBase = 0; ///< persistent ranges grow downwards from the capacity
    std::vector<std::uint32_t> m_freeBlocks;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_freeRanges; ///< (first, count)
    std::mutex m_mutex;
};

// --- resources -------------------------------------------------------------------------------
class D3D12Buffer final : public Buffer {
public:
    D3D12Buffer(D3D12Device& dev, const BufferDesc& desc);
    ~D3D12Buffer() override;
    void* mapped() override { return m_mapped; }
    bool isDeviceLocalHostVisible() const override { return m_gpuUploadHeap; }
    ID3D12Resource* resource() const { return m_resource.get(); }
    D3D12_GPU_VIRTUAL_ADDRESS address() const { return m_address; }
    /// Upload, readback and acceleration-structure buffers keep one state for their lifetime.
    bool fixedState() const { return m_fixed; }
    bool allowsUav() const { return m_uav; }

private:
    D3D12Device& m_dev;
    Com<ID3D12Resource> m_resource;
    D3D12_GPU_VIRTUAL_ADDRESS m_address = 0;
    void* m_mapped = nullptr;
    bool m_fixed = false;
    bool m_uav = false;
    bool m_gpuUploadHeap = false;
};

class D3D12Texture final : public Texture {
public:
    D3D12Texture(D3D12Device& dev, const TextureDesc& desc);
    /// Wraps a swapchain image.
    D3D12Texture(D3D12Device& dev, const TextureDesc& desc, Com<ID3D12Resource> image);
    ~D3D12Texture() override;
    ID3D12Resource* resource() const { return m_resource.get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE srv(Subresource sub);
    D3D12_CPU_DESCRIPTOR_HANDLE uav(std::uint32_t mip);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(std::uint32_t mip, std::uint32_t layer);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(bool readOnly);
    /// Created with simultaneous access (TextureDesc::concurrentQueues): its state returns to
    /// COMMON after every submission, so command lists track it from COMMON.
    bool decays() const { return m_simultaneous; }
    std::uint32_t arrayLayers() const;
    ResourceState state = ResourceState::Undefined;                  ///< last RHI state requested
    D3D12_RESOURCE_STATES d3dState = D3D12_RESOURCE_STATE_COMMON;    ///< actual state (non-decaying textures)

private:
    D3D12Device& m_dev;
    Com<ID3D12Resource> m_resource;
    bool m_simultaneous = false;
    struct ViewKey {
        std::uint32_t a, b, c, d;
        bool operator<(const ViewKey& o) const { return std::tie(a, b, c, d) < std::tie(o.a, o.b, o.c, o.d); }
    };
    std::map<ViewKey, D3D12_CPU_DESCRIPTOR_HANDLE> m_srvs, m_uavs, m_rtvs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> m_dsvs{};
};

class D3D12Sampler final : public Sampler {
public:
    D3D12Sampler(D3D12Device& dev, const SamplerDesc& desc);
    ~D3D12Sampler() override;
    D3D12_CPU_DESCRIPTOR_HANDLE handle() const { return m_handle; }

private:
    D3D12Device& m_dev;
    D3D12_CPU_DESCRIPTOR_HANDLE m_handle{};
};

class D3D12QueryPool final : public QueryPool {
public:
    D3D12QueryPool(D3D12Device& dev, QueryType type, std::uint32_t count);
    ~D3D12QueryPool() override;
    bool read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) override;
    /// The heap for a command list type (copy queues need their own timestamp heap).
    ID3D12QueryHeap* heap(QueueType queue);
    D3D12_QUERY_TYPE queryType() const;
    std::uint32_t valuesPerQuery() const { return m_type == QueryType::PipelineStatistics ? 11u : 1u; }
    ID3D12Resource* readback() const { return m_readback->resource(); }
    void noteWrite(std::uint32_t index, QueueType queue) { m_lastQueue[index] = queue; }

private:
    D3D12Device& m_dev;
    Com<ID3D12QueryHeap> m_heap, m_copyHeap;
    std::unique_ptr<D3D12Buffer> m_readback;
    std::vector<QueueType> m_lastQueue;
};

class D3D12TextureTable final : public TextureTable {
public:
    D3D12TextureTable(D3D12Device& dev, std::uint32_t capacity);
    ~D3D12TextureTable() override;
    void set(std::uint32_t index, Texture* texture) override;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle() const;

private:
    D3D12Device& m_dev;
    std::uint32_t m_first = 0;
};

class D3D12Accel final : public AccelerationStructure {
public:
    D3D12Accel(D3D12Device& dev, bool topLevel, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info,
               std::uint32_t maxInstances, const char* name);
    D3D12_GPU_VIRTUAL_ADDRESS address() const { return m_storageBuffer->address(); }
    D3D12_GPU_VIRTUAL_ADDRESS scratchAddress() const { return m_scratchBuffer->address(); }
    D3D12Buffer* scratchBuffer() const { return m_scratchBuffer.get(); }
    std::uint32_t maxInstances() const { return m_maxInstances; }
    bool allowUpdate = false;

private:
    std::unique_ptr<D3D12Buffer> m_storageBuffer, m_scratchBuffer;
    std::uint32_t m_maxInstances = 0;
};

// --- pipelines -------------------------------------------------------------------------------
/// Where each binding of a LayoutDesc lives in its root signature.
struct RootLayout {
    struct Entry {
        BindingType type;
        std::uint32_t slot;
        std::uint32_t index; ///< root parameter (root descriptors) or offset in the table
    };
    std::uint32_t pushConstantDwords = 0;
    int pushParam = -1;
    std::vector<Entry> rootDescriptors;
    std::vector<Entry> viewTable;
    int viewTableParam = -1;
    std::vector<Entry> samplerTable;
    int samplerTableParam = -1;
    int textureTableParam = -1;
};

struct RootSignature {
    Com<ID3D12RootSignature> signature;
    RootLayout layout;
};

class D3D12Pipeline final : public Pipeline {
public:
    D3D12Pipeline(D3D12Device& dev, const GraphicsPipelineDesc& desc);
    D3D12Pipeline(D3D12Device& dev, const ComputePipelineDesc& desc);
    D3D12Pipeline(D3D12Device& dev, const RayTracingPipelineDesc& desc);
    ~D3D12Pipeline() override;
    ID3D12PipelineState* pso() const { return m_pso.get(); }
    /// Ray-tracing pipelines: the state object and the shader-table regions for DispatchRays.
    ID3D12StateObject* stateObject() const { return m_stateObject.get(); }
    bool rayTracing() const { return static_cast<bool>(m_stateObject); }
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE raygenRecord() const { return m_raygen; }
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE missTable() const { return m_miss; }
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hitTable() const { return m_hit; }
    const RootSignature& root() const { return *m_root; }
    const LayoutDesc& layoutDesc() const { return m_layoutDesc; }
    D3D_PRIMITIVE_TOPOLOGY topology() const { return m_topology; }
    std::uint32_t vertexStride(std::uint32_t binding) const { return binding < m_strides.size() ? m_strides[binding] : 0; }
    const std::string& name() const { return m_name; }

private:
    D3D12Device& m_dev;
    Com<ID3D12PipelineState> m_pso;
    Com<ID3D12StateObject> m_stateObject;
    std::unique_ptr<D3D12Buffer> m_shaderTable;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE m_raygen{};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE m_miss{}, m_hit{};
    const RootSignature* m_root = nullptr;
    LayoutDesc m_layoutDesc;
    D3D_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::array<std::uint32_t, 16> m_strides{};
    std::string m_name;
};

// --- swapchain ---------------------------------------------------------------------------------
class D3D12Swapchain final : public Swapchain {
public:
    D3D12Swapchain(D3D12Device& dev, const SwapchainDesc& desc);
    ~D3D12Swapchain() override;
    Texture* acquire() override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Format format() const override { return m_format; }
    bool isHdr() const override { return m_hdr; }
    std::string presentModeName() const override;
    void present();

private:
    void wrapImages();
    D3D12Device& m_dev;
    SwapchainDesc m_desc;
    Com<IDXGISwapChain3> m_swapchain;
    std::vector<std::unique_ptr<D3D12Texture>> m_images;
    Format m_format = Format::BGRA8_UNORM;
    bool m_hdr = false;
    bool m_tearing = false;
};

// --- command list ------------------------------------------------------------------------------
class D3D12CommandList final : public CommandList {
public:
    D3D12CommandList(D3D12Device& dev, QueueType queue);
    ~D3D12CommandList() override;
    ID3D12GraphicsCommandList* handle() const { return m_list.get(); }
    void setLastSubmit(std::uint64_t value) { m_lastSubmit = value; }
    /// For feature tests that record D3D12 commands directly: flushes pending barriers and returns the
    /// list. Call endNative() afterwards; the next draw or dispatch then needs setPipeline() and re-binds
    /// all root state (viewports and scissors set natively are not restored).
    ID3D12GraphicsCommandList* beginNative();
    void endNative();

    void begin() override;
    void end() override;
    void transition(Texture* texture, ResourceState state) override;
    void transition(Buffer* buffer, ResourceState state) override;
    void uavBarrier() override;
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
    void setTextureTable(TextureTable* table) override;
    void drawMeshTasks(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void setShadingRate(std::uint32_t width, std::uint32_t height) override;
    void copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                    std::uint64_t size) override;
    void copyTexture(Texture* dst, Texture* src) override;
    void copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                             std::uint32_t mip, std::uint32_t layer) override;
    void copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                             std::uint64_t srcOffset, std::uint32_t rowPitch) override;
    void fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override;
    void resolveTexture(Texture* dst, Texture* src) override;
    void resetQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) override;
    void writeTimestamp(QueryPool* pool, std::uint32_t index) override;
    void beginQuery(QueryPool* pool, std::uint32_t index) override;
    void endQuery(QueryPool* pool, std::uint32_t index) override;
    void resolveQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) override;
    void beginMarker(const char* name) override;
    void endMarker() override;
    TransientAllocation allocTransient(std::uint64_t size, std::uint64_t alignment) override;
    void buildBlas(AccelerationStructure* blas, const BlasDesc& desc, bool update) override;
    void buildTlas(AccelerationStructure* tlas, std::span<const AccelInstance> instances, bool update) override;
    void accelBarrier() override;
    void setAccelerationStructure(std::uint32_t slot, AccelerationStructure* tlas) override;
    void traceRays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) override;

private:
    struct Bound {
        D3D12Buffer* buffer = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        D3D12Texture* texture = nullptr;
        Subresource sub{};
        std::uint32_t mip = 0;
        D3D12Sampler* sampler = nullptr;
        D3D12Accel* accel = nullptr;
    };
    static constexpr std::uint32_t kSlots = 16;
    Bound& slot(char cls, std::uint32_t index);
    const Bound& bound(BindingType type, std::uint32_t slot) const;
    void requireRecording(const char* what) const;
    bool isGraphics() const { return m_queue == QueueType::Graphics; }

    // states
    D3D12_RESOURCE_STATES stateFor(ResourceState s) const;
    D3D12_RESOURCE_STATES shaderReadState() const;
    D3D12_RESOURCE_STATES& trackedState(D3D12Texture* t);
    void applyShadingRate();
    void require(D3D12Texture* t, D3D12_RESOURCE_STATES want);
    void require(D3D12Buffer* b, D3D12_RESOURCE_STATES want);
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES want, const char* name);
    void flushBarriers();

    // draws
    void flushBindings(bool compute);
    void flushVertexBuffers();
    D3D12_GPU_DESCRIPTOR_HANDLE allocViews(std::uint32_t count, std::uint32_t& first);
    D3D12_GPU_DESCRIPTOR_HANDLE allocSamplers(std::uint32_t count, std::uint32_t& first);

    D3D12Device& m_dev;
    Com<ID3D12CommandAllocator> m_allocator;
    Com<ID3D12GraphicsCommandList> m_list;
    Com<ID3D12GraphicsCommandList4> m_list4; ///< ray tracing
    Com<ID3D12GraphicsCommandList5> m_list5; ///< variable-rate shading
    Com<ID3D12GraphicsCommandList6> m_list6; ///< mesh shaders
    std::uint64_t m_lastSubmit = 0;
    bool m_recording = false;
    bool m_inRenderPass = false;
    bool m_rateImage = false; ///< a shading-rate image is bound (render pass with shadingRateImage)
    D3D12_SHADING_RATE m_shadingRate = D3D12_SHADING_RATE_1X1;
    D3D12Pipeline* m_pipeline = nullptr;
    const RootSignature* m_root = nullptr;
    bool m_rootDirty = true, m_viewsDirty = true, m_samplersDirty = true, m_pushDirty = false, m_tableDirty = true;
    bool m_accelDirty = false;
    std::array<std::uint32_t, 32> m_push{};
    std::uint32_t m_pushBytes = 0;
    D3D12TextureTable* m_table = nullptr;
    std::array<Bound, kSlots> m_b{}, m_t{}, m_u{}, m_s{};
    struct VertexBinding {
        D3D12Buffer* buffer = nullptr;
        std::uint64_t offset = 0;
    };
    std::array<VertexBinding, 16> m_vbs{};
    bool m_vbsDirty = false;
    std::vector<std::pair<D3D12Texture*, D3D12Texture*>> m_pendingResolves;
    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_local; ///< decaying resources
    std::vector<D3D12Texture*> m_copyQueueTextures;
    // shader-visible descriptor blocks owned by this list
    std::vector<std::uint32_t> m_viewBlocks, m_samplerBlocks;
    std::size_t m_viewBlock = 0, m_samplerBlock = 0;
    std::uint32_t m_viewUsed = 0, m_samplerUsed = 0;
    // transient upload memory
    struct Chunk {
        std::unique_ptr<D3D12Buffer> buffer;
        std::uint64_t used = 0;
    };
    std::vector<Chunk> m_chunks;
    std::size_t m_chunkIndex = 0;
};

// --- device ------------------------------------------------------------------------------------
class D3D12Device final : public Device {
public:
    explicit D3D12Device(const DeviceCreateInfo& info);
    ~D3D12Device() override;

    // --- Device interface
    std::vector<Capability> capabilities() const override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    std::unique_ptr<Sampler> createSampler(const SamplerDesc& desc) override;
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override;
    std::unique_ptr<Pipeline> createRayTracingPipeline(const RayTracingPipelineDesc& desc) override;
    std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) override;
    std::unique_ptr<AccelerationStructure> createTlas(std::uint32_t maxInstances, bool allowUpdate) override;
    std::unique_ptr<TextureTable> createTextureTable(std::uint32_t capacity) override;
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
    std::uint32_t textureCopyPitchAlignment() const override { return D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; }
    MemoryBudget memoryBudget() const override;
    ValidationCounts validationCounts() const override { return globalValidationCounts(); }
    DeviceFaultReport deviceFaultReport() override;
    std::uint32_t renderTargetSampleCounts(Format format) const override;
    void setDebugName(Buffer* b, const char* name) override;
    void setDebugName(Texture* t, const char* name) override;

    // --- native access
    ID3D12Device* d3dDevice() const { return m_device.get(); }
    ID3D12Device2* device2() const { return m_device2.get(); }
    ID3D12Device5* device5() const { return m_device5.get(); }
    IDXGIAdapter1* dxgiAdapter() const { return m_adapterDxgi.get(); }
    IDXGIFactory4* dxgiFactory() const { return m_factory.get(); }
    ID3D12CommandQueue* queue(QueueType type) const { return m_queues[static_cast<std::size_t>(type)].queue.get(); }
    D3D_SHADER_MODEL shaderModel() const { return m_shaderModel; }
    D3D_FEATURE_LEVEL featureLevel() const { return m_featureLevel; }
    D3D12_RAYTRACING_TIER raytracingTier() const { return m_rtTier; }
    bool tearingSupported() const { return m_tearing; }
    bool gpuUploadHeap() const { return m_gpuUploadHeap; }
    bool copyQueueTimestamps() const { return m_copyTimestamps; }
    std::uint64_t timestampFrequency(QueueType q) const { return m_queues[static_cast<std::size_t>(q)].frequency; }

    // --- internal helpers shared by resources and command lists
    CpuDescriptorHeap& cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE type) { return *m_cpuHeaps[static_cast<std::size_t>(type)]; }
    GpuDescriptorHeap& viewHeap() { return *m_viewHeap; }
    GpuDescriptorHeap& samplerHeap() { return *m_samplerHeap; }
    const RootSignature& rootSignature(const LayoutDesc& layout, bool inputAssembler);
    /// Serializes and creates a root signature (also used by feature tests with native pipelines).
    Com<ID3D12RootSignature> createRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc);
    ID3D12CommandSignature* commandSignature(D3D12_INDIRECT_ARGUMENT_TYPE type, std::uint32_t stride);
    std::span<const std::uint8_t> shaderBytecode(const ShaderRef& ref) const;
    void deferDestroy(std::function<void()> fn);
    void collectGarbage();
    /// Throws for a failed HRESULT; device removal is reported with its reason.
    void check(HRESULT hr, const char* what) const;
    void setName(ID3D12Object* object, const char* name) const;

private:
    struct Queue {
        Com<ID3D12CommandQueue> queue;
        Com<ID3D12Fence> fence;
        HANDLE event = nullptr;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t frequency = 0;
    };
    void createQueues();
    void fillFeatures();
    void installMessageCallback();
    void drainMessages();
    std::uint64_t poll(Queue& q);

    Com<IDXGIFactory4> m_factory;
    Com<IDXGIAdapter1> m_adapterDxgi;
    Com<ID3D12Device> m_device;
    Com<ID3D12Device2> m_device2;
    Com<ID3D12Device5> m_device5;
    Com<ID3D12InfoQueue> m_infoQueue;
    DWORD m_callbackCookie = 0;
    std::array<Queue, 3> m_queues;
    std::array<std::unique_ptr<CpuDescriptorHeap>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_cpuHeaps;
    std::unique_ptr<GpuDescriptorHeap> m_viewHeap, m_samplerHeap;
    std::map<std::string, std::unique_ptr<RootSignature>> m_rootSignatures;
    std::mutex m_rootMutex;
    std::map<std::pair<int, std::uint32_t>, Com<ID3D12CommandSignature>> m_commandSignatures;
    std::unique_ptr<CommandList> m_uploadList;
    D3D_SHADER_MODEL m_shaderModel = D3D_SHADER_MODEL_6_0;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D12_RAYTRACING_TIER m_rtTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_RESOURCE_BINDING_TIER m_bindingTier = D3D12_RESOURCE_BINDING_TIER_1;
    bool m_tearing = false;
    bool m_gpuUploadHeap = false;
    bool m_copyTimestamps = false;
    bool m_validation = false;

    struct Garbage {
        std::array<std::uint64_t, 3> values;
        std::function<void()> fn;
    };
    std::deque<Garbage> m_garbage;
    std::mutex m_garbageMutex;
};

std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

} // namespace gt::rhi::d3d12
