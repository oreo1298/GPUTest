#include "d3d12_device.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    return h;
}

std::uint32_t layerCount(const TextureDesc& d) {
    if (d.type == TextureType::Tex3D) return 1;
    if (d.type == TextureType::Cube) return std::max(6u, d.depthOrLayers);
    return d.depthOrLayers;
}

D3D12_TEXTURE_ADDRESS_MODE address(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::Clamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

} // namespace

// --- buffer ----------------------------------------------------------------------------------------------
D3D12Buffer::D3D12Buffer(D3D12Device& dev, const BufferDesc& desc) : m_dev(dev) {
    m_desc = desc;
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    MemoryType memory = desc.memory;
    if (memory == MemoryType::DeviceUpload && !dev.gpuUploadHeap()) memory = MemoryType::Upload;
    switch (memory) {
    case MemoryType::Device:
        flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // storage use and GPU fills
        m_uav = true;
        break;
    case MemoryType::DeviceUpload:
        heap = D3D12_HEAP_TYPE_GPU_UPLOAD;
        flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        m_uav = true;
        m_gpuUploadHeap = true;
        break;
    case MemoryType::Upload:
        heap = D3D12_HEAP_TYPE_UPLOAD;
        initial = D3D12_RESOURCE_STATE_GENERIC_READ;
        m_fixed = true;
        break;
    case MemoryType::Readback:
        heap = D3D12_HEAP_TYPE_READBACK;
        initial = D3D12_RESOURCE_STATE_COPY_DEST;
        m_fixed = true;
        break;
    }
    if (any(desc.usage & BufferUsage::AccelStorage)) {
        initial = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        m_fixed = true;
    }
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alignUp(std::max<std::uint64_t>(desc.size, 4), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    const D3D12_HEAP_PROPERTIES hp = heapProps(heap);
    dev.check(dev.d3dDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr,
                                                      __uuidof(ID3D12Resource), m_resource.putVoid()),
              std::format("CreateCommittedResource(buffer '{}', {} bytes)", desc.name, desc.size).c_str());
    m_address = m_resource->GetGPUVirtualAddress();
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        const D3D12_RANGE noRead{0, 0};
        dev.check(m_resource->Map(0, heap == D3D12_HEAP_TYPE_READBACK ? nullptr : &noRead, &m_mapped), "Map");
    }
    dev.setName(m_resource.get(), desc.name);
}

D3D12Buffer::~D3D12Buffer() {
    Com<ID3D12Resource> r = m_resource;
    m_dev.deferDestroy([r] {});
}

// --- texture ---------------------------------------------------------------------------------------------
D3D12Texture::D3D12Texture(D3D12Device& dev, const TextureDesc& desc) : m_dev(dev) {
    m_desc = desc;
    const FormatInfo& fi = formatInfo(desc.format);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = desc.type == TextureType::Tex3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = static_cast<UINT16>(desc.type == TextureType::Tex3D ? desc.depthOrLayers : layerCount(desc));
    rd.MipLevels = static_cast<UINT16>(desc.mipLevels);
    rd.Format = toDxgiResource(desc.format);
    rd.SampleDesc = {desc.sampleCount, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const auto u = desc.usage;
    if (any(u & TextureUsage::ShadingRate) &&
        (!dev.features().shadingRateImage || desc.format != Format::R8_UINT || desc.type != TextureType::Tex2D))
        throw Error(std::format("texture '{}': shading-rate images need VRS tier 2 and a 2D R8_UINT format", desc.name));
    if (any(u & TextureUsage::RenderTarget)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (any(u & TextureUsage::DepthStencil)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (any(u & TextureUsage::Storage)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Simultaneous access lets other queues use the texture without graphics-only barrier states.
    if (desc.concurrentQueues && !fi.depth && desc.sampleCount == 1) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        m_simultaneous = true;
    }
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    if (any(u & TextureUsage::DepthStencil)) {
        clear.Format = toDxgi(desc.format);
        clear.DepthStencil = {desc.optimizedClear.depth, desc.optimizedClear.stencil};
        clearPtr = &clear;
    } else if (any(u & TextureUsage::RenderTarget)) {
        clear.Format = toDxgi(desc.format);
        std::memcpy(clear.Color, desc.optimizedClear.color.data(), sizeof(clear.Color));
        clearPtr = &clear;
    }
    const D3D12_HEAP_PROPERTIES hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    dev.check(dev.d3dDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                      clearPtr, __uuidof(ID3D12Resource), m_resource.putVoid()),
              std::format("CreateCommittedResource(texture '{}', {}x{} {})", desc.name, desc.width, desc.height,
                          fi.name)
                  .c_str());
    dev.setName(m_resource.get(), desc.name);
}

D3D12Texture::D3D12Texture(D3D12Device& dev, const TextureDesc& desc, Com<ID3D12Resource> image)
    : m_dev(dev), m_resource(std::move(image)) {
    m_desc = desc;
    d3dState = D3D12_RESOURCE_STATE_PRESENT;
    state = ResourceState::Present;
}

D3D12Texture::~D3D12Texture() {
    auto& views = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto& [k, h] : m_srvs) views.free(h);
    for (auto& [k, h] : m_uavs) views.free(h);
    for (auto& [k, h] : m_rtvs) m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV).free(h);
    for (auto& h : m_dsvs) m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV).free(h);
    Com<ID3D12Resource> r = m_resource;
    m_dev.deferDestroy([r] {});
}

std::uint32_t D3D12Texture::arrayLayers() const { return layerCount(m_desc); }

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Texture::srv(Subresource sub) {
    const ViewKey key{sub.baseMip, sub.mipCount, sub.baseLayer, sub.layerCount};
    if (auto it = m_srvs.find(key); it != m_srvs.end()) return it->second;
    const std::uint32_t mips = sub.mipCount ? sub.mipCount : m_desc.mipLevels - sub.baseMip;
    const std::uint32_t layers = sub.layerCount ? sub.layerCount : arrayLayers() - sub.baseLayer;
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = toDxgiSrv(m_desc.format);
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {sub.baseMip, mips, 0.0f};
    } else if (m_desc.type == TextureType::Cube && layers == 6) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        d.TextureCube = {sub.baseMip, mips, 0.0f};
    } else if (m_desc.sampleCount > 1) {
        if (layers > 1 || m_desc.type == TextureType::Tex2DArray) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {sub.baseLayer, layers};
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        }
    } else if (layers > 1 || m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {sub.baseMip, mips, sub.baseLayer, layers, 0, 0.0f};
    } else {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D = {sub.baseMip, mips, 0, 0.0f};
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE h = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).alloc();
    m_dev.d3dDevice()->CreateShaderResourceView(m_resource.get(), &d, h);
    m_srvs.emplace(key, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Texture::uav(std::uint32_t mip) {
    const ViewKey key{mip, 0, 0, 0};
    if (auto it = m_uavs.find(key); it != m_uavs.end()) return it->second;
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = {mip, 0, UINT_MAX};
    } else if (arrayLayers() > 1 || m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {mip, 0, arrayLayers(), 0};
    } else {
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D = {mip, 0};
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE h = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).alloc();
    m_dev.d3dDevice()->CreateUnorderedAccessView(m_resource.get(), nullptr, &d, h);
    m_uavs.emplace(key, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Texture::rtv(std::uint32_t mip, std::uint32_t layer) {
    const ViewKey key{mip, layer, 0, 0};
    if (auto it = m_rtvs.find(key); it != m_rtvs.end()) return it->second;
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    const bool array = arrayLayers() > 1 || m_desc.type != TextureType::Tex2D;
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D = {mip, layer, 1};
    } else if (m_desc.sampleCount > 1) {
        if (array) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {layer, 1};
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        }
    } else if (array) {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {mip, layer, 1, 0};
    } else {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D = {mip, 0};
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE h = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV).alloc();
    m_dev.d3dDevice()->CreateRenderTargetView(m_resource.get(), &d, h);
    m_rtvs.emplace(key, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Texture::dsv(bool readOnly) {
    D3D12_CPU_DESCRIPTOR_HANDLE& h = m_dsvs[readOnly ? 1 : 0];
    if (h.ptr) return h;
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    if (m_desc.sampleCount > 1) {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D = {0};
    }
    if (readOnly) {
        d.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (formatInfo(m_desc.format).stencil) d.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }
    h = m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV).alloc();
    m_dev.d3dDevice()->CreateDepthStencilView(m_resource.get(), &d, h);
    return h;
}

// --- sampler -----------------------------------------------------------------------------------------------
D3D12Sampler::D3D12Sampler(D3D12Device& dev, const SamplerDesc& s) : m_dev(dev) {
    D3D12_SAMPLER_DESC d{};
    const D3D12_FILTER_REDUCTION_TYPE reduction =
        s.compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (s.maxAnisotropy > 1.0f) {
        d.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
        d.MaxAnisotropy = static_cast<UINT>(std::clamp(s.maxAnisotropy, 1.0f, 16.0f));
    } else {
        auto t = [](Filter f) { return f == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
        d.Filter = D3D12_ENCODE_BASIC_FILTER(t(s.minFilter), t(s.magFilter), t(s.mipFilter), reduction);
        d.MaxAnisotropy = 1;
    }
    d.AddressU = address(s.addressU);
    d.AddressV = address(s.addressV);
    d.AddressW = address(s.addressW);
    d.MipLODBias = s.mipLodBias;
    d.ComparisonFunc = s.compare ? toD3d(*s.compare) : D3D12_COMPARISON_FUNC_NEVER;
    const float border = s.borderWhite ? 1.0f : 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = border;
    d.BorderColor[3] = s.borderWhite ? 1.0f : 0.0f;
    d.MinLOD = s.minLod;
    d.MaxLOD = s.maxLod;
    m_handle = dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER).alloc();
    dev.d3dDevice()->CreateSampler(&d, m_handle);
}

D3D12Sampler::~D3D12Sampler() { m_dev.cpuHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER).free(m_handle); }

// --- query pool ------------------------------------------------------------------------------------------
D3D12QueryPool::D3D12QueryPool(D3D12Device& dev, QueryType type, std::uint32_t count) : m_dev(dev) {
    m_type = type;
    m_count = count;
    m_lastQueue.assign(count, QueueType::Graphics);
    D3D12_QUERY_HEAP_DESC d{};
    d.Count = count;
    switch (type) {
    case QueryType::Timestamp: d.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; break;
    case QueryType::Occlusion: d.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; break;
    case QueryType::PipelineStatistics: d.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; break;
    }
    dev.check(dev.d3dDevice()->CreateQueryHeap(&d, __uuidof(ID3D12QueryHeap), m_heap.putVoid()), "CreateQueryHeap");
    if (type == QueryType::Timestamp && dev.copyQueueTimestamps()) {
        d.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
        dev.check(dev.d3dDevice()->CreateQueryHeap(&d, __uuidof(ID3D12QueryHeap), m_copyHeap.putVoid()),
                  "CreateQueryHeap(copy queue)");
    }
    m_readback = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{static_cast<std::uint64_t>(count) * valuesPerQuery() * sizeof(std::uint64_t),
                        BufferUsage::CopyDst, MemoryType::Readback, 0, "query-readback"});
}

D3D12QueryPool::~D3D12QueryPool() {
    Com<ID3D12QueryHeap> a = m_heap, b = m_copyHeap;
    m_dev.deferDestroy([a, b] {});
}

ID3D12QueryHeap* D3D12QueryPool::heap(QueueType queue) {
    if (queue != QueueType::Copy) return m_heap.get();
    if (!m_copyHeap) throw Error("this GPU cannot write timestamps on its copy queue");
    return m_copyHeap.get();
}

D3D12_QUERY_TYPE D3D12QueryPool::queryType() const {
    switch (m_type) {
    case QueryType::Timestamp: return D3D12_QUERY_TYPE_TIMESTAMP;
    case QueryType::Occlusion: return D3D12_QUERY_TYPE_OCCLUSION;
    case QueryType::PipelineStatistics: return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    }
    return D3D12_QUERY_TYPE_TIMESTAMP;
}

bool D3D12QueryPool::read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) {
    const auto* raw = static_cast<const std::uint64_t*>(m_readback->mapped());
    if (m_type == QueryType::PipelineStatistics) {
        // D3D12 order -> RHI order: IA vertices, IA primitives, VS, clipper primitives, PS, CS invocations.
        constexpr std::uint32_t kMap[6] = {0, 1, 2, 6, 7, 10};
        for (std::uint32_t i = 0; i < count; ++i)
            for (std::uint32_t k = 0; k < 6; ++k) {
                const std::size_t o = static_cast<std::size_t>(i) * 6 + k;
                if (o < out.size()) out[o] = raw[static_cast<std::size_t>(first + i) * 11 + kMap[k]];
            }
        return true;
    }
    for (std::uint32_t i = 0; i < count && i < out.size(); ++i) {
        const std::uint64_t v = raw[first + i];
        if (m_type == QueryType::Timestamp) {
            const std::uint64_t freq = m_dev.timestampFrequency(m_lastQueue[first + i]);
            out[i] = freq ? static_cast<std::uint64_t>(static_cast<double>(v) * 1e9 / static_cast<double>(freq)) : 0;
        } else {
            out[i] = v;
        }
    }
    return true;
}

// --- texture table ---------------------------------------------------------------------------------------
D3D12TextureTable::D3D12TextureTable(D3D12Device& dev, std::uint32_t capacity) : m_dev(dev) {
    m_capacity = capacity;
    m_first = dev.viewHeap().allocRange(capacity);
    // Unset entries read as zero instead of whatever the heap held.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullDesc.Texture2D.MipLevels = 1;
    for (std::uint32_t i = 0; i < capacity; ++i)
        dev.d3dDevice()->CreateShaderResourceView(nullptr, &nullDesc, dev.viewHeap().cpu(m_first + i));
}

D3D12TextureTable::~D3D12TextureTable() {
    D3D12Device* dev = &m_dev;
    const std::uint32_t first = m_first, count = m_capacity;
    m_dev.deferDestroy([dev, first, count] { dev->viewHeap().freeRange(first, count); });
}

void D3D12TextureTable::set(std::uint32_t index, Texture* texture) {
    if (index >= m_capacity) throw Error(std::format("texture table index {} out of range ({})", index, m_capacity));
    auto* t = static_cast<D3D12Texture*>(texture);
    m_dev.d3dDevice()->CopyDescriptorsSimple(1, m_dev.viewHeap().cpu(m_first + index), t->srv({}),
                                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12TextureTable::gpuHandle() const { return m_dev.viewHeap().gpu(m_first); }

// --- acceleration structures -------------------------------------------------------------------------------
D3D12Accel::D3D12Accel(D3D12Device& dev, bool topLevel, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info,
                       std::uint32_t maxInstances, const char* name)
    : m_maxInstances(maxInstances) {
    m_topLevel = topLevel;
    m_size = info.ResultDataMaxSizeInBytes;
    m_scratch = std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);
    m_storageBuffer = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{m_size, BufferUsage::AccelStorage, MemoryType::Device, 0, name});
    m_scratch = std::max<std::uint64_t>(m_scratch, 256);
    m_scratch = alignUp(m_scratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    m_scratchBuffer = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{m_scratch, BufferUsage::Storage, MemoryType::Device, 0, "accel-scratch"});
}

} // namespace gt::rhi::d3d12
