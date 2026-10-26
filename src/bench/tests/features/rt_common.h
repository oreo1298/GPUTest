#pragma once
// Shared pieces of the ray-tracing feature tests: procedural meshes and GPU upload.

#include "gt/rhi/rhi.h"

#include <cstdint>
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

} // namespace gt::bench::rt
