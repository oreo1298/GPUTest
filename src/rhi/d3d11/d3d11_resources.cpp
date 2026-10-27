#include "d3d11_device.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace gt::rhi::d3d11 {

// --- buffers ----------------------------------------------------------------------------------------------------
namespace {
UINT paddedWidth(std::uint64_t size, std::uint32_t stride) {
    const std::uint64_t unit = stride ? stride : 16;
    return static_cast<UINT>((std::max<std::uint64_t>(size, unit) + unit - 1) / unit * unit);
}
} // namespace

D3D11Buffer::D3D11Buffer(D3D11Device& dev, const BufferDesc& desc, const void* data) : m_dev(dev) {
    m_desc = desc;
    const auto u = desc.usage;
    if (any(u & BufferUsage::Uniform) && any(u & (BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Storage)))
        throw Error(std::format("buffer '{}': Direct3D 11 constant buffers cannot also be vertex, index or "
                                "storage buffers",
                                desc.name));
    if (desc.memory != MemoryType::Device) {
        m_shadow.resize(paddedWidth(desc.size, 0));
        if (data) std::memcpy(m_shadow.data(), data, desc.size);
    }
    if (isReadback()) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = static_cast<UINT>(m_shadow.size());
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev.check(dev.d3d()->CreateBuffer(&sd, nullptr, m_staging.put()), "CreateBuffer(readback staging)");
        return;
    }
    // Structured views need a structured buffer, which cannot also feed the input assembler or
    // indirect draws; storage buffers without such uses take the layout of their first view (a
    // declared structStride only picks the layout for uses that do not ask for one).
    m_structurable = any(u & BufferUsage::Storage) &&
                     !any(u & (BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect));
    if (desc.memory == MemoryType::Device && data) {
        m_initial.resize(desc.size);
        std::memcpy(m_initial.data(), data, desc.size);
    }
    if (!m_structurable) create(0);
}

void D3D11Buffer::create(std::uint32_t stride) {
    const auto u = m_desc.usage;
    // A structured buffer cannot also be a vertex, index or indirect buffer: those stay raw (a
    // declared structStride is then only a hint for D3D12).
    if (any(u & (BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect))) stride = 0;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = paddedWidth(m_desc.size, stride);
    bd.Usage = D3D11_USAGE_DEFAULT;
    if (any(u & BufferUsage::Uniform)) {
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    } else {
        if (any(u & BufferUsage::Vertex)) bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        if (any(u & BufferUsage::Index)) bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        if (any(u & BufferUsage::Storage)) {
            bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (stride) {
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = stride;
            } else {
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            }
        }
        if (any(u & BufferUsage::Indirect)) bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    }
    // Initial contents: the CPU copy (Upload memory) or the data given at creation.
    std::vector<std::byte> init;
    const std::vector<std::byte>& src = m_shadow.empty() ? m_initial : m_shadow;
    if (!src.empty()) {
        init.assign(bd.ByteWidth, std::byte{0});
        std::memcpy(init.data(), src.data(), std::min<std::size_t>(src.size(), bd.ByteWidth));
    }
    const D3D11_SUBRESOURCE_DATA data{init.data(), 0, 0};
    Com<ID3D11Buffer> fresh;
    m_dev.check(m_dev.d3d()->CreateBuffer(&bd, init.empty() ? nullptr : &data, fresh.put()),
                std::format("CreateBuffer('{}', {} bytes)", m_desc.name, m_desc.size).c_str());
    m_dev.setName(fresh.get(), m_desc.name);
    if (m_buffer) {
        // Re-created for another layout: keep the GPU contents.
        const D3D11_BOX box{0, 0, 0, std::min(m_width, bd.ByteWidth), 1, 1};
        m_dev.context()->CopySubresourceRegion(fresh.get(), 0, 0, 0, 0, m_buffer.get(), 0, &box);
        m_srvs.clear();
        m_uavs.clear();
        m_dev.bufferRecreated();
    }
    m_buffer = std::move(fresh);
    m_width = bd.ByteWidth;
    m_stride = stride;
    m_initial.clear();
    m_initial.shrink_to_fit();
    m_syncedSubmission = 0;
}

ID3D11Buffer* D3D11Buffer::gpu(std::uint32_t stride) {
    if (isReadback()) return nullptr;
    if (stride == kAnyLayout) {
        if (!m_buffer) create(m_desc.structStride);
        return m_buffer.get();
    }
    if (m_buffer && stride == m_stride) return m_buffer.get();
    if (!m_structurable && stride)
        throw Error(std::format("buffer '{}' is a vertex, index or indirect buffer: Direct3D 11 cannot view it as a "
                                "structured buffer of stride {} (read it as a ByteAddressBuffer)",
                                m_desc.name, stride));
    if (m_structurable) create(stride); // first use, or another layout: re-created with its contents
    return m_buffer.get();
}

bool D3D11Buffer::writePending(std::uint64_t offset, const void* data, std::uint64_t size) {
    if (m_buffer || !m_structurable || m_desc.memory != MemoryType::Device) return false;
    if (m_initial.size() < m_desc.size) m_initial.resize(m_desc.size);
    std::memcpy(m_initial.data() + offset, data, size);
    return true;
}

bool D3D11Buffer::fillPending(std::uint64_t offset, std::uint64_t size, std::uint32_t value) {
    if (m_buffer || !m_structurable || m_desc.memory != MemoryType::Device) return false;
    if (m_initial.size() < m_desc.size) m_initial.resize(m_desc.size);
    for (std::uint64_t i = 0; i + 4 <= size; i += 4) std::memcpy(m_initial.data() + offset + i, &value, 4);
    return true;
}

void D3D11Buffer::syncForSubmission(std::uint64_t submission) {
    if (!isUpload() || m_syncedSubmission == submission) return;
    ID3D11Buffer* b = gpu();
    if (any(m_desc.usage & BufferUsage::Uniform)) {
        m_dev.context()->UpdateSubresource(b, 0, nullptr, m_shadow.data(), 0, 0);
    } else {
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(std::min<std::size_t>(m_shadow.size(), m_width)), 1, 1};
        m_dev.context()->UpdateSubresource(b, 0, &box, m_shadow.data(), 0, 0);
    }
    m_syncedSubmission = submission;
}

void D3D11Buffer::syncRange(std::uint64_t submission, std::uint64_t bytes) {
    if (!bytes) return;
    ID3D11Buffer* b = gpu();
    const D3D11_BOX box{0, 0, 0, static_cast<UINT>(std::min<std::uint64_t>(bytes, m_width)), 1, 1};
    m_dev.context()->UpdateSubresource(b, 0, &box, m_shadow.data(), 0, 0);
    m_syncedSubmission = submission;
}

ID3D11ShaderResourceView* D3D11Buffer::srv(std::uint64_t offset, std::uint64_t size, std::uint32_t stride) {
    ID3D11Buffer* b = gpu(stride);
    if (!size) size = m_desc.size - offset;
    if (auto it = m_srvs.find({offset, size}); it != m_srvs.end()) return it->second.get();
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    if (m_stride) {
        if (offset % m_stride) throw Error(std::format("buffer '{}': view offset {} is not a multiple of the "
                                                       "structure stride {}", m_desc.name, offset, m_stride));
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.BufferEx = {static_cast<UINT>(offset / m_stride), static_cast<UINT>(size / m_stride), 0};
    } else {
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.BufferEx = {static_cast<UINT>(offset / 4), static_cast<UINT>(size / 4), D3D11_BUFFEREX_SRV_FLAG_RAW};
    }
    Com<ID3D11ShaderResourceView> v;
    m_dev.check(m_dev.d3d()->CreateShaderResourceView(b, &d, v.put()), "CreateShaderResourceView(buffer)");
    return m_srvs.emplace(std::make_pair(offset, size), std::move(v)).first->second.get();
}

ID3D11UnorderedAccessView* D3D11Buffer::uav(std::uint64_t offset, std::uint64_t size, std::uint32_t stride) {
    ID3D11Buffer* b = gpu(stride);
    if (!size) size = m_desc.size - offset;
    if (auto it = m_uavs.find({offset, size}); it != m_uavs.end()) return it->second.get();
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    if (m_stride) {
        if (offset % m_stride) throw Error(std::format("buffer '{}': view offset {} is not a multiple of the "
                                                       "structure stride {}", m_desc.name, offset, m_stride));
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.Buffer = {static_cast<UINT>(offset / m_stride), static_cast<UINT>(size / m_stride), 0};
    } else {
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.Buffer = {static_cast<UINT>(offset / 4), static_cast<UINT>(size / 4), D3D11_BUFFER_UAV_FLAG_RAW};
    }
    Com<ID3D11UnorderedAccessView> v;
    m_dev.check(m_dev.d3d()->CreateUnorderedAccessView(b, &d, v.put()), "CreateUnorderedAccessView(buffer)");
    return m_uavs.emplace(std::make_pair(offset, size), std::move(v)).first->second.get();
}

// --- textures --------------------------------------------------------------------------------------------------------
namespace {
std::uint32_t arrayLayers(const TextureDesc& d) {
    if (d.type == TextureType::Tex3D) return 1;
    if (d.type == TextureType::Cube) return std::max(6u, d.depthOrLayers);
    return std::max(1u, d.depthOrLayers);
}
} // namespace

D3D11Texture::D3D11Texture(D3D11Device& dev, const TextureDesc& desc) : m_dev(dev) {
    m_desc = desc;
    if (any(desc.usage & TextureUsage::ShadingRate))
        throw Error(std::format("texture '{}': shading-rate images are not available on Direct3D 11", desc.name));
    const auto u = desc.usage;
    UINT bind = 0;
    if (any(u & TextureUsage::Sampled)) bind |= D3D11_BIND_SHADER_RESOURCE;
    if (any(u & TextureUsage::Storage)) bind |= D3D11_BIND_UNORDERED_ACCESS;
    if (any(u & TextureUsage::RenderTarget)) bind |= D3D11_BIND_RENDER_TARGET;
    if (any(u & TextureUsage::DepthStencil)) bind |= D3D11_BIND_DEPTH_STENCIL;
    const DXGI_FORMAT format = toDxgiResource(desc.format);
    if (desc.type == TextureType::Tex3D) {
        D3D11_TEXTURE3D_DESC d{};
        d.Width = desc.width;
        d.Height = desc.height;
        d.Depth = desc.depthOrLayers;
        d.MipLevels = desc.mipLevels;
        d.Format = format;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        Com<ID3D11Texture3D> t;
        dev.check(dev.d3d()->CreateTexture3D(&d, nullptr, t.put()), "CreateTexture3D");
        m_resource = t.as<ID3D11Resource>();
    } else {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = desc.width;
        d.Height = desc.height;
        d.MipLevels = desc.mipLevels;
        d.ArraySize = arrayLayers(desc);
        d.Format = format;
        d.SampleDesc = {desc.sampleCount, 0};
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        if (desc.type == TextureType::Cube) d.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        Com<ID3D11Texture2D> t;
        dev.check(dev.d3d()->CreateTexture2D(&d, nullptr, t.put()),
                  std::format("CreateTexture2D('{}', {}x{} {})", desc.name, desc.width, desc.height,
                              formatInfo(desc.format).name)
                      .c_str());
        m_resource = t.as<ID3D11Resource>();
    }
}

D3D11Texture::D3D11Texture(D3D11Device& dev, const TextureDesc& desc, Com<ID3D11Texture2D> existing) : m_dev(dev) {
    m_desc = desc;
    m_resource = existing.as<ID3D11Resource>();
}

std::uint32_t D3D11Texture::layers() const { return arrayLayers(m_desc); }

UINT D3D11Texture::subresource(std::uint32_t mip, std::uint32_t layer) const {
    return D3D11CalcSubresource(mip, m_desc.type == TextureType::Tex3D ? 0 : layer, std::max(1u, m_desc.mipLevels));
}

ID3D11ShaderResourceView* D3D11Texture::srv(Subresource sub) {
    const auto key = std::make_tuple(sub.baseMip, sub.mipCount, sub.baseLayer, sub.layerCount);
    if (auto it = m_srvs.find(key); it != m_srvs.end()) return it->second.get();
    const std::uint32_t mips = sub.mipCount ? sub.mipCount : m_desc.mipLevels - sub.baseMip;
    const std::uint32_t layers = sub.layerCount ? sub.layerCount : arrayLayers(m_desc) - sub.baseLayer;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = toDxgiSrv(m_desc.format);
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {sub.baseMip, mips};
    } else if (m_desc.type == TextureType::Cube && layers == 6) {
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        d.TextureCube = {sub.baseMip, mips};
    } else if (m_desc.sampleCount > 1) {
        if (layers > 1 || m_desc.type == TextureType::Tex2DArray) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {sub.baseLayer, layers};
        } else {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
    } else if (layers > 1 || m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {sub.baseMip, mips, sub.baseLayer, layers};
    } else {
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D = {sub.baseMip, mips};
    }
    Com<ID3D11ShaderResourceView> v;
    m_dev.check(m_dev.d3d()->CreateShaderResourceView(m_resource.get(), &d, v.put()), "CreateShaderResourceView");
    return m_srvs.emplace(key, std::move(v)).first->second.get();
}

ID3D11UnorderedAccessView* D3D11Texture::uav(std::uint32_t mip) {
    if (auto it = m_uavs.find(mip); it != m_uavs.end()) return it->second.get();
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = {mip, 0, static_cast<UINT>(-1)};
    } else if (arrayLayers(m_desc) > 1 || m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {mip, 0, arrayLayers(m_desc)};
    } else {
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D = {mip};
    }
    Com<ID3D11UnorderedAccessView> v;
    m_dev.check(m_dev.d3d()->CreateUnorderedAccessView(m_resource.get(), &d, v.put()), "CreateUnorderedAccessView");
    return m_uavs.emplace(mip, std::move(v)).first->second.get();
}

ID3D11RenderTargetView* D3D11Texture::rtv(std::uint32_t mip, std::uint32_t layer) {
    const auto key = std::make_pair(mip, layer);
    if (auto it = m_rtvs.find(key); it != m_rtvs.end()) return it->second.get();
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    if (m_desc.type == TextureType::Tex3D) {
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D = {mip, layer, 1};
    } else if (m_desc.sampleCount > 1) {
        d.ViewDimension = m_desc.type == TextureType::Tex2D ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                            : D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        if (m_desc.type != TextureType::Tex2D) d.Texture2DMSArray = {layer, 1};
    } else if (m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {mip, layer, 1};
    } else {
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D = {mip};
    }
    Com<ID3D11RenderTargetView> v;
    m_dev.check(m_dev.d3d()->CreateRenderTargetView(m_resource.get(), &d, v.put()), "CreateRenderTargetView");
    return m_rtvs.emplace(key, std::move(v)).first->second.get();
}

ID3D11DepthStencilView* D3D11Texture::dsv(bool readOnly) {
    auto& slot = m_dsvs[readOnly ? 1 : 0];
    if (slot) return slot.get();
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = toDxgi(m_desc.format);
    if (m_desc.sampleCount > 1) {
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    } else if (m_desc.type != TextureType::Tex2D) {
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {0, 0, 1};
    } else {
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    }
    if (readOnly) {
        d.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        if (formatInfo(m_desc.format).stencil) d.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
    }
    m_dev.check(m_dev.d3d()->CreateDepthStencilView(m_resource.get(), &d, slot.put()), "CreateDepthStencilView");
    return slot.get();
}

// --- samplers -----------------------------------------------------------------------------------------------------
namespace {
D3D11_TEXTURE_ADDRESS_MODE address(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap: return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case AddressMode::Border: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}
} // namespace

D3D11Sampler::D3D11Sampler(D3D11Device& dev, const SamplerDesc& s) {
    D3D11_SAMPLER_DESC d{};
    const bool aniso = s.maxAnisotropy > 1.0f;
    const bool cmp = s.compare.has_value();
    if (aniso) {
        d.Filter = cmp ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
    } else {
        const int f = (s.minFilter == Filter::Linear ? 0x10 : 0) | (s.magFilter == Filter::Linear ? 0x4 : 0) |
                      (s.mipFilter == Filter::Linear ? 0x1 : 0);
        d.Filter = static_cast<D3D11_FILTER>(f | (cmp ? 0x80 : 0));
    }
    d.AddressU = address(s.addressU);
    d.AddressV = address(s.addressV);
    d.AddressW = address(s.addressW);
    d.MipLODBias = s.mipLodBias;
    d.MaxAnisotropy = aniso ? static_cast<UINT>(std::clamp(s.maxAnisotropy, 1.0f, 16.0f)) : 1u;
    d.ComparisonFunc = cmp ? toD3d(*s.compare) : D3D11_COMPARISON_NEVER;
    const float b = s.borderWhite ? 1.0f : 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = b;
    d.MinLOD = s.minLod;
    d.MaxLOD = s.maxLod;
    dev.check(dev.d3d()->CreateSamplerState(&d, m_state.put()), "CreateSamplerState");
}

// --- queries ----------------------------------------------------------------------------------------------------------
D3D11QueryPool::D3D11QueryPool(D3D11Device& dev, QueryType type, std::uint32_t count) : m_dev(dev) {
    if (type == QueryType::PipelineStatistics)
        throw Error("pipeline statistics queries are not implemented on Direct3D 11");
    m_type = type;
    m_count = count;
    m_queries.resize(count);
    D3D11_QUERY_DESC qd{type == QueryType::Timestamp ? D3D11_QUERY_TIMESTAMP : D3D11_QUERY_OCCLUSION, 0};
    for (auto& q : m_queries) dev.check(dev.d3d()->CreateQuery(&qd, q.put()), "CreateQuery");
}

bool D3D11QueryPool::read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) {
    const double toNs = 1e9 / m_dev.timestampFrequency();
    for (std::uint32_t i = 0; i < count; ++i) {
        UINT64 v = 0;
        if (m_dev.context()->GetData(m_queries[first + i].get(), &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return false;
        out[i] = m_type == QueryType::Timestamp ? static_cast<std::uint64_t>(static_cast<double>(v) * toNs) : v;
    }
    return true;
}

} // namespace gt::rhi::d3d11
