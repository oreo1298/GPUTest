#include "gt/bench/scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace gt::bench::scene {

void vertexLayout(rhi::GraphicsPipelineDesc& desc) {
    desc.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}, {1, rhi::Format::RGB32_FLOAT, 12}, {2, rhi::Format::RG32_FLOAT, 24}};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
}

namespace {

void quad(MeshData& m, Vec3 o, Vec3 a, Vec3 b, Vec3 n, float ua, float vb) {
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    const Vec3 p[4] = {o, o + a, o + a + b, o + b};
    const float uv[4][2] = {{0, 0}, {ua, 0}, {ua, vb}, {0, vb}};
    for (int i = 0; i < 4; ++i) m.vertices.push_back({p[i].x, p[i].y, p[i].z, n.x, n.y, n.z, uv[i][0], uv[i][1]});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

MeshData box(float sx, float sy, float sz) {
    MeshData m;
    const float x = sx / 2, y = sy / 2, z = sz / 2;
    // Each face: origin corner, two edges with a x b pointing outwards.
    quad(m, {x, -y, z}, {0, 0, -sz}, {0, sy, 0}, {1, 0, 0}, sz, sy);    // +X
    quad(m, {-x, -y, -z}, {0, 0, sz}, {0, sy, 0}, {-1, 0, 0}, sz, sy);  // -X
    quad(m, {-x, y, z}, {sx, 0, 0}, {0, 0, -sz}, {0, 1, 0}, sx, sz);    // +Y
    quad(m, {-x, -y, -z}, {sx, 0, 0}, {0, 0, sz}, {0, -1, 0}, sx, sz);  // -Y
    quad(m, {-x, -y, z}, {sx, 0, 0}, {0, sy, 0}, {0, 0, 1}, sx, sy);    // +Z
    quad(m, {x, -y, -z}, {-sx, 0, 0}, {0, sy, 0}, {0, 0, -1}, sx, sy);  // -Z
    return m;
}

MeshData sphere(std::uint32_t segments, std::uint32_t rings) {
    MeshData m;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = float(r) / float(rings), phi = v * kPi;
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float u = float(s) / float(segments), th = u * 2.0f * kPi;
            const Vec3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            m.vertices.push_back({n.x, n.y, n.z, n.x, n.y, n.z, u * 2.0f, v});
        }
    }
    for (std::uint32_t r = 0; r < rings; ++r)
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return m;
}

MeshData cylinder(std::uint32_t segments, bool caps) {
    MeshData m;
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float u = float(s) / float(segments), th = u * 2.0f * kPi;
        const float cx = std::cos(th), cz = std::sin(th);
        m.vertices.push_back({cx, 0, cz, cx, 0, cz, u * 3.0f, 0});
        m.vertices.push_back({cx, 1, cz, cx, 0, cz, u * 3.0f, 1});
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        const std::uint32_t a = s * 2;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    if (caps)
        for (int top = 0; top < 2; ++top) {
            const float y = float(top), ny = top ? 1.0f : -1.0f;
            const auto centre = static_cast<std::uint32_t>(m.vertices.size());
            m.vertices.push_back({0, y, 0, 0, ny, 0, 0.5f, 0.5f});
            for (std::uint32_t s = 0; s <= segments; ++s) {
                const float th = float(s) / float(segments) * 2.0f * kPi;
                m.vertices.push_back({std::cos(th), y, std::sin(th), 0, ny, 0, 0.5f + 0.5f * std::cos(th), 0.5f + 0.5f * std::sin(th)});
            }
            for (std::uint32_t s = 0; s < segments; ++s) {
                if (top) m.indices.insert(m.indices.end(), {centre, centre + s + 2, centre + s + 1});
                else m.indices.insert(m.indices.end(), {centre, centre + s + 1, centre + s + 2});
            }
        }
    return m;
}

MeshData cone(std::uint32_t segments) {
    MeshData m;
    const float slope = 1.0f / std::sqrt(2.0f);
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float u = float(s) / float(segments), th = u * 2.0f * kPi;
        const float cx = std::cos(th), cz = std::sin(th);
        m.vertices.push_back({cx, 0, cz, cx * slope, slope, cz * slope, u * 2.0f, 0});
        m.vertices.push_back({0, 1, 0, cx * slope, slope, cz * slope, u * 2.0f, 1});
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        const std::uint32_t a = s * 2;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 2});
    }
    return m;
}

MeshData torus(std::uint32_t segments, std::uint32_t sides, float minor) {
    MeshData m;
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const float u = float(i) / float(segments), th = u * 2.0f * kPi;
        for (std::uint32_t j = 0; j <= sides; ++j) {
            const float v = float(j) / float(sides), ph = v * 2.0f * kPi;
            const Vec3 n{std::cos(ph) * std::cos(th), std::sin(ph), std::cos(ph) * std::sin(th)};
            const Vec3 p{std::cos(th) * (1.0f + minor * std::cos(ph)), minor * std::sin(ph), std::sin(th) * (1.0f + minor * std::cos(ph))};
            m.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, u * 4.0f, v});
        }
    }
    for (std::uint32_t i = 0; i < segments; ++i)
        for (std::uint32_t j = 0; j < sides; ++j) {
            const std::uint32_t a = i * (sides + 1) + j, b = a + sides + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return m;
}

MeshData heightfield(std::uint32_t cells, float size, const std::function<float(float, float)>& height, float uvScale) {
    MeshData m;
    const std::uint32_t n = cells + 1;
    const float step = size / float(cells), e = step * 0.5f;
    m.vertices.reserve(std::size_t(n) * n);
    for (std::uint32_t iz = 0; iz < n; ++iz)
        for (std::uint32_t ix = 0; ix < n; ++ix) {
            const float x = -size / 2 + float(ix) * step, z = -size / 2 + float(iz) * step;
            const Vec3 nrm = normalize({height(x - e, z) - height(x + e, z), 2.0f * e, height(x, z - e) - height(x, z + e)});
            m.vertices.push_back({x, height(x, z), z, nrm.x, nrm.y, nrm.z, x / uvScale, z / uvScale});
        }
    m.indices.reserve(std::size_t(cells) * cells * 6);
    for (std::uint32_t iz = 0; iz < cells; ++iz)
        for (std::uint32_t ix = 0; ix < cells; ++ix) {
            const std::uint32_t a = iz * n + ix, b = a + n;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

void append(MeshData& dst, const MeshData& src, Vec3 offset, float scale, float rotationY, Vec3 stretch) {
    const auto base = static_cast<std::uint32_t>(dst.vertices.size());
    const float c = std::cos(rotationY), s = std::sin(rotationY);
    for (Vertex v : src.vertices) {
        const float x = v.px * stretch.x * scale, y = v.py * stretch.y * scale, z = v.pz * stretch.z * scale;
        v.px = c * x + s * z + offset.x;
        v.py = y + offset.y;
        v.pz = -s * x + c * z + offset.z;
        // Normals of a non-uniform stretch scale by the inverse stretch.
        Vec3 n = normalize({v.nx / stretch.x, v.ny / stretch.y, v.nz / stretch.z});
        v.nx = c * n.x + s * n.z;
        v.ny = n.y;
        v.nz = -s * n.x + c * n.z;
        dst.vertices.push_back(v);
    }
    for (std::uint32_t i : src.indices) dst.indices.push_back(base + i);
}

MeshData icosphere(std::uint32_t subdivisions) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<Vec3> p = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (Vec3& v : p) v = normalize(v);
    std::vector<std::uint32_t> tris = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,
                                       11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8,
                                       3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (std::uint32_t level = 0; level < subdivisions; ++level) {
        std::unordered_map<std::uint64_t, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint64_t key = (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            if (auto it = mid.find(key); it != mid.end()) return it->second;
            p.push_back(normalize((p[a] + p[b]) * 0.5f));
            return mid[key] = static_cast<std::uint32_t>(p.size() - 1);
        };
        std::vector<std::uint32_t> next;
        next.reserve(tris.size() * 4);
        for (std::size_t i = 0; i < tris.size(); i += 3) {
            const std::uint32_t a = tris[i], b = tris[i + 1], c = tris[i + 2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        tris = std::move(next);
    }
    MeshData m;
    m.vertices.reserve(p.size());
    for (const Vec3& v : p)
        m.vertices.push_back({v.x, v.y, v.z, v.x, v.y, v.z, 0.5f + std::atan2(v.z, v.x) / (2.0f * kPi),
                              0.5f - std::asin(std::clamp(v.y, -1.0f, 1.0f)) / kPi});
    m.indices = std::move(tris);
    return m;
}

MeshData parametric(std::uint32_t uSteps, std::uint32_t vSteps, const std::function<Vec3(float, float)>& f) {
    MeshData m;
    m.vertices.reserve(std::size_t(uSteps + 1) * (vSteps + 1));
    for (std::uint32_t j = 0; j <= vSteps; ++j)
        for (std::uint32_t i = 0; i <= uSteps; ++i) {
            const float u = float(i) / float(uSteps), v = float(j) / float(vSteps);
            const Vec3 p = f(u, v);
            m.vertices.push_back({p.x, p.y, p.z, 0, 1, 0, u, v});
        }
    for (std::uint32_t j = 0; j < vSteps; ++j)
        for (std::uint32_t i = 0; i < uSteps; ++i) {
            const std::uint32_t a = j * (uSteps + 1) + i, b = a + uSteps + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    computeNormals(m);
    return m;
}

void computeNormals(MeshData& m) {
    std::vector<Vec3> n(m.vertices.size(), Vec3{0, 0, 0});
    auto pos = [&](std::uint32_t i) { return Vec3{m.vertices[i].px, m.vertices[i].py, m.vertices[i].pz}; };
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const std::uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        const Vec3 fn = cross(pos(b) - pos(a), pos(c) - pos(a)); // length = twice the area
        n[a] = n[a] + fn, n[b] = n[b] + fn, n[c] = n[c] + fn;
    }
    for (std::size_t i = 0; i < n.size(); ++i) {
        const Vec3 v = dot(n[i], n[i]) > 0.0f ? normalize(n[i]) : Vec3{0, 1, 0};
        m.vertices[i].nx = v.x, m.vertices[i].ny = v.y, m.vertices[i].nz = v.z;
    }
}

namespace {
/// Smooth pseudo-noise in about [-1, 1] for displacing the blobs (sums of sines: cheap, deterministic).
float blobNoise(Vec3 p) {
    return 0.55f * std::sin(2.1f * p.x + 1.3f) * std::sin(1.7f * p.y + 0.4f) * std::sin(2.6f * p.z + 2.1f) +
           0.30f * std::sin(4.3f * p.x + 1.1f * p.y) * std::sin(3.7f * p.z - 0.6f * p.y + 0.8f) +
           0.15f * std::sin(8.1f * p.y + 2.3f * p.z) * std::sin(7.9f * p.x + 0.3f);
}
} // namespace

MeshData blob(std::uint32_t subdivisions) {
    MeshData m = icosphere(subdivisions);
    for (Vertex& v : m.vertices) {
        const Vec3 n{v.px, v.py, v.pz};
        const Vec3 p = n * (1.0f + 0.25f * blobNoise(n * 1.7f));
        v.px = p.x, v.py = p.y, v.pz = p.z;
    }
    computeNormals(m);
    return m;
}

MeshData flutedColumn(std::uint32_t steps, std::uint32_t rows, int flutes, float depth, float twist) {
    return parametric(steps, rows, [=](float u, float v) {
        // u runs clockwise seen from above so that u x v points outwards.
        const float a = -u * 2.0f * kPi;
        const float r = 1.0f - depth * (0.5f + 0.5f * std::cos(float(flutes) * a + twist * v));
        return Vec3{r * std::cos(a), v, r * std::sin(a)};
    });
}

MeshData lathe(std::uint32_t steps, std::uint32_t rows, const std::function<float(float)>& radius) {
    return parametric(steps, rows, [&](float u, float v) {
        const float a = -u * 2.0f * kPi, r = radius(v);
        return Vec3{r * std::cos(a), v, r * std::sin(a)};
    });
}

void appendTube(MeshData& dst, Vec3 a, Vec3 b, float r0, float r1, std::uint32_t segments) {
    const Vec3 axis = normalize(b - a);
    const Vec3 t = normalize(cross(axis, std::abs(axis.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 bt = cross(axis, t);
    // The surface normal leans along the axis by the taper.
    const float lean = (r0 - r1) / std::max(1e-6f, std::sqrt(dot(b - a, b - a)));
    const auto base = static_cast<std::uint32_t>(dst.vertices.size());
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float u = float(s) / float(segments), th = u * 2.0f * kPi;
        const Vec3 radial = t * std::cos(th) + bt * std::sin(th);
        const Vec3 n = normalize(radial + axis * lean);
        const Vec3 p0 = a + radial * r0, p1 = b + radial * r1;
        dst.vertices.push_back({p0.x, p0.y, p0.z, n.x, n.y, n.z, u, 0});
        dst.vertices.push_back({p1.x, p1.y, p1.z, n.x, n.y, n.z, u, 1});
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        const std::uint32_t i = base + s * 2;
        dst.indices.insert(dst.indices.end(), {i, i + 2, i + 1, i + 1, i + 2, i + 3});
    }
}

void appendCard(MeshData& dst, Vec3 c, Vec3 u, Vec3 v) {
    const Vec3 n = normalize(cross(u, v));
    const auto base = static_cast<std::uint32_t>(dst.vertices.size());
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& k : corners) {
        const Vec3 p = c + u * k[0] + v * k[1];
        dst.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, 0.5f + 0.5f * k[0], 0.5f + 0.5f * k[1]});
    }
    dst.indices.insert(dst.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

namespace {
float hash2(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / float(0xFFFFFF);
}
} // namespace

float noise2(float x, float y) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = int(fx), iy = int(fy);
    const float tx = x - fx, ty = y - fy;
    const float ux = tx * tx * (3 - 2 * tx), uy = ty * ty * (3 - 2 * ty);
    const float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

float fbm2(float x, float y, int octaves) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * noise2(x, y);
        x = x * 2.03f + 17.1f;
        y = y * 2.03f + 9.3f;
        amp *= 0.5f;
    }
    return sum;
}

std::uint32_t MeshPool::add(const MeshData& mesh) {
    SubMesh s;
    s.firstIndex = static_cast<std::uint32_t>(m_indices.size());
    s.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    s.baseVertex = static_cast<std::int32_t>(m_vertices.size());
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : mesh.vertices) {
        lo = {std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
        hi = {std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
    }
    s.center = (lo + hi) * 0.5f;
    for (const Vertex& v : mesh.vertices) {
        const Vec3 d = Vec3{v.px, v.py, v.pz} - s.center;
        s.radius = std::max(s.radius, std::sqrt(dot(d, d)));
    }
    m_vertices.insert(m_vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    m_indices.insert(m_indices.end(), mesh.indices.begin(), mesh.indices.end());
    m_meshes.push_back(s);
    return static_cast<std::uint32_t>(m_meshes.size() - 1);
}

void MeshPool::upload(rhi::Device& dev, rhi::BufferUsage extra) {
    using U = rhi::BufferUsage;
    m_vb = dev.createBuffer({m_vertices.size() * sizeof(Vertex), U::Vertex | U::Storage | U::CopyDst | extra,
                             rhi::MemoryType::Device, sizeof(Vertex), "scene-vertices"},
                            m_vertices.data());
    m_ib = dev.createBuffer({m_indices.size() * 4, U::Index | U::Storage | U::CopyDst | extra, rhi::MemoryType::Device, 4,
                             "scene-indices"},
                            m_indices.data());
}

CameraKey CameraPath::at(double t) const {
    if (m_keys.empty()) return {};
    if (m_keys.size() == 1) return m_keys[0];
    const double x = std::clamp(t, 0.0, 1.0) * double(m_keys.size() - 1);
    const auto i = std::min<std::size_t>(static_cast<std::size_t>(x), m_keys.size() - 2);
    const float f = static_cast<float>(x - double(i));
    auto key = [&](std::ptrdiff_t k) { return m_keys[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(k, 0, std::ptrdiff_t(m_keys.size()) - 1))]; };
    auto cr = [&](Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3) {
        const float f2 = f * f, f3 = f2 * f;
        return (p1 * 2.0f + (p2 - p0) * f + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * f2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * f3) *
               0.5f;
    };
    const auto ii = static_cast<std::ptrdiff_t>(i);
    return {cr(key(ii - 1).eye, key(ii).eye, key(ii + 1).eye, key(ii + 2).eye),
            cr(key(ii - 1).target, key(ii).target, key(ii + 1).target, key(ii + 2).target)};
}

std::array<std::array<float, 4>, 6> frustumPlanes(const Mat4& m) {
    auto row = [&](int r) { return std::array<float, 4>{m.m[0][r], m.m[1][r], m.m[2][r], m.m[3][r]}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    std::array<std::array<float, 4>, 6> p{};
    for (int k = 0; k < 4; ++k) {
        p[0][k] = r3[k] + r0[k];
        p[1][k] = r3[k] - r0[k];
        p[2][k] = r3[k] + r1[k];
        p[3][k] = r3[k] - r1[k];
        p[4][k] = r2[k];
        p[5][k] = r3[k] - r2[k];
    }
    for (auto& pl : p) {
        const float l = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        for (float& c : pl) c /= l;
    }
    return p;
}

bool sphereVisible(const std::array<std::array<float, 4>, 6>& planes, Vec3 c, float r) {
    for (const auto& p : planes)
        if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -r) return false;
    return true;
}

Mat4 orthographic(float l, float r, float b, float t, float n, float f) {
    Mat4 m = Mat4::identity();
    m.m[0][0] = 2.0f / (r - l);
    m.m[1][1] = 2.0f / (t - b);
    m.m[2][2] = 1.0f / (n - f);
    m.m[3][0] = -(r + l) / (r - l);
    m.m[3][1] = -(t + b) / (t - b);
    m.m[3][2] = n / (n - f);
    return m;
}

} // namespace gt::bench::scene
