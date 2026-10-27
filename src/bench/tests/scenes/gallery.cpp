// "Gallery" (ultimate tier): real-time path tracing (shaders/src/scenes/gallery.slang).
// A sculpture hall with skylights, a warm light panel, fluted columns, a glass bead chandelier,
// polished metals, a lacquered floor and a deep red wall for colour bleeding. The exhibits are dense
// procedural meshes (a displaced glass blob of up to 1.3 M triangles, a gold torus knot, a twisted
// copper column, a steel lattice sphere, a stone blob). A compute pass finds each pixel's primary
// hit and traces N paths from it (next-event estimation, Russian roulette); the albedo-demodulated
// result goes through an edge-aware a-trous wavelet filter before re-modulation and tone mapping.
// The score follows frame rate; paths per second are reported alongside.

#include "rt_scene.h"

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

namespace gt::bench {
namespace {

using namespace scene;

enum MeshKind {
    kBox, kCylinder, kSphere, kTorus, kBlob, kKnot, kTwist, kLattice, kFluted, kBead, kRing, kMeshKinds
};
enum Material { kPlaster, kFloor, kWood, kGold, kChrome, kCopper, kGlass, kEmissive, kPaint, kStone };

struct RectLight {
    float center[4]; ///< w = 0 sky portal, 1 emitter
    float u[4], v[4]; ///< half extents
    float radiance[4];
};

struct FrameData {
    Mat4 invViewProj;
    float cameraPos[4];
    float sunDir[4];
    float sunColor[4];
    float screen[4];
    std::uint32_t params[4];
    RectLight lights[8];
};
static_assert(sizeof(FrameData) == 64 + 5 * 16 + 8 * 64);

RectLight rectLight(Vec3 c, Vec3 u, Vec3 v, bool emitter, Vec3 radiance = {}) {
    return {{c.x, c.y, c.z, emitter ? 1.0f : 0.0f}, {u.x, u.y, u.z, 0}, {v.x, v.y, v.z, 0}, {radiance.x, radiance.y, radiance.z, 0}};
}

struct FilterParams {
    std::int32_t stepSize;
    float phiLuminance;
    float phiDistance;
    float pad;
};

constexpr float kSkylights[] = {-10.5f, -3.0f, 4.5f, 12.0f}; // centres along z: 4 m long, 6 m wide

/// Mesh detail and path budget at each quality.
struct Detail {
    std::uint32_t blobLevel;               ///< icosphere subdivisions of the blobs (20 * 4^n triangles)
    std::uint32_t knotSteps, knotSides;    ///< torus knot tube
    std::uint32_t twistSteps, twistRows;   ///< twisted column
    std::uint32_t latticeLevel;            ///< icosphere whose edges are the lattice's struts
    std::uint32_t flutedSteps, flutedRows; ///< wall columns
    std::uint32_t chandelierRings;
    bool floorTiles;                       ///< the floor as 1440 tile instances (else one slab)
    std::uint32_t paths, bounces;
};

Detail detailFor(Quality q, bool software) {
    if (software) return {3, 128, 8, 48, 12, 1, 48, 4, 1, false, 1, 2};
    switch (q) {
    case Quality::Low: return {5, 256, 12, 96, 24, 2, 96, 8, 2, false, 1, 3};
    case Quality::Medium: return {6, 512, 16, 128, 48, 2, 144, 12, 3, true, 3, 4};
    case Quality::High: return {7, 1024, 24, 192, 96, 3, 192, 24, 3, true, 8, 5};
    case Quality::Ultra: return {8, 2048, 32, 256, 128, 3, 256, 32, 3, true, 16, 6};
    }
    return {7, 1024, 24, 192, 96, 3, 192, 24, 3, true, 8, 5};
}

/// A (2, 5) torus knot tube about 3 units across, in the XZ plane.
MeshData torusKnot(std::uint32_t steps, std::uint32_t sides) {
    auto curve = [](float t) {
        const float r = 2.0f + std::cos(5.0f * t);
        return Vec3{r * std::cos(2.0f * t), std::sin(5.0f * t), r * std::sin(2.0f * t)};
    };
    return parametric(steps, sides, [&](float u, float v) {
        const float t = u * 2.0f * kPi, e = 1e-3f;
        const Vec3 c = curve(t), tangent = normalize(curve(t + e) - curve(t - e));
        // A periodic frame: away from the torus's core circle, made perpendicular to the tangent.
        const Vec3 core{2.0f * std::cos(2.0f * t), 0, 2.0f * std::sin(2.0f * t)};
        Vec3 nrm = c - core;
        nrm = normalize(nrm - tangent * dot(nrm, tangent));
        const Vec3 bin = cross(tangent, nrm);
        const float a = v * 2.0f * kPi;
        return c + (nrm * std::cos(a) + bin * std::sin(a)) * 0.35f;
    });
}

/// The edges of an icosphere as thin struts (a geodesic lattice of radius 1).
MeshData lattice(std::uint32_t level, float strut) {
    const MeshData ico = icosphere(level);
    MeshData m;
    std::vector<std::uint64_t> edges;
    for (std::size_t i = 0; i < ico.indices.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = ico.indices[i + k], b = ico.indices[i + (k + 1) % 3];
            edges.push_back((std::uint64_t(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    auto pos = [&](std::uint64_t i) {
        const Vertex& v = ico.vertices[static_cast<std::size_t>(i)];
        return Vec3{v.px, v.py, v.pz};
    };
    for (std::uint64_t e : edges) appendTube(m, pos(e >> 32), pos(e & 0xFFFFFFFFu), strut, strut, 6);
    return m;
}
constexpr Vec3 kPanelRadiance{7.0f, 5.74f, 4.2f}; // the emitter's instance colour is its radiance

std::vector<RtInstance> buildGallery(const Detail& detail) {
    std::vector<RtInstance> v;
    auto add = [&](MeshKind mesh, Vec3 t, Vec3 s, Vec3 c, Material m, float yaw = 0.0f, float roll = 0.0f) {
        v.push_back(rtInstance(mesh, t, s, yaw, roll, c, float(m)));
    };
    // Unit box is centred on the origin; cylinders run from y = 0 to 1.
    const Vec3 plaster{0.86f, 0.84f, 0.80f}, red{0.52f, 0.09f, 0.07f}, stone{0.86f, 0.84f, 0.80f},
        wood{0.42f, 0.27f, 0.15f};
    // Shell: floor, walls, ceiling with four skylights. The floor is one slab, or 0.5 m tiles (1440 instances)
    // over a dark grout bed that shows in the gaps.
    if (detail.floorTiles) {
        add(kBox, {0, -0.1f, 0}, {12.4f, 0.2f, 30.4f}, {0.22f, 0.21f, 0.20f}, kStone);
        for (int i = 0; i < 24; ++i)
            for (int k = 0; k < 60; ++k)
                add(kBox, {-5.75f + 0.5f * float(i), 0.015f, -14.75f + 0.5f * float(k)}, {0.49f, 0.03f, 0.49f},
                    {1, 1, 1}, kFloor);
    } else {
        add(kBox, {0, -0.1f, 0}, {12.4f, 0.2f, 30.4f}, {1, 1, 1}, kFloor);
    }
    add(kBox, {-6.1f, 3.5f, 0}, {0.2f, 7.0f, 30.4f}, plaster, kPlaster);
    add(kBox, {6.1f, 3.5f, 0}, {0.2f, 7.0f, 30.4f}, red, kPaint);
    add(kBox, {0, 3.5f, -15.1f}, {12.4f, 7.0f, 0.2f}, plaster, kPlaster);
    add(kBox, {0, 3.5f, 15.1f}, {12.4f, 7.0f, 0.2f}, plaster, kPlaster);
    add(kBox, {-4.6f, 7.1f, 0}, {3.2f, 0.2f, 30.4f}, plaster, kPlaster);
    add(kBox, {4.6f, 7.1f, 0}, {3.2f, 0.2f, 30.4f}, plaster, kPlaster);
    float z0 = -15.2f;
    for (float c : kSkylights) {
        add(kBox, {0, 7.1f, (z0 + c - 2.0f) / 2}, {6.0f, 0.2f, c - 2.0f - z0}, plaster, kPlaster);
        z0 = c + 2.0f;
    }
    add(kBox, {0, 7.1f, (z0 + 15.2f) / 2}, {6.0f, 0.2f, 15.2f - z0}, plaster, kPlaster);
    // Beams under the ceiling, columns along the walls.
    for (int i = 0; i < 11; ++i) add(kBox, {0, 6.85f, -15.0f + 3.0f * float(i)}, {12.0f, 0.3f, 0.25f}, wood, kWood);
    for (float x : {-5.3f, 5.3f})
        for (float z : {-12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
            add(kFluted, {x, 0, z}, {0.28f, 6.7f, 0.28f}, stone, kStone);
            add(kBox, {x, 0.15f, z}, {0.8f, 0.3f, 0.8f}, stone, kStone);
        }
    // Light installation on the far wall.
    add(kBox, {0, 3.4f, -14.95f}, {4.0f, 2.2f, 0.1f}, kPanelRadiance, kEmissive);
    add(kBox, {0, 3.4f, -14.98f}, {4.4f, 2.6f, 0.06f}, {0.08f, 0.08f, 0.08f}, kPaint);
    // Paintings on the left wall.
    const Vec3 paintings[] = {{0.15f, 0.25f, 0.55f}, {0.75f, 0.55f, 0.15f}, {0.20f, 0.45f, 0.25f}, {0.65f, 0.30f, 0.45f}};
    for (int i = 0; i < 4; ++i) {
        const float z = -9.0f + 6.0f * float(i);
        add(kBox, {-5.98f, 3.0f, z}, {0.04f, 1.7f, 2.3f}, wood, kWood);
        add(kBox, {-5.95f, 3.0f, z}, {0.04f, 1.4f, 2.0f}, paintings[i], kPaint);
    }
    // Plinths and sculptures.
    auto plinth = [&](float x, float z, float size, float height) {
        add(kBox, {x, height / 2, z}, {size, height, size}, plaster, kPlaster);
    };
    plinth(-2.5f, -7.0f, 1.0f, 1.0f);
    add(kBlob, {-2.5f, 1.66f, -7.0f}, {0.55f, 0.55f, 0.55f}, {0.92f, 0.97f, 0.95f}, kGlass, 0.4f);
    plinth(2.5f, -7.0f, 1.0f, 1.0f);
    add(kKnot, {2.5f, 1.75f, -7.0f}, {0.22f, 0.22f, 0.22f}, {1.0f, 0.78f, 0.34f}, kGold, 0.5f, 1.2f);
    add(kBox, {0, 0.3f, -1.0f}, {2.2f, 0.6f, 2.2f}, stone, kStone);
    add(kSphere, {0, 1.5f, -1.0f}, {0.9f, 0.9f, 0.9f}, {0.95f, 0.95f, 0.96f}, kChrome);
    plinth(-2.5f, 5.0f, 1.0f, 1.0f);
    add(kTwist, {-2.5f, 1.0f, 5.0f}, {0.35f, 1.3f, 0.35f}, {0.95f, 0.64f, 0.54f}, kCopper);
    add(kSphere, {-2.5f, 2.42f, 5.0f}, {0.16f, 0.16f, 0.16f}, {0.95f, 0.64f, 0.54f}, kCopper);
    plinth(2.5f, 5.0f, 1.0f, 0.8f);
    add(kBox, {2.5f, 1.05f, 5.0f}, {0.7f, 0.5f, 0.7f}, {0.12f, 0.22f, 0.65f}, kPaint, 0.3f);
    add(kBox, {2.5f, 1.5f, 5.0f}, {0.55f, 0.4f, 0.55f}, {0.85f, 0.62f, 0.10f}, kPaint, 0.9f);
    add(kBox, {2.5f, 1.88f, 5.0f}, {0.4f, 0.36f, 0.4f}, {0.90f, 0.90f, 0.88f}, kPlaster, 1.4f);
    plinth(-2.5f, 11.0f, 1.0f, 0.9f);
    add(kLattice, {-2.5f, 1.52f, 11.0f}, {0.6f, 0.6f, 0.6f}, {0.95f, 0.95f, 0.96f}, kChrome, 0.3f);
    plinth(2.5f, 11.0f, 1.0f, 0.8f);
    add(kBlob, {2.5f, 1.45f, 11.0f}, {0.55f, 0.55f, 0.55f}, {0.56f, 0.51f, 0.45f}, kStone, 2.1f, 0.3f);
    // A chandelier between the skylights: a chrome rod and rings hung with glass beads.
    const float hangZ = 0.75f;
    add(kCylinder, {0, 5.0f, hangZ}, {0.03f, 2.0f, 0.03f}, {0.95f, 0.95f, 0.96f}, kChrome);
    const float ringRadius[] = {0.6f, 1.0f, 1.4f}, ringY[] = {5.7f, 5.4f, 5.1f};
    const int ringBeads[] = {18, 30, 42};
    for (std::uint32_t r = 0; r < detail.chandelierRings && r < 3; ++r) {
        const float rr = ringRadius[r];
        add(kRing, {0, ringY[r], hangZ}, {rr, rr, rr}, {0.95f, 0.95f, 0.96f}, kChrome);
        for (int b = 0; b < ringBeads[r]; ++b) {
            const float a = (float(b) + 0.5f * float(r)) * 2.0f * kPi / float(ringBeads[r]);
            add(kBead, {rr * std::cos(a), ringY[r] - 0.1f, hangZ + rr * std::sin(a)}, {0.07f, 0.07f, 0.07f},
                {0.95f, 0.97f, 0.98f}, kGlass);
        }
    }
    // Benches.
    for (float z : {9.5f, -11.5f}) {
        add(kBox, {0, 0.45f, z}, {3.0f, 0.1f, 0.8f}, wood, kWood);
        for (float x : {-1.3f, 1.3f}) add(kBox, {x, 0.2f, z}, {0.12f, 0.4f, 0.6f}, wood, kWood);
    }
    return v;
}

class Gallery final : public Test {
public:
    Gallery() {
        m_info.id = "scene.gallery";
        m_info.name = "Gallery";
        m_info.description = "Real-time path tracing of a sculpture hall: several paths per pixel with sun sampling, "
                             "glass, metals and colour bleeding, denoised by an edge-aware wavelet filter.";
        m_info.category = Category::Scene;
        m_info.group = "Ultimate";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12};
        m_info.reference = 60.0; // provisional, see docs/SCORING.md
        m_info.tags = {"ultimate", "rt", "path-tracing", "cpu-light"};
        m_info.suites = {"extreme", "raytracing"};
        m_info.order = 60;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().rayQuery ? SupportResult::yes() : SupportResult::no("no ray queries");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        // Software renderers get coarser meshes and fewer paths (--param scene.full_detail=1 keeps all of it).
        const bool software = dev.adapter().isSoftware() && s.param("scene.full_detail") != "1";
        using U = rhi::BufferUsage;
        using TU = rhi::TextureUsage;
        using BT = rhi::BindingType;

        // --- content -----------------------------------------------------------------------------
        const Detail detail = detailFor(s.quality, software);
        MeshPool pool;
        const std::vector<std::uint32_t> meshIds = {
            pool.add(box(1, 1, 1)),
            pool.add(cylinder(software ? 16 : 32, true)),
            pool.add(sphere(software ? 24 : 48, software ? 16 : 32)),
            pool.add(torus(software ? 32 : 64, software ? 12 : 24, 0.22f)),
            pool.add(blob(detail.blobLevel)),
            pool.add(torusKnot(detail.knotSteps, detail.knotSides)),
            pool.add(flutedColumn(detail.twistSteps, detail.twistRows, 12, 0.12f, 2.0f * kPi)),
            pool.add(lattice(detail.latticeLevel, 0.018f)),
            pool.add(flutedColumn(detail.flutedSteps, detail.flutedRows, 24, 0.06f, 0.0f)),
            pool.add(sphere(software ? 8 : 12, software ? 6 : 8)),
            pool.add(torus(software ? 32 : 96, software ? 6 : 8, 0.02f))};
        pool.upload(dev, U::AccelInput);
        std::vector<RtInstance> instances = buildGallery(detail);
        const RtScene rs = buildRtScene(ctx, pool, meshIds, instances);

        // --- targets -----------------------------------------------------------------------------
        const std::uint32_t w = s.width, h = s.height;
        auto tex = [&](rhi::Format fmt, TU usage, const char* name) {
            rhi::TextureDesc td;
            td.format = fmt;
            td.width = w;
            td.height = h;
            td.usage = usage;
            td.name = name;
            return dev.createTexture(td);
        };
        auto radianceA = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "gallery-radiance-a");
        auto radianceB = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "gallery-radiance-b");
        auto gbuffer = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "gallery-gbuffer");
        auto albedo = tex(rhi::Format::RGBA8_UNORM, TU::Storage | TU::Sampled, "gallery-albedo");
        auto output = tex(rhi::Format::RGBA8_UNORM, TU::RenderTarget | TU::Sampled | TU::CopySrc, "gallery-output");

        // --- pipelines ---------------------------------------------------------------------------
        rhi::ComputePipelineDesc pd;
        pd.name = "gallery-pathtrace";
        pd.compute = {"scenes/gallery", "csPathTrace"};
        pd.layout.bindings = {{BT::UniformBuffer, 0},   {BT::AccelerationStructure, 0}, {BT::StorageBufferRO, 1},
                              {BT::StorageBufferRO, 2}, {BT::StorageBufferRO, 3},       {BT::StorageBufferRO, 4},
                              {BT::StorageTexture, 0},  {BT::StorageTexture, 1},        {BT::StorageTexture, 2}};
        auto tracePipe = dev.createComputePipeline(pd);
        rhi::ComputePipelineDesc fd;
        fd.name = "gallery-filter";
        fd.compute = {"scenes/gallery", "csFilter"};
        fd.layout.bindings = {{BT::UniformBuffer, 0}, {BT::SampledTexture, 5}, {BT::SampledTexture, 6}, {BT::StorageTexture, 3}};
        fd.layout.pushConstantBytes = sizeof(FilterParams);
        auto filterPipe = dev.createComputePipeline(fd);
        rhi::GraphicsPipelineDesc cd;
        cd.name = "gallery-composite";
        cd.vertex = {"scenes/gallery", "vsFullscreen"};
        cd.fragment = {"scenes/gallery", "psComposite"};
        cd.colorFormats = {rhi::Format::RGBA8_UNORM};
        cd.depth.test = false;
        cd.depth.write = false;
        cd.raster.cull = rhi::CullMode::None;
        cd.layout.bindings = {{BT::SampledTexture, 8}, {BT::SampledTexture, 7}};
        auto compositePipe = dev.createGraphicsPipeline(cd);

        const std::uint32_t paths = detail.paths, bounces = detail.bounces;
        const int filterPasses = 3; // steps 1, 2, 4: a 17x17 footprint
        std::uint64_t bvhTriangles = 0;
        for (const RtInstance& in : instances)
            bvhTriangles += pool[meshIds[static_cast<std::size_t>(in.info[0])]].indexCount / 3;
        auto& d = ctx.details();
        d["instances"] = instances.size();
        d["bvh_triangles"] = bvhTriangles;
        d["blob_triangles"] = pool[meshIds[kBlob]].indexCount / 3;
        d["paths_per_pixel"] = paths;
        d["bounces_per_path"] = bounces;
        d["filter_passes"] = filterPasses;
        if (software) d["note"] = "software renderer: coarser meshes, one path per pixel, two bounces";

        // --- frame -------------------------------------------------------------------------------
        const CameraPath path({{{0, 1.7f, 13.5f}, {0, 1.6f, -2}},
                               {{-3.2f, 1.7f, 7}, {-1, 1.4f, -1}},
                               {{-4.2f, 1.6f, -1.5f}, {-2.4f, 1.5f, -7}},
                               {{0.8f, 1.8f, -4}, {2.5f, 1.7f, -7}},
                               {{4.0f, 2.2f, -10}, {0, 3.0f, -15}},
                               {{3.0f, 4.8f, -12}, {-1, 1.0f, 3}}});
        const Vec3 sun = normalize({0.32f, 0.88f, -0.34f});
        std::vector<RectLight> lights;
        for (float c : kSkylights) lights.push_back(rectLight({0, 7.2f, c}, {3.0f, 0, 0}, {0, 0, 2.0f}, false));
        lights.push_back(rectLight({0, 3.4f, -14.9f}, {2.0f, 0, 0}, {0, 1.1f, 0}, true, kPanelRadiance));
        const float aspect = float(w) / float(h);
        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(600);
        fl.simDuration = 30.0;
        fl.realTime = true;
        fl.output = output.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        fl.record = [&](FrameContext& fc) {
            const CameraKey cam = path.at(fc.simTime / fl.simDuration);
            FrameData fdat{};
            fdat.invViewProj =
                inverse(perspective(70.0f * kPi / 180.0f, aspect, 0.05f, 200.0f) * lookAt(cam.eye, cam.target, {0, 1, 0}));
            fdat.cameraPos[0] = cam.eye.x, fdat.cameraPos[1] = cam.eye.y, fdat.cameraPos[2] = cam.eye.z;
            fdat.cameraPos[3] = static_cast<float>(fc.simTime);
            fdat.sunDir[0] = sun.x, fdat.sunDir[1] = sun.y, fdat.sunDir[2] = sun.z, fdat.sunDir[3] = 7.0f;
            fdat.sunColor[0] = 1.0f, fdat.sunColor[1] = 0.94f, fdat.sunColor[2] = 0.84f, fdat.sunColor[3] = 0.008f;
            fdat.screen[0] = float(w), fdat.screen[1] = float(h), fdat.screen[2] = 1.0f / float(w), fdat.screen[3] = 1.0f / float(h);
            // The noise pattern depends on the frame index; golden frames and passes repeat it.
            fdat.params[0] = fc.warmup ? 0u : fc.frame;
            fdat.params[1] = paths;
            fdat.params[2] = bounces;
            fdat.params[3] = static_cast<std::uint32_t>(lights.size());
            std::copy(lights.begin(), lights.end(), fdat.lights);

            rhi::CommandList& cmd = fc.cmd;
            for (rhi::Texture* t : {radianceA.get(), gbuffer.get(), albedo.get()})
                cmd.transition(t, rhi::ResourceState::UnorderedAccess);
            cmd.setUniformData(0, &fdat, sizeof(fdat));
            cmd.setPipeline(tracePipe.get());
            cmd.setAccelerationStructure(0, rs.tlas.get());
            cmd.setReadOnlyBuffer(1, rs.instances.get());
            cmd.setReadOnlyBuffer(2, rs.meshes.get());
            cmd.setReadOnlyBuffer(3, pool.vertexBuffer());
            cmd.setReadOnlyBuffer(4, pool.indexBuffer());
            cmd.setStorageTexture(0, radianceA.get());
            cmd.setStorageTexture(1, gbuffer.get());
            cmd.setStorageTexture(2, albedo.get());
            cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            for (rhi::Texture* t : {radianceA.get(), gbuffer.get(), albedo.get()})
                cmd.transition(t, rhi::ResourceState::ShaderRead);

            // A-trous passes ping-pong A -> B -> A -> B.
            rhi::Texture* src = radianceA.get();
            rhi::Texture* dst = radianceB.get();
            cmd.setPipeline(filterPipe.get());
            for (int i = 0; i < filterPasses; ++i) {
                // The first pass has no luminance edge stop so isolated bright samples get spread.
                const FilterParams fp{1 << i, i == 0 ? 1e4f : 4.0f, 0.02f, 0.0f};
                cmd.transition(dst, rhi::ResourceState::UnorderedAccess);
                cmd.setUniformData(0, &fdat, sizeof(fdat));
                cmd.pushConstants(&fp, sizeof(fp));
                cmd.setTexture(5, src);
                cmd.setTexture(6, gbuffer.get());
                cmd.setStorageTexture(3, dst);
                cmd.dispatch((w + 7) / 8, (h + 7) / 8);
                cmd.transition(dst, rhi::ResourceState::ShaderRead);
                std::swap(src, dst);
            }

            rhi::RenderPassDesc cp;
            cp.colors.push_back({output.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            cmd.beginRenderPass(cp);
            cmd.setPipeline(compositePipe.get());
            cmd.setTexture(8, src);
            cmd.setTexture(7, albedo.get());
            cmd.draw(3);
            cmd.endRenderPass();
        };
        const FrameLoopResult r = ctx.runFrames(fl);
        ctx.setFrameResult(r);
        if (!r.frameMs.empty()) {
            const double meanMs = std::accumulate(r.frameMs.begin(), r.frameMs.end(), 0.0) / double(r.frameMs.size());
            ctx.addMetric("mpaths_per_s", "Paths per second", double(w) * h * paths / (meanMs * 1e3), "Mpaths/s");
        }
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::Gallery)
