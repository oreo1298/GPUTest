#pragma once
// Shared pieces of the ray-tracing feature tests: procedural meshes and GPU upload.

#include "gt/rhi/rhi.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gt::bench::rt {

struct Mesh {
    std::vector<float> positions;       ///< xyz per vertex
    std::vector<std::uint32_t> indices; ///< three per triangle
    std::uint32_t vertexCount() const { return static_cast<std::uint32_t>(positions.size() / 3); }
    std::uint32_t triangleCount() const { return static_cast<std::uint32_t>(indices.size() / 3); }
};

/// Unit icosphere; each subdivision level multiplies the triangle count by four (level 0 = 20).
Mesh icosphere(int subdivisions);

/// Flat square grid in the XZ plane centred on the origin, `cells` x `cells` quads of two
/// triangles each. Cell c = iz * cells + ix owns triangles 2c (below its diagonal, u >= w)
/// and 2c + 1 (above it); vertex (ix, iz) is at index iz * (cells + 1) + ix.
Mesh grid(std::uint32_t cells, float size);

/// Mesh buffers usable as acceleration-structure build input (and as storage for compute).
struct GpuMesh {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    rhi::AccelTriangles triangles() const;
};
GpuMesh upload(rhi::Device& dev, const Mesh& mesh, const char* name);

/// Row-major 3x4 transform: uniform scale, then translation.
void scaleTranslate(float (&m)[3][4], float scale, float x, float y, float z);

/// Push constants of shaders/src/features/rt_common.slang.
struct RayParams {
    float camPos[4], camFwd[4], camRight[4], camUp[4];
    float boundsMin[4], boundsMax[4];
    std::uint32_t width, height, mode, seed;
};
static_assert(sizeof(RayParams) == 112);

/// A ground plane with side x side instanced spheres of random size, built and ready to trace
/// (instance i has InstanceID i; the ground is instance 0), plus a camera looking across it.
struct SphereField {
    GpuMesh sphereMesh, groundMesh;
    std::unique_ptr<rhi::AccelerationStructure> sphereBlas, groundBlas, tlas;
    std::uint32_t instances = 0;
    std::uint32_t trianglesPerSphere = 0;
    RayParams params{};
};
/// Records the builds with `submit` (e.g. TestContext::timeGpu).
SphereField buildSphereField(rhi::Device& dev, int side, int subdivisions, std::uint64_t seed, std::uint32_t width,
                             std::uint32_t height,
                             const std::function<void(const std::function<void(rhi::CommandList&)>&)>& submit);

} // namespace gt::bench::rt
