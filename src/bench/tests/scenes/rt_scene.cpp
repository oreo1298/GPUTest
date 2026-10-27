#include "rt_scene.h"

#include "gt/bench/context.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt::bench::scene {

RtInstance rtInstance(std::uint32_t mesh, Vec3 t, Vec3 s, float yaw, float roll, Vec3 color, float material) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cr = std::cos(roll), sr = std::sin(roll);
    const float r[3][3] = {{cy * cr, -cy * sr, sy}, {sr, cr, 0}, {-sy * cr, sy * sr, cy}}; // Ry * Rz
    RtInstance in{};
    for (int i = 0; i < 3; ++i) {
        in.row[i][0] = r[i][0] * s.x;
        in.row[i][1] = r[i][1] * s.y;
        in.row[i][2] = r[i][2] * s.z;
    }
    in.row[0][3] = t.x, in.row[1][3] = t.y, in.row[2][3] = t.z;
    in.color[0] = color.x, in.color[1] = color.y, in.color[2] = color.z, in.color[3] = material;
    in.info[0] = float(mesh);
    return in;
}

RtScene buildRtScene(TestContext& ctx, const MeshPool& pool, const std::vector<std::uint32_t>& meshIds,
                     std::vector<RtInstance>& instances) {
    rhi::Device& dev = ctx.device();
    using U = rhi::BufferUsage;
    struct MeshInfo {
        std::uint32_t firstIndex, indexCount;
        std::int32_t baseVertex;
        std::uint32_t pad;
    };
    RtScene rs;
    const std::size_t kinds = meshIds.size();
    std::stable_sort(instances.begin(), instances.end(),
                     [](const RtInstance& a, const RtInstance& b) { return a.info[0] < b.info[0]; });
    rs.groupFirst.assign(kinds, 0);
    rs.groupCount.assign(kinds, 0);
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const auto m = static_cast<std::size_t>(instances[i].info[0]);
        if (rs.groupCount[m]++ == 0) rs.groupFirst[m] = i;
    }
    std::vector<MeshInfo> meshes(kinds);
    for (std::size_t m = 0; m < kinds; ++m) {
        const SubMesh& sm = pool[meshIds[m]];
        meshes[m] = {sm.firstIndex, sm.indexCount, sm.baseVertex, 0};
    }
    rs.instances = dev.createBuffer({instances.size() * sizeof(RtInstance), U::Vertex | U::Storage | U::CopyDst,
                                     rhi::MemoryType::Device, sizeof(RtInstance), "rt-scene-instances"},
                                    instances.data());
    rs.meshes = dev.createBuffer({meshes.size() * sizeof(MeshInfo), U::Storage | U::CopyDst, rhi::MemoryType::Device,
                                  sizeof(MeshInfo), "rt-scene-meshes"},
                                 meshes.data());

    std::vector<rhi::BlasDesc> blasDescs(kinds);
    for (std::size_t m = 0; m < kinds; ++m) {
        const SubMesh& sm = pool[meshIds[m]];
        rhi::AccelTriangles t;
        t.vertices = pool.vertexBuffer();
        t.vertexOffset = std::uint64_t(sm.baseVertex) * sizeof(Vertex);
        t.vertexStride = sizeof(Vertex);
        t.vertexCount = static_cast<std::uint32_t>(pool.vertices().size() - std::size_t(sm.baseVertex));
        t.indices = pool.indexBuffer();
        t.indexOffset = std::uint64_t(sm.firstIndex) * 4;
        t.indexCount = sm.indexCount;
        blasDescs[m].geometries = {t};
        blasDescs[m].name = "rt-scene-blas";
        rs.blas.push_back(dev.createBlas(blasDescs[m]));
    }
    std::vector<rhi::AccelInstance> tlasInstances(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        std::memcpy(tlasInstances[i].transform, instances[i].row, sizeof(tlasInstances[i].transform));
        tlasInstances[i].id = static_cast<std::uint32_t>(i);
        tlasInstances[i].blas = rs.blas[static_cast<std::size_t>(instances[i].info[0])].get();
    }
    rs.tlas = dev.createTlas(static_cast<std::uint32_t>(tlasInstances.size()));
    ctx.timeGpu([&](rhi::CommandList& cmd) {
        for (std::size_t m = 0; m < kinds; ++m) cmd.buildBlas(rs.blas[m].get(), blasDescs[m]);
        cmd.accelBarrier();
        cmd.buildTlas(rs.tlas.get(), tlasInstances);
        cmd.accelBarrier();
    });
    return rs;
}

} // namespace gt::bench::scene
