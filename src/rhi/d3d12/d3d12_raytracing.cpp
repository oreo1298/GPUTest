#include "d3d12_device.h"

#include <cstring>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(const BlasDesc& desc) {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out;
    for (const AccelTriangles& g : desc.geometries) {
        D3D12_RAYTRACING_GEOMETRY_DESC d{};
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        auto* vb = static_cast<D3D12Buffer*>(g.vertices);
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.VertexCount = g.vertexCount;
        d.Triangles.VertexBuffer = {vb ? vb->address() + g.vertexOffset : 0, g.vertexStride};
        if (g.indices && g.indexCount) {
            d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            d.Triangles.IndexCount = g.indexCount;
            d.Triangles.IndexBuffer = static_cast<D3D12Buffer*>(g.indices)->address() + g.indexOffset;
        }
        out.push_back(d);
    }
    return out;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(const BlasDesc& desc,
                                                                const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geo) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = desc.fastBuild ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                              : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (desc.allowUpdate) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.NumDescs = static_cast<UINT>(geo.size());
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = geo.data();
    return in;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs(std::uint32_t count, bool allowUpdate) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allowUpdate) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.NumDescs = count;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    return in;
}

} // namespace

std::unique_ptr<AccelerationStructure> D3D12Device::createBlas(const BlasDesc& desc) {
    if (!m_features.rayQuery || !m_device5) throw Error("ray tracing is not supported by this GPU");
    for (const AccelTriangles& g : desc.geometries)
        if (!g.opacity.empty()) throw Error("opacity micromaps are not supported by the Direct3D 12 backend yet");
    const auto geo = geometryDescs(desc);
    const auto inputs = blasInputs(desc, geo);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    auto a = std::make_unique<D3D12Accel>(*this, false, info, 0, desc.name);
    a->allowUpdate = desc.allowUpdate;
    return a;
}

std::unique_ptr<AccelerationStructure> D3D12Device::createTlas(std::uint32_t maxInstances, bool allowUpdate) {
    if (!m_features.rayQuery || !m_device5) throw Error("ray tracing is not supported by this GPU");
    const auto inputs = tlasInputs(maxInstances, allowUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    auto a = std::make_unique<D3D12Accel>(*this, true, info, maxInstances, "tlas");
    a->allowUpdate = allowUpdate;
    return a;
}

void D3D12CommandList::buildBlas(AccelerationStructure* blas, const BlasDesc& desc, bool update) {
    requireRecording("buildBlas");
    if (!m_list4) throw Error("ray tracing is not supported by this command list");
    auto* a = static_cast<D3D12Accel*>(blas);
    for (const AccelTriangles& g : desc.geometries) {
        if (g.vertices) require(static_cast<D3D12Buffer*>(g.vertices), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g.indices) require(static_cast<D3D12Buffer*>(g.indices), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    require(a->scratchBuffer(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flushBarriers();
    const auto geo = geometryDescs(desc);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = blasInputs(desc, geo);
    if (update) {
        if (!a->allowUpdate) throw Error("buildBlas: structure was not created with allowUpdate");
        bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        bd.SourceAccelerationStructureData = a->address();
    }
    bd.DestAccelerationStructureData = a->address();
    bd.ScratchAccelerationStructureData = a->scratchAddress();
    m_list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    m_accelDirty = true;
}

void D3D12CommandList::buildTlas(AccelerationStructure* tlas, std::span<const AccelInstance> instances, bool update) {
    requireRecording("buildTlas");
    if (!m_list4) throw Error("ray tracing is not supported by this command list");
    auto* a = static_cast<D3D12Accel*>(tlas);
    if (instances.size() > a->maxInstances())
        throw Error(std::format("buildTlas: {} instances, structure holds {}", instances.size(), a->maxInstances()));
    if (m_accelDirty) accelBarrier(); // bottom-level builds must finish first
    const TransientAllocation t = allocTransient(instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                                 D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
    auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(t.cpu);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const AccelInstance& in = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        std::memcpy(d.Transform, in.transform, sizeof(d.Transform));
        d.InstanceID = in.id & 0xFFFFFFu;
        d.InstanceMask = in.mask;
        d.InstanceContributionToHitGroupIndex = in.hitGroupOffset & 0xFFFFFFu;
        d.Flags = in.forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure = static_cast<D3D12Accel*>(in.blas)->address();
        descs[i] = d;
    }
    require(a->scratchBuffer(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flushBarriers();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = tlasInputs(static_cast<std::uint32_t>(instances.size()), a->allowUpdate);
    bd.Inputs.InstanceDescs = static_cast<D3D12Buffer*>(t.buffer)->address() + t.offset;
    if (update) {
        if (!a->allowUpdate) throw Error("buildTlas: structure was not created with allowUpdate");
        bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        bd.SourceAccelerationStructureData = a->address();
    }
    bd.DestAccelerationStructureData = a->address();
    bd.ScratchAccelerationStructureData = a->scratchAddress();
    m_list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    m_accelDirty = true;
}

void D3D12CommandList::accelBarrier() {
    requireRecording("accelBarrier");
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    m_barriers.push_back(b);
    flushBarriers();
    m_accelDirty = false;
}

} // namespace gt::rhi::d3d12
