#include "gl_device.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

namespace gt::rhi::gl {
namespace {

constexpr std::uint64_t kChunkSize = 4ull * 1024 * 1024;
constexpr std::uint32_t kMaxAttachments = 8;

std::uint32_t bit(std::uint32_t slot) {
    return 1u << slot;
}

void checkSlot(std::uint32_t slot, const char* what) {
    if (slot >= Executor::kSlots) throw Error(std::format("{}: slot {} out of range", what, slot));
}

GLenum indexTypeOf(IndexType t) {
    return t == IndexType::Uint16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}
std::uint32_t indexSize(GLenum t) {
    return t == GL_UNSIGNED_SHORT ? 2u : 4u;
}

/// Binds `src` (mip/layer) for reading and `dst` for drawing, and blits colour.
void blitColor(GLDevice& dev, GLTexture* src, std::uint32_t srcMip, std::uint32_t srcLayer, GLTexture* dst,
               std::uint32_t dstMip, std::uint32_t dstLayer) {
    const GLuint readFbo = dev.readFramebuffer(src, srcMip, srcLayer);
    const GLuint drawFbo = dev.readFramebuffer(dst, dstMip, dstLayer);
    gl().glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    gl().glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
    gl().glReadBuffer(GL_COLOR_ATTACHMENT0);
    const auto sw = static_cast<GLint>(std::max(1u, src->desc().width >> srcMip));
    const auto sh = static_cast<GLint>(std::max(1u, src->desc().height >> srcMip));
    const auto dw = static_cast<GLint>(std::max(1u, dst->desc().width >> dstMip));
    const auto dh = static_cast<GLint>(std::max(1u, dst->desc().height >> dstMip));
    gl().glDisable(GL_SCISSOR_TEST);
    gl().glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

} // namespace

GLCommandList::GLCommandList(GLDevice& dev, QueueType queue) : m_dev(dev) {
    m_queue = queue;
}

GLCommandList::~GLCommandList() = default;

void GLCommandList::requireRecording(const char* what) const {
    if (!m_recording) throw Error(std::format("{}: command list is not recording", what));
}

void GLCommandList::begin() {
    if (m_recording) throw Error("begin: command list is already recording");
    if (m_lastSubmit) m_dev.wait(m_queue, m_lastSubmit);
    m_cmds.reset();
    for (Chunk& c : m_chunks)
        c.used = 0;
    m_chunk = 0;
    m_pendingResolves.clear();
    m_pipe = nullptr;
    m_uboMask = m_srvTexMask = m_srvBufMask = m_uavTexMask = m_uavBufMask = m_samplerMask = 0;
    m_push = {};
    m_pushDirty = false;
    m_uavPending = false;
    m_inRenderPass = false;
    m_recording = true;
}

void GLCommandList::end() {
    requireRecording("end");
    if (m_inRenderPass) endRenderPass();
    barrierIfNeeded();
    m_recording = false;
}

// --- barriers ----------------------------------------------------------------------------------------
void GLCommandList::barrierIfNeeded() {
    if (!m_uavPending) return;
    m_uavPending = false;
    m_cmds.push([](Executor&) { gl().glMemoryBarrier(GL_ALL_BARRIER_BITS); });
}

void GLCommandList::transition(Texture*, ResourceState) {
    requireRecording("transition");
    barrierIfNeeded();
}

void GLCommandList::transition(Buffer*, ResourceState state) {
    requireRecording("transition");
    barrierIfNeeded();
    if (state == ResourceState::HostRead)
        m_cmds.push([](Executor&) { gl().glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT); });
}

void GLCommandList::uavBarrier() {
    requireRecording("uavBarrier");
    m_uavPending = true; // be conservative: writes may also come from the previous list
    barrierIfNeeded();
}

// --- render passes ---------------------------------------------------------------------------------------
void GLCommandList::beginRenderPass(const RenderPassDesc& desc) {
    requireRecording("beginRenderPass");
    if (m_inRenderPass) throw Error("beginRenderPass: already inside a render pass");
    if (desc.shadingRateImage)
        throw Error("beginRenderPass: shading-rate images are not available on OpenGL");
    if (desc.colors.size() > kMaxAttachments) throw Error("beginRenderPass: too many colour attachments");
    barrierIfNeeded();
    struct Color {
        GLTexture* texture;
        std::uint32_t mip, layer;
        bool clear;
        std::array<float, 4> value;
    };
    struct Pass {
        std::array<Color, kMaxAttachments> colors;
        std::uint32_t count = 0;
        GLTexture* depth = nullptr;
        bool clearDepth = false, readOnly = false;
        float depthValue = 1.0f;
        std::uint8_t stencilValue = 0;
        std::uint32_t width = 0, height = 0;
    } pass;
    for (const ColorAttachment& c : desc.colors) {
        auto* t = static_cast<GLTexture*>(c.texture);
        pass.colors[pass.count++] = {t, c.mip, c.layer, c.load == LoadOp::Clear, c.clear};
        pass.width = std::max(1u, t->desc().width >> c.mip);
        pass.height = std::max(1u, t->desc().height >> c.mip);
        if (c.resolveTarget) m_pendingResolves.emplace_back(static_cast<GLTexture*>(c.resolveTarget), t);
    }
    if (desc.depth.texture) {
        pass.depth = static_cast<GLTexture*>(desc.depth.texture);
        pass.clearDepth = desc.depth.load == LoadOp::Clear && !desc.depth.readOnly;
        pass.readOnly = desc.depth.readOnly;
        pass.depthValue = desc.depth.clearDepth;
        pass.stencilValue = desc.depth.clearStencil;
        if (!pass.width) pass.width = pass.depth->desc().width, pass.height = pass.depth->desc().height;
    }
    m_cmds.push([pass](Executor& e) {
        std::vector<std::tuple<GLTexture*, std::uint32_t, std::uint32_t>> colors;
        for (std::uint32_t i = 0; i < pass.count; ++i)
            colors.emplace_back(pass.colors[i].texture, pass.colors[i].mip, pass.colors[i].layer);
        e.dev.framebuffer(colors, pass.depth);
        gl().glDisable(GL_SCISSOR_TEST);
        for (std::uint32_t i = 0; i < pass.count; ++i) {
            const Color& c = pass.colors[i];
            if (!c.clear) continue;
            const auto idx = static_cast<GLint>(i);
            gl().glColorMaski(static_cast<GLuint>(i), GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            const FormatInfo& fi = formatInfo(c.texture->desc().format);
            if (fi.isInteger) {
                const GLuint v[4] = {static_cast<GLuint>(c.value[0]), static_cast<GLuint>(c.value[1]),
                                     static_cast<GLuint>(c.value[2]), static_cast<GLuint>(c.value[3])};
                gl().glClearBufferuiv(GL_COLOR, idx, v);
            } else {
                gl().glClearBufferfv(GL_COLOR, idx, c.value.data());
            }
        }
        if (pass.depth && pass.clearDepth) {
            gl().glDepthMask(GL_TRUE);
            if (formatInfo(pass.depth->desc().format).stencil)
                gl().glClearBufferfi(GL_DEPTH_STENCIL, 0, pass.depthValue, pass.stencilValue);
            else
                gl().glClearBufferfv(GL_DEPTH, 0, &pass.depthValue);
        }
        e.depthReadOnly = pass.readOnly;
        e.pipelineDirty = true; // the clears changed write masks
        e.passHeight = pass.height;
        gl().glViewport(0, 0, static_cast<GLsizei>(pass.width), static_cast<GLsizei>(pass.height));
        gl().glDepthRange(0.0, 1.0);
        gl().glEnable(GL_SCISSOR_TEST);
        gl().glScissor(0, 0, static_cast<GLsizei>(pass.width), static_cast<GLsizei>(pass.height));
    });
    m_inRenderPass = true;
}

void GLCommandList::endRenderPass() {
    requireRecording("endRenderPass");
    if (!m_inRenderPass) return;
    m_inRenderPass = false;
    for (auto [dst, src] : m_pendingResolves)
        resolveTexture(dst, src);
    m_pendingResolves.clear();
    m_cmds.push([](Executor& e) { e.depthReadOnly = false; });
}

void GLCommandList::setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) {
    requireRecording("setViewport");
    m_cmds.push([=](Executor&) {
        gl().glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(w),
                        static_cast<GLsizei>(h));
        gl().glDepthRange(minDepth, maxDepth);
    });
}

void GLCommandList::setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
    requireRecording("setScissor");
    m_cmds.push([=](Executor&) { gl().glScissor(x, y, static_cast<GLsizei>(w), static_cast<GLsizei>(h)); });
}

// --- bindings ----------------------------------------------------------------------------------------------
void GLCommandList::setPipeline(Pipeline* pipeline) {
    requireRecording("setPipeline");
    auto* p = static_cast<GLPipeline*>(pipeline);
    if (p == m_pipe) return;
    m_pipe = p;
    m_cmds.push([p](Executor& e) {
        e.pipeline = p;
        e.pipelineDirty = true;
    });
}

void GLCommandList::setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) {
    requireRecording("setVertexBuffer");
    if (binding >= 16) throw Error("setVertexBuffer: binding out of range");
    auto* b = static_cast<GLBuffer*>(buffer);
    m_cmds.push([=](Executor& e) {
        e.vertex[binding] = {b, offset};
        e.vertexDirty = true;
    });
}

void GLCommandList::setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) {
    requireRecording("setIndexBuffer");
    auto* b = static_cast<GLBuffer*>(buffer);
    const GLenum t = indexTypeOf(type);
    m_cmds.push([=](Executor& e) {
        e.indexBuffer = b;
        e.indexType = t;
        e.indexOffset = offset;
        gl().glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->name());
    });
}

void GLCommandList::setUniformBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                     std::uint64_t size) {
    requireRecording("setUniformBuffer");
    checkSlot(slot, "setUniformBuffer");
    auto* b = static_cast<GLBuffer*>(buffer);
    m_uboMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.ubo[slot] = {b, offset, size}; });
}

void GLCommandList::setUniformData(std::uint32_t slot, const void* data, std::uint64_t size) {
    requireRecording("setUniformData");
    const std::uint64_t padded = (size + 15) / 16 * 16;
    const TransientAllocation t =
        allocTransient(padded, static_cast<std::uint64_t>(m_dev.uniformAlignment()));
    std::memcpy(t.cpu, data, size);
    setUniformBuffer(slot, t.buffer, t.offset, padded);
}

void GLCommandList::setTexture(std::uint32_t slot, Texture* texture, Subresource sub) {
    requireRecording("setTexture");
    checkSlot(slot, "setTexture");
    auto* t = static_cast<GLTexture*>(texture);
    m_srvTexMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.srvTex[slot] = {t, sub, 0}; });
}

void GLCommandList::setStorageTexture(std::uint32_t slot, Texture* texture, std::uint32_t mip) {
    requireRecording("setStorageTexture");
    checkSlot(slot, "setStorageTexture");
    auto* t = static_cast<GLTexture*>(texture);
    m_uavTexMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.uavTex[slot] = {t, {}, mip}; });
}

void GLCommandList::setStorageBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                     std::uint64_t size) {
    requireRecording("setStorageBuffer");
    checkSlot(slot, "setStorageBuffer");
    auto* b = static_cast<GLBuffer*>(buffer);
    m_uavBufMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.uavBuf[slot] = {b, offset, size}; });
}

void GLCommandList::setReadOnlyBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                      std::uint64_t size) {
    requireRecording("setReadOnlyBuffer");
    checkSlot(slot, "setReadOnlyBuffer");
    auto* b = static_cast<GLBuffer*>(buffer);
    m_srvBufMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.srvBuf[slot] = {b, offset, size}; });
}

void GLCommandList::setSampler(std::uint32_t slot, Sampler* sampler) {
    requireRecording("setSampler");
    checkSlot(slot, "setSampler");
    auto* s = static_cast<GLSampler*>(sampler);
    m_samplerMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.samplers[slot] = s; });
}

void GLCommandList::pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) {
    requireRecording("pushConstants");
    if (offset + size > sizeof(m_push)) throw Error("pushConstants: more than 128 bytes");
    std::memcpy(reinterpret_cast<std::byte*>(m_push.data()) + offset, data, size);
    m_pushDirty = true;
}

void GLCommandList::beforeWork(const char* what) {
    if (!m_pipe) throw Error(std::format("{}: no pipeline bound", what));
    const ProgramBindings& pb = m_pipe->bindings();
    auto need = [&](bool ok, char cls, std::uint32_t slot) {
        if (!ok) throw Error(std::format("{}: slot {}{} used by the pipeline is not bound", what, cls, slot));
    };
    for (std::size_t k = 0; k < pb.ubos.size(); ++k)
        if (static_cast<int>(k) != pb.pushBinding) need(m_uboMask & bit(pb.ubos[k]), 'b', pb.ubos[k]);
    for (auto [cls, slot] : pb.ssbos)
        need((cls == 't' ? m_srvBufMask : m_uavBufMask) & bit(slot), cls, slot);
    for (std::uint32_t slot : pb.images)
        need(m_uavTexMask & bit(slot), 'u', slot);
    for (const ProgramBindings::Tex& t : pb.textures) {
        need(m_srvTexMask & bit(t.t), 't', t.t);
        if (t.s >= 0)
            need(m_samplerMask & bit(static_cast<std::uint32_t>(t.s)), 's', static_cast<std::uint32_t>(t.s));
    }
    if (pb.pushBinding >= 0 && m_pushDirty) {
        constexpr std::uint64_t kPushBytes = sizeof(m_push);
        const TransientAllocation t =
            allocTransient(kPushBytes, static_cast<std::uint64_t>(m_dev.uniformAlignment()));
        std::memcpy(t.cpu, m_push.data(), kPushBytes);
        auto* b = static_cast<GLBuffer*>(t.buffer);
        const std::uint64_t off = t.offset;
        m_cmds.push([b, off](Executor& e) { e.push = {b, off, kPushBytes}; });
        m_pushDirty = false;
    }
    if (!pb.ssbos.empty() || !pb.images.empty()) m_uavPending = true;
}

// --- work
// ------------------------------------------------------------------------------------------------------
void GLCommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                         std::uint32_t firstInstance) {
    requireRecording("draw");
    beforeWork("draw");
    m_cmds.push([=](Executor& e) {
        e.prepareDraw(static_cast<std::int32_t>(firstVertex), firstInstance);
        const GLenum mode = e.pipeline->primitive();
        if (firstInstance && e.dev.hasBaseInstance())
            gl().glDrawArraysInstancedBaseInstance(mode, static_cast<GLint>(firstVertex),
                                                   static_cast<GLsizei>(vertexCount),
                                                   static_cast<GLsizei>(instanceCount), firstInstance);
        else
            gl().glDrawArraysInstanced(mode, static_cast<GLint>(firstVertex),
                                       static_cast<GLsizei>(vertexCount),
                                       static_cast<GLsizei>(instanceCount));
    });
}

void GLCommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount,
                                std::uint32_t firstIndex, std::int32_t vertexOffset,
                                std::uint32_t firstInstance) {
    requireRecording("drawIndexed");
    beforeWork("drawIndexed");
    m_cmds.push([=](Executor& e) {
        if (!e.indexBuffer) throw Error("drawIndexed: no index buffer bound");
        e.prepareDraw(vertexOffset, firstInstance);
        const GLenum mode = e.pipeline->primitive();
        const auto* ptr = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(e.indexOffset + std::uint64_t(firstIndex) * indexSize(e.indexType)));
        if (firstInstance && e.dev.hasBaseInstance())
            gl().glDrawElementsInstancedBaseVertexBaseInstance(
                mode, static_cast<GLsizei>(indexCount), e.indexType, ptr, static_cast<GLsizei>(instanceCount),
                vertexOffset, firstInstance);
        else
            gl().glDrawElementsInstancedBaseVertex(mode, static_cast<GLsizei>(indexCount), e.indexType, ptr,
                                                   static_cast<GLsizei>(instanceCount), vertexOffset);
    });
}

void GLCommandList::drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                 std::uint32_t stride) {
    requireRecording("drawIndirect");
    beforeWork("drawIndirect");
    auto* b = static_cast<GLBuffer*>(args);
    m_cmds.push([=](Executor& e) {
        e.prepareDraw(0, 0);
        gl().glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b->name());
        const GLenum mode = e.pipeline->primitive();
        if (drawCount > 1 && gl().glMultiDrawArraysIndirect)
            gl().glMultiDrawArraysIndirect(mode,
                                           reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset)),
                                           static_cast<GLsizei>(drawCount), static_cast<GLsizei>(stride));
        else
            for (std::uint32_t i = 0; i < drawCount; ++i)
                gl().glDrawArraysIndirect(mode, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                                                    offset + std::uint64_t(i) * stride)));
    });
}

void GLCommandList::drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                        std::uint32_t stride) {
    requireRecording("drawIndexedIndirect");
    beforeWork("drawIndexedIndirect");
    auto* b = static_cast<GLBuffer*>(args);
    m_cmds.push([=](Executor& e) {
        if (!e.indexBuffer) throw Error("drawIndexedIndirect: no index buffer bound");
        e.prepareDraw(0, 0);
        gl().glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b->name());
        const GLenum mode = e.pipeline->primitive();
        if (drawCount > 1 && gl().glMultiDrawElementsIndirect)
            gl().glMultiDrawElementsIndirect(
                mode, e.indexType, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset)),
                static_cast<GLsizei>(drawCount), static_cast<GLsizei>(stride));
        else
            for (std::uint32_t i = 0; i < drawCount; ++i)
                gl().glDrawElementsIndirect(mode, e.indexType,
                                            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                                                offset + std::uint64_t(i) * stride)));
    });
}

void GLCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    requireRecording("dispatch");
    beforeWork("dispatch");
    m_cmds.push([=](Executor& e) {
        e.prepareDraw(0, 0);
        gl().glDispatchCompute(x, y, z);
    });
    m_uavPending = true;
}

void GLCommandList::dispatchIndirect(Buffer* args, std::uint64_t offset) {
    requireRecording("dispatchIndirect");
    beforeWork("dispatchIndirect");
    auto* b = static_cast<GLBuffer*>(args);
    m_cmds.push([=](Executor& e) {
        e.prepareDraw(0, 0);
        gl().glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, b->name());
        gl().glDispatchComputeIndirect(static_cast<GLintptr>(offset));
    });
    m_uavPending = true;
}

// --- transfers
// ---------------------------------------------------------------------------------------------------
void GLCommandList::copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                               std::uint64_t size) {
    requireRecording("copyBuffer");
    barrierIfNeeded();
    auto* d = static_cast<GLBuffer*>(dst);
    auto* s = static_cast<GLBuffer*>(src);
    m_cmds.push([=](Executor&) {
        gl().glBindBuffer(GL_COPY_READ_BUFFER, s->name());
        gl().glBindBuffer(GL_COPY_WRITE_BUFFER, d->name());
        gl().glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(srcOffset),
                                 static_cast<GLintptr>(dstOffset), static_cast<GLsizeiptr>(size));
    });
}

void GLCommandList::copyTexture(Texture* dst, Texture* src) {
    requireRecording("copyTexture");
    barrierIfNeeded();
    auto* d = static_cast<GLTexture*>(dst);
    auto* s = static_cast<GLTexture*>(src);
    m_cmds.push([=](Executor& e) {
        const TextureDesc& sd = s->desc();
        const std::uint32_t mips = std::min(std::max(1u, sd.mipLevels), std::max(1u, d->desc().mipLevels));
        const std::uint32_t depth = sd.type == TextureType::Cube ? 6u : std::max(1u, sd.depthOrLayers);
        if (gl().glCopyImageSubData) {
            for (std::uint32_t m = 0; m < mips; ++m)
                gl().glCopyImageSubData(
                    s->name(), s->target(), static_cast<GLint>(m), 0, 0, 0, d->name(), d->target(),
                    static_cast<GLint>(m), 0, 0, 0, static_cast<GLsizei>(std::max(1u, sd.width >> m)),
                    static_cast<GLsizei>(std::max(1u, sd.height >> m)),
                    static_cast<GLsizei>(sd.type == TextureType::Tex3D ? std::max(1u, depth >> m) : depth));
        } else {
            for (std::uint32_t m = 0; m < mips; ++m)
                blitColor(e.dev, s, m, 0, d, m, 0);
        }
    });
}

void GLCommandList::copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch,
                                        Texture* src, std::uint32_t mip, std::uint32_t layer) {
    requireRecording("copyTextureToBuffer");
    barrierIfNeeded();
    auto* d = static_cast<GLBuffer*>(dst);
    auto* s = static_cast<GLTexture*>(src);
    m_cmds.push([=](Executor& e) {
        const TextureDesc& sd = s->desc();
        const FormatInfo& fi = formatInfo(sd.format);
        const auto w = static_cast<GLsizei>(std::max(1u, sd.width >> mip));
        const auto h = static_cast<GLsizei>(std::max(1u, sd.height >> mip));
        const GLuint fbo = e.dev.readFramebuffer(s, mip, layer);
        gl().glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        if (!fi.depth) gl().glReadBuffer(GL_COLOR_ATTACHMENT0);
        gl().glBindBuffer(GL_PIXEL_PACK_BUFFER, d->name());
        gl().glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl().glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(rowPitch / fi.bytesPerBlock));
        gl().glReadPixels(0, 0, w, h, s->format().format, s->format().type,
                          reinterpret_cast<void*>(static_cast<std::uintptr_t>(dstOffset)));
        gl().glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        gl().glPixelStorei(GL_PACK_ALIGNMENT, 4);
        gl().glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    });
}

void GLCommandList::copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                                        std::uint64_t srcOffset, std::uint32_t rowPitch) {
    requireRecording("copyBufferToTexture");
    barrierIfNeeded();
    auto* d = static_cast<GLTexture*>(dst);
    auto* s = static_cast<GLBuffer*>(src);
    m_cmds.push([=](Executor&) {
        const TextureDesc& dd = d->desc();
        const FormatInfo& fi = formatInfo(dd.format);
        const GlFormat& f = d->format();
        const auto w = static_cast<GLsizei>(std::max(1u, dd.width >> mip));
        const auto h = static_cast<GLsizei>(std::max(1u, dd.height >> mip));
        const auto* ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(srcOffset));
        const GLsizei rows = (h + static_cast<GLsizei>(fi.blockDim) - 1) / static_cast<GLsizei>(fi.blockDim);
        const auto bytes = static_cast<GLsizei>(rowPitch) * rows;
        gl().glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s->name());
        gl().glBindTexture(d->target(), d->name());
        gl().glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!f.compressed)
            gl().glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(rowPitch / fi.bytesPerBlock));
        if (dd.type == TextureType::Tex2DArray || dd.type == TextureType::Tex3D) {
            if (f.compressed)
                gl().glCompressedTexSubImage3D(d->target(), static_cast<GLint>(mip), 0, 0,
                                               static_cast<GLint>(layer), w, h, 1, f.internal, bytes, ptr);
            else
                gl().glTexSubImage3D(d->target(), static_cast<GLint>(mip), 0, 0, static_cast<GLint>(layer), w,
                                     h, 1, f.format, f.type, ptr);
        } else {
            const GLenum target =
                dd.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer : d->target();
            if (f.compressed)
                gl().glCompressedTexSubImage2D(target, static_cast<GLint>(mip), 0, 0, w, h, f.internal, bytes,
                                               ptr);
            else
                gl().glTexSubImage2D(target, static_cast<GLint>(mip), 0, 0, w, h, f.format, f.type, ptr);
        }
        gl().glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl().glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl().glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    });
}

void GLCommandList::fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size,
                               std::uint32_t value) {
    requireRecording("fillBuffer");
    barrierIfNeeded();
    auto* b = static_cast<GLBuffer*>(buffer);
    if (m_dev.hasClearBuffer()) {
        m_cmds.push([=](Executor&) {
            gl().glBindBuffer(GL_COPY_WRITE_BUFFER, b->name());
            gl().glClearBufferSubData(GL_COPY_WRITE_BUFFER, GL_R32UI, static_cast<GLintptr>(offset),
                                      static_cast<GLsizeiptr>(size), GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
        });
        return;
    }
    const TransientAllocation t = allocTransient(size, 4);
    auto* words = static_cast<std::uint32_t*>(t.cpu);
    std::fill(words, words + size / 4, value);
    copyBuffer(buffer, offset, t.buffer, t.offset, size);
}

void GLCommandList::resolveTexture(Texture* dst, Texture* src) {
    requireRecording("resolveTexture");
    auto* d = static_cast<GLTexture*>(dst);
    auto* s = static_cast<GLTexture*>(src);
    m_cmds.push([=](Executor& e) { blitColor(e.dev, s, 0, 0, d, 0, 0); });
}

// --- queries and markers
// ------------------------------------------------------------------------------------------
void GLCommandList::resetQueries(QueryPool*, std::uint32_t, std::uint32_t) {
    requireRecording("resetQueries");
}

void GLCommandList::writeTimestamp(QueryPool* pool, std::uint32_t index) {
    requireRecording("writeTimestamp");
    auto* p = static_cast<GLQueryPool*>(pool);
    const GLuint q = p->query(index);
    m_cmds.push([q](Executor&) { gl().glQueryCounter(q, GL_TIMESTAMP); });
}

void GLCommandList::beginQuery(QueryPool* pool, std::uint32_t index) {
    requireRecording("beginQuery");
    auto* p = static_cast<GLQueryPool*>(pool);
    const GLuint q = p->query(index);
    const GLenum target = p->target();
    m_cmds.push([=](Executor&) { gl().glBeginQuery(target, q); });
}

void GLCommandList::endQuery(QueryPool* pool, std::uint32_t /*index*/) {
    requireRecording("endQuery");
    const GLenum target = static_cast<GLQueryPool*>(pool)->target();
    m_cmds.push([=](Executor&) { gl().glEndQuery(target); });
}

void GLCommandList::resolveQueries(QueryPool*, std::uint32_t, std::uint32_t) {
    requireRecording("resolveQueries");
}

void GLCommandList::beginMarker(const char* name) {
    requireRecording("beginMarker");
    if (!gl().glPushDebugGroup) return;
    m_cmds.push([text = std::string(name)](Executor&) {
        gl().glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, text.c_str());
    });
}

void GLCommandList::endMarker() {
    requireRecording("endMarker");
    if (!gl().glPopDebugGroup) return;
    m_cmds.push([](Executor&) { gl().glPopDebugGroup(); });
}

TransientAllocation GLCommandList::allocTransient(std::uint64_t size, std::uint64_t alignment) {
    alignment = std::max<std::uint64_t>(alignment, 16);
    for (;; ++m_chunk) {
        if (m_chunk >= m_chunks.size()) {
            Chunk c;
            const std::uint64_t bytes = std::max(kChunkSize, size + alignment);
            c.buffer = std::make_unique<GLBuffer>(m_dev,
                                                  BufferDesc{bytes,
                                                             BufferUsage::Uniform | BufferUsage::Storage |
                                                                 BufferUsage::CopySrc | BufferUsage::Vertex |
                                                                 BufferUsage::Index,
                                                             MemoryType::Upload, 0, "transient"},
                                                  nullptr);
            m_chunks.push_back(std::move(c));
        }
        Chunk& c = m_chunks[m_chunk];
        const std::uint64_t off = (c.used + alignment - 1) / alignment * alignment;
        if (off + size <= c.buffer->desc().size) {
            c.used = off + size;
            return {static_cast<std::byte*>(c.buffer->mapped()) + off, c.buffer.get(), off, size};
        }
    }
}

} // namespace gt::rhi::gl
