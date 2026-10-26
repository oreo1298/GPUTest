#include "vk_device.h"

#include "gt/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gt::rhi::vk {

AdapterInfo describePhysicalDevice(VkPhysicalDevice pd, std::uint32_t index);

namespace {

constexpr std::uint32_t kNotPromoted = 0;

struct WantedExtension {
    const char* name;
    std::uint32_t promotedTo; ///< core version that makes the extension unnecessary (0 = never)
    std::vector<const char*> deps;
};

/// Vulkan header version (minor*1000 + patch) that introduced recent extensions; only consulted
/// when an older validation layer is active, so it need not be exhaustive.
std::uint32_t introducedIn(const char* name) {
    struct E {
        const char* name;
        std::uint32_t version;
    };
    static const E kTable[] = {
        {VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, 3296},
        {VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, 4333},
        {VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME, 4304},
        {VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME, 4307},
        {VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME, 4313},
        {VK_EXT_SHADER_FLOAT8_EXTENSION_NAME, 4317},
        {VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, 3302},
        {VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, 3277},
        {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, 3278},
    };
    for (const auto& e : kTable)
        if (std::strcmp(e.name, name) == 0) return e.version;
    return 0;
}

// Extensions enabled whenever supported (and their dependencies are satisfied).
const std::vector<WantedExtension>& wantedExtensions() {
    static const std::vector<WantedExtension> kList = {
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_PCI_BUS_INFO_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_16BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_1, {}},
        {VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_SPIRV_1_4_EXTENSION_NAME, VK_API_VERSION_1_2, {VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME}},
        {VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, VK_API_VERSION_1_2, {}},
        {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_API_VERSION_1_3, {}},
        {VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_API_VERSION_1_4, {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME}},
        {VK_KHR_MAINTENANCE_6_EXTENSION_NAME, VK_API_VERSION_1_4, {}},
        {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
          VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME}},
        {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME}},
        {VK_KHR_RAY_QUERY_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME}},
        {VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME, kNotPromoted, {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME}},
        {VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, kNotPromoted, {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME}},
        {VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME}},
        {VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME}},
        {VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME}},
        {VK_EXT_MESH_SHADER_EXTENSION_NAME, kNotPromoted, {VK_KHR_SPIRV_1_4_EXTENSION_NAME}},
        {VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, kNotPromoted, {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME}},
        {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, kNotPromoted, {}},
        {VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME, kNotPromoted, {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME}},
        {VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_SHADER_FLOAT8_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
          VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME}},
        {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, kNotPromoted, {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME}},
        {VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_SHADER_CLOCK_EXTENSION_NAME, kNotPromoted, {}},
        {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_HDR_METADATA_EXTENSION_NAME, kNotPromoted, {VK_KHR_SWAPCHAIN_EXTENSION_NAME}},
        {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, kNotPromoted, {}},
        {VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME}},
        {VK_AMDX_SHADER_ENQUEUE_EXTENSION_NAME,
         kNotPromoted,
         {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME,
          VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
          VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME}},
        {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, kNotPromoted, {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME}},
        {VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME}},
        {VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME, kNotPromoted, {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME}},
    };
    return kList;
}

template <typename T>
void chain(void**& tail, T& s) {
    s.pNext = nullptr;
    *tail = &s;
    tail = &s.pNext;
}

VkFence getFence(VkDevice dev, std::vector<VkFence>& freeList) {
    if (!freeList.empty()) {
        VkFence f = freeList.back();
        freeList.pop_back();
        vkResetFences(dev, 1, &f);
        return f;
    }
    VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &ci, nullptr, &f), "vkCreateFence");
    return f;
}

} // namespace

std::uint32_t bindingIndex(BindingType type, std::uint32_t slot) {
    switch (type) {
    case BindingType::UniformBuffer: return kShiftB + slot;
    case BindingType::SampledTexture:
    case BindingType::StorageBufferRO:
    case BindingType::AccelerationStructure: return kShiftT + slot;
    case BindingType::StorageBufferRW:
    case BindingType::StorageTexture: return kShiftU + slot;
    case BindingType::Sampler: return kShiftS + slot;
    }
    return slot;
}

VkDescriptorType descriptorType(BindingType type) {
    switch (type) {
    case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::StorageBufferRO:
    case BindingType::StorageBufferRW: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case BindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

// --- device ------------------------------------------------------------------------------------
VulkanDevice::VulkanDevice(const DeviceCreateInfo& info) {
    m_api = Api::Vulkan;
    Instance& inst = instance(info.validation);
    m_instance = inst.instance;
    m_validationHeaderLimit = inst.validationHeaderLimit;
    m_debugUtils = inst.debugUtils;

    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(m_instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> pds(count);
    check(vkEnumeratePhysicalDevices(m_instance, &count, pds.data()), "vkEnumeratePhysicalDevices");
    if (info.adapterIndex >= count)
        throw Error(std::format("Vulkan adapter index {} out of range ({} adapters)", info.adapterIndex, count));
    m_physical = pds[info.adapterIndex];
    m_adapter = describePhysicalDevice(m_physical, info.adapterIndex);

    vkGetPhysicalDeviceProperties(m_physical, &m_props);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    m_apiVersion = std::min(m_props.apiVersion, inst.apiVersion);
    if (m_apiVersion < VK_API_VERSION_1_1)
        throw Error(std::format("{} only supports Vulkan {} (1.1 required)", m_adapter.name,
                                versionString(m_props.apiVersion)));

    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
    m_supportedExtensions.resize(count);
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, m_supportedExtensions.data());

    pickQueues();
    createLogicalDevice();
    fillFeatures();
    log::info("Vulkan device: {} ({}, driver {}, API {}), {} extensions enabled", m_adapter.name,
              m_adapter.driverName, m_adapter.driverVersion, versionString(m_apiVersion), m_enabledExtensions.size());
}

VulkanDevice::~VulkanDevice() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    m_uploadList.reset();
    collectGarbage();
    while (!m_garbage.empty()) {
        m_garbage.front().fn();
        m_garbage.pop_front();
    }
    for (auto& [key, fb] : m_framebuffers) vkDestroyFramebuffer(m_device, fb.fb, nullptr);
    for (auto& [key, rp] : m_renderPasses) vkDestroyRenderPass(m_device, rp, nullptr);
    for (auto& q : m_queues) {
        for (auto& [v, f] : q->inflight) vkDestroyFence(m_device, f, nullptr);
        if (q->timeline) vkDestroySemaphore(m_device, q->timeline, nullptr);
    }
    for (VkFence f : m_freeFences) vkDestroyFence(m_device, f, nullptr);
    if (m_tableLayout) vkDestroyDescriptorSetLayout(m_device, m_tableLayout, nullptr);
    if (m_allocator) vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
}

bool VulkanDevice::extensionSupported(std::string_view name) const {
    return std::any_of(m_supportedExtensions.begin(), m_supportedExtensions.end(),
                       [&](const VkExtensionProperties& e) { return name == e.extensionName; });
}

std::vector<CooperativeMatrixConfig> VulkanDevice::cooperativeMatrixConfigs() {
    std::vector<CooperativeMatrixConfig> out;
    if (!m_features.cooperativeMatrix) return out;
    std::uint32_t n = 0;
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(m_physical, &n, nullptr);
    std::vector<VkCooperativeMatrixPropertiesKHR> props(n, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(m_physical, &n, props.data());
    const bool bf16 = m_feat.bfloat16.shaderBFloat16CooperativeMatrix != VK_FALSE;
    const bool fp8 = m_feat.float8.shaderFloat8CooperativeMatrix != VK_FALSE;
    auto name = [&](VkComponentTypeKHR t) -> std::string {
        switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "f64";
        case VK_COMPONENT_TYPE_SINT8_KHR: return "s8";
        case VK_COMPONENT_TYPE_SINT16_KHR: return "s16";
        case VK_COMPONENT_TYPE_SINT32_KHR: return "s32";
        case VK_COMPONENT_TYPE_SINT64_KHR: return "s64";
        case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
        case VK_COMPONENT_TYPE_UINT16_KHR: return "u16";
        case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
        case VK_COMPONENT_TYPE_UINT64_KHR: return "u64";
        case VK_COMPONENT_TYPE_BFLOAT16_KHR: return bf16 ? "bf16" : "";
        case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT: return fp8 ? "e4m3" : "";
        case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return fp8 ? "e5m2" : "";
        default: return ""; // types this build does not know or did not enable
        }
    };
    for (const auto& p : props) {
        if (p.scope != VK_SCOPE_SUBGROUP_KHR) continue;
        CooperativeMatrixConfig c{p.MSize, p.NSize, p.KSize, name(p.AType), name(p.BType), name(p.CType), name(p.ResultType),
                                  p.saturatingAccumulation != VK_FALSE};
        if (c.a.empty() || c.b.empty() || c.c.empty() || c.result.empty()) continue;
        out.push_back(std::move(c));
    }
    return out;
}

VkShaderStageFlags VulkanDevice::supportedShaderStages() const {
    VkShaderStageFlags f = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    const auto& e = enabledFeatures();
    if (e.mesh.taskShader) f |= VK_SHADER_STAGE_TASK_BIT_EXT;
    if (e.mesh.meshShader) f |= VK_SHADER_STAGE_MESH_BIT_EXT;
    if (e.rtPipeline.rayTracingPipeline)
        f |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
             VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    return f;
}

bool VulkanDevice::extensionEnabled(std::string_view name) const {
    return std::find(m_enabledExtensions.begin(), m_enabledExtensions.end(), name) != m_enabledExtensions.end();
}

void VulkanDevice::pickQueues() {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, fams.data());

    int gfx = -1, compute = -1, copy = -1;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto f = fams[i].queueFlags;
        if (gfx < 0 && (f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT)) gfx = static_cast<int>(i);
        if (compute < 0 && (f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) compute = static_cast<int>(i);
        if (copy < 0 && (f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            copy = static_cast<int>(i);
    }
    if (gfx < 0) throw Error("no graphics+compute queue family");

    auto make = [&](int family, std::uint32_t index) {
        auto q = std::make_unique<QueueState>();
        q->family = static_cast<std::uint32_t>(family);
        q->index = index;
        q->flags = fams[static_cast<std::size_t>(family)].queueFlags;
        q->timestampValidBits = fams[static_cast<std::size_t>(family)].timestampValidBits;
        m_queues.push_back(std::move(q));
        return m_queues.back().get();
    };
    QueueState* g = make(gfx, 0);
    m_queueFor[static_cast<int>(QueueType::Graphics)] = g;
    QueueState* c = nullptr;
    if (compute >= 0) {
        c = make(compute, 0);
        m_features.asyncComputeQueue = true;
    } else if (fams[static_cast<std::size_t>(gfx)].queueCount > 1) {
        c = make(gfx, 1);
        m_features.asyncComputeQueue = true;
    } else {
        c = g;
    }
    m_queueFor[static_cast<int>(QueueType::Compute)] = c;
    if (copy >= 0) {
        m_queueFor[static_cast<int>(QueueType::Copy)] = make(copy, 0);
        m_features.copyQueue = true;
    } else {
        m_queueFor[static_cast<int>(QueueType::Copy)] = c;
    }
    m_features.timestampsOnComputeQueue = c->timestampValidBits > 0;
}

void VulkanDevice::createLogicalDevice() {
    // --- extensions
    auto supported = [&](const char* n) { return extensionSupported(n); };
    std::vector<const char*> enabled;
    auto isEnabled = [&](const char* n) {
        return std::any_of(enabled.begin(), enabled.end(), [&](const char* e) { return std::strcmp(e, n) == 0; });
    };
    const auto& wanted = wantedExtensions();
    auto findWanted = [&](const char* n) -> const WantedExtension* {
        for (const auto& w : wanted)
            if (std::strcmp(w.name, n) == 0) return &w;
        return nullptr;
    };
    // A dependency is satisfied when promoted to core at our API version, or enabled.
    std::function<bool(const char*)> satisfied = [&](const char* n) -> bool {
        const WantedExtension* w = findWanted(n);
        if (w && w->promotedTo != kNotPromoted && m_apiVersion >= w->promotedTo) return true;
        return isEnabled(n);
    };
    // Multiple passes so dependencies listed later still resolve.
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& w : wanted) {
            if (isEnabled(w.name) || !supported(w.name)) continue;
            if (m_validationHeaderLimit && introducedIn(w.name) > m_validationHeaderLimit) continue;
            if (w.promotedTo != kNotPromoted && m_apiVersion >= w.promotedTo &&
                std::strcmp(w.name, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) != 0)
                continue;
            if (std::all_of(w.deps.begin(), w.deps.end(), satisfied)) enabled.push_back(w.name);
        }
    }
    for (const char* e : enabled) m_enabledExtensions.emplace_back(e);
    auto has = [&](const char* n) { return isEnabled(n); };

    // --- features: query the full chain, then sanitize
    FeatureSet& f = m_feat;
    void** tail = &f.core.pNext;
    const bool v12 = m_apiVersion >= VK_API_VERSION_1_2;
    const bool v13 = m_apiVersion >= VK_API_VERSION_1_3;
    const bool v14 = m_apiVersion >= VK_API_VERSION_1_4;
    if (v12) {
        chain(tail, f.v11);
        chain(tail, f.v12);
    } else {
        if (has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) chain(tail, f.timelineSemaphore);
        if (has(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) chain(tail, f.hostQueryReset);
        if (has(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) chain(tail, f.float16Int8);
        if (has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) chain(tail, f.bufferDeviceAddress);
        if (has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) chain(tail, f.descriptorIndexing);
        if (has(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME)) chain(tail, f.atomicInt64);
        if (has(VK_KHR_8BIT_STORAGE_EXTENSION_NAME)) chain(tail, f.storage8);
    }
    if (v13) {
        chain(tail, f.v13);
    } else {
        if (has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) chain(tail, f.sync2);
        if (has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) chain(tail, f.dynamicRendering);
        if (has(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME)) chain(tail, f.integerDotProduct);
        if (has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) chain(tail, f.subgroupSizeControl);
    }
    if (v14) chain(tail, f.v14);
    if (has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) chain(tail, f.accelStruct);
    if (has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) chain(tail, f.rtPipeline);
    if (has(VK_KHR_RAY_QUERY_EXTENSION_NAME)) chain(tail, f.rayQuery);
    if (has(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME)) chain(tail, f.rtMaintenance1);
    if (has(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME)) chain(tail, f.rtPositionFetch);
    if (has(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME)) chain(tail, f.serExt);
    else if (has(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME)) chain(tail, f.serNv);
    if (has(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME)) chain(tail, f.omm);
    if (has(VK_EXT_MESH_SHADER_EXTENSION_NAME)) chain(tail, f.mesh);
    if (has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) chain(tail, f.fsr);
    if (has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) chain(tail, f.coopMatrix);
    if (has(VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME)) chain(tail, f.coopMatrix2);
    if (has(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME)) chain(tail, f.coopVector);
    if (has(VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME)) chain(tail, f.bfloat16);
    if (has(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) chain(tail, f.float8);
    if (has(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME)) chain(tail, f.descriptorBuffer);
    if (has(VK_EXT_SHADER_OBJECT_EXTENSION_NAME)) chain(tail, f.shaderObject);
    if (has(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) chain(tail, f.imageAtomicInt64);
    if (has(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME)) chain(tail, f.atomicFloat);
    if (has(VK_AMDX_SHADER_ENQUEUE_EXTENSION_NAME)) chain(tail, f.shaderEnqueue);
    if (has(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME)) chain(tail, f.memoryPriority);
    if (has(VK_KHR_SHADER_CLOCK_EXTENSION_NAME)) chain(tail, f.shaderClock);
    if (has(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME)) chain(tail, f.dgc);
    if (has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) chain(tail, f.fault);
    vkGetPhysicalDeviceFeatures2(m_physical, &f.core);

    // Never enable features that change performance or memory behaviour of the measured workloads.
    f.core.features.robustBufferAccess = VK_FALSE;
    f.v11.protectedMemory = VK_FALSE;
    f.v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    f.v12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f.bufferDeviceAddress.bufferDeviceAddressCaptureReplay = VK_FALSE;
    f.bufferDeviceAddress.bufferDeviceAddressMultiDevice = VK_FALSE;
    f.v13.robustImageAccess = VK_FALSE;
    f.accelStruct.accelerationStructureCaptureReplay = VK_FALSE;
    f.rtPipeline.rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE;
    f.rtPipeline.rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
    f.descriptorBuffer.descriptorBufferCaptureReplay = VK_FALSE;
    f.omm.micromapCaptureReplay = VK_FALSE;
    f.fault.deviceFaultVendorBinary = VK_FALSE; // descriptions and codes are enough; no vendor crash dumps
    if (!has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) f.mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    if (!f.v11.multiview) f.mesh.multiviewMeshShader = VK_FALSE;

    // --- queues
    std::vector<VkDeviceQueueCreateInfo> qcis;
    std::vector<std::vector<float>> priorities;
    for (const auto& q : m_queues) {
        auto it = std::find_if(qcis.begin(), qcis.end(),
                               [&](const VkDeviceQueueCreateInfo& c) { return c.queueFamilyIndex == q->family; });
        if (it == qcis.end()) {
            VkDeviceQueueCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            ci.queueFamilyIndex = q->family;
            ci.queueCount = q->index + 1;
            qcis.push_back(ci);
        } else {
            it->queueCount = std::max(it->queueCount, q->index + 1);
        }
    }
    priorities.resize(qcis.size());
    for (std::size_t i = 0; i < qcis.size(); ++i) {
        priorities[i].assign(qcis[i].queueCount, 1.0f);
        qcis[i].pQueuePriorities = priorities[i].data();
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f.core;
    dci.queueCreateInfoCount = static_cast<std::uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    check(vkCreateDevice(m_physical, &dci, nullptr, &m_device), "vkCreateDevice");
    volkLoadDevice(m_device);

    for (auto& q : m_queues) vkGetDeviceQueue(m_device, q->family, q->index, &q->queue);

    m_timeline = v12 ? f.v12.timelineSemaphore : f.timelineSemaphore.timelineSemaphore;
    if (m_timeline) {
        for (auto& q : m_queues) {
            VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            sci.pNext = &tci;
            check(vkCreateSemaphore(m_device, &sci, nullptr, &q->timeline), "vkCreateSemaphore(timeline)");
        }
    }
    m_pushDescriptors = has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    m_memoryBudgetExt = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    m_deviceFault = has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && f.fault.deviceFault && vkGetDeviceFaultInfoEXT;

    // --- VMA
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = m_physical;
    aci.device = m_device;
    aci.instance = m_instance;
    aci.vulkanApiVersion = std::min<std::uint32_t>(m_apiVersion, VK_API_VERSION_1_3);
    aci.pVulkanFunctions = &fns;
    const bool bda = v12 ? f.v12.bufferDeviceAddress : f.bufferDeviceAddress.bufferDeviceAddress;
    if (bda) aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (m_memoryBudgetExt) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    if (f.memoryPriority.memoryPriority) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    if (v13 || has(VK_KHR_MAINTENANCE_4_EXTENSION_NAME)) aci.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    check(vmaCreateAllocator(&aci, &m_allocator), "vmaCreateAllocator");

    for (std::uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        const auto flags = m_memProps.memoryTypes[i].propertyFlags;
        const auto heap = m_memProps.memoryHeaps[m_memProps.memoryTypes[i].heapIndex];
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            heap.size > 256ull * 1024 * 1024)
            m_rebar = true;
    }
}

void VulkanDevice::fillFeatures() {
    const auto& f = m_feat;
    const bool v12 = m_apiVersion >= VK_API_VERSION_1_2;
    const bool v13 = m_apiVersion >= VK_API_VERSION_1_3;
    DeviceFeatures& d = m_features;
    d.timestamps = m_props.limits.timestampComputeAndGraphics || queue(QueueType::Graphics).timestampValidBits > 0;
    d.timestampPeriodNs = m_props.limits.timestampPeriod;
    d.geometryShader = f.core.features.geometryShader;
    d.tessellation = f.core.features.tessellationShader;
    d.wireframe = f.core.features.fillModeNonSolid;
    d.anisotropicFiltering = f.core.features.samplerAnisotropy;
    d.textureCompressionBC = f.core.features.textureCompressionBC;
    d.shaderFloat64 = f.core.features.shaderFloat64;
    d.shaderInt64 = f.core.features.shaderInt64;
    d.shaderInt16 = f.core.features.shaderInt16;
    d.multiDrawIndirect = f.core.features.multiDrawIndirect;
    d.pipelineStatistics = f.core.features.pipelineStatisticsQuery;
    d.shaderFloat16 = v12 ? f.v12.shaderFloat16 : f.float16Int8.shaderFloat16;
    d.shaderInt8 = v12 ? f.v12.shaderInt8 : f.float16Int8.shaderInt8;
    d.storage16Bit = f.v11.storageBuffer16BitAccess;
    d.storage8Bit = v12 ? f.v12.storageBuffer8BitAccess : f.storage8.storageBuffer8BitAccess;
    d.integerDotProduct = v13 ? f.v13.shaderIntegerDotProduct : f.integerDotProduct.shaderIntegerDotProduct;
    d.atomicInt64 = v12 ? f.v12.shaderBufferInt64Atomics : f.atomicInt64.shaderBufferInt64Atomics;
    d.bufferDeviceAddress = v12 ? f.v12.bufferDeviceAddress : f.bufferDeviceAddress.bufferDeviceAddress;
    d.descriptorIndexing = v12 ? f.v12.descriptorIndexing : f.descriptorIndexing.runtimeDescriptorArray;
    d.drawIndirectCount = v12 ? f.v12.drawIndirectCount : extensionEnabled(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    d.rayQuery = f.rayQuery.rayQuery;
    d.rayTracingPipeline = f.rtPipeline.rayTracingPipeline;
    d.shaderExecutionReordering = d.rayTracingPipeline &&
                                  extensionEnabled(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) &&
                                  f.serExt.rayTracingInvocationReorder;
    d.shaderExecutionReorderingNv = d.rayTracingPipeline && !d.shaderExecutionReordering &&
                                    extensionEnabled(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) &&
                                    f.serNv.rayTracingInvocationReorder;
    d.meshShader = f.mesh.meshShader;
    d.taskShader = f.mesh.meshShader && f.mesh.taskShader;
    d.variableRateShading = f.fsr.pipelineFragmentShadingRate;
    if (d.variableRateShading) {
        d.maxShadingRate = 2; // 1x1, 1x2, 2x1 and 2x2 are always supported
        std::uint32_t n = 0;
        vkGetPhysicalDeviceFragmentShadingRatesKHR(m_physical, &n, nullptr);
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(n, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
        vkGetPhysicalDeviceFragmentShadingRatesKHR(m_physical, &n, rates.data());
        for (const auto& r : rates)
            if (r.fragmentSize.width == 4 && r.fragmentSize.height == 4 && (r.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
                d.maxShadingRate = 4;
    }
    d.memoryBudget = m_memoryBudgetExt;
    d.resizableBar = m_rebar && m_adapter.type == AdapterType::Discrete;

    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sizeControl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceDescriptorIndexingProperties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    void** tail = &sub.pNext;
    const bool hasIndexing = v12 || extensionEnabled(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    if (hasIndexing) {
        *tail = &indexing;
        tail = &indexing.pNext;
    }
    if (v13 || extensionEnabled(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) *tail = &sizeControl;
    vkGetPhysicalDeviceProperties2(m_physical, &p2);
    {
        // Texture tables: a partially bound, variable-size array of sampled images in set 1.
        const auto& ix = v12 ? f.v12 : VkPhysicalDeviceVulkan12Features{};
        const bool runtimeArray = v12 ? ix.runtimeDescriptorArray : f.descriptorIndexing.runtimeDescriptorArray;
        const bool nonUniform = v12 ? ix.shaderSampledImageArrayNonUniformIndexing
                                    : f.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
        const bool partial = v12 ? ix.descriptorBindingPartiallyBound : f.descriptorIndexing.descriptorBindingPartiallyBound;
        const bool variable = v12 ? ix.descriptorBindingVariableDescriptorCount
                                  : f.descriptorIndexing.descriptorBindingVariableDescriptorCount;
        m_tableUpdateAfterBind = v12 ? ix.descriptorBindingSampledImageUpdateAfterBind
                                     : f.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind;
        if (hasIndexing && runtimeArray && nonUniform && partial && variable) {
            const std::uint64_t setLimit = m_tableUpdateAfterBind ? indexing.maxDescriptorSetUpdateAfterBindSampledImages
                                                                  : m_props.limits.maxDescriptorSetSampledImages;
            const std::uint64_t stageLimit = m_tableUpdateAfterBind
                                                 ? std::min(indexing.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                            indexing.maxPerStageUpdateAfterBindResources)
                                                 : std::min(m_props.limits.maxPerStageDescriptorSampledImages,
                                                            m_props.limits.maxPerStageResources);
            // Leave room for the ordinary set-0 bindings of the same pipeline.
            const std::uint64_t n = std::min<std::uint64_t>({65536, setLimit, stageLimit > 64 ? stageLimit - 64 : 0});
            d.maxBindlessTextures = n >= 256 ? static_cast<std::uint32_t>(n) : 0;
        }
    }
    d.descriptorBuffer = extensionEnabled(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    d.cooperativeMatrix = extensionEnabled(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) && f.coopMatrix.cooperativeMatrix &&
                          f.v12.vulkanMemoryModel;
    d.cooperativeVector = extensionEnabled(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME) && f.coopVector.cooperativeVector;
    d.subgroupSize = sub.subgroupSize;
    d.minSubgroupSize = sizeControl.minSubgroupSize ? sizeControl.minSubgroupSize : sub.subgroupSize;
    d.maxSubgroupSize = sizeControl.maxSubgroupSize ? sizeControl.maxSubgroupSize : sub.subgroupSize;
    d.subgroupOps = (sub.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) &&
                    (sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
    const auto& l = m_props.limits;
    d.maxTexture2D = l.maxImageDimension2D;
    d.maxMsaaSamples = 1;
    for (std::uint32_t s : {2u, 4u, 8u, 16u})
        if ((l.framebufferColorSampleCounts & s) && (l.framebufferDepthSampleCounts & s)) d.maxMsaaSamples = s;
    d.maxComputeSharedMemory = l.maxComputeSharedMemorySize;
    d.maxComputeWorkgroupSize = {l.maxComputeWorkGroupSize[0], l.maxComputeWorkGroupSize[1],
                                 l.maxComputeWorkGroupSize[2]};
    d.maxComputeWorkgroupInvocations = l.maxComputeWorkGroupInvocations;
    d.maxStorageBufferRange = l.maxStorageBufferRange;
    d.maxBufferSize = l.maxStorageBufferRange;
}

// --- submission / sync --------------------------------------------------------------------------
void VulkanDevice::pollQueue(QueueState& q) {
    while (!q.inflight.empty()) {
        auto [value, fence] = q.inflight.front();
        VkResult r = vkGetFenceStatus(m_device, fence);
        if (r == VK_NOT_READY) break;
        if (r != VK_SUCCESS) throwResult(r, "vkGetFenceStatus");
        q.completed = std::max(q.completed, value);
        m_freeFences.push_back(fence);
        q.inflight.pop_front();
    }
}

std::uint64_t VulkanDevice::submit(QueueType type, const SubmitInfo& info) {
    QueueState& q = queue(type);
    std::vector<VkCommandBuffer> cmds;
    cmds.reserve(info.lists.size());
    for (CommandList* l : info.lists) cmds.push_back(static_cast<VulkanCommandList*>(l)->handle());

    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<std::uint64_t> waitValues;
    for (const QueueWait& w : info.waits) {
        QueueState& other = queue(w.queue);
        if (&other == &q) continue; // same queue: implicit ordering
        if (m_timeline) {
            waitSems.push_back(other.timeline);
            waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            waitValues.push_back(w.value);
        } else {
            wait(w.queue, w.value); // fallback: serialize on the CPU
        }
    }
    auto* sc = static_cast<VulkanSwapchain*>(info.present);
    if (sc) {
        waitSems.push_back(sc->acquireSemaphore());
        waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
        waitValues.push_back(0);
    }
    const std::uint64_t value = q.submitted + 1;
    std::vector<VkSemaphore> signalSems;
    std::vector<std::uint64_t> signalValues;
    if (m_timeline) {
        signalSems.push_back(q.timeline);
        signalValues.push_back(value);
    }
    if (sc) {
        signalSems.push_back(sc->presentSemaphore());
        signalValues.push_back(0);
    }
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = static_cast<std::uint32_t>(waitValues.size());
    tsi.pWaitSemaphoreValues = waitValues.data();
    tsi.signalSemaphoreValueCount = static_cast<std::uint32_t>(signalValues.size());
    tsi.pSignalSemaphoreValues = signalValues.data();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (m_timeline) si.pNext = &tsi;
    si.waitSemaphoreCount = static_cast<std::uint32_t>(waitSems.size());
    si.pWaitSemaphores = waitSems.data();
    si.pWaitDstStageMask = waitStages.data();
    si.commandBufferCount = static_cast<std::uint32_t>(cmds.size());
    si.pCommandBuffers = cmds.data();
    si.signalSemaphoreCount = static_cast<std::uint32_t>(signalSems.size());
    si.pSignalSemaphores = signalSems.data();
    VkFence fence = getFence(m_device, m_freeFences);
    VkResult r = vkQueueSubmit(q.queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        m_freeFences.push_back(fence);
        throwResult(r, "vkQueueSubmit");
    }
    q.submitted = value;
    q.inflight.emplace_back(value, fence);
    for (CommandList* l : info.lists) static_cast<VulkanCommandList*>(l)->setLastSubmit(value);
    if (sc) sc->markAcquireConsumed(value);
    collectGarbage();
    return value;
}

void VulkanDevice::present(Swapchain* swapchain) {
    auto* sc = static_cast<VulkanSwapchain*>(swapchain);
    VkSemaphore wait = sc->presentSemaphore();
    VkSwapchainKHR handle = sc->handle();
    std::uint32_t index = sc->imageIndex();
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &handle;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(queue(QueueType::Graphics).queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        sc->markOutOfDate();
        return;
    }
    if (r != VK_SUCCESS) throwResult(r, "vkQueuePresentKHR");
}

void VulkanDevice::wait(QueueType type, std::uint64_t value) {
    QueueState& q = queue(type);
    if (value <= q.completed) return;
    for (auto& [v, fence] : q.inflight) {
        if (v >= value) {
            VkResult r = vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
            if (r != VK_SUCCESS) throwResult(r, "vkWaitForFences");
            break;
        }
    }
    pollQueue(q);
}

bool VulkanDevice::isComplete(QueueType type, std::uint64_t value) {
    QueueState& q = queue(type);
    if (value <= q.completed) return true;
    pollQueue(q);
    return value <= q.completed;
}

void VulkanDevice::waitIdle() {
    VkResult r = vkDeviceWaitIdle(m_device);
    if (r != VK_SUCCESS) throwResult(r, "vkDeviceWaitIdle");
    for (auto& q : m_queues) pollQueue(*q);
    collectGarbage();
}

void VulkanDevice::deferDestroy(std::function<void()> fn) {
    Garbage g;
    for (int i = 0; i < 3; ++i) g.values[static_cast<std::size_t>(i)] = m_queueFor[static_cast<std::size_t>(i)]->submitted;
    g.fn = std::move(fn);
    m_garbage.push_back(std::move(g));
}

void VulkanDevice::collectGarbage() {
    for (auto& q : m_queues) pollQueue(*q);
    while (!m_garbage.empty()) {
        const auto& g = m_garbage.front();
        bool done = true;
        for (int i = 0; i < 3; ++i)
            done = done && g.values[static_cast<std::size_t>(i)] <= m_queueFor[static_cast<std::size_t>(i)]->completed;
        if (!done) break;
        g.fn();
        m_garbage.pop_front();
    }
}

// --- render pass / framebuffer caches --------------------------------------------------------------------
VkRenderPass VulkanDevice::renderPass(const std::vector<VkFormat>& colors, VkFormat depth,
                                      VkSampleCountFlagBits samples, const std::vector<LoadOp>& colorLoads,
                                      LoadOp depthLoad, StoreOp depthStore, bool depthReadOnly) {
    std::string key = std::format("{}|{}|{}|{}|{}|", static_cast<int>(depth), static_cast<int>(samples),
                                  static_cast<int>(depthLoad), static_cast<int>(depthStore), depthReadOnly);
    for (std::size_t i = 0; i < colors.size(); ++i)
        key += std::format("{}:{},", static_cast<int>(colors[i]),
                           static_cast<int>(i < colorLoads.size() ? colorLoads[i] : LoadOp::Load));
    // Pipelines may be created from several threads (feature.pipelines compiles in parallel).
    std::lock_guard lock(m_renderPassMutex);
    if (auto it = m_renderPasses.find(key); it != m_renderPasses.end()) return it->second;

    auto loadOp = [](LoadOp l) {
        switch (l) {
        case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    };
    std::vector<VkAttachmentDescription> atts;
    std::vector<VkAttachmentReference> colorRefs;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        VkAttachmentDescription a{};
        a.format = colors[i];
        a.samples = samples;
        a.loadOp = loadOp(i < colorLoads.size() ? colorLoads[i] : LoadOp::Load);
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRefs.push_back({static_cast<std::uint32_t>(atts.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        atts.push_back(a);
    }
    VkAttachmentReference depthRef{};
    const VkImageLayout depthLayout = depthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                    : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (depth != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription a{};
        a.format = depth;
        a.samples = samples;
        a.loadOp = loadOp(depthLoad);
        a.storeOp = depthStore == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.stencilLoadOp = a.loadOp;
        a.stencilStoreOp = a.storeOp;
        a.initialLayout = depthLayout;
        a.finalLayout = depthLayout;
        depthRef = {static_cast<std::uint32_t>(atts.size()), depthLayout};
        atts.push_back(a);
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<std::uint32_t>(colorRefs.size());
    sub.pColorAttachments = colorRefs.data();
    sub.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depthRef : nullptr;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<std::uint32_t>(atts.size());
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    check(vkCreateRenderPass(m_device, &ci, nullptr, &rp), "vkCreateRenderPass");
    m_renderPasses.emplace(key, rp);
    return rp;
}

VkRenderPass VulkanDevice::compatibleRenderPass(const std::vector<Format>& colors, Format depth,
                                                std::uint32_t samples) {
    std::vector<VkFormat> vc;
    for (Format c : colors) vc.push_back(toVk(c));
    return renderPass(vc, toVk(depth), toVkSamples(samples), {}, LoadOp::Load, StoreOp::Store, false);
}

VkFramebuffer VulkanDevice::framebuffer(VkRenderPass compat, const std::vector<VulkanTexture*>& textures,
                                        const std::vector<VkImageView>& views, std::uint32_t width,
                                        std::uint32_t height) {
    std::string key = std::format("{}|{}x{}|", reinterpret_cast<std::uintptr_t>(compat), width, height);
    for (VkImageView v : views) key += std::format("{},", reinterpret_cast<std::uintptr_t>(v));
    if (auto it = m_framebuffers.find(key); it != m_framebuffers.end()) return it->second.fb;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = compat;
    ci.attachmentCount = static_cast<std::uint32_t>(views.size());
    ci.pAttachments = views.data();
    ci.width = width;
    ci.height = height;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    check(vkCreateFramebuffer(m_device, &ci, nullptr, &fb), "vkCreateFramebuffer");
    m_framebuffers.emplace(key, FbEntry{fb, views});
    for (VulkanTexture* t : textures) t->addFramebuffer(fb);
    return fb;
}

void VulkanDevice::forgetFramebuffer(VkFramebuffer fb) {
    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end(); ++it) {
        if (it->second.fb == fb) {
            m_framebuffers.erase(it);
            VkDevice dev = m_device;
            deferDestroy([dev, fb] { vkDestroyFramebuffer(dev, fb, nullptr); });
            return;
        }
    }
}

// --- factories ----------------------------------------------------------------------------------------------
std::unique_ptr<Buffer> VulkanDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    auto b = std::make_unique<VulkanBuffer>(*this, desc);
    if (initialData) {
        if (b->mapped()) std::memcpy(b->mapped(), initialData, desc.size);
        else uploadBuffer(b.get(), initialData, desc.size, 0);
    }
    return b;
}
std::unique_ptr<Texture> VulkanDevice::createTexture(const TextureDesc& desc) {
    return std::make_unique<VulkanTexture>(*this, desc);
}
std::unique_ptr<Sampler> VulkanDevice::createSampler(const SamplerDesc& desc) {
    return std::make_unique<VulkanSampler>(*this, desc);
}
std::unique_ptr<Pipeline> VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    return std::make_unique<VulkanPipeline>(*this, desc);
}
std::unique_ptr<Pipeline> VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    return std::make_unique<VulkanPipeline>(*this, desc);
}
std::unique_ptr<QueryPool> VulkanDevice::createQueryPool(QueryType type, std::uint32_t count) {
    return std::make_unique<VulkanQueryPool>(*this, type, count);
}
std::unique_ptr<CommandList> VulkanDevice::createCommandList(QueueType queue) {
    return std::make_unique<VulkanCommandList>(*this, queue);
}
std::unique_ptr<Swapchain> VulkanDevice::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<VulkanSwapchain>(*this, desc);
}

VkSurfaceKHR VulkanDevice::createSurface(void* window) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), m_instance, nullptr, &surface))
        throw Error(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
    return surface;
}

// --- uploads / readback ----------------------------------------------------------------------------------------
void VulkanDevice::uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    VulkanBuffer staging(*this, BufferDesc{size, BufferUsage::CopySrc, MemoryType::Upload, 0, "staging"});
    std::memcpy(staging.mapped(), data, size);
    m_uploadList->begin();
    m_uploadList->transition(dst, ResourceState::CopyDst);
    m_uploadList->copyBuffer(dst, dstOffset, &staging, 0, size);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    const std::uint64_t v = submit(QueueType::Graphics, SubmitInfo{lists});
    wait(QueueType::Graphics, v);
}

void VulkanDevice::uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                                 std::uint32_t rowPitch) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    const auto& d = dst->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const std::uint32_t h = std::max(1u, d.height >> mip);
    const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
    const std::uint64_t size = static_cast<std::uint64_t>(rowPitch) * rows;
    VulkanBuffer staging(*this, BufferDesc{size, BufferUsage::CopySrc, MemoryType::Upload, 0, "staging"});
    std::memcpy(staging.mapped(), data, size);
    m_uploadList->begin();
    m_uploadList->transition(dst, ResourceState::CopyDst);
    m_uploadList->copyBufferToTexture(dst, mip, layer, &staging, 0, rowPitch);
    m_uploadList->transition(dst, ResourceState::ShaderRead);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    const std::uint64_t v = submit(QueueType::Graphics, SubmitInfo{lists});
    wait(QueueType::Graphics, v);
}

std::vector<std::uint8_t> VulkanDevice::readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    const auto& d = src->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const std::uint32_t w = std::max(1u, d.width >> mip);
    const std::uint32_t h = std::max(1u, d.height >> mip);
    const std::uint32_t rowPitch = ((w + fi.blockDim - 1) / fi.blockDim) * fi.bytesPerBlock;
    const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
    const std::uint64_t size = static_cast<std::uint64_t>(rowPitch) * rows;
    VulkanBuffer rb(*this, BufferDesc{size, BufferUsage::CopyDst, MemoryType::Readback, 0, "readback"});
    const ResourceState prev = static_cast<VulkanTexture*>(src)->state;
    m_uploadList->begin();
    m_uploadList->transition(src, ResourceState::CopySrc);
    m_uploadList->copyTextureToBuffer(&rb, 0, rowPitch, src, mip, layer);
    m_uploadList->transition(&rb, ResourceState::HostRead);
    if (prev != ResourceState::Undefined) m_uploadList->transition(src, prev);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    const std::uint64_t v = submit(QueueType::Graphics, SubmitInfo{lists});
    wait(QueueType::Graphics, v);
    rb.invalidate();
    std::vector<std::uint8_t> out(size);
    std::memcpy(out.data(), rb.mapped(), size);
    return out;
}

std::uint32_t VulkanDevice::renderTargetSampleCounts(Format format) const {
    const bool depth = formatInfo(format).depth;
    VkImageFormatProperties props{};
    const VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        m_physical, toVk(format), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &props);
    return r == VK_SUCCESS ? static_cast<std::uint32_t>(props.sampleCounts) : 1u;
}

DeviceFaultReport VulkanDevice::deviceFaultReport() {
    DeviceFaultReport r;
    if (!m_deviceFault) return r;
    r.supported = true;
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult res = vkGetDeviceFaultInfoEXT(m_device, &counts, nullptr);
    if (res < VK_SUCCESS) {
        r.description = std::format("vkGetDeviceFaultInfoEXT failed: {}", resultName(res));
        return r;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
    res = vkGetDeviceFaultInfoEXT(m_device, &counts, &info);
    if (res < VK_SUCCESS) {
        r.description = std::format("vkGetDeviceFaultInfoEXT failed: {}", resultName(res));
        return r;
    }
    r.description = info.description;
    auto addressType = [](VkDeviceFaultAddressTypeEXT t) -> const char* {
        switch (t) {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "no address";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "invalid read";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "invalid write";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "invalid execute";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer (unknown)";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer (invalid)";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer (fault)";
        default: return "address";
        }
    };
    for (std::uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addresses[i];
        r.details.push_back(std::format("{} at 0x{:x} (±0x{:x})", addressType(a.addressType),
                                        static_cast<unsigned long long>(a.reportedAddress),
                                        static_cast<unsigned long long>(a.addressPrecision)));
    }
    for (std::uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& v = vendor[i];
        r.details.push_back(std::format("vendor fault 0x{:x} (data 0x{:x}): {}", static_cast<unsigned long long>(v.vendorFaultCode),
                                        static_cast<unsigned long long>(v.vendorFaultData), v.description));
    }
    return r;
}

MemoryBudget VulkanDevice::memoryBudget() const {
    MemoryBudget mb;
    if (!m_memoryBudgetExt) return mb;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    p.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(m_physical, &p);
    for (std::uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; ++i) {
        if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            mb.localUsage += budget.heapUsage[i];
            mb.localBudget += budget.heapBudget[i];
        } else {
            mb.nonLocalUsage += budget.heapUsage[i];
            mb.nonLocalBudget += budget.heapBudget[i];
        }
    }
    mb.valid = true;
    return mb;
}

void VulkanDevice::setObjectName(VkObjectType type, std::uint64_t handle, const char* name) {
    if (!m_debugUtils || !vkSetDebugUtilsObjectNameEXT || !name || !*name) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(m_device, &ni);
}

void VulkanDevice::setDebugName(Buffer* b, const char* name) {
    setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(static_cast<VulkanBuffer*>(b)->handle()),
                  name);
}
void VulkanDevice::setDebugName(Texture* t, const char* name) {
    setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(static_cast<VulkanTexture*>(t)->handle()),
                  name);
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) { return std::make_unique<VulkanDevice>(info); }

} // namespace gt::rhi::vk
