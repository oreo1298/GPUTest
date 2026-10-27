#include "d3d11_device.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace gt::rhi::d3d11 {
namespace {

D3D11_BLEND toD3d(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero: return D3D11_BLEND_ZERO;
    case BlendFactor::One: return D3D11_BLEND_ONE;
    case BlendFactor::SrcColor: return D3D11_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor: return D3D11_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstColor: return D3D11_BLEND_DEST_COLOR;
    case BlendFactor::InvDstColor: return D3D11_BLEND_INV_DEST_COLOR;
    case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP toD3d(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return D3D11_BLEND_OP_ADD;
    case BlendOp::Subtract: return D3D11_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min: return D3D11_BLEND_OP_MIN;
    case BlendOp::Max: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::uint32_t bit(std::uint32_t slot) { return 1u << slot; }

const char* stageName(StageIndex s) {
    constexpr const char* kNames[] = {"vertex", "hull", "domain", "geometry", "pixel", "compute"};
    return kNames[s];
}

} // namespace

void D3D11Pipeline::reflect(StageIndex stage, std::span<const std::uint8_t> code) {
    m_present[stage] = true;
    StageSlots& st = m_slots[stage];
    for (const dxbc::ResourceBinding& b : dxbc::reflect(code)) {
        for (std::uint32_t i = 0; i < b.count; ++i) {
            const std::uint32_t slot = b.slot + i;
            const bool cb = b.kind == dxbc::ResourceKind::ConstantBuffer;
            if (slot >= (cb ? kCbSlots : kSlots))
                throw Error(std::format("pipeline '{}': {} shader register {}{} is beyond the {} slots the "
                                        "Direct3D 11 backend binds",
                                        m_name, stageName(stage),
                                        cb ? 'b' : b.kind == dxbc::ResourceKind::Sampler ? 's' : b.uav ? 'u' : 't',
                                        slot, cb ? kCbSlots : kSlots));
            if (b.kind == dxbc::ResourceKind::TypedBuffer || b.kind == dxbc::ResourceKind::Other)
                throw Error(std::format("pipeline '{}': '{}' ({} shader) is a resource type the Direct3D 11 backend "
                                        "does not bind (typed buffers and the like)",
                                        m_name, b.name, stageName(stage)));
            if (cb) {
                st.cb |= bit(slot);
            } else if (b.kind == dxbc::ResourceKind::Sampler) {
                st.sampler |= bit(slot);
            } else {
                SlotInfo& info = b.uav ? m_uav[slot] : m_srv[slot];
                const StageSlots& all = m_used;
                const bool seen = (b.uav ? all.uav : all.srv) & bit(slot);
                if (seen && (info.kind != b.kind || info.stride != b.stride))
                    throw Error(std::format("pipeline '{}': register {}{} is declared differently in two stages",
                                            m_name, b.uav ? 'u' : 't', slot));
                info = {b.kind, b.stride};
                (b.uav ? st.uav : st.srv) |= bit(slot);
            }
            m_used.cb |= st.cb;
            m_used.srv |= st.srv;
            m_used.uav |= st.uav;
            m_used.sampler |= st.sampler;
        }
    }
}

D3D11Pipeline::D3D11Pipeline(D3D11Device& dev, const GraphicsPipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = false;
    if (d.mesh.valid() || d.task.valid()) throw Error("mesh shaders are not available on Direct3D 11");
    if (d.variableRate) throw Error("variable-rate shading is not available on Direct3D 11");
    if (!d.vertex.valid()) throw Error(std::format("pipeline '{}' has no vertex shader", d.name));
    ID3D11Device1* device = dev.d3d();
    auto check = [&](HRESULT hr, const char* what) { dev.check(hr, std::format("{}({})", what, d.name).c_str()); };

    const auto vs = dev.shaderBytecode(d.vertex);
    check(device->CreateVertexShader(vs.data(), vs.size(), nullptr, m_vs.put()), "CreateVertexShader");
    reflect(kVS, vs);
    if (d.hull.valid()) {
        const auto code = dev.shaderBytecode(d.hull);
        check(device->CreateHullShader(code.data(), code.size(), nullptr, m_hs.put()), "CreateHullShader");
        reflect(kHS, code);
    }
    if (d.domain.valid()) {
        const auto code = dev.shaderBytecode(d.domain);
        check(device->CreateDomainShader(code.data(), code.size(), nullptr, m_ds.put()), "CreateDomainShader");
        reflect(kDS, code);
    }
    if (d.geometry.valid()) {
        const auto code = dev.shaderBytecode(d.geometry);
        check(device->CreateGeometryShader(code.data(), code.size(), nullptr, m_gs.put()), "CreateGeometryShader");
        reflect(kGS, code);
    }
    if (d.fragment.valid()) {
        const auto code = dev.shaderBytecode(d.fragment);
        check(device->CreatePixelShader(code.data(), code.size(), nullptr, m_ps.put()), "CreatePixelShader");
        reflect(kPS, code);
    }
    for (StageIndex s : {kVS, kHS, kDS, kGS})
        if (m_slots[s].uav && dev.featureLevel() < D3D_FEATURE_LEVEL_11_1)
            throw Error(std::format("pipeline '{}': UAVs in the {} shader need feature level 11_1", d.name, stageName(s)));

    // Vertex input: semantic ATTRIB<location> (see the shaders' [[vk::location]] declarations).
    for (const VertexBinding& b : d.vertexBindings) {
        if (b.binding >= m_strides.size()) throw Error("vertex binding out of range");
        m_strides[b.binding] = b.stride;
        m_vertexBindings |= bit(b.binding);
    }
    if (!d.attributes.empty()) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> inputs;
        for (const VertexAttribute& a : d.attributes) {
            bool perInstance = false;
            for (const VertexBinding& b : d.vertexBindings)
                if (b.binding == a.binding) perInstance = b.perInstance;
            inputs.push_back({"ATTRIB", a.location, toDxgi(a.format), a.binding, a.offset,
                              perInstance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA,
                              perInstance ? 1u : 0u});
        }
        check(device->CreateInputLayout(inputs.data(), static_cast<UINT>(inputs.size()), vs.data(), vs.size(),
                                        m_inputLayout.put()),
              "CreateInputLayout");
    }

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = d.raster.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rs.CullMode = d.raster.cull == CullMode::None    ? D3D11_CULL_NONE
                  : d.raster.cull == CullMode::Front ? D3D11_CULL_FRONT
                                                     : D3D11_CULL_BACK;
    rs.FrontCounterClockwise = d.raster.frontCounterClockwise;
    rs.DepthBias = static_cast<INT>(d.raster.depthBias >= 0.0f ? d.raster.depthBias + 0.5f : d.raster.depthBias - 0.5f);
    rs.SlopeScaledDepthBias = d.raster.depthBiasSlope;
    rs.DepthClipEnable = !d.raster.depthClamp;
    rs.ScissorEnable = TRUE; // render passes set a full-target scissor
    rs.MultisampleEnable = d.sampleCount > 1;
    check(device->CreateRasterizerState(&rs, m_raster.put()), "CreateRasterizerState");

    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (std::size_t i = 0; i < 8; ++i) {
        const BlendAttachment b = i < d.blend.size() ? d.blend[i] : BlendAttachment{};
        D3D11_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[i];
        t.BlendEnable = b.enable;
        t.SrcBlend = toD3d(b.srcColor);
        t.DestBlend = toD3d(b.dstColor);
        t.BlendOp = toD3d(b.colorOp);
        t.SrcBlendAlpha = toD3d(b.srcAlpha);
        t.DestBlendAlpha = toD3d(b.dstAlpha);
        t.BlendOpAlpha = toD3d(b.alphaOp);
        t.RenderTargetWriteMask = b.writeMask;
    }
    check(device->CreateBlendState(&bd, m_blend.put()), "CreateBlendState");

    D3D11_DEPTH_STENCIL_DESC ds{};
    if (d.depthFormat != Format::Unknown) {
        ds.DepthEnable = d.depth.test;
        ds.DepthWriteMask = d.depth.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = toD3d(d.depth.compare);
    } else {
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                  D3D11_COMPARISON_ALWAYS};
    check(device->CreateDepthStencilState(&ds, m_depth.put()), "CreateDepthStencilState");
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // for passes with a read-only depth attachment
    check(device->CreateDepthStencilState(&ds, m_depthReadOnly.put()), "CreateDepthStencilState");

    switch (d.topology) {
    case PrimitiveTopology::TriangleList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case PrimitiveTopology::TriangleStrip: m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case PrimitiveTopology::LineList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case PrimitiveTopology::LineStrip: m_topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case PrimitiveTopology::PointList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case PrimitiveTopology::PatchList:
        m_topology = static_cast<D3D11_PRIMITIVE_TOPOLOGY>(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST +
                                                           std::max(1u, d.patchControlPoints) - 1);
        break;
    }
    m_creationMs = msSince(t0);
}

D3D11Pipeline::D3D11Pipeline(D3D11Device& dev, const ComputePipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = true;
    const auto cs = dev.shaderBytecode(d.compute);
    dev.check(dev.d3d()->CreateComputeShader(cs.data(), cs.size(), nullptr, m_cs.put()),
              std::format("CreateComputeShader({})", d.name).c_str());
    reflect(kCS, cs);
    m_creationMs = msSince(t0);
}

void D3D11Pipeline::bindState(ID3D11DeviceContext* ctx, bool depthReadOnly) const {
    if (m_isCompute) {
        ctx->CSSetShader(m_cs.get(), nullptr, 0);
        return;
    }
    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ctx->HSSetShader(m_hs.get(), nullptr, 0);
    ctx->DSSetShader(m_ds.get(), nullptr, 0);
    ctx->GSSetShader(m_gs.get(), nullptr, 0);
    ctx->PSSetShader(m_ps.get(), nullptr, 0);
    ctx->IASetInputLayout(m_inputLayout.get());
    ctx->IASetPrimitiveTopology(m_topology);
    ctx->RSSetState(m_raster.get());
    ctx->OMSetBlendState(m_blend.get(), nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(depthReadOnly ? m_depthReadOnly.get() : m_depth.get(), 0);
}

} // namespace gt::rhi::d3d11
