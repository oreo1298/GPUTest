#include "d3d12_device.h"

#include <deque>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

/// Geometry descriptions of a BLAS; OMM triangles point into `triangles` and `links`.
struct BlasGeometry {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    std::vector<D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC> triangles;
    std::vector<D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC> links;
};

BlasGeometry geometryDescs(const BlasDesc& desc, const std::vector<D3D12Accel::Micromap>* micromaps = nullptr) {
    BlasGeometry out;
    out.triangles.reserve(desc.geometries.size()); // stable addresses for the OMM descs
    out.links.reserve(desc.geometries.size());
    for (std::size_t gi = 0; gi < desc.geometries.size(); ++gi) {
        const AccelTriangles& g = desc.geometries[gi];
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC tri{};
        auto* vb = static_cast<D3D12Buffer*>(g.vertices);
        tri.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        tri.VertexCount = g.vertexCount;
        tri.VertexBuffer = {vb ? vb->address() + g.vertexOffset : 0, g.vertexStride};
        if (g.indices && g.indexCount) {
            tri.IndexFormat = DXGI_FORMAT_R32_UINT;
            tri.IndexCount = g.indexCount;
            tri.IndexBuffer = static_cast<D3D12Buffer*>(g.indices)->address() + g.indexOffset;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC d{};
        d.Flags = g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        const D3D12Accel::Micromap* mm = micromaps && gi < micromaps->size() ? &(*micromaps)[gi] : nullptr;
        if (mm && mm->storage) {
            // Triangle i uses micromap i (32-bit index buffer), one OMM array per geometry.
            D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC link{};
            link.OpacityMicromapIndexBuffer = {mm->indices->address(), 4};
            link.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
            link.OpacityMicromapBaseLocation = 0;
            link.OpacityMicromapArray = mm->storage->address();
            out.triangles.push_back(tri);
            out.links.push_back(link);
            d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
            d.OmmTriangles = {&out.triangles.back(), &out.links.back()};
        } else {
            d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            d.Triangles = tri;
        }
        out.descs.push_back(d);
    }
    return out;
}

/// Inputs of a level-0, 4-state opacity micromap array with one micromap per triangle.
struct MicromapInputs {
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY histogram{};
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC array{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
};

void describeMicromap(const D3D12Accel::Micromap& m, MicromapInputs& mi) {
    mi.histogram = {m.count, 0, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE};
    mi.array.NumOmmHistogramEntries = 1;
    mi.array.pOmmHistogram = &mi.histogram;
    mi.array.InputBuffer = m.data->address();
    mi.array.PerOmmDescs = {m.descs->address(), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
    mi.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
    mi.inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    mi.inputs.NumDescs = 1;
    mi.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    mi.inputs.pOpacityMicromapArrayDesc = &mi.array;
}

D3D12Accel::Micromap createMicromap(D3D12Device& dev, std::span<const TriangleOpacity> opacity, const char* name) {
    const auto n = static_cast<std::uint32_t>(opacity.size());
    // One byte per micromap (a single 2-bit micro-triangle at subdivision level 0).
    std::vector<std::uint8_t> data(std::max<std::size_t>(n, 4));
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> descs(n);
    std::vector<std::uint32_t> indices(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        data[i] = static_cast<std::uint8_t>(opacity[i]); // 0 transparent, 1 opaque, 2 unknown-transparent
        descs[i].ByteOffset = i;
        descs[i].SubdivisionLevel = 0;
        descs[i].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
        indices[i] = i;
    }
    D3D12Accel::Micromap m;
    m.count = n;
    using U = BufferUsage;
    auto make = [&](const void* bytes, std::size_t size, const char* what) {
        auto b = std::make_unique<D3D12Buffer>(dev, BufferDesc{std::max<std::uint64_t>(size, 16), U::AccelInput | U::CopyDst,
                                                               MemoryType::Device, 0, what});
        dev.uploadBuffer(b.get(), bytes, size, 0);
        return b;
    };
    m.data = make(data.data(), data.size(), "omm-data");
    m.descs = make(descs.data(), descs.size() * sizeof(descs[0]), "omm-descs");
    m.indices = make(indices.data(), indices.size() * 4, "omm-indices");
    MicromapInputs mi;
    describeMicromap(m, mi);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev.device5()->GetRaytracingAccelerationStructurePrebuildInfo(&mi.inputs, &info);
    m.storage = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{std::max<std::uint64_t>(info.ResultDataMaxSizeInBytes, 256), U::AccelStorage, MemoryType::Device,
                        0, name});
    m.scratch = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{alignUp(std::max<std::uint64_t>(info.ScratchDataSizeInBytes, 256),
                                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT),
                        U::Storage, MemoryType::Device, 0, "omm-scratch"});
    return m;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(const BlasDesc& desc, const BlasGeometry& g) {
    const auto& geo = g.descs;
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
    if ((!m_features.rayQuery && !m_features.rayTracingPipeline) || !m_device5)
        throw Error("ray tracing is not supported by this GPU");
    std::vector<D3D12Accel::Micromap> micromaps(desc.geometries.size());
    for (std::size_t i = 0; i < desc.geometries.size(); ++i) {
        const AccelTriangles& t = desc.geometries[i];
        if (t.opacity.empty()) continue;
        if (!m_features.opacityMicromap) throw Error("opacity micromaps are not supported by this GPU");
        if (t.opaque) throw Error("opacity micromaps need non-opaque geometry");
        if (t.opacity.size() != (t.indices ? t.indexCount : t.vertexCount) / 3)
            throw Error("opacity micromap: one state per triangle is required");
        micromaps[i] = createMicromap(*this, t.opacity, desc.name);
    }
    const BlasGeometry geo = geometryDescs(desc, &micromaps);
    const auto inputs = blasInputs(desc, geo);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    auto a = std::make_unique<D3D12Accel>(*this, false, info, 0, desc.name);
    a->allowUpdate = desc.allowUpdate;
    a->micromaps = std::move(micromaps);
    return a;
}

std::unique_ptr<AccelerationStructure> D3D12Device::createTlas(std::uint32_t maxInstances, bool allowUpdate) {
    if ((!m_features.rayQuery && !m_features.rayTracingPipeline) || !m_device5)
        throw Error("ray tracing is not supported by this GPU");
    const auto inputs = tlasInputs(maxInstances, allowUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    auto a = std::make_unique<D3D12Accel>(*this, true, info, maxInstances, "tlas");
    a->allowUpdate = allowUpdate;
    return a;
}

// --- ray-tracing pipelines --------------------------------------------------------------------------------
D3D12Pipeline::D3D12Pipeline(D3D12Device& dev, const RayTracingPipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = true; // bound through the compute root signature
    m_layoutDesc = d.layout;
    m_root = &dev.rootSignature(d.layout, false);
    ID3D12Device5* dev5 = dev.device5();
    if (!dev5) throw Error("ray-tracing pipelines need ID3D12Device5");

    // Every shader is its own DXIL library; exports are renamed so entry names never clash.
    struct Library {
        D3D12_EXPORT_DESC exportDesc{};
        D3D12_DXIL_LIBRARY_DESC desc{};
        std::wstring name, original;
    };
    std::deque<Library> libraries; // stable addresses for the subobjects
    auto addShader = [&](const ShaderRef& ref) -> std::wstring {
        if (!ref.valid()) return {};
        Library& lib = libraries.emplace_back();
        lib.name = L"s" + std::to_wstring(libraries.size() - 1);
        lib.original = widen(ref.entry);
        lib.exportDesc = {lib.name.c_str(), lib.original.c_str(), D3D12_EXPORT_FLAG_NONE};
        const auto code = dev.shaderBytecode(ref);
        lib.desc = {{code.data(), code.size()}, 1, &lib.exportDesc};
        return lib.name;
    };
    const std::wstring raygen = addShader(d.rayGen);
    std::vector<std::wstring> misses;
    for (const ShaderRef& m : d.miss) misses.push_back(addShader(m));
    struct Group {
        D3D12_HIT_GROUP_DESC desc{};
        std::wstring name, closest, any, intersection;
    };
    std::deque<Group> groups;
    for (const RayTracingHitGroup& h : d.hitGroups) {
        Group& g = groups.emplace_back();
        g.name = L"hg" + std::to_wstring(groups.size() - 1);
        g.closest = addShader(h.closestHit);
        g.any = addShader(h.anyHit);
        g.intersection = addShader(h.intersection);
        g.desc.HitGroupExport = g.name.c_str();
        g.desc.Type = h.intersection.valid() ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE : D3D12_HIT_GROUP_TYPE_TRIANGLES;
        g.desc.ClosestHitShaderImport = g.closest.empty() ? nullptr : g.closest.c_str();
        g.desc.AnyHitShaderImport = g.any.empty() ? nullptr : g.any.c_str();
        g.desc.IntersectionShaderImport = g.intersection.empty() ? nullptr : g.intersection.c_str();
    }
    const D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{d.maxPayloadBytes, d.maxAttributeBytes};
    const D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{std::max(1u, d.maxRecursion)};
    const D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{m_root->signature.get()};
    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    for (Library& lib : libraries) subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib.desc});
    for (Group& g : groups) subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &g.desc});
    subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig});
    subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig});
    subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot});
    const D3D12_STATE_OBJECT_DESC sd{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                                     static_cast<UINT>(subobjects.size()), subobjects.data()};
    dev.check(dev5->CreateStateObject(&sd, __uuidof(ID3D12StateObject), m_stateObject.putVoid()),
              std::format("CreateStateObject({})", d.name).c_str());
    dev.setName(m_stateObject.get(), d.name);

    // Shader tables: ray generation, then the miss records, then the hit groups (identifiers only).
    auto props = m_stateObject.as<ID3D12StateObjectProperties>();
    if (!props) throw Error("ID3D12StateObjectProperties unavailable");
    const std::uint64_t record = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    const std::uint64_t table = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    auto alignUp = [](std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; };
    const std::uint64_t missOffset = alignUp(record, table);
    const std::uint64_t hitOffset = alignUp(missOffset + record * std::max<std::size_t>(misses.size(), 1), table);
    const std::uint64_t size = hitOffset + record * std::max<std::size_t>(groups.size(), 1);
    m_shaderTable = std::make_unique<D3D12Buffer>(
        dev, BufferDesc{size, BufferUsage::ShaderTable, MemoryType::Upload, 0, "shader-table"});
    auto* base = static_cast<std::uint8_t*>(m_shaderTable->mapped());
    auto copyId = [&](const std::wstring& name, std::uint64_t offset) {
        const void* id = props->GetShaderIdentifier(name.c_str());
        if (!id) throw Error(std::format("pipeline '{}': no shader identifier for an export", d.name));
        std::memcpy(base + offset, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    };
    copyId(raygen, 0);
    for (std::size_t i = 0; i < misses.size(); ++i) copyId(misses[i], missOffset + i * record);
    for (std::size_t i = 0; i < groups.size(); ++i) copyId(groups[i].name, hitOffset + i * record);
    const D3D12_GPU_VIRTUAL_ADDRESS va = m_shaderTable->address();
    m_raygen = {va, record};
    m_miss = {va + missOffset, record * misses.size(), record};
    m_hit = {va + hitOffset, record * groups.size(), record};
    m_creationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void D3D12CommandList::traceRays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    if (!m_pipeline || !m_pipeline->rayTracing()) throw Error("traceRays: no ray-tracing pipeline bound");
    if (!m_list4) throw Error("ray tracing is not supported by this command list");
    flushBindings(true);
    flushBarriers();
    D3D12_DISPATCH_RAYS_DESC r{};
    r.RayGenerationShaderRecord = m_pipeline->raygenRecord();
    r.MissShaderTable = m_pipeline->missTable();
    r.HitGroupTable = m_pipeline->hitTable();
    r.Width = width;
    r.Height = height;
    r.Depth = depth;
    m_list4->DispatchRays(&r);
}

void D3D12CommandList::buildBlas(AccelerationStructure* blas, const BlasDesc& desc, bool update) {
    requireRecording("buildBlas");
    if (!m_list4) throw Error("ray tracing is not supported by this command list");
    auto* a = static_cast<D3D12Accel*>(blas);
    // Rebuilding a structure recorded earlier without a barrier in between (repeated builds, a refit
    // right after its build) would race on its storage and scratch memory.
    if (std::find(m_builtAccels.begin(), m_builtAccels.end(), a) != m_builtAccels.end()) accelBarrier();
    for (const AccelTriangles& g : desc.geometries) {
        if (g.vertices) require(static_cast<D3D12Buffer*>(g.vertices), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g.indices) require(static_cast<D3D12Buffer*>(g.indices), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // Micromaps first (once: their contents never change), then the BLAS that references them.
    bool builtMicromap = false;
    for (D3D12Accel::Micromap& m : a->micromaps) {
        if (!m.storage || m.built) continue;
        for (D3D12Buffer* b : {m.data.get(), m.descs.get(), m.indices.get()})
            require(b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        require(m.scratch.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flushBarriers();
        MicromapInputs mi;
        describeMicromap(m, mi);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC md{};
        md.Inputs = mi.inputs;
        md.DestAccelerationStructureData = m.storage->address();
        md.ScratchAccelerationStructureData = m.scratch->address();
        m_list4->BuildRaytracingAccelerationStructure(&md, 0, nullptr);
        m.built = true;
        builtMicromap = true;
    }
    if (builtMicromap) accelBarrier();
    for (const D3D12Accel::Micromap& m : a->micromaps)
        if (m.indices) require(m.indices.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    require(a->scratchBuffer(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flushBarriers();
    const BlasGeometry geo = geometryDescs(desc, &a->micromaps);
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
    m_builtAccels.push_back(a);
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
    m_builtAccels.push_back(a);
}

void D3D12CommandList::accelBarrier() {
    requireRecording("accelBarrier");
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    m_barriers.push_back(b);
    flushBarriers();
    m_accelDirty = false;
    m_builtAccels.clear();
}

} // namespace gt::rhi::d3d12
