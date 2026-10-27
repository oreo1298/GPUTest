#include "d3d12_device.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

constexpr std::uint64_t kChunkSize = 4ull * 1024 * 1024;
const D3D12_RESOURCE_STATES kGraphicsOnlyStates =
    D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE |
    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_STREAM_OUT |
    D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
const D3D12_RESOURCE_STATES kWriteStates =
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
    D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;


D3D12_COMMAND_LIST_TYPE listType(QueueType q) {
    switch (q) {
    case QueueType::Graphics: return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case QueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case QueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

bool writes(D3D12_RESOURCE_STATES s) { return (s & kWriteStates) != 0; }

char registerClass(BindingType t) {
    switch (t) {
    case BindingType::UniformBuffer: return 'b';
    case BindingType::StorageBufferRW:
    case BindingType::StorageTexture: return 'u';
    case BindingType::Sampler: return 's';
    default: return 't';
    }
}

} // namespace

D3D12CommandList::D3D12CommandList(D3D12Device& dev, QueueType queue) : m_dev(dev) {
    m_queue = queue;
    ID3D12Device* d = dev.d3dDevice();
    dev.check(d->CreateCommandAllocator(listType(queue), __uuidof(ID3D12CommandAllocator), m_allocator.putVoid()),
              "CreateCommandAllocator");
    dev.check(d->CreateCommandList(0, listType(queue), m_allocator.get(), nullptr, __uuidof(ID3D12GraphicsCommandList),
                                   m_list.putVoid()),
              "CreateCommandList");
    m_list->Close();
    m_list4 = m_list.as<ID3D12GraphicsCommandList4>();
    m_list5 = m_list.as<ID3D12GraphicsCommandList5>();
    m_list6 = m_list.as<ID3D12GraphicsCommandList6>();
}

D3D12CommandList::~D3D12CommandList() {
    D3D12Device* dev = &m_dev;
    Com<ID3D12CommandAllocator> a = m_allocator;
    Com<ID3D12GraphicsCommandList> l = m_list;
    std::vector<std::uint32_t> views = m_viewBlocks, samplers = m_samplerBlocks;
    m_dev.deferDestroy([dev, a, l, views, samplers] {
        for (std::uint32_t b : views) dev->viewHeap().freeBlock(b);
        for (std::uint32_t b : samplers) dev->samplerHeap().freeBlock(b);
    });
}

void D3D12CommandList::requireRecording(const char* what) const {
    if (!m_recording) throw Error(std::format("CommandList::{} called outside begin()/end()", what));
}

void D3D12CommandList::begin() {
    if (m_lastSubmit) m_dev.wait(m_queue, m_lastSubmit);
    m_dev.check(m_allocator->Reset(), "ID3D12CommandAllocator::Reset");
    m_dev.check(m_list->Reset(m_allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    m_recording = true;
    m_inRenderPass = false;
    m_pipeline = nullptr;
    m_root = nullptr;
    m_rootDirty = m_viewsDirty = m_samplersDirty = m_tableDirty = true;
    m_pushDirty = false;
    m_pushBytes = 0;
    m_accelDirty = false;
    m_builtAccels.clear();
    m_table = nullptr;
    m_b = {};
    m_t = {};
    m_u = {};
    m_s = {};
    m_vbs = {};
    m_vbsDirty = false;
    m_shadingRate = D3D12_SHADING_RATE_1X1;
    m_rateImage = false;
    m_barriers.clear();
    m_local.clear();
    m_copyQueueTextures.clear();
    m_pendingResolves.clear();
    for (auto& c : m_chunks) c.used = 0;
    m_chunkIndex = 0;
    m_viewBlock = m_samplerBlock = 0;
    m_viewUsed = m_samplerUsed = 0;
    if (m_queue != QueueType::Copy) {
        ID3D12DescriptorHeap* heaps[] = {m_dev.viewHeap().heap(), m_dev.samplerHeap().heap()};
        m_list->SetDescriptorHeaps(2, heaps);
    }
}

void D3D12CommandList::end() {
    requireRecording("end");
    if (m_inRenderPass) endRenderPass();
    flushBarriers();
    // Textures used on a copy queue decay to COMMON once the submission completes.
    for (D3D12Texture* t : m_copyQueueTextures) t->d3dState = D3D12_RESOURCE_STATE_COMMON;
    m_dev.check(m_list->Close(), "ID3D12GraphicsCommandList::Close");
    m_recording = false;
}

ID3D12GraphicsCommandList* D3D12CommandList::beginNative() {
    requireRecording("beginNative");
    flushBarriers();
    return m_list.get();
}

void D3D12CommandList::endNative() {
    requireRecording("endNative");
    m_pipeline = nullptr;
    m_root = nullptr;
    m_rootDirty = m_viewsDirty = m_samplersDirty = m_tableDirty = true;
    m_pushDirty = m_pushBytes > 0;
    m_vbsDirty = true;
    if (m_queue != QueueType::Copy) {
        ID3D12DescriptorHeap* heaps[] = {m_dev.viewHeap().heap(), m_dev.samplerHeap().heap()};
        m_list->SetDescriptorHeaps(2, heaps);
    }
}

// --- resource states -------------------------------------------------------------------------------------
D3D12_RESOURCE_STATES D3D12CommandList::shaderReadState() const {
    return isGraphics() ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

D3D12_RESOURCE_STATES D3D12CommandList::stateFor(ResourceState s) const {
    switch (s) {
    case ResourceState::Undefined:
    case ResourceState::Common:
    case ResourceState::Present: return D3D12_RESOURCE_STATE_COMMON;
    case ResourceState::VertexBuffer:
    case ResourceState::UniformBuffer: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case ResourceState::ShaderRead: return shaderReadState();
    case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::DepthRead:
        return isGraphics() ? D3D12_RESOURCE_STATE_DEPTH_READ | shaderReadState()
                            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case ResourceState::CopySrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::CopyDst:
    case ResourceState::HostRead: return D3D12_RESOURCE_STATE_COPY_DEST;
    case ResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case ResourceState::ShadingRateSource: return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_RESOURCE_STATES& D3D12CommandList::trackedState(D3D12Texture* t) {
    if (!t->decays() && m_queue != QueueType::Copy) return t->d3dState;
    auto [it, inserted] = m_local.try_emplace(t->resource(), D3D12_RESOURCE_STATE_COMMON);
    if (inserted && m_queue == QueueType::Copy && !t->decays()) {
        if (t->d3dState != D3D12_RESOURCE_STATE_COMMON)
            throw Error(std::format("texture '{}' must be in the Common state before copy-queue use", t->desc().name));
        m_copyQueueTextures.push_back(t);
    }
    return it->second;
}

void D3D12CommandList::barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES want,
                               const char* name) {
    if (current == want) return;
    if (m_queue == QueueType::Compute && (current & kGraphicsOnlyStates))
        throw Error(std::format("'{}' is in a graphics-only state and cannot be used on the compute queue; create it "
                                "with TextureDesc::concurrentQueues and transition it to ShaderRead on the graphics "
                                "queue before the hand-over",
                                name));
    // A combined read state that already includes the wanted reads stays as it is (e.g. a depth
    // buffer attached read-only and sampled at the same time).
    if (current != D3D12_RESOURCE_STATE_COMMON && !writes(current) && !writes(want) && (current & want) == want) return;
    // Barriers waiting in the batch all take effect at the same point: chain onto a pending
    // transition of the same resource instead of adding a second one (A->B, B->C becomes A->C).
    for (auto it = m_barriers.rbegin(); it != m_barriers.rend(); ++it) {
        if (it->Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || it->Transition.pResource != r) continue;
        if (it->Transition.StateBefore == want)
            m_barriers.erase(std::next(it).base());
        else
            it->Transition.StateAfter = want;
        current = want;
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = current;
    b.Transition.StateAfter = want;
    m_barriers.push_back(b);
    current = want;
}

void D3D12CommandList::require(D3D12Texture* t, D3D12_RESOURCE_STATES want) {
    if (m_queue == QueueType::Copy && want != D3D12_RESOURCE_STATE_COPY_SOURCE && want != D3D12_RESOURCE_STATE_COPY_DEST)
        want = D3D12_RESOURCE_STATE_COMMON;
    barrier(t->resource(), trackedState(t), want, t->desc().name);
}

void D3D12CommandList::require(D3D12Buffer* b, D3D12_RESOURCE_STATES want) {
    if (b->fixedState()) return;
    if (m_queue == QueueType::Copy && want != D3D12_RESOURCE_STATE_COPY_SOURCE && want != D3D12_RESOURCE_STATE_COPY_DEST)
        want = D3D12_RESOURCE_STATE_COMMON;
    auto [it, inserted] = m_local.try_emplace(b->resource(), D3D12_RESOURCE_STATE_COMMON);
    barrier(b->resource(), it->second, want, b->desc().name);
}

void D3D12CommandList::flushBarriers() {
    if (m_barriers.empty()) return;
    m_list->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
    m_barriers.clear();
}

void D3D12CommandList::transition(Texture* texture, ResourceState state) {
    requireRecording("transition");
    auto* t = static_cast<D3D12Texture*>(texture);
    D3D12_RESOURCE_STATES want = stateFor(state);
    // Cross-queue textures without simultaneous access (depth, MSAA) are handed to the compute queue
    // in a read state it can use; pixel-shader binds add PIXEL_SHADER_RESOURCE when needed.
    if (state == ResourceState::ShaderRead && isGraphics() && t->desc().concurrentQueues && !t->decays())
        want = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && trackedState(t) == want) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = t->resource();
        m_barriers.push_back(b);
    } else {
        require(t, want);
    }
    t->state = state;
}

void D3D12CommandList::transition(Buffer* buffer, ResourceState state) {
    requireRecording("transition");
    auto* b = static_cast<D3D12Buffer*>(buffer);
    if (b->fixedState()) return;
    const D3D12_RESOURCE_STATES want = stateFor(state);
    auto it = m_local.find(b->resource());
    if (want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && it != m_local.end() && it->second == want) {
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = b->resource();
        m_barriers.push_back(rb);
        return;
    }
    require(b, want);
}

void D3D12CommandList::uavBarrier() {
    requireRecording("uavBarrier");
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    m_barriers.push_back(b);
}

// --- render passes -----------------------------------------------------------------------------------------
void D3D12CommandList::beginRenderPass(const RenderPassDesc& desc) {
    requireRecording("beginRenderPass");
    if (m_inRenderPass) throw Error("beginRenderPass: already inside a render pass");
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    std::uint32_t width = 0, height = 0;
    m_pendingResolves.clear();
    for (const ColorAttachment& c : desc.colors) {
        auto* t = static_cast<D3D12Texture*>(c.texture);
        require(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        t->state = ResourceState::RenderTarget;
        rtvs.push_back(t->rtv(c.mip, c.layer));
        width = std::max(1u, t->desc().width >> c.mip);
        height = std::max(1u, t->desc().height >> c.mip);
        if (c.resolveTarget) m_pendingResolves.emplace_back(static_cast<D3D12Texture*>(c.resolveTarget), t);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12Texture* depth = static_cast<D3D12Texture*>(desc.depth.texture);
    if (depth) {
        if (desc.depth.readOnly) {
            require(depth, D3D12_RESOURCE_STATE_DEPTH_READ | shaderReadState());
            depth->state = ResourceState::DepthRead;
        } else {
            require(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depth->state = ResourceState::DepthWrite;
        }
        dsv = depth->dsv(desc.depth.readOnly);
        width = width ? width : depth->desc().width;
        height = height ? height : depth->desc().height;
    }
    auto* rateImage = static_cast<D3D12Texture*>(desc.shadingRateImage);
    if (rateImage) {
        if (!m_dev.features().shadingRateImage || !m_list5)
            throw Error("beginRenderPass: shading-rate images are not supported (D3D12 VRS tier 2)");
        require(rateImage, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        rateImage->state = ResourceState::ShadingRateSource;
    }
    flushBarriers();
    for (std::size_t i = 0; i < desc.colors.size(); ++i)
        if (desc.colors[i].load == LoadOp::Clear) m_list->ClearRenderTargetView(rtvs[i], desc.colors[i].clear.data(), 0, nullptr);
    if (depth && desc.depth.load == LoadOp::Clear && !desc.depth.readOnly) {
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (formatInfo(depth->desc().format).stencil) flags |= D3D12_CLEAR_FLAG_STENCIL;
        m_list->ClearDepthStencilView(dsv, flags, desc.depth.clearDepth, desc.depth.clearStencil, 0, nullptr);
    }
    m_list->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), FALSE, depth ? &dsv : nullptr);
    if (rateImage) {
        m_list5->RSSetShadingRateImage(rateImage->resource());
        m_rateImage = true;
        applyShadingRate();
    }
    m_inRenderPass = true;
    setViewport(0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    setScissor(0, 0, width, height);
}

void D3D12CommandList::endRenderPass() {
    requireRecording("endRenderPass");
    if (!m_inRenderPass) return;
    m_inRenderPass = false;
    if (m_rateImage) {
        m_list5->RSSetShadingRateImage(nullptr);
        m_rateImage = false;
        applyShadingRate();
    }
    for (auto [dst, src] : m_pendingResolves) resolveTexture(dst, src);
    m_pendingResolves.clear();
}

void D3D12CommandList::setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) {
    const D3D12_VIEWPORT vp{x, y, w, h, minDepth, maxDepth};
    m_list->RSSetViewports(1, &vp);
}

void D3D12CommandList::setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
    const D3D12_RECT r{x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(h)};
    m_list->RSSetScissorRects(1, &r);
}

// --- bindings ----------------------------------------------------------------------------------------------
void D3D12CommandList::setPipeline(Pipeline* pipeline) {
    requireRecording("setPipeline");
    auto* p = static_cast<D3D12Pipeline*>(pipeline);
    if (p == m_pipeline) return;
    const bool typeChanged = !m_pipeline || m_pipeline->isCompute() != p->isCompute();
    if (p->rayTracing()) {
        if (!m_list4) throw Error("ray tracing is not supported by this command list");
        m_list4->SetPipelineState1(p->stateObject());
    } else {
        m_list->SetPipelineState(p->pso());
    }
    if (typeChanged || m_root != &p->root()) {
        m_root = &p->root();
        if (p->isCompute()) m_list->SetComputeRootSignature(m_root->signature.get());
        else m_list->SetGraphicsRootSignature(m_root->signature.get());
        m_rootDirty = m_viewsDirty = m_samplersDirty = m_tableDirty = true;
        m_pushDirty = m_pushBytes > 0;
    }
    if (!p->isCompute()) {
        m_list->IASetPrimitiveTopology(p->topology());
        m_vbsDirty = true; // strides come from the pipeline
    }
    m_pipeline = p;
}

void D3D12CommandList::setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) {
    if (binding >= m_vbs.size()) throw Error(std::format("vertex buffer binding {} out of range", binding));
    m_vbs[binding] = {static_cast<D3D12Buffer*>(buffer), offset};
    m_vbsDirty = true;
}

void D3D12CommandList::setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) {
    requireRecording("setIndexBuffer");
    auto* b = static_cast<D3D12Buffer*>(buffer);
    require(b, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b->address() + offset;
    v.SizeInBytes = static_cast<UINT>(b->desc().size - offset);
    v.Format = type == IndexType::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    m_list->IASetIndexBuffer(&v);
}

void D3D12CommandList::flushVertexBuffers() {
    if (!m_vbsDirty || !m_pipeline) return;
    for (std::uint32_t i = 0; i < m_vbs.size(); ++i) {
        const VertexBinding& vb = m_vbs[i];
        if (!vb.buffer) continue;
        require(vb.buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        D3D12_VERTEX_BUFFER_VIEW v{};
        v.BufferLocation = vb.buffer->address() + vb.offset;
        v.SizeInBytes = static_cast<UINT>(vb.buffer->desc().size - vb.offset);
        v.StrideInBytes = m_pipeline->vertexStride(i);
        m_list->IASetVertexBuffers(i, 1, &v);
    }
    m_vbsDirty = false;
}

D3D12CommandList::Bound& D3D12CommandList::slot(char cls, std::uint32_t index) {
    if (index >= kSlots) throw Error(std::format("binding slot {}{} out of range", cls, index));
    switch (cls) {
    case 'b': return m_b[index];
    case 't': return m_t[index];
    case 'u': return m_u[index];
    default: return m_s[index];
    }
}

const D3D12CommandList::Bound& D3D12CommandList::bound(BindingType type, std::uint32_t s) const {
    switch (registerClass(type)) {
    case 'b': return m_b[s];
    case 't': return m_t[s];
    case 'u': return m_u[s];
    default: return m_s[s];
    }
}

void D3D12CommandList::setUniformBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('b', s);
    b = Bound{};
    b.buffer = static_cast<D3D12Buffer*>(buffer);
    b.offset = offset;
    b.size = size;
    m_rootDirty = m_viewsDirty = true;
}

void D3D12CommandList::setUniformData(std::uint32_t s, const void* data, std::uint64_t size) {
    const TransientAllocation a = allocTransient(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    std::memcpy(a.cpu, data, size);
    setUniformBuffer(s, a.buffer, a.offset, size);
}

void D3D12CommandList::setTexture(std::uint32_t s, Texture* texture, Subresource sub) {
    Bound& b = slot('t', s);
    b = Bound{};
    b.texture = static_cast<D3D12Texture*>(texture);
    b.sub = sub;
    m_viewsDirty = true;
}

void D3D12CommandList::setStorageTexture(std::uint32_t s, Texture* texture, std::uint32_t mip) {
    Bound& b = slot('u', s);
    b = Bound{};
    b.texture = static_cast<D3D12Texture*>(texture);
    b.mip = mip;
    m_viewsDirty = true;
}

void D3D12CommandList::setStorageBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('u', s);
    b = Bound{};
    b.buffer = static_cast<D3D12Buffer*>(buffer);
    b.offset = offset;
    b.size = size;
    m_rootDirty = m_viewsDirty = true;
}

void D3D12CommandList::setReadOnlyBuffer(std::uint32_t s, Buffer* buffer, std::uint64_t offset, std::uint64_t size) {
    Bound& b = slot('t', s);
    b = Bound{};
    b.buffer = static_cast<D3D12Buffer*>(buffer);
    b.offset = offset;
    b.size = size;
    m_rootDirty = m_viewsDirty = true;
}

void D3D12CommandList::setSampler(std::uint32_t s, Sampler* sampler) {
    slot('s', s).sampler = static_cast<D3D12Sampler*>(sampler);
    m_samplersDirty = true;
}

void D3D12CommandList::setAccelerationStructure(std::uint32_t s, AccelerationStructure* tlas) {
    Bound& b = slot('t', s);
    b = Bound{};
    b.accel = static_cast<D3D12Accel*>(tlas);
    m_rootDirty = m_viewsDirty = true;
}

void D3D12CommandList::setTextureTable(TextureTable* table) {
    m_table = static_cast<D3D12TextureTable*>(table);
    m_tableDirty = true;
}

void D3D12CommandList::pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) {
    if (offset + size > sizeof(m_push)) throw Error("pushConstants: more than 128 bytes");
    std::memcpy(reinterpret_cast<std::uint8_t*>(m_push.data()) + offset, data, size);
    m_pushBytes = std::max(m_pushBytes, offset + size);
    m_pushDirty = true;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12CommandList::allocViews(std::uint32_t count, std::uint32_t& first) {
    GpuDescriptorHeap& heap = m_dev.viewHeap();
    if (count > heap.blockSize()) throw Error("too many views in one pipeline layout");
    for (;;) {
        if (m_viewBlock < m_viewBlocks.size()) {
            if (m_viewUsed + count <= heap.blockSize()) {
                first = m_viewBlocks[m_viewBlock] + m_viewUsed;
                m_viewUsed += count;
                return heap.gpu(first);
            }
            ++m_viewBlock;
            m_viewUsed = 0;
            continue;
        }
        m_viewBlocks.push_back(heap.allocBlock());
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12CommandList::allocSamplers(std::uint32_t count, std::uint32_t& first) {
    GpuDescriptorHeap& heap = m_dev.samplerHeap();
    if (count > heap.blockSize()) throw Error("too many samplers in one pipeline layout");
    for (;;) {
        if (m_samplerBlock < m_samplerBlocks.size()) {
            if (m_samplerUsed + count <= heap.blockSize()) {
                first = m_samplerBlocks[m_samplerBlock] + m_samplerUsed;
                m_samplerUsed += count;
                return heap.gpu(first);
            }
            ++m_samplerBlock;
            m_samplerUsed = 0;
            continue;
        }
        m_samplerBlocks.push_back(heap.allocBlock());
    }
}

void D3D12CommandList::flushBindings(bool compute) {
    if (!m_pipeline) throw Error("draw/dispatch without a pipeline");
    if (m_pipeline->isCompute() != compute)
        throw Error(std::format("pipeline '{}' is a {} pipeline", m_pipeline->name(), compute ? "graphics" : "compute"));
    if (m_accelDirty) accelBarrier(); // work after a build sees the finished structure
    const RootLayout& L = m_root->layout;
    ID3D12GraphicsCommandList* l = m_list.get();
    ID3D12Device* d = m_dev.d3dDevice();
    const char* name = m_pipeline->name().c_str();

    auto missing = [&](const RootLayout::Entry& e) {
        return Error(std::format("pipeline '{}': slot {}{} not bound", name, registerClass(e.type), e.slot));
    };
    // Bound resources get a state that allows their use (recorded before the draw). Dispatches only
    // need the non-pixel read state, which keeps textures handed to the compute queue usable there.
    const D3D12_RESOURCE_STATES readState = compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : shaderReadState();
    auto prepare = [&](const RootLayout::Entry& e) {
        const Bound& b = bound(e.type, e.slot);
        switch (e.type) {
        case BindingType::UniformBuffer:
            if (!b.buffer) throw missing(e);
            require(b.buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            break;
        case BindingType::StorageBufferRO:
            if (!b.buffer) throw missing(e);
            require(b.buffer, readState);
            break;
        case BindingType::StorageBufferRW:
            if (!b.buffer) throw missing(e);
            require(b.buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            break;
        case BindingType::SampledTexture:
            if (!b.texture) throw missing(e);
            require(b.texture, readState);
            break;
        case BindingType::StorageTexture:
            if (!b.texture) throw missing(e);
            require(b.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            break;
        case BindingType::AccelerationStructure:
            if (!b.accel) throw missing(e);
            break;
        case BindingType::Sampler:
            if (!b.sampler) throw missing(e);
            break;
        }
    };

    if (m_pushDirty && L.pushParam >= 0) {
        const auto param = static_cast<UINT>(L.pushParam);
        if (compute) l->SetComputeRoot32BitConstants(param, L.pushConstantDwords, m_push.data(), 0);
        else l->SetGraphicsRoot32BitConstants(param, L.pushConstantDwords, m_push.data(), 0);
    }
    m_pushDirty = false;

    if (m_rootDirty) {
        for (const auto& e : L.rootDescriptors) {
            prepare(e);
            const Bound& b = bound(e.type, e.slot);
            const D3D12_GPU_VIRTUAL_ADDRESS va = b.accel ? b.accel->address() : b.buffer->address() + b.offset;
            if (e.type == BindingType::UniformBuffer) {
                if (compute) l->SetComputeRootConstantBufferView(e.index, va);
                else l->SetGraphicsRootConstantBufferView(e.index, va);
            } else if (e.type == BindingType::StorageBufferRW) {
                if (compute) l->SetComputeRootUnorderedAccessView(e.index, va);
                else l->SetGraphicsRootUnorderedAccessView(e.index, va);
            } else {
                if (compute) l->SetComputeRootShaderResourceView(e.index, va);
                else l->SetGraphicsRootShaderResourceView(e.index, va);
            }
        }
        m_rootDirty = false;
    }

    if (m_viewsDirty && L.viewTableParam >= 0) {
        std::uint32_t first = 0;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = allocViews(static_cast<std::uint32_t>(L.viewTable.size()), first);
        GpuDescriptorHeap& heap = m_dev.viewHeap();
        for (const auto& e : L.viewTable) {
            prepare(e);
            const Bound& b = bound(e.type, e.slot);
            const D3D12_CPU_DESCRIPTOR_HANDLE dst = heap.cpu(first + e.index);
            switch (e.type) {
            case BindingType::SampledTexture:
                d->CopyDescriptorsSimple(1, dst, b.texture->srv(b.sub), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                break;
            case BindingType::StorageTexture:
                d->CopyDescriptorsSimple(1, dst, b.texture->uav(b.mip), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                break;
            case BindingType::UniformBuffer: {
                const std::uint64_t size = b.size ? b.size : b.buffer->desc().size - b.offset;
                D3D12_CONSTANT_BUFFER_VIEW_DESC cv{b.buffer->address() + b.offset,
                                                   static_cast<UINT>(alignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))};
                d->CreateConstantBufferView(&cv, dst);
                break;
            }
            case BindingType::StorageBufferRO:
            case BindingType::StorageBufferRW: {
                const std::uint64_t size = b.size ? b.size : b.buffer->desc().size - b.offset;
                const std::uint32_t stride = b.buffer->desc().structStride;
                const std::uint64_t unit = stride ? stride : 4;
                if (e.type == BindingType::StorageBufferRO) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                    sv.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.Buffer.FirstElement = b.offset / unit;
                    sv.Buffer.NumElements = static_cast<UINT>(size / unit);
                    sv.Buffer.StructureByteStride = stride;
                    sv.Buffer.Flags = stride ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
                    d->CreateShaderResourceView(b.buffer->resource(), &sv, dst);
                } else {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
                    uv.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uv.Buffer.FirstElement = b.offset / unit;
                    uv.Buffer.NumElements = static_cast<UINT>(size / unit);
                    uv.Buffer.StructureByteStride = stride;
                    uv.Buffer.Flags = stride ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
                    d->CreateUnorderedAccessView(b.buffer->resource(), nullptr, &uv, dst);
                }
                break;
            }
            case BindingType::AccelerationStructure: {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.RaytracingAccelerationStructure.Location = b.accel->address();
                d->CreateShaderResourceView(nullptr, &sv, dst);
                break;
            }
            case BindingType::Sampler: break;
            }
        }
        const auto param = static_cast<UINT>(L.viewTableParam);
        if (compute) l->SetComputeRootDescriptorTable(param, gpu);
        else l->SetGraphicsRootDescriptorTable(param, gpu);
    }
    m_viewsDirty = false;

    if (m_samplersDirty && L.samplerTableParam >= 0) {
        std::uint32_t first = 0;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = allocSamplers(static_cast<std::uint32_t>(L.samplerTable.size()), first);
        for (const auto& e : L.samplerTable) {
            prepare(e);
            d->CopyDescriptorsSimple(1, m_dev.samplerHeap().cpu(first + e.index), bound(e.type, e.slot).sampler->handle(),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        const auto param = static_cast<UINT>(L.samplerTableParam);
        if (compute) l->SetComputeRootDescriptorTable(param, gpu);
        else l->SetGraphicsRootDescriptorTable(param, gpu);
    }
    m_samplersDirty = false;

    if (m_tableDirty && L.textureTableParam >= 0) {
        if (!m_table) throw Error(std::format("pipeline '{}': no texture table bound", name));
        const auto param = static_cast<UINT>(L.textureTableParam);
        if (compute) l->SetComputeRootDescriptorTable(param, m_table->gpuHandle());
        else l->SetGraphicsRootDescriptorTable(param, m_table->gpuHandle());
    }
    m_tableDirty = false;
}

// --- work ------------------------------------------------------------------------------------------------
void D3D12CommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                            std::uint32_t firstInstance) {
    flushBindings(false);
    flushVertexBuffers();
    flushBarriers();
    m_list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                   std::int32_t vertexOffset, std::uint32_t firstInstance) {
    flushBindings(false);
    flushVertexBuffers();
    flushBarriers();
    m_list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D12CommandList::drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount, std::uint32_t stride) {
    flushBindings(false);
    flushVertexBuffers();
    auto* a = static_cast<D3D12Buffer*>(args);
    require(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    flushBarriers();
    m_list->ExecuteIndirect(m_dev.commandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, stride), drawCount, a->resource(),
                            offset, nullptr, 0);
}

void D3D12CommandList::drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                           std::uint32_t stride) {
    flushBindings(false);
    flushVertexBuffers();
    auto* a = static_cast<D3D12Buffer*>(args);
    require(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    flushBarriers();
    m_list->ExecuteIndirect(m_dev.commandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, stride), drawCount,
                            a->resource(), offset, nullptr, 0);
}

void D3D12CommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    flushBindings(true);
    flushBarriers();
    m_list->Dispatch(x, y, z);
}

void D3D12CommandList::dispatchIndirect(Buffer* args, std::uint64_t offset) {
    flushBindings(true);
    auto* a = static_cast<D3D12Buffer*>(args);
    require(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    flushBarriers();
    m_list->ExecuteIndirect(m_dev.commandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, 0), 1, a->resource(), offset,
                            nullptr, 0);
}

void D3D12CommandList::drawMeshTasks(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!m_dev.features().meshShader || !m_list6) throw Error("drawMeshTasks: mesh shaders are not supported by this GPU");
    flushBindings(false);
    flushBarriers();
    m_list6->DispatchMesh(x, y, z);
}

void D3D12CommandList::setShadingRate(std::uint32_t width, std::uint32_t height) {
    requireRecording("setShadingRate");
    if (!m_dev.features().variableRateShading || !m_list5)
        throw Error("setShadingRate: variable-rate shading is not supported");
    D3D12_SHADING_RATE rate = D3D12_SHADING_RATE_1X1;
    if (width >= 4 && height >= 4) rate = D3D12_SHADING_RATE_4X4;
    else if (width >= 4 && height >= 2) rate = D3D12_SHADING_RATE_4X2;
    else if (width >= 2 && height >= 4) rate = D3D12_SHADING_RATE_2X4;
    else if (width >= 2 && height >= 2) rate = D3D12_SHADING_RATE_2X2;
    else if (width >= 2) rate = D3D12_SHADING_RATE_2X1;
    else if (height >= 2) rate = D3D12_SHADING_RATE_1X2;
    m_shadingRate = rate;
    applyShadingRate();
}

void D3D12CommandList::applyShadingRate() {
    // Final rate = combiner1(combiner0(per-draw, per-primitive), image): an open shading-rate image overrides.
    const D3D12_SHADING_RATE_COMBINER combiners[2] = {
        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
        m_rateImage ? D3D12_SHADING_RATE_COMBINER_OVERRIDE : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    m_list5->RSSetShadingRate(m_shadingRate, combiners);
}

// --- transfers ---------------------------------------------------------------------------------------------
void D3D12CommandList::copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                                  std::uint64_t size) {
    requireRecording("copyBuffer");
    auto* d = static_cast<D3D12Buffer*>(dst);
    auto* s = static_cast<D3D12Buffer*>(src);
    require(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(d, D3D12_RESOURCE_STATE_COPY_DEST);
    flushBarriers();
    m_list->CopyBufferRegion(d->resource(), dstOffset, s->resource(), srcOffset, size);
}

void D3D12CommandList::copyTexture(Texture* dst, Texture* src) {
    requireRecording("copyTexture");
    auto* d = static_cast<D3D12Texture*>(dst);
    auto* s = static_cast<D3D12Texture*>(src);
    require(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(d, D3D12_RESOURCE_STATE_COPY_DEST);
    s->state = ResourceState::CopySrc;
    d->state = ResourceState::CopyDst;
    flushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d->resource();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s->resource();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, s->desc().width, s->desc().height, 1};
    m_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
}

namespace {
D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(const TextureDesc& t, std::uint32_t mip, std::uint64_t offset,
                                             std::uint32_t rowPitch) {
    const FormatInfo& fi = formatInfo(t.format);
    auto extent = [&](std::uint32_t v) {
        const std::uint32_t e = std::max(1u, v >> mip);
        return (e + fi.blockDim - 1) / fi.blockDim * fi.blockDim;
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
    f.Offset = offset;
    f.Footprint.Format = toDxgiResource(t.format);
    f.Footprint.Width = extent(t.width);
    f.Footprint.Height = extent(t.height);
    f.Footprint.Depth = 1;
    f.Footprint.RowPitch = rowPitch;
    return f;
}
} // namespace

void D3D12CommandList::copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                                           std::uint32_t mip, std::uint32_t layer) {
    requireRecording("copyTextureToBuffer");
    auto* s = static_cast<D3D12Texture*>(src);
    auto* d = static_cast<D3D12Buffer*>(dst);
    require(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(d, D3D12_RESOURCE_STATE_COPY_DEST);
    s->state = ResourceState::CopySrc;
    flushBarriers();
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s->resource();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = mip + layer * s->desc().mipLevels;
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d->resource();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = footprint(s->desc(), mip, dstOffset, rowPitch);
    m_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12CommandList::copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                                           std::uint64_t srcOffset, std::uint32_t rowPitch) {
    requireRecording("copyBufferToTexture");
    auto* d = static_cast<D3D12Texture*>(dst);
    auto* s = static_cast<D3D12Buffer*>(src);
    require(d, D3D12_RESOURCE_STATE_COPY_DEST);
    require(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d->state = ResourceState::CopyDst;
    flushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d->resource();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = mip + layer * d->desc().mipLevels;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s->resource();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = footprint(d->desc(), mip, srcOffset, rowPitch);
    m_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12CommandList::fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) {
    requireRecording("fillBuffer");
    auto* b = static_cast<D3D12Buffer*>(buffer);
    if (!size) size = b->desc().size - offset;
    if (m_queue != QueueType::Copy && b->allowsUav() && offset % 4 == 0 && size % 4 == 0) {
        require(b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flushBarriers();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.FirstElement = offset / 4;
        uv.Buffer.NumElements = static_cast<UINT>(size / 4);
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        // The clear needs the view both in the shader-visible heap and in a CPU-only heap.
        std::uint32_t first = 0;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu = allocViews(1, first);
        m_dev.d3dDevice()->CreateUnorderedAccessView(b->resource(), nullptr, &uv, m_dev.viewHeap().cpu(first));
        CpuDescriptorHeap& cpuHeap = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpuHeap.alloc();
        m_dev.d3dDevice()->CreateUnorderedAccessView(b->resource(), nullptr, &uv, cpu);
        const UINT values[4] = {value, value, value, value};
        m_list->ClearUnorderedAccessViewUint(gpu, cpu, b->resource(), values, 0, nullptr);
        cpuHeap.free(cpu); // read when the command is recorded
        return;
    }
    // Copy queues and CPU-visible buffers: copy the pattern from upload memory.
    for (std::uint64_t done = 0; done < size;) {
        const std::uint64_t n = std::min(size - done, kChunkSize / 2);
        const TransientAllocation a = allocTransient(n, 16);
        auto* p = static_cast<std::uint8_t*>(a.cpu);
        for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * ((offset + done + i) % 4)));
        copyBuffer(b, offset + done, a.buffer, a.offset, n);
        done += n;
    }
}

void D3D12CommandList::resolveTexture(Texture* dst, Texture* src) {
    requireRecording("resolveTexture");
    auto* d = static_cast<D3D12Texture*>(dst);
    auto* s = static_cast<D3D12Texture*>(src);
    require(s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    require(d, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    s->state = ResourceState::CopySrc;
    d->state = ResourceState::CopyDst;
    flushBarriers();
    m_list->ResolveSubresource(d->resource(), 0, s->resource(), 0, toDxgi(s->desc().format));
}

// --- queries -------------------------------------------------------------------------------------------------
void D3D12CommandList::resetQueries(QueryPool*, std::uint32_t, std::uint32_t) {}

void D3D12CommandList::writeTimestamp(QueryPool* pool, std::uint32_t index) {
    auto* p = static_cast<D3D12QueryPool*>(pool);
    flushBarriers();
    p->noteWrite(index, m_queue);
    m_list->EndQuery(p->heap(m_queue), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void D3D12CommandList::beginQuery(QueryPool* pool, std::uint32_t index) {
    auto* p = static_cast<D3D12QueryPool*>(pool);
    m_list->BeginQuery(p->heap(m_queue), p->queryType(), index);
}

void D3D12CommandList::endQuery(QueryPool* pool, std::uint32_t index) {
    auto* p = static_cast<D3D12QueryPool*>(pool);
    m_list->EndQuery(p->heap(m_queue), p->queryType(), index);
}

void D3D12CommandList::resolveQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) {
    auto* p = static_cast<D3D12QueryPool*>(pool);
    flushBarriers();
    m_list->ResolveQueryData(p->heap(m_queue), p->queryType(), first, count, p->readback(),
                             static_cast<UINT64>(first) * p->valuesPerQuery() * sizeof(std::uint64_t));
}

void D3D12CommandList::beginMarker(const char* name) {
    // PIX convention: metadata 1 = ANSI string.
    m_list->BeginEvent(1, name, static_cast<UINT>(std::strlen(name) + 1));
}

void D3D12CommandList::endMarker() { m_list->EndEvent(); }

TransientAllocation D3D12CommandList::allocTransient(std::uint64_t size, std::uint64_t alignment) {
    alignment = std::max<std::uint64_t>(alignment, 16);
    while (m_chunkIndex < m_chunks.size()) {
        Chunk& c = m_chunks[m_chunkIndex];
        const std::uint64_t off = alignUp(c.used, alignment);
        if (off + size <= c.buffer->desc().size) {
            c.used = off + size;
            return {static_cast<std::uint8_t*>(c.buffer->mapped()) + off, c.buffer.get(), off, size};
        }
        ++m_chunkIndex;
    }
    const BufferDesc bd{std::max(kChunkSize, alignUp(size, alignment)),
                        BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index |
                            BufferUsage::CopySrc,
                        MemoryType::Upload, 0, "transient"};
    Chunk c;
    c.buffer = std::make_unique<D3D12Buffer>(m_dev, bd);
    c.used = size;
    m_chunks.push_back(std::move(c));
    m_chunkIndex = m_chunks.size() - 1;
    return {m_chunks.back().buffer->mapped(), m_chunks.back().buffer.get(), 0, size};
}

} // namespace gt::rhi::d3d12
