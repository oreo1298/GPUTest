#pragma once
// Vulkan backend. Public so feature tests can reach native handles:
//   auto* vkdev = dynamic_cast<gt::rhi::vk::VulkanDevice*>(&device);

#include "vk_common.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace gt::rhi::vk {

class VulkanDevice;

constexpr std::uint32_t kShiftB = 0, kShiftT = 16, kShiftU = 32, kShiftS = 48;
std::uint32_t bindingIndex(BindingType type, std::uint32_t slot);
VkDescriptorType descriptorType(BindingType type);

struct ImageStateInfo {
    VkImageLayout layout;
    VkAccessFlags access;
    VkPipelineStageFlags stages;
};
ImageStateInfo imageState(ResourceState s, bool depth);
ImageStateInfo bufferState(ResourceState s);

// --- resources ------------------------------------------------------------------------
class VulkanBuffer final : public Buffer {
public:
    VulkanBuffer(VulkanDevice& dev, const BufferDesc& desc);
    ~VulkanBuffer() override;
    void* mapped() override { return m_mapped; }
    bool isDeviceLocalHostVisible() const override { return m_deviceLocalHostVisible; }
    VkBuffer handle() const { return m_buffer; }
    VkDeviceAddress address() const { return m_address; }
    VmaAllocation allocation() const { return m_alloc; }
    /// Makes GPU writes visible to the CPU for non-coherent readback memory.
    void invalidate();
    /// Makes CPU writes visible to the GPU for non-coherent upload memory.
    void flush();
    ResourceState state = ResourceState::Undefined;

private:
    VulkanDevice& m_dev;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_alloc = nullptr;
    void* m_mapped = nullptr;
    VkDeviceAddress m_address = 0;
    bool m_deviceLocalHostVisible = false;
};

class VulkanTexture final : public Texture {
public:
    VulkanTexture(VulkanDevice& dev, const TextureDesc& desc);
    /// Wraps an image owned elsewhere (swapchain).
    VulkanTexture(VulkanDevice& dev, const TextureDesc& desc, VkImage image);
    ~VulkanTexture() override;
    VkImage handle() const { return m_image; }
    VkImageAspectFlags aspect() const { return m_aspect; }
    /// Cached image view for a subresource range (count 0 = all remaining).
    VkImageView view(Subresource sub, bool forAttachment = false);
    void addFramebuffer(VkFramebuffer fb) { m_framebuffers.push_back(fb); }
    ResourceState state = ResourceState::Undefined;
    VkImageLayout layout() const;

private:
    VulkanDevice& m_dev;
    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_alloc = nullptr;
    bool m_owned = true;
    VkImageAspectFlags m_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    struct ViewKey {
        Subresource sub;
        bool attachment;
        bool operator<(const ViewKey& o) const {
            return std::tie(sub.baseMip, sub.mipCount, sub.baseLayer, sub.layerCount, attachment) <
                   std::tie(o.sub.baseMip, o.sub.mipCount, o.sub.baseLayer, o.sub.layerCount, o.attachment);
        }
    };
    std::map<ViewKey, VkImageView> m_views;
    std::vector<VkFramebuffer> m_framebuffers;
};

class VulkanSampler final : public Sampler {
public:
    VulkanSampler(VulkanDevice& dev, const SamplerDesc& desc);
    ~VulkanSampler() override;
    VkSampler handle() const { return m_sampler; }

private:
    VulkanDevice& m_dev;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

class VulkanPipeline final : public Pipeline {
public:
    VulkanPipeline(VulkanDevice& dev, const GraphicsPipelineDesc& desc);
    VulkanPipeline(VulkanDevice& dev, const ComputePipelineDesc& desc);
    VulkanPipeline(VulkanDevice& dev, const RayTracingPipelineDesc& desc);
    ~VulkanPipeline() override;
    /// Shader-binding-table regions (ray-tracing pipelines only).
    const VkStridedDeviceAddressRegionKHR& raygenRegion() const { return m_sbtRegions[0]; }
    const VkStridedDeviceAddressRegionKHR& missRegion() const { return m_sbtRegions[1]; }
    const VkStridedDeviceAddressRegionKHR& hitRegion() const { return m_sbtRegions[2]; }
    const VkStridedDeviceAddressRegionKHR& callableRegion() const { return m_sbtRegions[3]; }
    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layoutHandle() const { return m_layout; }
    VkDescriptorSetLayout setLayout() const { return m_setLayout; }
    VkPipelineBindPoint bindPoint() const { return m_bindPoint; }
    const LayoutDesc& layoutDesc() const { return m_layoutDesc; }
    const std::string& name() const { return m_name; }

private:
    void createLayout(const LayoutDesc& layout);
    VulkanDevice& m_dev;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    LayoutDesc m_layoutDesc;
    std::string m_name;
    std::unique_ptr<VulkanBuffer> m_sbt;
    std::array<VkStridedDeviceAddressRegionKHR, 4> m_sbtRegions{};
};

class VulkanAccel final : public AccelerationStructure {
public:
    VulkanAccel(VulkanDevice& dev, bool topLevel, const VkAccelerationStructureBuildSizesInfoKHR& sizes,
                std::uint32_t maxInstances, const char* name);
    ~VulkanAccel() override;
    VkAccelerationStructureKHR handle() const { return m_handle; }
    VkDeviceAddress address() const { return m_address; }
    VkDeviceAddress scratchAddress() const { return m_scratchAddress; }
    VulkanBuffer* instanceBuffer() const { return m_instances.get(); }
    std::uint32_t maxInstances() const { return m_maxInstances; }

private:
    VulkanDevice& m_dev;
    VkAccelerationStructureKHR m_handle = VK_NULL_HANDLE;
    VkDeviceAddress m_address = 0;
    VkDeviceAddress m_scratchAddress = 0;
    std::unique_ptr<VulkanBuffer> m_storage, m_scratchBuffer, m_instances;
    std::uint32_t m_maxInstances = 0;
};

class VulkanQueryPool final : public QueryPool {
public:
    VulkanQueryPool(VulkanDevice& dev, QueryType type, std::uint32_t count);
    ~VulkanQueryPool() override;
    bool read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) override;
    VkQueryPool handle() const { return m_pool; }

private:
    VulkanDevice& m_dev;
    VkQueryPool m_pool = VK_NULL_HANDLE;
};

// --- queues ----------------------------------------------------------------------------
struct QueueState {
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::uint32_t index = 0;
    VkQueueFlags flags = 0;
    std::uint32_t timestampValidBits = 0;
    VkSemaphore timeline = VK_NULL_HANDLE; ///< when timeline semaphores are supported
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::deque<std::pair<std::uint64_t, VkFence>> inflight;
};

// --- swapchain --------------------------------------------------------------------------
class VulkanSwapchain final : public Swapchain {
public:
    VulkanSwapchain(VulkanDevice& dev, const SwapchainDesc& desc);
    ~VulkanSwapchain() override;
    Texture* acquire() override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Format format() const override { return m_format; }
    bool isHdr() const override { return m_hdr; }
    std::string presentModeName() const override;

    // used by VulkanDevice::submit/present
    VkSemaphore acquireSemaphore() const { return m_acquireSems[m_acquireSlot]; }
    VkSemaphore presentSemaphore() const { return m_presentSems[m_imageIndex]; }
    void markAcquireConsumed(std::uint64_t graphicsValue) { m_acquireUse[m_acquireSlot] = graphicsValue; }
    VkSwapchainKHR handle() const { return m_swapchain; }
    std::uint32_t imageIndex() const { return m_imageIndex; }
    void markOutOfDate() { m_outOfDate = true; }

private:
    void create(std::uint32_t width, std::uint32_t height);
    void destroySwapchain();
    VulkanDevice& m_dev;
    SwapchainDesc m_desc;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    Format m_format = Format::BGRA8_UNORM;
    bool m_hdr = false;
    std::vector<std::unique_ptr<VulkanTexture>> m_images;
    std::vector<VkSemaphore> m_acquireSems;
    std::vector<std::uint64_t> m_acquireUse;
    std::vector<VkSemaphore> m_presentSems;
    std::uint32_t m_acquireSlot = 0;
    std::uint32_t m_imageIndex = 0;
    bool m_outOfDate = false;
};

// --- command list -------------------------------------------------------------------------
class VulkanCommandList final : public CommandList {
public:
    VulkanCommandList(VulkanDevice& dev, QueueType queue);
    ~VulkanCommandList() override;
    VkCommandBuffer handle() const { return m_cmd; }
    void setLastSubmit(std::uint64_t value) { m_lastSubmit = value; }

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
    void resolveQueries(QueryPool*, std::uint32_t, std::uint32_t) override {}
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
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = 0;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkSampler sampler = VK_NULL_HANDLE;
        VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    };
    static constexpr std::uint32_t kSlots = 16;
    Bound& slot(char cls, std::uint32_t index);
    void flushBindings();
    void requireRecording(const char* what) const;

    VulkanDevice& m_dev;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    std::uint64_t m_lastSubmit = 0;
    bool m_recording = false;
    bool m_inRenderPass = false;
    VulkanPipeline* m_pipeline = nullptr;
    bool m_bindingsDirty = true;
    bool m_accelDirty = false; ///< a build was recorded since the last acceleration-structure barrier
    std::array<Bound, kSlots> m_b{}, m_t{}, m_u{}, m_s{};
    std::vector<std::pair<VulkanTexture*, VulkanTexture*>> m_pendingResolves;
    // transient memory
    struct Chunk {
        std::unique_ptr<VulkanBuffer> buffer;
        std::uint64_t used = 0;
    };
    std::vector<Chunk> m_chunks;
    std::size_t m_chunkIndex = 0;
    // descriptor sets (fallback when push descriptors are unavailable)
    std::vector<VkDescriptorPool> m_descPools;
    std::size_t m_descPoolIndex = 0;
};

// --- device ---------------------------------------------------------------------------------
/// Feature structs queried for (and enabled on) the device.
struct FeatureSet {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStruct{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtMaintenance1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR rtPositionFetch{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT serExt{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV serNv{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopMatrix{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrix2FeaturesNV coopMatrix2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
    VkPhysicalDeviceCooperativeVectorFeaturesNV coopVector{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV};
    VkPhysicalDeviceShaderBfloat16FeaturesKHR bfloat16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT float8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageAtomicInt64{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDeviceShaderEnqueueFeaturesAMDX shaderEnqueue{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ENQUEUE_FEATURES_AMDX};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriority{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDeviceShaderClockFeaturesKHR shaderClock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    // Pre-1.2 fallbacks (only chained when the device API version lacks the core struct).
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphore{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures hostQueryReset{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features float16Int8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceShaderAtomicInt64Features atomicInt64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    VkPhysicalDevice8BitStorageFeatures storage8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures integerDotProduct{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroupSizeControl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDevicePushDescriptorProperties pushDescriptorProps{};
};

class VulkanDevice final : public Device {
public:
    VulkanDevice(const DeviceCreateInfo& info);
    ~VulkanDevice() override;

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
    ValidationCounts validationCounts() const override { return globalValidationCounts(); }
    DeviceFaultReport deviceFaultReport() override;
    std::uint32_t renderTargetSampleCounts(Format format) const override;
    void setDebugName(Buffer* b, const char* name) override;
    void setDebugName(Texture* t, const char* name) override;

    // --- native access
    VkInstance vkInstance() const { return m_instance; }
    VkPhysicalDevice vkPhysicalDevice() const { return m_physical; }
    VkDevice vkDevice() const { return m_device; }
    VmaAllocator allocator() const { return m_allocator; }
    std::uint32_t apiVersion() const { return m_apiVersion; }
    const VkPhysicalDeviceProperties& properties() const { return m_props; }
    const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return m_memProps; }
    const FeatureSet& enabledFeatures() const { return m_feat; }
    bool extensionEnabled(std::string_view name) const;
    bool extensionSupported(std::string_view name) const;
    const std::vector<std::string>& enabledExtensions() const { return m_enabledExtensions; }
    QueueState& queue(QueueType type) { return *m_queueFor[static_cast<int>(type)]; }
    bool pushDescriptors() const { return m_pushDescriptors; }
    bool debugUtils() const { return m_debugUtils; }
    bool timelineSemaphores() const { return m_timeline; }

    // --- internal helpers shared by resources / command lists
    void deferDestroy(std::function<void()> fn);
    void collectGarbage();
    VkRenderPass renderPass(const std::vector<VkFormat>& colors, VkFormat depth, VkSampleCountFlagBits samples,
                            const std::vector<LoadOp>& colorLoads, LoadOp depthLoad, StoreOp depthStore,
                            bool depthReadOnly);
    VkRenderPass compatibleRenderPass(const std::vector<Format>& colors, Format depth, std::uint32_t samples);
    VkFramebuffer framebuffer(VkRenderPass compat, const std::vector<VulkanTexture*>& textures,
                              const std::vector<VkImageView>& views, std::uint32_t width, std::uint32_t height);
    void forgetFramebuffer(VkFramebuffer fb);
    VkShaderModule createShaderModule(const ShaderRef& ref);
    std::uint64_t uniformAlignment() const { return m_props.limits.minUniformBufferOffsetAlignment; }
    std::uint64_t storageAlignment() const { return m_props.limits.minStorageBufferOffsetAlignment; }
    VkSurfaceKHR createSurface(void* window);
    void setObjectName(VkObjectType type, std::uint64_t handle, const char* name);
    bool hostVisibleDeviceLocalAvailable() const { return m_rebar; }
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rayTracingProperties();
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR& accelProperties();
    bool tlasUpdatable(const AccelerationStructure* tlas) const {
        auto it = m_tlasUpdatable.find(tlas);
        return it != m_tlasUpdatable.end() && it->second;
    }

private:
    void pickQueues();
    void createLogicalDevice();
    void fillFeatures();
    void pollQueue(QueueState& q);
    std::unique_ptr<CommandList> m_uploadList;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    std::uint32_t m_apiVersion = VK_API_VERSION_1_1;
    VkPhysicalDeviceProperties m_props{};
    VkPhysicalDeviceMemoryProperties m_memProps{};
    FeatureSet m_feat;
    std::vector<VkExtensionProperties> m_supportedExtensions;
    std::vector<std::string> m_enabledExtensions;
    std::vector<std::unique_ptr<QueueState>> m_queues;
    std::array<QueueState*, 3> m_queueFor{};
    bool m_pushDescriptors = false;
    bool m_debugUtils = false;
    bool m_timeline = false;
    bool m_memoryBudgetExt = false;
    bool m_deviceFault = false; ///< VK_EXT_device_fault enabled
    bool m_rebar = false;
    bool m_rtPropsQueried = false;
    std::map<const AccelerationStructure*, bool> m_tlasUpdatable;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    std::uint32_t m_validationHeaderLimit = 0;
    std::vector<VkFence> m_freeFences;

    struct Garbage {
        std::array<std::uint64_t, 3> values;
        std::function<void()> fn;
    };
    std::deque<Garbage> m_garbage;
    std::map<std::string, VkRenderPass> m_renderPasses;
    struct FbEntry {
        VkFramebuffer fb;
        std::vector<VkImageView> views;
    };
    std::map<std::string, FbEntry> m_framebuffers;
};

std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

} // namespace gt::rhi::vk
