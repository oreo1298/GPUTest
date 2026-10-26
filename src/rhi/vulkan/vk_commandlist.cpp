#include "vk_device.h"

#include <algorithm>
#include <cstring>

namespace gt::rhi::vk {
namespace {
constexpr std::uint64_t kChunkSize = 4ull * 1024 * 1024;
constexpr std::uint32_t kSetsPerPool = 256;

std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return a ? (v + a - 1) / a * a : v; }
} // namespace

VulkanCommandList::VulkanCommandList(VulkanDevice& dev, QueueType queue) : m_dev(dev) {
    m_queue = queue;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = dev.queue(queue).family;
    check(vkCreateCommandPool(dev.vkDevice(), &pci, nullptr, &m_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(dev.vkDevice(), &ai, &m_cmd), "vkAllocateCommandBuffers");
}

VulkanCommandList::~VulkanCommandList() {
    VkDevice d = m_dev.vkDevice();
    VkCommandPool p = m_pool;
    std::vector<VkDescriptorPool> pools = m_descPools;
    m_chunks.clear(); // buffers defer their own destruction
    m_dev.deferDestroy([d, p, pools] {
        vkDestroyCommandPool(d, p, nullptr);
        for (VkDescriptorPool dp : pools) vkDestroyDescriptorPool(d, dp, nullptr);
    });
}

void VulkanCommandList::requireRecording(const char* what) const {
    if (!m_recording) throw Error(std::format("CommandList::{} called outside begin()/end()", what));
}

void VulkanCommandList::begin() {
    if (m_lastSubmit) m_dev.wait(m_queue, m_lastSubmit);
    check(vkResetCommandPool(m_dev.vkDevice(), m_pool, 0), "vkResetCommandPool");
    for (auto& c : m_chunks) c.used = 0;
    m_chunkIndex = 0;
    for (VkDescriptorPool dp : m_descPools) vkResetDescriptorPool(m_dev.vkDevice(), dp, 0);
    m_descPoolIndex = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(m_cmd, &bi), "vkBeginCommandBuffer");
    m_recording = true;
    m_inRenderPass = false;
    m_pipeline = nullptr;
    m_bindingsDirty = true;
    m_b = {};
    m_t = {};
    m_u = {};
    m_s = {};
}

void VulkanCommandList::end() {
    requireRecording("end");
    if (m_inRenderPass) endRenderPass();
    for (std::size_t i = 0; i <= m_chunkIndex && i < m_chunks.size(); ++i)
        if (m_chunks[i].used) m_chunks[i].buffer->flush();
    check(vkEndCommandBuffer(m_cmd), "vkEndCommandBuffer");
    m_recording = false;
}

// --- barriers ------------------------------------------------------------------------------------------
void VulkanCommandList::transition(Texture* texture, ResourceState state) {
    requireRecording("transition");
    auto* t = static_cast<VulkanTexture*>(texture);
    if (t->state == state && state != ResourceState::UnorderedAccess) return;
    const bool depth = formatInfo(t->desc().format).depth;
    const ImageStateInfo from = imageState(t->state, depth);
    const ImageStateInfo to = imageState(state, depth);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = from.access;
    b.dstAccessMask = to.access;
    b.oldLayout = from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->handle();
    b.subresourceRange = {t->aspect(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(m_cmd, from.stages, to.stages, 0, 0, nullptr, 0, nullptr, 1, &b);
    t->state = state;
}

void VulkanCommandList::transition(Buffer* buffer, ResourceState state) {
    requireRecording("transition");
    auto* b = static_cast<VulkanBuffer*>(buffer);
    if (b->state == state && state != ResourceState::UnorderedAccess) return;
    const ImageStateInfo from = bufferState(b->state);
    const ImageStateInfo to = bufferState(state);
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = from.access;
    bb.dstAccessMask = to.access;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = b->handle();
    bb.offset = 0;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(m_cmd, from.stages, to.stages, 0, 0, nullptr, 1, &bb, 0, nullptr);
    b->state = state;
}

void VulkanCommandList::uavBarrier() {
    requireRecording("uavBarrier");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

// --- render passes ---------------------------------------------------------------------------------------
void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc) {
    requireRecording("beginRenderPass");
    if (m_inRenderPass) throw Error("beginRenderPass: already inside a render pass");
    std::vector<VkFormat> colorFormats;
    std::vector<Format> colorFmts;
    std::vector<LoadOp> loads;
    std::vector<VkImageView> views;
    std::vector<VulkanTexture*> texs;
    std::vector<VkClearValue> clears;
    std::uint32_t width = 0, height = 0, samples = 1;
    m_pendingResolves.clear();
    for (const ColorAttachment& c : desc.colors) {
        auto* t = static_cast<VulkanTexture*>(c.texture);
        transition(t, ResourceState::RenderTarget);
        colorFormats.push_back(toVk(t->desc().format));
        colorFmts.push_back(t->desc().format);
        loads.push_back(c.load);
        views.push_back(t->view(Subresource{c.mip, 1, c.layer, 1}, true));
        texs.push_back(t);
        VkClearValue cv{};
        std::memcpy(cv.color.float32, c.clear.data(), sizeof(float) * 4);
        clears.push_back(cv);
        width = std::max(1u, t->desc().width >> c.mip);
        height = std::max(1u, t->desc().height >> c.mip);
        samples = t->desc().sampleCount;
        if (c.resolveTarget) m_pendingResolves.emplace_back(static_cast<VulkanTexture*>(c.resolveTarget), t);
    }
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    Format depthFmt = Format::Unknown;
    if (desc.depth.texture) {
        auto* t = static_cast<VulkanTexture*>(desc.depth.texture);
        transition(t, desc.depth.readOnly ? ResourceState::DepthRead : ResourceState::DepthWrite);
        depthFormat = toVk(t->desc().format);
        depthFmt = t->desc().format;
        views.push_back(t->view(Subresource{0, 1, 0, 1}, true));
        texs.push_back(t);
        VkClearValue cv{};
        cv.depthStencil = {desc.depth.clearDepth, desc.depth.clearStencil};
        clears.push_back(cv);
        width = width ? width : t->desc().width;
        height = height ? height : t->desc().height;
        samples = t->desc().sampleCount;
    }
    VkRenderPass rp = m_dev.renderPass(colorFormats, depthFormat, toVkSamples(samples), loads, desc.depth.load,
                                       desc.depth.store, desc.depth.readOnly);
    VkRenderPass compat = m_dev.compatibleRenderPass(colorFmts, depthFmt, samples);
    if (desc.depth.readOnly) compat = rp; // read-only depth layouts change compatibility-relevant state
    VkFramebuffer fb = m_dev.framebuffer(compat, texs, views, width, height);

    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {width, height}};
    bi.clearValueCount = static_cast<std::uint32_t>(clears.size());
    bi.pClearValues = clears.data();
    vkCmdBeginRenderPass(m_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    m_inRenderPass = true;
    setViewport(0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    setScissor(0, 0, width, height);
}

void VulkanCommandList::endRenderPass() {
    requireRecording("endRenderPass");
    if (!m_inRenderPass) return;
    vkCmdEndRenderPass(m_cmd);
    m_inRenderPass = false;
    for (auto [dst, src] : m_pendingResolves) resolveTexture(dst, src);
    m_pendingResolves.clear();
}

void VulkanCommandList::setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) {
    // Negative height flips Y so clip space matches D3D (y up) on every backend.
    VkViewport vp{x, y + h, w, -h, minDepth, maxDepth};
    vkCmdSetViewport(m_cmd, 0, 1, &vp);
}

void VulkanCommandList::setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
    VkRect2D r{{x, y}, {w, h}};
    vkCmdSetScissor(m_cmd, 0, 1, &r);
}

// --- bindings --------------------------------------------------------------------------------------------
void VulkanCommandList::setPipeline(Pipeline* pipeline) {
    requireRecording("setPipeline");
    auto* p = static_cast<VulkanPipeline*>(pipeline);
    if (p == m_pipeline) return;
    vkCmdBindPipeline(m_cmd, p->bindPoint(), p->handle());
    m_pipeline = p;
    m_bindingsDirty = true;
    m_tableDirty = true;
}

void VulkanCommandList::setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) {
    VkBuffer b = static_cast<VulkanBuffer*>(buffer)->handle();
    VkDeviceSize o = offset;
    vkCmdBindVertexBuffers(m_cmd, binding, 1, &b, &o);
}

void VulkanCommandList::setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) {
    vkCmdBindIndexBuffer(m_cmd, static_cast<VulkanBuffer*>(buffer)->handle(), offset,
                         type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

VulkanCommandList::Bound& VulkanCommandList::slot(char cls, std::uint32_t index) {
    if (index >= kSlots) throw Error(std::format("binding slot {}{} out of range", cls, index));
    m_bindingsDirty = true;
    switch (cls) {
    case 'b': return m_b[index];
    case 't': return m_t[index];
    case 'u': return m_u[index];
    default: return m_s[index];
    }
}

void VulkanCommandList::setUniformBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('b', s);
    b.buffer = static_cast<VulkanBuffer*>(buffer)->handle();
    b.offset = offset;
    b.range = size ? size : VK_WHOLE_SIZE;
}

void VulkanCommandList::setUniformData(std::uint32_t s, const void* data, std::uint64_t size) {
    TransientAllocation a = allocTransient(size, m_dev.uniformAlignment());
    std::memcpy(a.cpu, data, size);
    setUniformBuffer(s, a.buffer, a.offset, size);
}

void VulkanCommandList::setTexture(std::uint32_t s, Texture* texture, Subresource sub) {
    auto* t = static_cast<VulkanTexture*>(texture);
    Bound& b = slot('t', s);
    b = Bound{};
    b.view = t->view(sub);
    b.layout = t->layout();
    if (b.layout == VK_IMAGE_LAYOUT_UNDEFINED) b.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanCommandList::setStorageTexture(std::uint32_t s, Texture* texture, std::uint32_t mip) {
    auto* t = static_cast<VulkanTexture*>(texture);
    Bound& b = slot('u', s);
    b = Bound{};
    b.view = t->view(Subresource{mip, 1, 0, 0});
    b.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void VulkanCommandList::setStorageBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('u', s);
    b = Bound{};
    b.buffer = static_cast<VulkanBuffer*>(buffer)->handle();
    b.offset = offset;
    b.range = size ? size : VK_WHOLE_SIZE;
}

void VulkanCommandList::setReadOnlyBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('t', s);
    b = Bound{};
    b.buffer = static_cast<VulkanBuffer*>(buffer)->handle();
    b.offset = offset;
    b.range = size ? size : VK_WHOLE_SIZE;
}

void VulkanCommandList::setSampler(std::uint32_t s, Sampler* sampler) {
    slot('s', s).sampler = static_cast<VulkanSampler*>(sampler)->handle();
}

void VulkanCommandList::pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) {
    if (!m_pipeline) throw Error("pushConstants: no pipeline bound");
    vkCmdPushConstants(m_cmd, m_pipeline->layoutHandle(), VK_SHADER_STAGE_ALL, offset, size, data);
}

void VulkanCommandList::flushBindings() {
    if (!m_pipeline) throw Error("draw/dispatch without a pipeline");
    if (m_accelDirty) accelBarrier(); // work after a build sees the finished structure
    if (m_tableDirty && m_pipeline->layoutDesc().textureTable) {
        if (!m_tableSet) throw Error(std::format("pipeline '{}': no texture table bound", m_pipeline->name()));
        vkCmdBindDescriptorSets(m_cmd, m_pipeline->bindPoint(), m_pipeline->layoutHandle(), 1, 1, &m_tableSet, 0, nullptr);
        m_tableDirty = false;
    }
    if (!m_bindingsDirty) return;
    const auto& layout = m_pipeline->layoutDesc();
    if (layout.bindings.empty()) {
        m_bindingsDirty = false;
        return;
    }
    const std::size_t n = layout.bindings.size();
    std::vector<VkWriteDescriptorSet> writes(n);
    std::vector<VkDescriptorBufferInfo> bufInfos(n);
    std::vector<VkDescriptorImageInfo> imgInfos(n);
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelInfos(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Binding& bd = layout.bindings[i];
        const Bound* b = nullptr;
        char cls = 's';
        switch (bd.type) {
        case BindingType::UniformBuffer: cls = 'b'; break;
        case BindingType::SampledTexture:
        case BindingType::StorageBufferRO:
        case BindingType::AccelerationStructure: cls = 't'; break;
        case BindingType::StorageBufferRW:
        case BindingType::StorageTexture: cls = 'u'; break;
        case BindingType::Sampler: cls = 's'; break;
        }
        b = cls == 'b' ? &m_b[bd.slot] : cls == 't' ? &m_t[bd.slot] : cls == 'u' ? &m_u[bd.slot] : &m_s[bd.slot];
        VkWriteDescriptorSet& w = writes[i];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = bindingIndex(bd.type, bd.slot);
        w.descriptorCount = 1;
        w.descriptorType = descriptorType(bd.type);
        switch (bd.type) {
        case BindingType::UniformBuffer:
        case BindingType::StorageBufferRO:
        case BindingType::StorageBufferRW:
            if (!b->buffer)
                throw Error(std::format("pipeline '{}': buffer slot {}{} not bound", m_pipeline->name(), cls, bd.slot));
            bufInfos[i] = {b->buffer, b->offset, b->range};
            w.pBufferInfo = &bufInfos[i];
            break;
        case BindingType::SampledTexture:
        case BindingType::StorageTexture:
            if (!b->view)
                throw Error(std::format("pipeline '{}': texture slot {}{} not bound", m_pipeline->name(), cls, bd.slot));
            imgInfos[i] = {VK_NULL_HANDLE, b->view, b->layout};
            w.pImageInfo = &imgInfos[i];
            break;
        case BindingType::Sampler:
            if (!b->sampler)
                throw Error(std::format("pipeline '{}': sampler slot s{} not bound", m_pipeline->name(), bd.slot));
            imgInfos[i] = {b->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w.pImageInfo = &imgInfos[i];
            break;
        case BindingType::AccelerationStructure:
            if (!b->accel)
                throw Error(std::format("pipeline '{}': acceleration structure slot t{} not bound", m_pipeline->name(), bd.slot));
            accelInfos[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            accelInfos[i].accelerationStructureCount = 1;
            accelInfos[i].pAccelerationStructures = &b->accel;
            w.pNext = &accelInfos[i];
            break;
        }
    }
    if (m_dev.pushDescriptors()) {
        vkCmdPushDescriptorSetKHR(m_cmd, m_pipeline->bindPoint(), m_pipeline->layoutHandle(), 0,
                                  static_cast<std::uint32_t>(writes.size()), writes.data());
    } else {
        VkDescriptorSet set = VK_NULL_HANDLE;
        for (;;) {
            if (m_descPoolIndex >= m_descPools.size()) {
                VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerPool * 4},
                                                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 4},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetsPerPool * 4},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerPool * 2},
                                                {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 2},
                                                {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSetsPerPool}};
                VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pci.maxSets = kSetsPerPool;
                // The acceleration-structure pool size is only valid when the extension is enabled.
                pci.poolSizeCount = m_dev.extensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ? 6 : 5;
                pci.pPoolSizes = sizes;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                check(vkCreateDescriptorPool(m_dev.vkDevice(), &pci, nullptr, &pool), "vkCreateDescriptorPool");
                m_descPools.push_back(pool);
            }
            VkDescriptorSetLayout sl = m_pipeline->setLayout();
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = m_descPools[m_descPoolIndex];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &sl;
            VkResult r = vkAllocateDescriptorSets(m_dev.vkDevice(), &ai, &set);
            if (r == VK_SUCCESS) break;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                throwResult(r, "vkAllocateDescriptorSets");
            ++m_descPoolIndex;
        }
        for (auto& w : writes) w.dstSet = set;
        vkUpdateDescriptorSets(m_dev.vkDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(m_cmd, m_pipeline->bindPoint(), m_pipeline->layoutHandle(), 0, 1, &set, 0, nullptr);
    }
    m_bindingsDirty = false;
}

// --- work --------------------------------------------------------------------------------------------------
void VulkanCommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                             std::uint32_t firstInstance) {
    flushBindings();
    vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                    std::int32_t vertexOffset, std::uint32_t firstInstance) {
    flushBindings();
    vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                     std::uint32_t stride) {
    flushBindings();
    vkCmdDrawIndirect(m_cmd, static_cast<VulkanBuffer*>(args)->handle(), offset, drawCount, stride);
}

void VulkanCommandList::drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                            std::uint32_t stride) {
    flushBindings();
    vkCmdDrawIndexedIndirect(m_cmd, static_cast<VulkanBuffer*>(args)->handle(), offset, drawCount, stride);
}

void VulkanCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    flushBindings();
    vkCmdDispatch(m_cmd, x, y, z);
}

void VulkanCommandList::drawMeshTasks(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!m_dev.features().meshShader) throw Error("drawMeshTasks: mesh shaders are not supported by this GPU");
    flushBindings();
    vkCmdDrawMeshTasksEXT(m_cmd, x, y, z);
}

void VulkanCommandList::setShadingRate(std::uint32_t width, std::uint32_t height) {
    requireRecording("setShadingRate");
    if (!m_dev.features().variableRateShading) throw Error("setShadingRate: variable-rate shading is not supported");
    const VkExtent2D size{width, height};
    const VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                                                       VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
    vkCmdSetFragmentShadingRateKHR(m_cmd, &size, ops);
}

void VulkanCommandList::dispatchIndirect(Buffer* args, std::uint64_t offset) {
    flushBindings();
    vkCmdDispatchIndirect(m_cmd, static_cast<VulkanBuffer*>(args)->handle(), offset);
}

// --- transfers ---------------------------------------------------------------------------------------------
void VulkanCommandList::copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                                   std::uint64_t size) {
    requireRecording("copyBuffer");
    VkBufferCopy r{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(m_cmd, static_cast<VulkanBuffer*>(src)->handle(), static_cast<VulkanBuffer*>(dst)->handle(), 1,
                    &r);
}

void VulkanCommandList::copyTexture(Texture* dst, Texture* src) {
    requireRecording("copyTexture");
    auto* d = static_cast<VulkanTexture*>(dst);
    auto* s = static_cast<VulkanTexture*>(src);
    transition(s, ResourceState::CopySrc);
    transition(d, ResourceState::CopyDst);
    VkImageCopy c{};
    c.srcSubresource = {s->aspect(), 0, 0, 1};
    c.dstSubresource = {d->aspect(), 0, 0, 1};
    c.extent = {s->desc().width, s->desc().height, 1};
    vkCmdCopyImage(m_cmd, s->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VulkanCommandList::copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch,
                                            Texture* src, std::uint32_t mip, std::uint32_t layer) {
    requireRecording("copyTextureToBuffer");
    auto* s = static_cast<VulkanTexture*>(src);
    transition(s, ResourceState::CopySrc);
    transition(dst, ResourceState::CopyDst);
    const FormatInfo& fi = formatInfo(s->desc().format);
    VkBufferImageCopy c{};
    c.bufferOffset = dstOffset;
    c.bufferRowLength = rowPitch / fi.bytesPerBlock * fi.blockDim;
    c.imageSubresource = {s->aspect() & ~VK_IMAGE_ASPECT_STENCIL_BIT, mip, layer, 1};
    c.imageExtent = {std::max(1u, s->desc().width >> mip), std::max(1u, s->desc().height >> mip), 1};
    vkCmdCopyImageToBuffer(m_cmd, s->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(dst)->handle(), 1, &c);
}

void VulkanCommandList::copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                                            std::uint64_t srcOffset, std::uint32_t rowPitch) {
    requireRecording("copyBufferToTexture");
    auto* d = static_cast<VulkanTexture*>(dst);
    transition(d, ResourceState::CopyDst);
    transition(src, ResourceState::CopySrc);
    const FormatInfo& fi = formatInfo(d->desc().format);
    VkBufferImageCopy c{};
    c.bufferOffset = srcOffset;
    c.bufferRowLength = rowPitch / fi.bytesPerBlock * fi.blockDim;
    c.imageSubresource = {d->aspect() & ~VK_IMAGE_ASPECT_STENCIL_BIT, mip, layer, 1};
    c.imageExtent = {std::max(1u, d->desc().width >> mip), std::max(1u, d->desc().height >> mip), 1};
    vkCmdCopyBufferToImage(m_cmd, static_cast<VulkanBuffer*>(src)->handle(), d->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VulkanCommandList::fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) {
    requireRecording("fillBuffer");
    transition(buffer, ResourceState::CopyDst);
    vkCmdFillBuffer(m_cmd, static_cast<VulkanBuffer*>(buffer)->handle(), offset, size ? size : VK_WHOLE_SIZE, value);
}

void VulkanCommandList::resolveTexture(Texture* dst, Texture* src) {
    requireRecording("resolveTexture");
    auto* d = static_cast<VulkanTexture*>(dst);
    auto* s = static_cast<VulkanTexture*>(src);
    transition(s, ResourceState::CopySrc);
    transition(d, ResourceState::CopyDst);
    VkImageResolve r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {s->desc().width, s->desc().height, 1};
    vkCmdResolveImage(m_cmd, s->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->handle(),
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

// --- queries ------------------------------------------------------------------------------------------------
void VulkanCommandList::resetQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) {
    vkCmdResetQueryPool(m_cmd, static_cast<VulkanQueryPool*>(pool)->handle(), first, count);
}
void VulkanCommandList::writeTimestamp(QueryPool* pool, std::uint32_t index) {
    vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, static_cast<VulkanQueryPool*>(pool)->handle(),
                        index);
}
void VulkanCommandList::beginQuery(QueryPool* pool, std::uint32_t index) {
    vkCmdBeginQuery(m_cmd, static_cast<VulkanQueryPool*>(pool)->handle(), index, 0);
}
void VulkanCommandList::endQuery(QueryPool* pool, std::uint32_t index) {
    vkCmdEndQuery(m_cmd, static_cast<VulkanQueryPool*>(pool)->handle(), index);
}

void VulkanCommandList::beginMarker(const char* name) {
    if (!m_dev.debugUtils() || !vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(m_cmd, &l);
}
void VulkanCommandList::endMarker() {
    if (!m_dev.debugUtils() || !vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(m_cmd);
}

TransientAllocation VulkanCommandList::allocTransient(std::uint64_t size, std::uint64_t alignment) {
    alignment = std::max<std::uint64_t>({alignment, m_dev.uniformAlignment(), m_dev.storageAlignment(), 16});
    while (m_chunkIndex < m_chunks.size()) {
        Chunk& c = m_chunks[m_chunkIndex];
        const std::uint64_t off = alignUp(c.used, alignment);
        if (off + size <= c.buffer->desc().size) {
            c.used = off + size;
            return {static_cast<std::uint8_t*>(c.buffer->mapped()) + off, c.buffer.get(), off, size};
        }
        ++m_chunkIndex;
    }
    BufferDesc bd{std::max(kChunkSize, alignUp(size, alignment)),
                  BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index |
                      BufferUsage::CopySrc,
                  MemoryType::Upload, 0, "transient"};
    Chunk c;
    c.buffer = std::make_unique<VulkanBuffer>(m_dev, bd);
    c.used = size;
    m_chunks.push_back(std::move(c));
    m_chunkIndex = m_chunks.size() - 1;
    return {m_chunks.back().buffer->mapped(), m_chunks.back().buffer.get(), 0, size};
}

} // namespace gt::rhi::vk
