#pragma once
// Scene toolkit shared by the scene benchmarks: the standard vertex format, procedural
// meshes (everything is generated: no third-party assets), a pool that packs meshes into
// one vertex and one index buffer, a spline camera path and frustum helpers.

#include "gt/bench/math.h"
#include "gt/rhi/rhi.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gt::bench::scene {

/// position, normal, uv — 32 bytes; attribute locations 0, 1, 2.
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(Vertex) == 32);

/// Vertex input description for Vertex (binding 0).
void vertexLayout(rhi::GraphicsPipelineDesc& desc);

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; ///< counter-clockwise triangles seen from outside
    std::uint32_t triangleCount() const { return static_cast<std::uint32_t>(indices.size() / 3); }
};

// --- generators (unit-ish sizes, centred on the origin unless noted) ---------------------------
MeshData box(float sx, float sy, float sz);                       ///< full extents sx, sy, sz
MeshData sphere(std::uint32_t segments, std::uint32_t rings);     ///< radius 1
MeshData cylinder(std::uint32_t segments, bool caps);             ///< radius 1, y from 0 to 1
MeshData cone(std::uint32_t segments);                            ///< radius 1 at y = 0, apex at y = 1
MeshData torus(std::uint32_t segments, std::uint32_t sides, float minor); ///< major radius 1, in the XZ plane
/// Grid in the XZ plane (y = height(x, z)), `cells` x `cells` quads over [-size/2, size/2]²; normals from the
/// height function by central differences; uv = world xz / uvScale.
MeshData heightfield(std::uint32_t cells, float size, const std::function<float(float, float)>& height, float uvScale = 4.0f);

/// Appends `src` to `dst` transformed by translation + uniform scale + rotation about Y.
void append(MeshData& dst, const MeshData& src, Vec3 offset, float scale = 1.0f, float rotationY = 0.0f,
            Vec3 stretch = {1, 1, 1});
/// Appends an open tube from `a` (radius r0) to `b` (radius r1) with `segments` sides.
void appendTube(MeshData& dst, Vec3 a, Vec3 b, float r0, float r1, std::uint32_t segments);
/// Appends a two-triangle card centred on `c` spanning ±u and ±v (uv over [0, 1]², v along `v`), facing u × v.
void appendCard(MeshData& dst, Vec3 c, Vec3 u, Vec3 v);

/// Random numbers for building content. Portable on purpose: the std:: distributions differ between standard
/// libraries, so generated scenes (and their scores) would differ between Linux and Windows builds.
class Random {
public:
    explicit Random(std::uint64_t seed) : m_state(seed) {}
    std::uint32_t next() { // SplitMix64
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 32);
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); } ///< [0, 1)
    float uniform(float lo, float hi) { return lo + (hi - lo) * uniform(); }

private:
    std::uint64_t m_state;
};

/// Smooth value noise in [0, 1] (deterministic, matches nothing on the GPU; for building geometry).
float noise2(float x, float y);
float fbm2(float x, float y, int octaves);

// --- mesh pool ----------------------------------------------------------------------------------
struct SubMesh {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::int32_t baseVertex = 0;
    Vec3 center;          ///< bounding sphere in object space
    float radius = 0.0f;
};

/// Packs meshes into one vertex buffer and one 32-bit index buffer.
class MeshPool {
public:
    std::uint32_t add(const MeshData& mesh); ///< returns the sub-mesh index
    const SubMesh& operator[](std::uint32_t i) const { return m_meshes[i]; }
    std::size_t size() const { return m_meshes.size(); }
    /// Creates the GPU buffers (vertex: Vertex | Storage, index: Index | Storage); extra usage flags are added.
    void upload(rhi::Device& dev, rhi::BufferUsage extraUsage = rhi::BufferUsage::None);
    rhi::Buffer* vertexBuffer() const { return m_vb.get(); }
    rhi::Buffer* indexBuffer() const { return m_ib.get(); }
    std::uint64_t triangleCount() const { return m_indices.size() / 3; }
    const std::vector<Vertex>& vertices() const { return m_vertices; }
    const std::vector<std::uint32_t>& indices() const { return m_indices; }

private:
    std::vector<Vertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
    std::vector<SubMesh> m_meshes;
    std::unique_ptr<rhi::Buffer> m_vb, m_ib;
};

// --- camera -------------------------------------------------------------------------------------
struct CameraKey {
    Vec3 eye;
    Vec3 target;
};

/// Catmull-Rom spline through keyframes, evaluated at t in [0, 1] over the whole path.
class CameraPath {
public:
    explicit CameraPath(std::vector<CameraKey> keys) : m_keys(std::move(keys)) {}
    CameraKey at(double t) const;

private:
    std::vector<CameraKey> m_keys;
};

/// Inward-facing world-space frustum planes (left, right, bottom, top, near, far) of a
/// column-major view-projection matrix with depth in [0, 1]; xyz normalised.
std::array<std::array<float, 4>, 6> frustumPlanes(const Mat4& viewProj);
bool sphereVisible(const std::array<std::array<float, 4>, 6>& planes, Vec3 center, float radius);

/// Right-handed orthographic projection, depth mapped to [0, 1].
Mat4 orthographic(float left, float right, float bottom, float top, float zNear, float zFar);

} // namespace gt::bench::scene
