#include "rt_common.h"

#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace gt::bench::rt {

Mesh icosphere(int subdivisions) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<std::array<float, 3>> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::uint32_t> f = {0, 11, 5, 0, 5,  1,  0,  1,  7,  0,  7,  10, 0,  10, 11, 1, 5, 9, 5, 11,
                                    4, 11, 10, 2, 10, 7,  6,  7,  1,  8,  3,  9,  4,  3,  4,  2, 3, 2, 6, 3,
                                    6, 8,  3,  8, 9,  4,  9,  5,  2,  4,  11, 6,  2,  10, 8,  6, 7, 9, 8, 1};
    auto normalize = [](std::array<float, 3> p) {
        const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        return std::array<float, 3>{p[0] / l, p[1] / l, p[2] / l};
    };
    for (auto& p : v) p = normalize(p);
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            if (auto it = mid.find(key); it != mid.end()) return it->second;
            v.push_back(normalize({(v[a][0] + v[b][0]) / 2, (v[a][1] + v[b][1]) / 2, (v[a][2] + v[b][2]) / 2}));
            const auto idx = static_cast<std::uint32_t>(v.size() - 1);
            mid.emplace(key, idx);
            return idx;
        };
        std::vector<std::uint32_t> next;
        next.reserve(f.size() * 4);
        for (std::size_t i = 0; i < f.size(); i += 3) {
            const std::uint32_t a = f[i], b = f[i + 1], c = f[i + 2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        f = std::move(next);
    }
    Mesh m;
    m.positions.reserve(v.size() * 3);
    for (const auto& p : v) m.positions.insert(m.positions.end(), p.begin(), p.end());
    m.indices = std::move(f);
    return m;
}

Mesh grid(std::uint32_t cells, float size) {
    Mesh m;
    const std::uint32_t n = cells + 1;
    m.positions.reserve(std::size_t(n) * n * 3);
    for (std::uint32_t iz = 0; iz < n; ++iz)
        for (std::uint32_t ix = 0; ix < n; ++ix)
            m.positions.insert(m.positions.end(), {(float(ix) / float(cells) - 0.5f) * size, 0.0f,
                                                   (float(iz) / float(cells) - 0.5f) * size});
    m.indices.reserve(std::size_t(cells) * cells * 6);
    for (std::uint32_t iz = 0; iz < cells; ++iz)
        for (std::uint32_t ix = 0; ix < cells; ++ix) {
            const std::uint32_t v00 = iz * n + ix, v10 = v00 + 1, v01 = v00 + n, v11 = v01 + 1;
            m.indices.insert(m.indices.end(), {v00, v11, v10, v00, v01, v11});
        }
    return m;
}

rhi::AccelTriangles GpuMesh::triangles() const {
    rhi::AccelTriangles t;
    t.vertices = vertices.get();
    t.vertexCount = vertexCount;
    t.indices = indices.get();
    t.indexCount = indexCount;
    return t;
}

GpuMesh upload(rhi::Device& dev, const Mesh& mesh, const char* name) {
    using U = rhi::BufferUsage;
    GpuMesh g;
    g.vertexCount = mesh.vertexCount();
    g.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    g.vertices = dev.createBuffer({mesh.positions.size() * sizeof(float), U::AccelInput | U::Storage | U::CopyDst | U::CopySrc,
                                   rhi::MemoryType::Device, 0, name},
                                  mesh.positions.data());
    g.indices = dev.createBuffer({mesh.indices.size() * sizeof(std::uint32_t), U::AccelInput | U::CopyDst,
                                  rhi::MemoryType::Device, 0, name},
                                 mesh.indices.data());
    return g;
}

void scaleTranslate(float (&m)[3][4], float scale, float x, float y, float z) {
    m[0][0] = scale, m[0][1] = 0, m[0][2] = 0, m[0][3] = x;
    m[1][0] = 0, m[1][1] = scale, m[1][2] = 0, m[1][3] = y;
    m[2][0] = 0, m[2][1] = 0, m[2][2] = scale, m[2][3] = z;
}

} // namespace gt::bench::rt
