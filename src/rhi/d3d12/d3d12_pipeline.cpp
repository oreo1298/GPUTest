#include "d3d12_device.h"

#include <chrono>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

D3D12_BLEND toD3d(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InvDstColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP toD3d(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return D3D12_BLEND_OP_ADD;
    case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min: return D3D12_BLEND_OP_MIN;
    case BlendOp::Max: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

D3D12_SHADER_BYTECODE bytecode(D3D12Device& dev, const ShaderRef& ref) {
    if (!ref.valid()) return {};
    const auto code = dev.shaderBytecode(ref);
    return {code.data(), code.size()};
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/// One pipeline-state-stream subobject (the layout d3dx12.h uses).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

struct MeshPipelineStream {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sampleMask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depthFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
};

} // namespace

D3D12Pipeline::D3D12Pipeline(D3D12Device& dev, const GraphicsPipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = false;
    m_layoutDesc = d.layout;
    const bool mesh = d.mesh.valid();
    if (mesh && !dev.features().meshShader) throw Error("mesh shaders are not supported by this GPU");
    if (d.variableRate && !dev.features().variableRateShading)
        throw Error("variable-rate shading is not supported by this GPU");
    m_root = &dev.rootSignature(d.layout, !mesh);
    for (const VertexBinding& b : d.vertexBindings)
        if (b.binding < m_strides.size()) m_strides[b.binding] = b.stride;

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode = d.raster.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rs.CullMode = d.raster.cull == CullMode::None    ? D3D12_CULL_MODE_NONE
                  : d.raster.cull == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                     : D3D12_CULL_MODE_BACK;
    rs.FrontCounterClockwise = d.raster.frontCounterClockwise;
    rs.DepthBias = static_cast<INT>(d.raster.depthBias >= 0.0f ? d.raster.depthBias + 0.5f : d.raster.depthBias - 0.5f);
    rs.SlopeScaledDepthBias = d.raster.depthBiasSlope;
    rs.DepthClipEnable = !d.raster.depthClamp;
    rs.MultisampleEnable = d.sampleCount > 1;
    rs.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (std::size_t i = 0; i < d.colorFormats.size() && i < 8; ++i) {
        const BlendAttachment b = i < d.blend.size() ? d.blend[i] : BlendAttachment{};
        D3D12_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[i];
        t.BlendEnable = b.enable;
        t.SrcBlend = toD3d(b.srcColor);
        t.DestBlend = toD3d(b.dstColor);
        t.BlendOp = toD3d(b.colorOp);
        t.SrcBlendAlpha = toD3d(b.srcAlpha);
        t.DestBlendAlpha = toD3d(b.dstAlpha);
        t.BlendOpAlpha = toD3d(b.alphaOp);
        t.LogicOp = D3D12_LOGIC_OP_NOOP;
        t.RenderTargetWriteMask = b.writeMask;
    }

    D3D12_DEPTH_STENCIL_DESC ds{};
    if (d.depthFormat != Format::Unknown) {
        ds.DepthEnable = d.depth.test;
        ds.DepthWriteMask = d.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = toD3d(d.depth.compare);
    }
    ds.FrontFace = ds.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                  D3D12_COMPARISON_FUNC_ALWAYS};

    D3D12_RT_FORMAT_ARRAY targets{};
    targets.NumRenderTargets = static_cast<UINT>(std::min<std::size_t>(d.colorFormats.size(), 8));
    for (UINT i = 0; i < targets.NumRenderTargets; ++i) targets.RTFormats[i] = toDxgi(d.colorFormats[i]);
    const DXGI_FORMAT depthFormat = toDxgi(d.depthFormat);
    const DXGI_SAMPLE_DESC samples{d.sampleCount, 0};

    HRESULT hr = S_OK;
    if (mesh) {
        ID3D12Device2* dev2 = dev.device2();
        if (!dev2) throw Error("mesh pipelines need ID3D12Device2");
        MeshPipelineStream s{};
        s.root.value = m_root->signature.get();
        s.as.value = bytecode(dev, d.task);
        s.ms.value = bytecode(dev, d.mesh);
        s.ps.value = bytecode(dev, d.fragment);
        s.blend.value = bd;
        s.sampleMask.value = UINT_MAX;
        s.raster.value = rs;
        s.depth.value = ds;
        s.targets.value = targets;
        s.depthFormat.value = depthFormat;
        s.samples.value = samples;
        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
        hr = dev2->CreatePipelineState(&sd, __uuidof(ID3D12PipelineState), m_pso.putVoid());
    } else {
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
        for (const VertexAttribute& a : d.attributes) {
            bool perInstance = false;
            for (const VertexBinding& b : d.vertexBindings)
                if (b.binding == a.binding) perInstance = b.perInstance;
            inputs.push_back({"ATTRIB", a.location, toDxgi(a.format), a.binding, a.offset,
                              perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                          : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                              perInstance ? 1u : 0u});
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_root->signature.get();
        pd.VS = bytecode(dev, d.vertex);
        pd.PS = bytecode(dev, d.fragment);
        pd.HS = bytecode(dev, d.hull);
        pd.DS = bytecode(dev, d.domain);
        pd.GS = bytecode(dev, d.geometry);
        pd.BlendState = bd;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState = rs;
        pd.DepthStencilState = ds;
        pd.InputLayout = {inputs.data(), static_cast<UINT>(inputs.size())};
        switch (d.topology) {
        case PrimitiveTopology::TriangleList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case PrimitiveTopology::TriangleStrip:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case PrimitiveTopology::LineList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            m_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case PrimitiveTopology::LineStrip:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            m_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case PrimitiveTopology::PointList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            m_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case PrimitiveTopology::PatchList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            m_topology = static_cast<D3D_PRIMITIVE_TOPOLOGY>(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST +
                                                             std::max(1u, d.patchControlPoints) - 1);
            break;
        }
        pd.NumRenderTargets = targets.NumRenderTargets;
        for (UINT i = 0; i < targets.NumRenderTargets; ++i) pd.RTVFormats[i] = targets.RTFormats[i];
        pd.DSVFormat = depthFormat;
        pd.SampleDesc = samples;
        hr = dev.d3dDevice()->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), m_pso.putVoid());
    }
    dev.check(hr, std::format("CreateGraphicsPipelineState({})", d.name).c_str());
    dev.setName(m_pso.get(), d.name);
    m_creationMs = msSince(t0);
}

D3D12Pipeline::D3D12Pipeline(D3D12Device& dev, const ComputePipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = true;
    m_layoutDesc = d.layout;
    m_root = &dev.rootSignature(d.layout, false);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_root->signature.get();
    pd.CS = bytecode(dev, d.compute);
    dev.check(dev.d3dDevice()->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), m_pso.putVoid()),
              std::format("CreateComputePipelineState({})", d.name).c_str());
    dev.setName(m_pso.get(), d.name);
    m_creationMs = msSince(t0);
}

D3D12Pipeline::~D3D12Pipeline() {
    Com<ID3D12PipelineState> p = m_pso;
    m_dev.deferDestroy([p] {});
}

} // namespace gt::rhi::d3d12
