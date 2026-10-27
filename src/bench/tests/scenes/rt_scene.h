#pragma once
// Ray-traced scene plumbing shared by the ultimate-tier scenes: instances with a 3x4 transform,
// colour and material (the layout of Instance in shaders/src/scenes/rt_scene.slang), one BLAS
// per pool mesh and a TLAS over every instance. Hit shaders fetch normals through the instance
// and mesh tables plus the pool's vertex and index buffers.

#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/rhi/rhi.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace gt::bench {
class TestContext;
}

namespace gt::bench::scene {

struct RtInstance {
    float row[3][4]; ///< object-to-world, rows of a 3x4 matrix
    float color[4];  ///< rgb, w = material id
    float info[4];   ///< x = mesh index into RtScene::meshes
};
static_assert(sizeof(RtInstance) == 80);

/// Rows of R_y(yaw) * R_z(roll) * S + t.
RtInstance rtInstance(std::uint32_t mesh, Vec3 t, Vec3 s, float yaw, float roll, Vec3 color, float material);

struct RtScene {
    std::unique_ptr<rhi::Buffer> instances; ///< RtInstance[] (vertex + storage), sorted by mesh
    std::unique_ptr<rhi::Buffer> meshes;    ///< {firstIndex, indexCount, baseVertex, flags (1 = alpha-tested)}[]
    std::vector<std::unique_ptr<rhi::AccelerationStructure>> blas;
    std::unique_ptr<rhi::AccelerationStructure> tlas;
    std::vector<std::uint32_t> groupFirst, groupCount; ///< instance ranges per mesh, for instanced draws
};

/// Sorts `instances` by mesh, uploads the tables and builds the acceleration structures (timed
/// as setup work). `meshIds[k]` is the pool sub-mesh behind mesh index k; the pool must have
/// been uploaded with BufferUsage::AccelInput. Meshes flagged in `alphaTested` get non-opaque
/// geometry: rays see their triangles as candidates and test the leaf shape (rt_scene.slang).
RtScene buildRtScene(TestContext& ctx, const MeshPool& pool, const std::vector<std::uint32_t>& meshIds,
                     std::vector<RtInstance>& instances, const std::vector<bool>& alphaTested = {});

} // namespace gt::bench::scene
