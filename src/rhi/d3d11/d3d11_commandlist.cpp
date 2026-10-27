#include "d3d11_device.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace gt::rhi::d3d11 {
namespace {

constexpr std::uint64_t kChunkSize = 4ull * 1024 * 1024;
constexpr std::size_t kDataBlock = 64 * 1024;
constexpr std::uint32_t kMaxAttachments = 8;

std::uint32_t bit(std::uint32_t slot) { return 1u << slot; }
/// Number of slots up to and including the highest set bit.
UINT span(std::uint32_t mask) { return mask ? 32u - static_cast<UINT>(std::countl_zero(mask)) : 0u; }

void checkSlot(std::uint32_t slot, std::uint32_t limit, const char* what) {
    if (slot >= limit) throw Error(std::format("{}: slot {} out of range", what, slot));
}

template <typename F>
void forEachBit(std::uint32_t mask, F&& f) {
    while (mask) {
        f(static_cast<std::uint32_t>(std::countr_zero(mask)));
        mask &= mask - 1;
    }
}

// Per-stage binding entry points of the immediate context.
void setCbs(ID3D11DeviceContext1* c, StageIndex s, UINT n, ID3D11Buffer* const* b, const UINT* first, const UINT* num) {
    switch (s) {
    case kVS: first ? c->VSSetConstantBuffers1(0, n, b, first, num) : c->VSSetConstantBuffers(0, n, b); break;
    case kHS: first ? c->HSSetConstantBuffers1(0, n, b, first, num) : c->HSSetConstantBuffers(0, n, b); break;
    case kDS: first ? c->DSSetConstantBuffers1(0, n, b, first, num) : c->DSSetConstantBuffers(0, n, b); break;
    case kGS: first ? c->GSSetConstantBuffers1(0, n, b, first, num) : c->GSSetConstantBuffers(0, n, b); break;
    case kPS: first ? c->PSSetConstantBuffers1(0, n, b, first, num) : c->PSSetConstantBuffers(0, n, b); break;
    case kCS: first ? c->CSSetConstantBuffers1(0, n, b, first, num) : c->CSSetConstantBuffers(0, n, b); break;
    case kStages: break;
    }
}
void setSrvs(ID3D11DeviceContext1* c, StageIndex s, UINT n, ID3D11ShaderResourceView* const* v) {
    switch (s) {
    case kVS: c->VSSetShaderResources(0, n, v); break;
    case kHS: c->HSSetShaderResources(0, n, v); break;
    case kDS: c->DSSetShaderResources(0, n, v); break;
    case kGS: c->GSSetShaderResources(0, n, v); break;
    case kPS: c->PSSetShaderResources(0, n, v); break;
    case kCS: c->CSSetShaderResources(0, n, v); break;
    case kStages: break;
    }
}
void setSamplers(ID3D11DeviceContext1* c, StageIndex s, UINT n, ID3D11SamplerState* const* v) {
    switch (s) {
    case kVS: c->VSSetSamplers(0, n, v); break;
    case kHS: c->HSSetSamplers(0, n, v); break;
    case kDS: c->DSSetSamplers(0, n, v); break;
    case kGS: c->GSSetSamplers(0, n, v); break;
    case kPS: c->PSSetSamplers(0, n, v); break;
    case kCS: c->CSSetSamplers(0, n, v); break;
    case kStages: break;
    }
}

constexpr StageIndex kGraphicsStages[] = {kVS, kHS, kDS, kGS, kPS};

} // namespace

// --- executor ------------------------------------------------------------------------------------------------
void Executor::reset() {
    cb = {};
    srvBuf = {};
    uavBuf = {};
    srvTex = {};
    uavTex = {};
    samplers = {};
    vertex = {};
    indexBuffer = nullptr;
    pipeline = appliedGraphics = appliedCompute = nullptr;
    depthReadOnly = appliedDepthReadOnly = false;
    graphicsDirty = computeDirty = vertexDirty = indexDirty = true;
    renderTargets = 0;
    bufferEpoch = dev.bufferEpoch();
}

void Executor::syncEpoch() {
    if (bufferEpoch == dev.bufferEpoch()) return;
    bufferEpoch = dev.bufferEpoch();
    graphicsDirty = computeDirty = vertexDirty = indexDirty = true;
}

void Executor::clearSrvs(StageIndex s) {
    if (!boundSrvs[s]) return;
    std::array<ID3D11ShaderResourceView*, kSlots> none{};
    setSrvs(ctx, s, boundSrvs[s], none.data());
    boundSrvs[s] = 0;
}

void Executor::unbindForPass() {
    for (std::uint32_t s = 0; s < kStages; ++s) clearSrvs(static_cast<StageIndex>(s));
    if (boundComputeUavs) {
        std::array<ID3D11UnorderedAccessView*, kSlots> none{};
        ctx->CSSetUnorderedAccessViews(0, boundComputeUavs, none.data(), nullptr);
        boundComputeUavs = 0;
    }
    boundGraphicsUavs = 0; // OMSetRenderTargets replaces the output-merger UAVs
    graphicsDirty = computeDirty = true;
}

void Executor::uploadConstants(std::uint32_t slot, const void* data, std::uint64_t size) {
    if (size > 65536) throw Error("constant data larger than 64 KiB");
    std::uint32_t cls = 0;
    while ((256ull << (2 * cls)) < size) ++cls;
    const UINT bytes = 256u << (2 * cls);
    Com<ID3D11Buffer>& b = m_constants[slot][cls];
    if (!b) {
        D3D11_BUFFER_DESC d{bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        dev.check(dev.d3d()->CreateBuffer(&d, nullptr, b.put()), "CreateBuffer(dynamic constants)");
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    dev.check(ctx->Map(b.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(dynamic constants)");
    std::memcpy(m.pData, data, size);
    ctx->Unmap(b.get(), 0);
    BoundBuffer& bound = cb[slot];
    if (bound.dynamic != b.get() || bound.buffer) { // a rename by discard keeps the binding valid
        bound = {nullptr, 0, bytes, b.get()};
        bindingsChanged();
    }
}

ID3D11ShaderResourceView* Executor::srvFor(std::uint32_t slot, const SlotInfo& info) {
    using K = dxbc::ResourceKind;
    if (info.kind == K::Texture) {
        const BoundTexture& t = srvTex[slot];
        if (!t.texture) throw Error(std::format("register t{} (a texture) has no texture bound", slot));
        return t.texture->srv(t.sub);
    }
    const BoundBuffer& b = srvBuf[slot];
    if (!b.buffer) throw Error(std::format("register t{} (a buffer) has no buffer bound", slot));
    D3D11Buffer* buf = b.buffer;
    const std::uint32_t stride = info.kind == K::StructuredBuffer ? info.stride : 0;
    if (buf->isUpload() && stride && !buf->structurable() && buf->stride() != stride) {
        // CPU data that cannot take the shader's structured layout (transient memory, vertex data):
        // copy it into a dynamic structured buffer.
        DynamicStructured& d = m_dynamicSrvs[slot];
        const std::uint64_t size = b.size ? b.size : buf->desc().size - b.offset;
        const auto elements = static_cast<UINT>(std::max<std::uint64_t>(1, size / stride));
        if (!d.buffer || d.stride != stride || d.capacity < elements) {
            d.capacity = std::bit_ceil(std::max(elements, 64u));
            d.stride = stride;
            D3D11_BUFFER_DESC bd{d.capacity * stride, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                                 D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride};
            dev.check(dev.d3d()->CreateBuffer(&bd, nullptr, d.buffer.put()), "CreateBuffer(dynamic structured)");
            d.view.reset();
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        dev.check(ctx->Map(d.buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(dynamic structured)");
        std::memcpy(m.pData, buf->shadow() + b.offset, std::min<std::uint64_t>(size, std::uint64_t(elements) * stride));
        ctx->Unmap(d.buffer.get(), 0);
        if (!d.view || d.viewElements != elements) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_UNKNOWN;
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = 0;
            vd.Buffer.NumElements = elements;
            dev.check(dev.d3d()->CreateShaderResourceView(d.buffer.get(), &vd, d.view.put()),
                      "CreateShaderResourceView(dynamic structured)");
            d.viewElements = elements;
        }
        return d.view.get();
    }
    buf->gpu(stride);
    if (buf->isUpload()) buf->syncForSubmission(dev.replaying());
    return buf->srv(b.offset, b.size, stride);
}

ID3D11UnorderedAccessView* Executor::uavFor(std::uint32_t slot, const SlotInfo& info) {
    using K = dxbc::ResourceKind;
    if (info.kind == K::Texture) {
        const BoundTexture& t = uavTex[slot];
        if (!t.texture) throw Error(std::format("register u{} (a texture) has no texture bound", slot));
        return t.texture->uav(t.mip);
    }
    const BoundBuffer& b = uavBuf[slot];
    if (!b.buffer) throw Error(std::format("register u{} (a buffer) has no buffer bound", slot));
    if (b.buffer->isUpload() || b.buffer->isReadback())
        throw Error(std::format("buffer '{}': Upload and Readback memory cannot be written by shaders on Direct3D 11",
                                b.buffer->desc().name));
    return b.buffer->uav(b.offset, b.size, info.kind == K::StructuredBuffer ? info.stride : 0);
}

void Executor::bindStage(StageIndex s) {
    const StageSlots& st = pipeline->slots(s);
    // Constant buffers (b13 holds the push constants).
    std::array<ID3D11Buffer*, kCbSlots> cbs{};
    std::array<UINT, kCbSlots> first{}, num{};
    bool offsets = false;
    forEachBit(st.cb, [&](std::uint32_t slot) {
        const BoundBuffer& b = cb[slot];
        if (b.dynamic) {
            cbs[slot] = b.dynamic;
            num[slot] = static_cast<UINT>(b.size / 16);
        } else if (b.buffer) {
            if (b.buffer->isUpload()) b.buffer->syncForSubmission(dev.replaying());
            cbs[slot] = b.buffer->gpu();
            first[slot] = static_cast<UINT>(b.offset / 16);
            const std::uint64_t size = b.size ? b.size : b.buffer->desc().size - b.offset;
            num[slot] = static_cast<UINT>((size + 255) / 256 * 16);
            offsets |= b.offset != 0;
        } else {
            throw Error(std::format("register b{} has no constant buffer bound", slot));
        }
    });
    if (const UINT n = span(st.cb)) setCbs(ctx, s, n, cbs.data(), offsets ? first.data() : nullptr, num.data());

    std::array<ID3D11ShaderResourceView*, kSlots> srvs{};
    forEachBit(st.srv, [&](std::uint32_t slot) { srvs[slot] = srvFor(slot, pipeline->srvInfo(slot)); });
    const UINT nSrv = span(st.srv);
    if (const UINT n = std::max(nSrv, boundSrvs[s])) setSrvs(ctx, s, n, srvs.data()); // also clears stale slots
    boundSrvs[s] = nSrv;

    std::array<ID3D11SamplerState*, kSlots> smp{};
    forEachBit(st.sampler, [&](std::uint32_t slot) {
        if (!samplers[slot]) throw Error(std::format("register s{} has no sampler bound", slot));
        smp[slot] = samplers[slot]->state();
    });
    if (const UINT n = span(st.sampler)) setSamplers(ctx, s, n, smp.data());
}

void Executor::prepareDraw() {
    if (!pipeline || pipeline->isCompute()) throw Error("draw: the bound pipeline is not a graphics pipeline");
    syncEpoch();
    if (boundComputeUavs) { // compute output read by this draw must not stay bound for writing
        std::array<ID3D11UnorderedAccessView*, kSlots> none{};
        ctx->CSSetUnorderedAccessViews(0, boundComputeUavs, none.data(), nullptr);
        boundComputeUavs = 0;
        computeDirty = true;
    }
    if (appliedGraphics != pipeline || appliedDepthReadOnly != depthReadOnly) {
        pipeline->bindState(ctx, depthReadOnly);
        appliedGraphics = pipeline;
        appliedDepthReadOnly = depthReadOnly;
        graphicsDirty = vertexDirty = true;
    }
    if (graphicsDirty) {
        std::uint32_t uavMask = 0;
        for (StageIndex s : kGraphicsStages) {
            if (pipeline->hasStage(s)) {
                bindStage(s);
                uavMask |= pipeline->slots(s).uav;
            } else {
                clearSrvs(s);
            }
        }
        if (uavMask || boundGraphicsUavs) {
            // Pixel-stage UAVs share the output-merger slots with the render targets.
            const auto start = uavMask ? static_cast<UINT>(std::countr_zero(uavMask)) : renderTargets;
            if (start < renderTargets)
                throw Error(std::format("pipeline '{}': UAV register u{} overlaps render target {}; Direct3D 11 "
                                        "needs UAV registers above the render targets",
                                        pipeline->name(), start, start));
            std::array<ID3D11UnorderedAccessView*, kSlots> uavs{};
            forEachBit(uavMask, [&](std::uint32_t slot) { uavs[slot - start] = uavFor(slot, pipeline->uavInfo(slot)); });
            const UINT count = uavMask ? span(uavMask) - start : 0;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                           start, count, count ? uavs.data() : nullptr, nullptr);
            boundGraphicsUavs = count;
        }
        graphicsDirty = false;
    }
    if (vertexDirty) {
        std::array<ID3D11Buffer*, 16> buffers{};
        std::array<UINT, 16> strides{}, offsets{};
        const std::uint32_t mask = pipeline->vertexBindings();
        forEachBit(mask, [&](std::uint32_t binding) {
            const Vertex& v = vertex[binding];
            if (!v.buffer) throw Error(std::format("draw: vertex binding {} has no buffer", binding));
            if (v.buffer->isUpload()) v.buffer->syncForSubmission(dev.replaying());
            buffers[binding] = v.buffer->gpu();
            strides[binding] = pipeline->vertexStride(binding);
            offsets[binding] = static_cast<UINT>(v.offset);
        });
        if (const UINT n = span(mask)) ctx->IASetVertexBuffers(0, n, buffers.data(), strides.data(), offsets.data());
        vertexDirty = false;
    }
    if (indexDirty && indexBuffer) {
        if (indexBuffer->isUpload()) indexBuffer->syncForSubmission(dev.replaying());
        ctx->IASetIndexBuffer(indexBuffer->gpu(), indexFormat, static_cast<UINT>(indexOffset));
        indexDirty = false;
    }
}

void Executor::prepareDispatch() {
    if (!pipeline || !pipeline->isCompute()) throw Error("dispatch: the bound pipeline is not a compute pipeline");
    syncEpoch();
    if (appliedCompute != pipeline) {
        pipeline->bindState(ctx, false);
        appliedCompute = pipeline;
        computeDirty = true;
    }
    if (computeDirty) {
        // Resources this dispatch writes may still be bound as inputs: clear those first.
        for (StageIndex s : kGraphicsStages) clearSrvs(s);
        if (boundGraphicsUavs) {
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr,
                                                           nullptr, renderTargets, 0, nullptr, nullptr);
            boundGraphicsUavs = 0;
        }
        clearSrvs(kCS);
        const StageSlots& st = pipeline->slots(kCS);
        std::array<ID3D11UnorderedAccessView*, kSlots> uavs{};
        forEachBit(st.uav, [&](std::uint32_t slot) { uavs[slot] = uavFor(slot, pipeline->uavInfo(slot)); });
        const UINT nUav = span(st.uav);
        if (const UINT n = std::max(nUav, boundComputeUavs)) ctx->CSSetUnorderedAccessViews(0, n, uavs.data(), nullptr);
        boundComputeUavs = nUav;
        bindStage(kCS);
        computeDirty = false;
        graphicsDirty = vertexDirty = indexDirty = true; // the runtime unbinds inputs that alias UAVs
    }
}

// --- command list -------------------------------------------------------------------------------------------
D3D11CommandList::D3D11CommandList(D3D11Device& dev, QueueType queue) : m_dev(dev) { m_queue = queue; }

D3D11CommandList::~D3D11CommandList() = default;

void D3D11CommandList::requireRecording(const char* what) const {
    if (!m_recording) throw Error(std::format("{}: command list is not recording", what));
}

void D3D11CommandList::begin() {
    if (m_recording) throw Error("begin: command list is already recording");
    if (m_lastSubmit) m_dev.wait(m_queue, m_lastSubmit);
    m_cmds.reset();
    for (Chunk& c : m_chunks) c.used = 0;
    m_chunk = 0;
    m_dataBlock = 0;
    m_dataUsed = 0;
    m_pendingResolves.clear();
    m_pipe = nullptr;
    m_cbMask = m_srvTexMask = m_srvBufMask = m_uavTexMask = m_uavBufMask = m_samplerMask = 0;
    m_push = {};
    m_pushDirty = true;
    m_inRenderPass = false;
    m_recording = true;
}

void D3D11CommandList::end() {
    requireRecording("end");
    if (m_inRenderPass) endRenderPass();
    m_recording = false;
}

void D3D11CommandList::uploadTransient(std::uint64_t submission) {
    for (std::size_t i = 0; i < m_chunks.size() && i <= m_chunk; ++i)
        m_chunks[i].buffer->syncRange(submission, m_chunks[i].used);
}

const std::byte* D3D11CommandList::stash(const void* data, std::size_t size) {
    if (size > kDataBlock) throw Error("constant data larger than 64 KiB");
    const std::size_t padded = (size + 15) / 16 * 16;
    if (m_data.empty() || m_dataUsed + padded > kDataBlock) {
        if (!m_data.empty()) ++m_dataBlock;
        if (m_dataBlock >= m_data.size()) m_data.push_back(std::make_unique<std::byte[]>(kDataBlock));
        m_dataUsed = 0;
    }
    std::byte* p = m_data[m_dataBlock].get() + m_dataUsed;
    std::memcpy(p, data, size);
    m_dataUsed += padded;
    return p;
}

// --- render passes ----------------------------------------------------------------------------------------
void D3D11CommandList::beginRenderPass(const RenderPassDesc& desc) {
    requireRecording("beginRenderPass");
    if (m_inRenderPass) throw Error("beginRenderPass: already inside a render pass");
    if (desc.shadingRateImage) throw Error("beginRenderPass: shading-rate images are not available on Direct3D 11");
    if (desc.colors.size() > kMaxAttachments) throw Error("beginRenderPass: too many colour attachments");
    struct Color {
        D3D11Texture* texture;
        std::uint32_t mip, layer;
        bool clear;
        std::array<float, 4> value;
    };
    struct Pass {
        std::array<Color, kMaxAttachments> colors;
        std::uint32_t count = 0;
        D3D11Texture* depth = nullptr;
        bool clearDepth = false, readOnly = false;
        float depthValue = 1.0f;
        std::uint8_t stencilValue = 0;
        std::uint32_t width = 0, height = 0;
    } pass;
    for (const ColorAttachment& c : desc.colors) {
        auto* t = static_cast<D3D11Texture*>(c.texture);
        pass.colors[pass.count++] = {t, c.mip, c.layer, c.load == LoadOp::Clear, c.clear};
        pass.width = std::max(1u, t->desc().width >> c.mip);
        pass.height = std::max(1u, t->desc().height >> c.mip);
        if (c.resolveTarget) m_pendingResolves.emplace_back(static_cast<D3D11Texture*>(c.resolveTarget), t);
    }
    if (desc.depth.texture) {
        pass.depth = static_cast<D3D11Texture*>(desc.depth.texture);
        pass.clearDepth = desc.depth.load == LoadOp::Clear && !desc.depth.readOnly;
        pass.readOnly = desc.depth.readOnly;
        pass.depthValue = desc.depth.clearDepth;
        pass.stencilValue = desc.depth.clearStencil;
        if (!pass.width) pass.width = pass.depth->desc().width, pass.height = pass.depth->desc().height;
    }
    m_cmds.push([pass](Executor& e) {
        e.unbindForPass();
        std::array<ID3D11RenderTargetView*, kMaxAttachments> rtvs{};
        for (std::uint32_t i = 0; i < pass.count; ++i)
            rtvs[i] = pass.colors[i].texture->rtv(pass.colors[i].mip, pass.colors[i].layer);
        ID3D11DepthStencilView* dsv = pass.depth ? pass.depth->dsv(pass.readOnly) : nullptr;
        // Also unbinds every output-merger UAV.
        e.ctx->OMSetRenderTargetsAndUnorderedAccessViews(pass.count, rtvs.data(), dsv, pass.count, 0, nullptr, nullptr);
        for (std::uint32_t i = 0; i < pass.count; ++i)
            if (pass.colors[i].clear) e.ctx->ClearRenderTargetView(rtvs[i], pass.colors[i].value.data());
        if (pass.depth && pass.clearDepth) {
            UINT flags = D3D11_CLEAR_DEPTH;
            if (formatInfo(pass.depth->desc().format).stencil) flags |= D3D11_CLEAR_STENCIL;
            e.ctx->ClearDepthStencilView(dsv, flags, pass.depthValue, pass.stencilValue);
        }
        e.renderTargets = pass.count;
        e.depthReadOnly = pass.readOnly;
        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(pass.width), static_cast<float>(pass.height), 0.0f, 1.0f};
        e.ctx->RSSetViewports(1, &vp);
        const D3D11_RECT r{0, 0, static_cast<LONG>(pass.width), static_cast<LONG>(pass.height)};
        e.ctx->RSSetScissorRects(1, &r);
    });
    m_inRenderPass = true;
}

void D3D11CommandList::endRenderPass() {
    requireRecording("endRenderPass");
    if (!m_inRenderPass) return;
    m_inRenderPass = false;
    m_cmds.push([](Executor& e) {
        e.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        e.renderTargets = 0;
        e.boundGraphicsUavs = 0;
        e.depthReadOnly = false;
    });
    for (auto [dst, src] : m_pendingResolves) resolveTexture(dst, src);
    m_pendingResolves.clear();
}

void D3D11CommandList::setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) {
    requireRecording("setViewport");
    m_cmds.push([=](Executor& e) {
        const D3D11_VIEWPORT vp{x, y, w, h, minDepth, maxDepth};
        e.ctx->RSSetViewports(1, &vp);
    });
}

void D3D11CommandList::setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
    requireRecording("setScissor");
    m_cmds.push([=](Executor& e) {
        const D3D11_RECT r{x, y, static_cast<LONG>(x + static_cast<std::int32_t>(w)),
                           static_cast<LONG>(y + static_cast<std::int32_t>(h))};
        e.ctx->RSSetScissorRects(1, &r);
    });
}

// --- bindings -------------------------------------------------------------------------------------------------
void D3D11CommandList::setPipeline(Pipeline* pipeline) {
    requireRecording("setPipeline");
    auto* p = static_cast<D3D11Pipeline*>(pipeline);
    if (p == m_pipe) return;
    m_pipe = p;
    m_cmds.push([p](Executor& e) { e.pipeline = p; });
}

void D3D11CommandList::setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) {
    requireRecording("setVertexBuffer");
    checkSlot(binding, 16, "setVertexBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    m_cmds.push([=](Executor& e) {
        e.vertex[binding] = {b, offset};
        e.vertexDirty = true;
    });
}

void D3D11CommandList::setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) {
    requireRecording("setIndexBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    const DXGI_FORMAT f = type == IndexType::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    m_cmds.push([=](Executor& e) {
        e.indexBuffer = b;
        e.indexFormat = f;
        e.indexOffset = offset;
        e.indexDirty = true;
    });
}

void D3D11CommandList::setUniformBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                        std::uint64_t size) {
    requireRecording("setUniformBuffer");
    checkSlot(slot, kPushSlot, "setUniformBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    if (!any(b->desc().usage & BufferUsage::Uniform))
        throw Error(std::format("setUniformBuffer: buffer '{}' was not created with BufferUsage::Uniform", b->desc().name));
    if (offset && (!m_dev.constantBufferOffsets() || offset % 256))
        throw Error("setUniformBuffer: constant-buffer offsets need the Direct3D 11.1 offsetting feature and "
                    "256-byte alignment");
    m_cbMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.cb[slot] = {b, offset, size, nullptr};
        e.bindingsChanged();
    });
}

void D3D11CommandList::setUniformData(std::uint32_t slot, const void* data, std::uint64_t size) {
    requireRecording("setUniformData");
    checkSlot(slot, kPushSlot, "setUniformData");
    const std::byte* copy = stash(data, size);
    m_cbMask |= bit(slot);
    m_cmds.push([=](Executor& e) { e.uploadConstants(slot, copy, size); });
}

void D3D11CommandList::setTexture(std::uint32_t slot, Texture* texture, Subresource sub) {
    requireRecording("setTexture");
    checkSlot(slot, kSlots, "setTexture");
    auto* t = static_cast<D3D11Texture*>(texture);
    m_srvTexMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.srvTex[slot] = {t, sub, 0};
        e.bindingsChanged();
    });
}

void D3D11CommandList::setStorageTexture(std::uint32_t slot, Texture* texture, std::uint32_t mip) {
    requireRecording("setStorageTexture");
    checkSlot(slot, kSlots, "setStorageTexture");
    auto* t = static_cast<D3D11Texture*>(texture);
    m_uavTexMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.uavTex[slot] = {t, {}, mip};
        e.bindingsChanged();
    });
}

void D3D11CommandList::setStorageBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                        std::uint64_t size) {
    requireRecording("setStorageBuffer");
    checkSlot(slot, kSlots, "setStorageBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    m_uavBufMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.uavBuf[slot] = {b, offset, size, nullptr};
        e.bindingsChanged();
    });
}

void D3D11CommandList::setReadOnlyBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                                         std::uint64_t size) {
    requireRecording("setReadOnlyBuffer");
    checkSlot(slot, kSlots, "setReadOnlyBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    m_srvBufMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.srvBuf[slot] = {b, offset, size, nullptr};
        e.bindingsChanged();
    });
}

void D3D11CommandList::setSampler(std::uint32_t slot, Sampler* sampler) {
    requireRecording("setSampler");
    checkSlot(slot, kSlots, "setSampler");
    auto* s = static_cast<D3D11Sampler*>(sampler);
    m_samplerMask |= bit(slot);
    m_cmds.push([=](Executor& e) {
        e.samplers[slot] = s;
        e.bindingsChanged();
    });
}

void D3D11CommandList::pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) {
    requireRecording("pushConstants");
    if (offset + size > sizeof(m_push)) throw Error("pushConstants: more than 128 bytes");
    std::memcpy(reinterpret_cast<std::byte*>(m_push.data()) + offset, data, size);
    m_pushDirty = true;
}

void D3D11CommandList::beforeWork(const char* what) {
    requireRecording(what);
    if (!m_pipe) throw Error(std::format("{}: no pipeline bound", what));
    const StageSlots& u = m_pipe->used();
    auto need = [&](bool ok, char cls, std::uint32_t slot, const char* kind) {
        if (!ok)
            throw Error(std::format("{}: register {}{} ({}) used by pipeline '{}' is not bound", what, cls, slot, kind,
                                    m_pipe->name()));
    };
    forEachBit(u.cb & ~bit(kPushSlot), [&](std::uint32_t s) { need(m_cbMask & bit(s), 'b', s, "constant buffer"); });
    forEachBit(u.srv, [&](std::uint32_t s) {
        const bool tex = m_pipe->srvInfo(s).kind == dxbc::ResourceKind::Texture;
        need((tex ? m_srvTexMask : m_srvBufMask) & bit(s), 't', s, tex ? "texture" : "buffer");
    });
    forEachBit(u.uav, [&](std::uint32_t s) {
        const bool tex = m_pipe->uavInfo(s).kind == dxbc::ResourceKind::Texture;
        need((tex ? m_uavTexMask : m_uavBufMask) & bit(s), 'u', s, tex ? "storage texture" : "storage buffer");
    });
    forEachBit(u.sampler, [&](std::uint32_t s) { need(m_samplerMask & bit(s), 's', s, "sampler"); });
    if ((u.cb & bit(kPushSlot)) && m_pushDirty) {
        m_cmds.push([data = m_push](Executor& e) { e.uploadConstants(kPushSlot, data.data(), sizeof(data)); });
        m_pushDirty = false;
    }
}

// --- work ---------------------------------------------------------------------------------------------------------
void D3D11CommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                            std::uint32_t firstInstance) {
    beforeWork("draw");
    m_cmds.push([=](Executor& e) {
        e.prepareDraw();
        e.ctx->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    });
}

void D3D11CommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                   std::int32_t vertexOffset, std::uint32_t firstInstance) {
    beforeWork("drawIndexed");
    m_cmds.push([=](Executor& e) {
        if (!e.indexBuffer) throw Error("drawIndexed: no index buffer bound");
        e.prepareDraw();
        e.ctx->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    });
}

void D3D11CommandList::drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                    std::uint32_t stride) {
    beforeWork("drawIndirect");
    auto* b = static_cast<D3D11Buffer*>(args);
    m_cmds.push([=](Executor& e) {
        e.prepareDraw();
        if (b->isUpload()) b->syncForSubmission(e.dev.replaying());
        ID3D11Buffer* buffer = b->gpu();
        for (std::uint32_t i = 0; i < drawCount; ++i)
            e.ctx->DrawInstancedIndirect(buffer, static_cast<UINT>(offset + std::uint64_t(i) * stride));
    });
}

void D3D11CommandList::drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                           std::uint32_t stride) {
    beforeWork("drawIndexedIndirect");
    auto* b = static_cast<D3D11Buffer*>(args);
    m_cmds.push([=](Executor& e) {
        if (!e.indexBuffer) throw Error("drawIndexedIndirect: no index buffer bound");
        e.prepareDraw();
        if (b->isUpload()) b->syncForSubmission(e.dev.replaying());
        ID3D11Buffer* buffer = b->gpu();
        for (std::uint32_t i = 0; i < drawCount; ++i)
            e.ctx->DrawIndexedInstancedIndirect(buffer, static_cast<UINT>(offset + std::uint64_t(i) * stride));
    });
}

void D3D11CommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    beforeWork("dispatch");
    m_cmds.push([=](Executor& e) {
        e.prepareDispatch();
        e.ctx->Dispatch(x, y, z);
    });
}

void D3D11CommandList::dispatchIndirect(Buffer* args, std::uint64_t offset) {
    beforeWork("dispatchIndirect");
    auto* b = static_cast<D3D11Buffer*>(args);
    m_cmds.push([=](Executor& e) {
        e.prepareDispatch();
        if (b->isUpload()) b->syncForSubmission(e.dev.replaying());
        e.ctx->DispatchIndirect(b->gpu(), static_cast<UINT>(offset));
    });
}

// --- transfers ---------------------------------------------------------------------------------------------------
void D3D11CommandList::copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                                  std::uint64_t size) {
    requireRecording("copyBuffer");
    auto* d = static_cast<D3D11Buffer*>(dst);
    auto* s = static_cast<D3D11Buffer*>(src);
    if (s->isReadback()) throw Error("copyBuffer: Readback memory cannot be a copy source on Direct3D 11");
    if (d->isUpload()) throw Error("copyBuffer: Upload memory cannot be a copy destination on Direct3D 11");
    m_cmds.push([=](Executor& e) {
        if (s->isUpload()) s->syncForSubmission(e.dev.replaying());
        ID3D11Buffer* from = s->gpu();
        const D3D11_BOX box{static_cast<UINT>(srcOffset), 0, 0, static_cast<UINT>(srcOffset + size), 1, 1};
        if (d->isReadback()) {
            e.ctx->CopySubresourceRegion(d->staging(), 0, static_cast<UINT>(dstOffset), 0, 0, from, 0, &box);
            D3D11Device* dev = &e.dev;
            e.dev.afterReplay([dev, d, dstOffset, size] {
                D3D11_MAPPED_SUBRESOURCE m{};
                dev->check(dev->context()->Map(d->staging(), 0, D3D11_MAP_READ, 0, &m), "Map(readback buffer)");
                std::memcpy(d->shadow() + dstOffset, static_cast<const std::byte*>(m.pData) + dstOffset, size);
                dev->context()->Unmap(d->staging(), 0);
            });
        } else {
            e.ctx->CopySubresourceRegion(d->gpu(), 0, static_cast<UINT>(dstOffset), 0, 0, from, 0, &box);
        }
    });
}

void D3D11CommandList::copyTexture(Texture* dst, Texture* src) {
    requireRecording("copyTexture");
    auto* d = static_cast<D3D11Texture*>(dst);
    auto* s = static_cast<D3D11Texture*>(src);
    m_cmds.push([=](Executor& e) {
        const TextureDesc& sd = s->desc();
        const TextureDesc& dd = d->desc();
        if (sd.mipLevels == dd.mipLevels && s->layers() == d->layers() && sd.width == dd.width &&
            sd.height == dd.height && sd.depthOrLayers == dd.depthOrLayers) {
            e.ctx->CopyResource(d->resource(), s->resource());
            return;
        }
        const std::uint32_t mips = std::min(std::max(1u, sd.mipLevels), std::max(1u, dd.mipLevels));
        const std::uint32_t layers = std::min(s->layers(), d->layers());
        for (std::uint32_t l = 0; l < layers; ++l)
            for (std::uint32_t m = 0; m < mips; ++m)
                e.ctx->CopySubresourceRegion(d->resource(), d->subresource(m, l), 0, 0, 0, s->resource(),
                                             s->subresource(m, l), nullptr);
    });
}

void D3D11CommandList::copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                                           std::uint32_t mip, std::uint32_t layer) {
    requireRecording("copyTextureToBuffer");
    auto* d = static_cast<D3D11Buffer*>(dst);
    auto* s = static_cast<D3D11Texture*>(src);
    if (!d->isReadback())
        throw Error("copyTextureToBuffer: Direct3D 11 copies textures into Readback buffers only");
    m_cmds.push([=](Executor& e) {
        const TextureDesc& sd = s->desc();
        const FormatInfo& fi = formatInfo(sd.format);
        const std::uint32_t w = std::max(1u, sd.width >> mip), h = std::max(1u, sd.height >> mip);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = toDxgiResource(sd.format);
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        e.dev.check(e.dev.d3d()->CreateTexture2D(&td, nullptr, staging.put()), "CreateTexture2D(staging)");
        if (sd.type == TextureType::Tex3D) {
            const D3D11_BOX box{0, 0, layer, w, h, layer + 1};
            e.ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, s->resource(), s->subresource(mip, 0), &box);
        } else {
            e.ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, s->resource(), s->subresource(mip, layer), nullptr);
        }
        D3D11Device* dev = &e.dev;
        const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
        const std::uint32_t tight = ((w + fi.blockDim - 1) / fi.blockDim) * fi.bytesPerBlock;
        e.dev.afterReplay([dev, d, staging, dstOffset, rowPitch, rows, tight] {
            D3D11_MAPPED_SUBRESOURCE m{};
            dev->check(dev->context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m), "Map(staging texture)");
            for (std::uint32_t r = 0; r < rows; ++r)
                std::memcpy(d->shadow() + dstOffset + std::uint64_t(r) * rowPitch,
                            static_cast<const std::byte*>(m.pData) + std::size_t(r) * m.RowPitch, tight);
            dev->context()->Unmap(staging.get(), 0);
        });
    });
}

void D3D11CommandList::copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                                           std::uint64_t srcOffset, std::uint32_t rowPitch) {
    requireRecording("copyBufferToTexture");
    auto* d = static_cast<D3D11Texture*>(dst);
    auto* s = static_cast<D3D11Buffer*>(src);
    if (!s->isUpload()) throw Error("copyBufferToTexture: Direct3D 11 copies into textures from Upload buffers only");
    m_cmds.push([=](Executor& e) {
        const TextureDesc& dd = d->desc();
        const FormatInfo& fi = formatInfo(dd.format);
        const std::uint32_t h = std::max(1u, dd.height >> mip);
        const UINT rows = (h + fi.blockDim - 1) / fi.blockDim;
        const std::byte* data = s->shadow() + srcOffset;
        if (dd.type == TextureType::Tex3D) {
            const std::uint32_t w = std::max(1u, dd.width >> mip);
            const D3D11_BOX box{0, 0, layer, w, h, layer + 1};
            e.ctx->UpdateSubresource(d->resource(), d->subresource(mip, 0), &box, data, rowPitch, rowPitch * rows);
        } else {
            e.ctx->UpdateSubresource(d->resource(), d->subresource(mip, layer), nullptr, data, rowPitch, rowPitch * rows);
        }
    });
}

void D3D11CommandList::fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) {
    requireRecording("fillBuffer");
    auto* b = static_cast<D3D11Buffer*>(buffer);
    m_cmds.push([=](Executor& e) {
        if (b->fillPending(offset, size, value)) return; // not on the GPU yet: becomes its initial contents
        if (b->mapped()) {
            for (std::uint64_t i = 0; i + 4 <= size; i += 4) std::memcpy(b->shadow() + offset + i, &value, 4);
            b->forceResync();
            return;
        }
        if (any(b->desc().usage & BufferUsage::Storage)) {
            const UINT v[4] = {value, value, value, value};
            e.ctx->ClearUnorderedAccessViewUint(b->uav(offset, size, b->stride()), v);
            return;
        }
        std::vector<std::uint32_t> words(size / 4, value);
        const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + size), 1, 1};
        e.ctx->UpdateSubresource(b->gpu(), 0, &box, words.data(), 0, 0);
    });
}

void D3D11CommandList::resolveTexture(Texture* dst, Texture* src) {
    requireRecording("resolveTexture");
    auto* d = static_cast<D3D11Texture*>(dst);
    auto* s = static_cast<D3D11Texture*>(src);
    m_cmds.push([=](Executor& e) {
        e.ctx->ResolveSubresource(d->resource(), 0, s->resource(), 0, toDxgi(s->desc().format));
    });
}

// --- queries and markers ----------------------------------------------------------------------------------------
void D3D11CommandList::writeTimestamp(QueryPool* pool, std::uint32_t index) {
    requireRecording("writeTimestamp");
    ID3D11Query* q = static_cast<D3D11QueryPool*>(pool)->query(index);
    m_cmds.push([q](Executor& e) { e.ctx->End(q); });
}

void D3D11CommandList::beginQuery(QueryPool* pool, std::uint32_t index) {
    requireRecording("beginQuery");
    ID3D11Query* q = static_cast<D3D11QueryPool*>(pool)->query(index);
    m_cmds.push([q](Executor& e) { e.ctx->Begin(q); });
}

void D3D11CommandList::endQuery(QueryPool* pool, std::uint32_t index) {
    requireRecording("endQuery");
    ID3D11Query* q = static_cast<D3D11QueryPool*>(pool)->query(index);
    m_cmds.push([q](Executor& e) { e.ctx->End(q); });
}

void D3D11CommandList::beginMarker(const char* name) {
    requireRecording("beginMarker");
    if (!m_dev.annotation()) return;
    m_cmds.push([text = widen(name)](Executor& e) { e.dev.annotation()->BeginEvent(text.c_str()); });
}

void D3D11CommandList::endMarker() {
    requireRecording("endMarker");
    if (!m_dev.annotation()) return;
    m_cmds.push([](Executor& e) { e.dev.annotation()->EndEvent(); });
}

TransientAllocation D3D11CommandList::allocTransient(std::uint64_t size, std::uint64_t alignment) {
    alignment = std::max<std::uint64_t>(alignment, 16);
    for (;; ++m_chunk) {
        if (m_chunk >= m_chunks.size()) {
            Chunk c;
            const std::uint64_t bytes = std::max(kChunkSize, size + alignment);
            c.buffer = std::make_unique<D3D11Buffer>(
                m_dev,
                BufferDesc{bytes, BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::Vertex | BufferUsage::Index,
                           MemoryType::Upload, 0, "transient"},
                nullptr);
            m_chunks.push_back(std::move(c));
        }
        Chunk& c = m_chunks[m_chunk];
        const std::uint64_t off = (c.used + alignment - 1) / alignment * alignment;
        if (off + size <= c.buffer->desc().size) {
            c.used = off + size;
            return {c.buffer->shadow() + off, c.buffer.get(), off, size};
        }
    }
}

} // namespace gt::rhi::d3d11
