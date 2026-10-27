#include "gt/bench/scene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using namespace gt::bench;
using namespace gt::bench::scene;

namespace {
/// Signed volume by the divergence theorem: positive when triangles wind counter-clockwise seen from outside.
double signedVolume(const MeshData& m) {
    double v = 0.0;
    for (std::size_t i = 0; i < m.indices.size(); i += 3) {
        const Vertex& a = m.vertices[m.indices[i]];
        const Vertex& b = m.vertices[m.indices[i + 1]];
        const Vertex& c = m.vertices[m.indices[i + 2]];
        v += (a.px * (b.py * c.pz - b.pz * c.py) - a.py * (b.px * c.pz - b.pz * c.px) + a.pz * (b.px * c.py - b.py * c.px)) / 6.0;
    }
    return v;
}

/// Every triangle's geometric normal agrees with its vertex normals.
bool normalsAgree(const MeshData& m) {
    for (std::size_t i = 0; i < m.indices.size(); i += 3) {
        const Vertex& a = m.vertices[m.indices[i]];
        const Vertex& b = m.vertices[m.indices[i + 1]];
        const Vertex& c = m.vertices[m.indices[i + 2]];
        const Vec3 n = cross(Vec3{b.px - a.px, b.py - a.py, b.pz - a.pz}, Vec3{c.px - a.px, c.py - a.py, c.pz - a.pz});
        if (dot(n, n) < 1e-12f) continue; // degenerate (sphere poles)
        if (dot(n, Vec3{a.nx + b.nx + c.nx, a.ny + b.ny + c.ny, a.nz + b.nz + c.nz}) <= 0.0f) return false;
    }
    return true;
}
} // namespace

TEST_CASE("closed procedural meshes wind outwards", "[scene]") {
    CHECK(signedVolume(box(2, 4, 6)) == Approx(48.0).epsilon(1e-4));
    CHECK(signedVolume(sphere(48, 24)) == Approx(4.0 / 3.0 * 3.14159265).epsilon(0.02));
    CHECK(signedVolume(cylinder(64, true)) == Approx(3.14159265).epsilon(0.01));
    CHECK(signedVolume(torus(64, 32, 0.25f)) == Approx(2.0 * 3.14159265 * 3.14159265 * 0.25 * 0.25).epsilon(0.02));
    for (const MeshData& m : {box(1, 2, 3), sphere(16, 8), cylinder(16, true), cone(16), torus(16, 8, 0.3f)})
        CHECK(normalsAgree(m));
    CHECK(normalsAgree(heightfield(8, 10.0f, [](float x, float z) { return std::sin(x) * std::cos(z); })));
}

TEST_CASE("append transforms positions and normals", "[scene]") {
    MeshData m;
    append(m, box(1, 1, 1), {10, 0, 0}, 2.0f, 3.14159265f / 2.0f);
    CHECK(m.vertices.size() == 24);
    CHECK(signedVolume(m) == Approx(8.0).epsilon(1e-4));
    CHECK(normalsAgree(m));
}

TEST_CASE("camera path passes through its keys", "[scene]") {
    CameraPath path({{{0, 0, 0}, {0, 0, 1}}, {{10, 0, 0}, {10, 0, 1}}, {{10, 10, 0}, {10, 10, 1}}});
    CHECK(path.at(0.0).eye.x == Approx(0.0));
    CHECK(path.at(0.5).eye.x == Approx(10.0));
    CHECK(path.at(1.0).eye.y == Approx(10.0));
}

TEST_CASE("frustum planes cull spheres behind the camera", "[scene]") {
    const Mat4 vp = perspective(1.0f, 1.0f, 0.1f, 100.0f) * lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const auto planes = frustumPlanes(vp);
    CHECK(sphereVisible(planes, {0, 0, -10}, 1.0f));
    CHECK_FALSE(sphereVisible(planes, {0, 0, 10}, 1.0f));
    CHECK_FALSE(sphereVisible(planes, {0, 0, -200}, 1.0f));
    CHECK(sphereVisible(planes, {0, 0, 0.5f}, 1.0f)); // straddles the near plane
}
