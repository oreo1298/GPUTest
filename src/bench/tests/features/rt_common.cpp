#include "rt_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>
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

SphereField buildSphereField(rhi::Device& dev, int side, int subdivisions, std::uint64_t seed, std::uint32_t width,
                             std::uint32_t height,
                             const std::function<void(const std::function<void(rhi::CommandList&)>&)>& submit) {
    const float spacing = 2.5f, half = spacing * float(side) / 2.0f;
    SphereField f;
    const Mesh sphere = icosphere(subdivisions);
    f.trianglesPerSphere = sphere.triangleCount();
    f.sphereMesh = upload(dev, sphere, "field-sphere");
    f.groundMesh = upload(dev, grid(64, half * 2.0f + 20.0f), "field-ground");
    rhi::BlasDesc sphereDesc, groundDesc;
    sphereDesc.geometries = {f.sphereMesh.triangles()};
    sphereDesc.name = "field-sphere-blas";
    groundDesc.geometries = {f.groundMesh.triangles()};
    groundDesc.name = "field-ground-blas";
    f.sphereBlas = dev.createBlas(sphereDesc);
    f.groundBlas = dev.createBlas(groundDesc);

    std::vector<rhi::AccelInstance> instances(1);
    instances[0].blas = f.groundBlas.get();
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    std::uniform_real_distribution<float> jitter(-0.5f, 0.5f), radius(0.4f, 1.0f);
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            rhi::AccelInstance in;
            const float r = radius(rng);
            scaleTranslate(in.transform, r, (float(x) + 0.5f) * spacing - half + jitter(rng), r,
                           (float(z) + 0.5f) * spacing - half + jitter(rng));
            in.id = static_cast<std::uint32_t>(instances.size());
            in.blas = f.sphereBlas.get();
            instances.push_back(in);
        }
    f.instances = static_cast<std::uint32_t>(instances.size());
    f.tlas = dev.createTlas(f.instances);
    submit([&](rhi::CommandList& cmd) {
        cmd.buildBlas(f.sphereBlas.get(), sphereDesc);
        cmd.buildBlas(f.groundBlas.get(), groundDesc);
        cmd.accelBarrier();
        cmd.buildTlas(f.tlas.get(), instances);
        cmd.accelBarrier();
    });

    // Camera above the near edge of the field, looking across it.
    auto normalize = [](float (&v)[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& c : v) c /= l;
    };
    auto set4 = [](float (&dst)[4], const float (&src)[3], float w) {
        dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = w;
    };
    RayParams& p = f.params;
    const float eye[3] = {0.0f, 5.0f, -half - 4.0f}, target[3] = {0.0f, 0.0f, half * 0.3f};
    float fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalize(fwd);
    float right[3] = {fwd[2], 0.0f, -fwd[0]};
    normalize(right);
    const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2],
                         fwd[0] * right[1] - fwd[1] * right[0]};
    set4(p.camPos, eye, std::tan(0.5f * 50.0f * 3.14159265f / 180.0f));
    set4(p.camFwd, fwd, float(width) / float(height));
    set4(p.camRight, right, 0.0f);
    set4(p.camUp, up, 0.0f);
    const float bmin[3] = {-half, 0.05f, -half}, bmax[3] = {half, 3.0f, half};
    set4(p.boundsMin, bmin, 0.0f);
    set4(p.boundsMax, bmax, 0.0f);
    p.width = width;
    p.height = height;
    p.seed = static_cast<std::uint32_t>(seed);
    return f;
}

} // namespace gt::bench::rt
