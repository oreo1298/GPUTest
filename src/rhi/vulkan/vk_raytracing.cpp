// Ray tracing for the Vulkan backend: acceleration structures (VK_KHR_acceleration_structure),
// ray-tracing pipelines with an internal shader binding table (VK_KHR_ray_tracing_pipeline),
// and the command-list side (builds, barriers, descriptor binding, vkCmdTraceRaysKHR).
// Ray queries need nothing beyond the acceleration-structure binding.

#include "vk_device.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gt::rhi::vk {
namespace {

std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return a ? (v + a - 1) / a * a : v; }

struct BlasGeometry {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<std::uint32_t> primitives;
};

BlasGeometry describe(const BlasDesc& desc) {
    BlasGeometry g;
    for (const AccelTriangles& t : desc.geometries) {
        auto* vb = static_cast<VulkanBuffer*>(t.vertices);
        auto* ib = static_cast<VulkanBuffer*>(t.indices);
        if (!vb) throw Error("BLAS geometry without a vertex buffer");
        VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& tri = geo.geometry.triangles;
        tri = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = vb->address() + t.vertexOffset;
        tri.vertexStride = t.vertexStride;
        tri.maxVertex = t.vertexCount ? t.vertexCount - 1 : 0;
        if (ib) {
            tri.indexType = VK_INDEX_TYPE_UINT32;
            tri.indexData.deviceAddress = ib->address() + t.indexOffset;
        } else {
            tri.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        const std::uint32_t prims = (ib ? t.indexCount : t.vertexCount) / 3;
        g.geometries.push_back(geo);
        g.ranges.push_back({prims, 0, 0, 0});
        g.primitives.push_back(prims);
    }
    return g;
}

VkBuildAccelerationStructureFlagsKHR blasFlags(const BlasDesc& d) {
    VkBuildAccelerationStructureFlagsKHR f =
        d.fastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (d.allowUpdate) f |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    return f;
}

constexpr VkBuildAccelerationStructureFlagsKHR kTlasFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

} // namespace

// --- properties ------------------------------------------------------------------------------
const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& VulkanDevice::rayTracingProperties() {
    if (!m_rtPropsQueried) {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        void** tail = &p2.pNext;
        if (extensionEnabled(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
            *tail = &m_rtProps;
            tail = &m_rtProps.pNext;
        }
        if (extensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) *tail = &m_asProps;
        vkGetPhysicalDeviceProperties2(m_physical, &p2);
        m_rtPropsQueried = true;
    }
    return m_rtProps;
}

const VkPhysicalDeviceAccelerationStructurePropertiesKHR& VulkanDevice::accelProperties() {
    rayTracingProperties();
    return m_asProps;
}

// --- acceleration structures --------------------------------------------------------------------
VulkanAccel::VulkanAccel(VulkanDevice& dev, bool topLevel, const VkAccelerationStructureBuildSizesInfoKHR& sizes,
                         std::uint32_t maxInstances, const char* name)
    : m_dev(dev), m_maxInstances(maxInstances) {
    m_topLevel = topLevel;
    m_size = sizes.accelerationStructureSize;
    m_scratch = std::max(sizes.buildScratchSize, sizes.updateScratchSize);
    m_storage = std::make_unique<VulkanBuffer>(
        dev, BufferDesc{m_size, BufferUsage::AccelStorage, MemoryType::Device, 0, name});
    const std::uint64_t align = std::max<std::uint64_t>(dev.accelProperties().minAccelerationStructureScratchOffsetAlignment, 1);
    m_scratchBuffer = std::make_unique<VulkanBuffer>(
        dev, BufferDesc{m_scratch + align, BufferUsage::Storage | BufferUsage::AccelStorage, MemoryType::Device, 0, "accel-scratch"});
    m_scratchAddress = alignUp(m_scratchBuffer->address(), align);
    if (topLevel)
        m_instances = std::make_unique<VulkanBuffer>(
            dev, BufferDesc{std::max<std::uint64_t>(1, maxInstances) * sizeof(VkAccelerationStructureInstanceKHR),
                            BufferUsage::AccelInput | BufferUsage::CopyDst, MemoryType::Device, 0, "tlas-instances"});
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = m_storage->handle();
    ci.size = m_size;
    ci.type = topLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    check(vkCreateAccelerationStructureKHR(dev.vkDevice(), &ci, nullptr, &m_handle), "vkCreateAccelerationStructureKHR");
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = m_handle;
    m_address = vkGetAccelerationStructureDeviceAddressKHR(dev.vkDevice(), &ai);
    dev.setObjectName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<std::uint64_t>(m_handle), name);
}

VulkanAccel::~VulkanAccel() {
    VkDevice d = m_dev.vkDevice();
    VkAccelerationStructureKHR h = m_handle;
    m_dev.deferDestroy([d, h] { vkDestroyAccelerationStructureKHR(d, h, nullptr); });
}

std::unique_ptr<AccelerationStructure> VulkanDevice::createBlas(const BlasDesc& desc) {
    if (!m_features.rayQuery && !m_features.rayTracingPipeline) throw Error("ray tracing is not supported by this GPU");
    BlasGeometry g = describe(desc);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = blasFlags(desc);
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = static_cast<std::uint32_t>(g.geometries.size());
    bi.pGeometries = g.geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi,
                                            g.primitives.data(), &sizes);
    return std::make_unique<VulkanAccel>(*this, false, sizes, 0, desc.name);
}

std::unique_ptr<AccelerationStructure> VulkanDevice::createTlas(std::uint32_t maxInstances, bool allowUpdate) {
    if (!m_features.rayQuery && !m_features.rayTracingPipeline) throw Error("ray tracing is not supported by this GPU");
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = kTlasFlags | (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxInstances, &sizes);
    auto t = std::make_unique<VulkanAccel>(*this, true, sizes, maxInstances, "tlas");
    m_tlasUpdatable[t.get()] = allowUpdate;
    return t;
}

// --- ray-tracing pipelines ------------------------------------------------------------------------
VulkanPipeline::VulkanPipeline(VulkanDevice& dev, const RayTracingPipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!dev.features().rayTracingPipeline) throw Error("ray-tracing pipelines are not supported by this GPU");
    m_isCompute = false;
    m_bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    createLayout(d.layout);

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    auto addStage = [&](const ShaderRef& ref, VkShaderStageFlagBits stage) -> std::uint32_t {
        if (!ref.valid()) return VK_SHADER_UNUSED_KHR;
        VkShaderModule m = dev.createShaderModule(ref);
        modules.push_back(m);
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage = stage;
        s.module = m;
        s.pName = "main";
        stages.push_back(s);
        return static_cast<std::uint32_t>(stages.size() - 1);
    };
    auto general = [&](std::uint32_t stage) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        g.generalShader = stage;
        g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(g);
    };
    general(addStage(d.rayGen, VK_SHADER_STAGE_RAYGEN_BIT_KHR));
    for (const auto& m : d.miss) general(addStage(m, VK_SHADER_STAGE_MISS_BIT_KHR));
    for (const auto& h : d.hitGroups) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = h.intersection.valid() ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                                        : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        g.generalShader = VK_SHADER_UNUSED_KHR;
        g.closestHitShader = addStage(h.closestHit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        g.anyHitShader = addStage(h.anyHit, VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
        g.intersectionShader = addStage(h.intersection, VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
        groups.push_back(g);
    }
    const auto& props = dev.rayTracingProperties();
    VkRayTracingPipelineCreateInfoKHR ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    ci.stageCount = static_cast<std::uint32_t>(stages.size());
    ci.pStages = stages.data();
    ci.groupCount = static_cast<std::uint32_t>(groups.size());
    ci.pGroups = groups.data();
    ci.maxPipelineRayRecursionDepth = std::min(std::max(d.maxRecursion, 1u), props.maxRayRecursionDepth);
    ci.layout = m_layout;
    VkResult r = vkCreateRayTracingPipelinesKHR(dev.vkDevice(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    for (VkShaderModule m : modules) vkDestroyShaderModule(dev.vkDevice(), m, nullptr);
    if (r != VK_SUCCESS) throwResult(r, std::format("vkCreateRayTracingPipelinesKHR({})", d.name).c_str());
    dev.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(m_pipeline), d.name);

    // Shader binding table: raygen | misses | hit groups, each region aligned to the base alignment.
    const std::uint32_t handleSize = props.shaderGroupHandleSize;
    const std::uint64_t stride = alignUp(handleSize, props.shaderGroupHandleAlignment);
    const std::uint64_t base = std::max<std::uint64_t>(props.shaderGroupBaseAlignment, 1);
    const std::uint64_t missCount = d.miss.size(), hitCount = d.hitGroups.size();
    const std::uint64_t rgenSize = alignUp(stride, base);
    const std::uint64_t missSize = alignUp(stride * missCount, base);
    const std::uint64_t hitSize = alignUp(stride * hitCount, base);
    std::vector<std::uint8_t> handles(std::size_t(handleSize) * groups.size());
    check(vkGetRayTracingShaderGroupHandlesKHR(dev.vkDevice(), m_pipeline, 0, static_cast<std::uint32_t>(groups.size()),
                                               handles.size(), handles.data()),
          "vkGetRayTracingShaderGroupHandlesKHR");
    m_sbt = std::make_unique<VulkanBuffer>(
        dev, BufferDesc{rgenSize + missSize + hitSize + base, BufferUsage::ShaderTable, MemoryType::Upload, 0, "sbt"});
    const VkDeviceAddress start = alignUp(m_sbt->address(), base);
    auto* mapped = static_cast<std::uint8_t*>(m_sbt->mapped()) + (start - m_sbt->address());
    std::size_t group = 0;
    auto put = [&](std::uint64_t offset) {
        std::memcpy(mapped + offset, handles.data() + group * handleSize, handleSize);
        ++group;
    };
    put(0);
    for (std::uint64_t i = 0; i < missCount; ++i) put(rgenSize + i * stride);
    for (std::uint64_t i = 0; i < hitCount; ++i) put(rgenSize + missSize + i * stride);
    m_sbt->flush();
    m_sbtRegions[0] = {start, rgenSize, rgenSize};
    m_sbtRegions[1] = {missCount ? start + rgenSize : 0, stride, missCount ? missSize : 0};
    m_sbtRegions[2] = {hitCount ? start + rgenSize + missSize : 0, stride, hitCount ? hitSize : 0};
    m_sbtRegions[3] = {0, 0, 0};
    m_creationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::unique_ptr<Pipeline> VulkanDevice::createRayTracingPipeline(const RayTracingPipelineDesc& desc) {
    return std::make_unique<VulkanPipeline>(*this, desc);
}

// --- command list --------------------------------------------------------------------------------
void VulkanCommandList::buildBlas(AccelerationStructure* blas, const BlasDesc& desc, bool update) {
    requireRecording("buildBlas");
    auto* as = static_cast<VulkanAccel*>(blas);
    // Inputs (vertex data written by copies or shaders) and earlier builds sharing the scratch memory.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    BlasGeometry g = describe(desc);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = blasFlags(desc);
    bi.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.srcAccelerationStructure = update ? as->handle() : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = as->handle();
    bi.geometryCount = static_cast<std::uint32_t>(g.geometries.size());
    bi.pGeometries = g.geometries.data();
    bi.scratchData.deviceAddress = as->scratchAddress();
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = g.ranges.data();
    vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &bi, &ranges);
    m_accelDirty = true;
}

void VulkanCommandList::buildTlas(AccelerationStructure* tlas, std::span<const AccelInstance> instances, bool update) {
    requireRecording("buildTlas");
    auto* as = static_cast<VulkanAccel*>(tlas);
    if (instances.size() > as->maxInstances()) throw Error("buildTlas: more instances than the TLAS was created for");
    // Instance records go through transient upload memory into the TLAS's device-local instance buffer.
    const std::uint64_t bytes = std::max<std::uint64_t>(1, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR);
    TransientAllocation t = allocTransient(bytes, 16);
    auto* rec = static_cast<VkAccelerationStructureInstanceKHR*>(t.cpu);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const AccelInstance& in = instances[i];
        VkAccelerationStructureInstanceKHR r{};
        std::memcpy(r.transform.matrix, in.transform, sizeof(r.transform.matrix));
        r.instanceCustomIndex = in.id & 0xFFFFFFu;
        r.mask = in.mask;
        r.instanceShaderBindingTableRecordOffset = in.hitGroupOffset & 0xFFFFFFu;
        r.flags = in.forceOpaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : 0;
        r.accelerationStructureReference = static_cast<VulkanAccel*>(in.blas)->address();
        rec[i] = r;
    }
    copyBuffer(as->instanceBuffer(), 0, t.buffer, t.offset, instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geo.geometry.instances.data.deviceAddress = as->instanceBuffer()->address();
    const bool updatable = m_dev.tlasUpdatable(as);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = kTlasFlags | (updatable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    bi.mode = update && updatable ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.srcAccelerationStructure = bi.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? as->handle() : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = as->handle();
    bi.geometryCount = 1;
    bi.pGeometries = &geo;
    bi.scratchData.deviceAddress = as->scratchAddress();
    const VkAccelerationStructureBuildRangeInfoKHR range{static_cast<std::uint32_t>(instances.size()), 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &bi, &ranges);
    m_accelDirty = true;
}

void VulkanCommandList::accelBarrier() {
    requireRecording("accelBarrier");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkPipelineStageFlags dst = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (m_dev.features().rayTracingPipeline) dst |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
    m_accelDirty = false;
}

void VulkanCommandList::setAccelerationStructure(std::uint32_t slotIndex, AccelerationStructure* tlas) {
    Bound& b = slot('t', slotIndex);
    b = {};
    b.accel = static_cast<VulkanAccel*>(tlas)->handle();
    m_bindingsDirty = true;
}

void VulkanCommandList::traceRays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    requireRecording("traceRays");
    if (!m_pipeline || m_pipeline->bindPoint() != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
        throw Error("traceRays without a ray-tracing pipeline");
    flushBindings();
    vkCmdTraceRaysKHR(m_cmd, &m_pipeline->raygenRegion(), &m_pipeline->missRegion(), &m_pipeline->hitRegion(),
                      &m_pipeline->callableRegion(), width, height, depth);
}

} // namespace gt::rhi::vk
