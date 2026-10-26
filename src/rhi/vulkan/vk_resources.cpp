#include "vk_device.h"

#include <algorithm>
#include <chrono>

namespace gt::rhi::vk {

ImageStateInfo imageState(ResourceState s, bool depth) {
    switch (s) {
    case ResourceState::Undefined: return {VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    case ResourceState::Common:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::ShaderRead:
        return {depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::UnorderedAccess:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::RenderTarget:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    case ResourceState::DepthWrite:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT};
    case ResourceState::DepthRead:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    case ResourceState::CopySrc:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case ResourceState::CopyDst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case ResourceState::Present: return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
    default:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    }
}

ImageStateInfo bufferState(ResourceState s) {
    switch (s) {
    case ResourceState::Undefined:
    case ResourceState::Common: return {VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    case ResourceState::VertexBuffer:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT};
    case ResourceState::IndexBuffer:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT};
    case ResourceState::UniformBuffer:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::ShaderRead:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::UnorderedAccess:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    case ResourceState::IndirectArgument:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT};
    case ResourceState::CopySrc:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case ResourceState::CopyDst:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case ResourceState::HostRead:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT};
    default:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    }
}

// --- buffer -------------------------------------------------------------------------------------
VulkanBuffer::VulkanBuffer(VulkanDevice& dev, const BufferDesc& desc) : m_dev(dev) {
    m_desc = desc;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = std::max<std::uint64_t>(desc.size, 4);
    auto u = desc.usage;
    if (any(u & BufferUsage::Vertex)) ci.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u & BufferUsage::Index)) ci.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u & BufferUsage::Uniform)) ci.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u & BufferUsage::Storage)) ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u & BufferUsage::Indirect)) ci.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    // Transfers are always allowed: staging uploads, readbacks and fills need them.
    ci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (dev.features().bufferDeviceAddress && any(u & (BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index)))
        ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (any(u & BufferUsage::AccelInput))
        ci.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (any(u & BufferUsage::AccelStorage))
        ci.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (any(u & BufferUsage::Micromap))
        ci.usage |= VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (any(u & BufferUsage::ShaderTable))
        ci.usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    // Buffers are shared between queue families (cheap for buffers, needed for async compute).
    std::vector<std::uint32_t> families;
    for (QueueType q : {QueueType::Graphics, QueueType::Compute, QueueType::Copy}) {
        const std::uint32_t fam = dev.queue(q).family;
        if (std::find(families.begin(), families.end(), fam) == families.end()) families.push_back(fam);
    }
    if (families.size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        ci.pQueueFamilyIndices = families.data();
    }

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.memory) {
    case MemoryType::Device: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
    case MemoryType::Upload:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        break;
    case MemoryType::Readback:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        break;
    case MemoryType::DeviceUpload:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    }
    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(dev.allocator(), &ci, &ai, &m_buffer, &m_alloc, &info);
    if (r != VK_SUCCESS) throwResult(r, "vmaCreateBuffer");
    m_mapped = info.pMappedData;
    VkMemoryPropertyFlags props = 0;
    vmaGetAllocationMemoryProperties(dev.allocator(), m_alloc, &props);
    m_deviceLocalHostVisible =
        (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bi.buffer = m_buffer;
        m_address = vkGetBufferDeviceAddress(dev.vkDevice(), &bi);
    }
    dev.setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(m_buffer), desc.name);
}

VulkanBuffer::~VulkanBuffer() {
    VmaAllocator a = m_dev.allocator();
    VkBuffer b = m_buffer;
    VmaAllocation al = m_alloc;
    m_dev.deferDestroy([a, b, al] { vmaDestroyBuffer(a, b, al); });
}

void VulkanBuffer::invalidate() { vmaInvalidateAllocation(m_dev.allocator(), m_alloc, 0, VK_WHOLE_SIZE); }
void VulkanBuffer::flush() { vmaFlushAllocation(m_dev.allocator(), m_alloc, 0, VK_WHOLE_SIZE); }

// --- texture -------------------------------------------------------------------------------------
namespace {
VkImageAspectFlags aspectFor(Format f) {
    const auto& fi = formatInfo(f);
    if (fi.depth) return VK_IMAGE_ASPECT_DEPTH_BIT | (fi.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
} // namespace

VulkanTexture::VulkanTexture(VulkanDevice& dev, const TextureDesc& desc) : m_dev(dev) {
    m_desc = desc;
    m_aspect = aspectFor(desc.format);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = desc.type == TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = toVk(desc.format);
    ci.extent = {desc.width, desc.height, desc.type == TextureType::Tex3D ? desc.depthOrLayers : 1u};
    ci.mipLevels = desc.mipLevels;
    ci.arrayLayers = desc.type == TextureType::Tex3D ? 1u : desc.depthOrLayers;
    if (desc.type == TextureType::Cube) {
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.arrayLayers = std::max(6u, desc.depthOrLayers);
    }
    ci.samples = toVkSamples(desc.sampleCount);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    auto u = desc.usage;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (any(u & TextureUsage::Sampled)) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(u & TextureUsage::Storage)) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(u & TextureUsage::RenderTarget)) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(u & TextureUsage::DepthStencil)) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<std::uint32_t> families;
    if (desc.concurrentQueues) {
        for (QueueType q : {QueueType::Graphics, QueueType::Compute, QueueType::Copy}) {
            const std::uint32_t fam = dev.queue(q).family;
            if (std::find(families.begin(), families.end(), fam) == families.end()) families.push_back(fam);
        }
        if (families.size() > 1) {
            ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
            ci.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
            ci.pQueueFamilyIndices = families.data();
        }
    }
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (any(u & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)) &&
        static_cast<std::uint64_t>(desc.width) * desc.height >= 1024ull * 1024)
        ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VkResult r = vmaCreateImage(dev.allocator(), &ci, &ai, &m_image, &m_alloc, nullptr);
    if (r != VK_SUCCESS) throwResult(r, "vmaCreateImage");
    dev.setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(m_image), desc.name);
}

VulkanTexture::VulkanTexture(VulkanDevice& dev, const TextureDesc& desc, VkImage image)
    : m_dev(dev), m_image(image), m_owned(false) {
    m_desc = desc;
    m_aspect = aspectFor(desc.format);
}

VulkanTexture::~VulkanTexture() {
    for (VkFramebuffer fb : m_framebuffers) m_dev.forgetFramebuffer(fb);
    VkDevice d = m_dev.vkDevice();
    std::vector<VkImageView> views;
    for (auto& [k, v] : m_views) views.push_back(v);
    VmaAllocator a = m_dev.allocator();
    VkImage img = m_owned ? m_image : VK_NULL_HANDLE;
    VmaAllocation al = m_alloc;
    m_dev.deferDestroy([d, views, a, img, al] {
        for (VkImageView v : views) vkDestroyImageView(d, v, nullptr);
        if (img) vmaDestroyImage(a, img, al);
    });
}

VkImageLayout VulkanTexture::layout() const { return imageState(state, formatInfo(m_desc.format).depth).layout; }

VkImageView VulkanTexture::view(Subresource sub, bool forAttachment) {
    ViewKey key{sub, forAttachment};
    if (auto it = m_views.find(key); it != m_views.end()) return it->second;
    const std::uint32_t layers = m_desc.type == TextureType::Tex3D ? 1u
                                 : m_desc.type == TextureType::Cube ? std::max(6u, m_desc.depthOrLayers)
                                                                    : m_desc.depthOrLayers;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = m_image;
    ci.format = toVk(m_desc.format);
    ci.subresourceRange.aspectMask = m_aspect;
    if (!forAttachment && formatInfo(m_desc.format).depth) ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ci.subresourceRange.baseMipLevel = sub.baseMip;
    ci.subresourceRange.levelCount = sub.mipCount ? sub.mipCount : m_desc.mipLevels - sub.baseMip;
    ci.subresourceRange.baseArrayLayer = sub.baseLayer;
    ci.subresourceRange.layerCount = sub.layerCount ? sub.layerCount : layers - sub.baseLayer;
    if (m_desc.type == TextureType::Tex3D) ci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    else if (m_desc.type == TextureType::Cube && !forAttachment && ci.subresourceRange.layerCount == 6)
        ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    else if (ci.subresourceRange.layerCount > 1 || m_desc.type == TextureType::Tex2DArray)
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageView v = VK_NULL_HANDLE;
    check(vkCreateImageView(m_dev.vkDevice(), &ci, nullptr, &v), "vkCreateImageView");
    m_views.emplace(key, v);
    return v;
}

// --- sampler -------------------------------------------------------------------------------------
VulkanSampler::VulkanSampler(VulkanDevice& dev, const SamplerDesc& d) : m_dev(dev) {
    auto addr = [](AddressMode m) {
        switch (m) {
        case AddressMode::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = d.magFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = d.minFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = d.mipFilter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = addr(d.addressU);
    ci.addressModeV = addr(d.addressV);
    ci.addressModeW = addr(d.addressW);
    ci.mipLodBias = d.mipLodBias;
    if (d.maxAnisotropy > 1.0f && dev.features().anisotropicFiltering) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(d.maxAnisotropy, dev.properties().limits.maxSamplerAnisotropy);
    }
    if (d.compare) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = toVk(*d.compare);
    }
    ci.minLod = d.minLod;
    ci.maxLod = d.maxLod;
    ci.borderColor = d.borderWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    check(vkCreateSampler(dev.vkDevice(), &ci, nullptr, &m_sampler), "vkCreateSampler");
}

VulkanSampler::~VulkanSampler() {
    VkDevice d = m_dev.vkDevice();
    VkSampler s = m_sampler;
    m_dev.deferDestroy([d, s] { vkDestroySampler(d, s, nullptr); });
}

// --- query pool ----------------------------------------------------------------------------------
VulkanQueryPool::VulkanQueryPool(VulkanDevice& dev, QueryType type, std::uint32_t count) : m_dev(dev) {
    m_type = type;
    m_count = count;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryCount = count;
    switch (type) {
    case QueryType::Timestamp: ci.queryType = VK_QUERY_TYPE_TIMESTAMP; break;
    case QueryType::Occlusion: ci.queryType = VK_QUERY_TYPE_OCCLUSION; break;
    case QueryType::PipelineStatistics:
        ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        break;
    }
    check(vkCreateQueryPool(dev.vkDevice(), &ci, nullptr, &m_pool), "vkCreateQueryPool");
}

VulkanQueryPool::~VulkanQueryPool() {
    VkDevice d = m_dev.vkDevice();
    VkQueryPool p = m_pool;
    m_dev.deferDestroy([d, p] { vkDestroyQueryPool(d, p, nullptr); });
}

bool VulkanQueryPool::read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) {
    const std::uint32_t perQuery = m_type == QueryType::PipelineStatistics ? 6 : 1;
    std::vector<std::uint64_t> raw(static_cast<std::size_t>(count) * (perQuery + 1));
    VkResult r = vkGetQueryPoolResults(m_dev.vkDevice(), m_pool, first, count, raw.size() * sizeof(std::uint64_t),
                                       raw.data(), (perQuery + 1) * sizeof(std::uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r == VK_ERROR_DEVICE_LOST) throwResult(r, "vkGetQueryPoolResults");
    bool all = true;
    const std::uint32_t validBits = m_dev.queue(QueueType::Graphics).timestampValidBits;
    const std::uint64_t mask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
    const double period = m_dev.properties().limits.timestampPeriod;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t* q = &raw[static_cast<std::size_t>(i) * (perQuery + 1)];
        if (q[perQuery] == 0) all = false;
        for (std::uint32_t k = 0; k < perQuery; ++k) {
            const std::size_t o = static_cast<std::size_t>(i) * perQuery + k;
            if (o >= out.size()) continue;
            out[o] = m_type == QueryType::Timestamp
                         ? static_cast<std::uint64_t>(static_cast<double>(q[k] & mask) * period)
                         : q[k];
        }
    }
    return all;
}

} // namespace gt::rhi::vk
