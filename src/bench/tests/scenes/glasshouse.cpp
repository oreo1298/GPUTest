// "Glasshouse" (ultimate tier): hybrid ray tracing (shaders/src/scenes/glasshouse.slang).
// A steel-and-glass conservatory with a pond, a chrome sculpture, leafy trees and shrubs and beds
// of grass. The G-buffer is rasterised: leaves are alpha-tested cards, grass blades come from mesh
// shaders where the GPU has them (an identical vertex-shader path otherwise) and are shaded at 2x2
// where variable-rate shading exists. Then compute passes use ray queries, which test the leaf
// shape on every leaf card they cross: soft sun shadows, short ambient-occlusion rays and one-bounce
// diffuse GI (each hit with a shadow ray of its own), an edge-aware wavelet filter for the GI, and
// shading with one reflection ray per smooth surface plus one per glass pane in view. A composite
// pass tone-maps.

#include "rt_scene.h"

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/bench/test.h"

#include <cmath>
#include <utility>
#include <vector>

namespace gt::bench {
namespace {

using namespace scene;

enum MeshKind { kBox, kCylinder, kSphere, kTorus, kBark, kTreeLeaves, kShrubLeaves, kMeshKinds };
enum Material { kGround, kSteel, kChrome, kWater, kLeaves, kBarkMaterial, kSoil, kGrass, kStone };

struct FrameData {
    Mat4 viewProj;
    Mat4 invViewProj;
    float cameraPos[4];
    float sunDir[4];
    float sunColor[4];
    float screen[4];
    std::uint32_t params[4];
    std::uint32_t rays[4];
    float grass[4];
    float beds[8][4];
};
static_assert(sizeof(FrameData) == 128 + 7 * 16 + 128);

struct FilterParams {
    std::int32_t stepSize;
    float phiNormal;
    float phiPlane;
    float pad;
};

struct Bed {
    Vec3 centre;
    float half;
};

/// How much there is to draw and trace at each quality.
struct Detail {
    std::uint32_t treeLeaves, shrubLeaves; ///< leaf cards per tree / shrub
    std::uint32_t bladesPerRow;            ///< grass blades per row of a 1 m patch
    std::uint32_t shadowRays, aoRays, giRays;
    bool reflectionShadows;     ///< reflection and glass hits get a shadow ray
    int giFilterPasses;
};

Detail detailFor(Quality q, bool software) {
    if (software) return {250, 80, 4, 1, 1, 1, true, 2};
    switch (q) {
    case Quality::Low: return {1200, 300, 8, 1, 1, 0, false, 0};
    case Quality::Medium: return {2500, 600, 12, 1, 2, 1, true, 3};
    case Quality::High: return {5000, 1200, 20, 2, 4, 2, true, 3};
    case Quality::Ultra: return {9000, 2000, 28, 4, 8, 4, true, 4};
    }
    return {5000, 1200, 20, 2, 4, 2, true, 3};
}

/// A broadleaf tree about 7 m tall: a tapered trunk, seven limbs with three twigs each, and `leaves` cards in
/// clusters around the limb and twig ends.
std::pair<MeshData, MeshData> makeTree(std::uint32_t leaves, std::uint32_t segments) {
    Random rng(0x7EE5u);
    MeshData bark, cards;
    appendTube(bark, {0, 0, 0}, {0, 3.0f, 0}, 0.24f, 0.15f, segments);
    std::vector<Vec3> tips;
    const int limbs = 7;
    for (int i = 0; i < limbs; ++i) {
        const float yaw = (float(i) + rng.uniform(-0.3f, 0.3f)) * 2.0f * kPi / float(limbs);
        const Vec3 start{0, rng.uniform(2.1f, 2.9f), 0};
        const Vec3 dir = normalize({std::cos(yaw), rng.uniform(0.5f, 1.1f), std::sin(yaw)});
        const Vec3 end = start + dir * rng.uniform(1.7f, 2.5f);
        appendTube(bark, start, end, 0.11f, 0.05f, segments / 2 + 2);
        tips.push_back(end);
        for (int j = 0; j < 3; ++j) {
            const Vec3 from = start + (end - start) * rng.uniform(0.45f, 0.95f);
            const Vec3 bend{rng.uniform(-0.8f, 0.8f), rng.uniform(-0.1f, 0.7f), rng.uniform(-0.8f, 0.8f)};
            const Vec3 d = normalize(dir + bend);
            const Vec3 to = from + d * rng.uniform(0.7f, 1.2f);
            appendTube(bark, from, to, 0.045f, 0.02f, 4);
            tips.push_back(to);
        }
    }
    for (std::uint32_t k = 0; k < leaves; ++k) {
        Vec3 off;
        do off = {rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)};
        while (dot(off, off) > 1.0f);
        const Vec3 c = tips[k % tips.size()] + off * 0.95f;
        const Vec3 n = normalize({rng.uniform(-1, 1), rng.uniform(0.2f, 1.5f), rng.uniform(-1, 1)});
        const Vec3 t = normalize(cross(n, std::abs(n.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        const Vec3 b = cross(n, t);
        const float spin = rng.uniform(0.0f, 2.0f * kPi), len = rng.uniform(0.09f, 0.14f);
        const Vec3 along = t * std::cos(spin) + b * std::sin(spin), across = cross(n, along);
        appendCard(cards, c, across * (len * 0.55f), along * len);
    }
    return {std::move(bark), std::move(cards)};
}

/// A rounded shrub: `leaves` cards in an ellipsoid 1.5 m wide and 1.2 m tall, standing on y = 0.
MeshData makeShrub(std::uint32_t leaves) {
    Random rng(0x5B1Bu);
    MeshData cards;
    for (std::uint32_t k = 0; k < leaves; ++k) {
        Vec3 off;
        do off = {rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)};
        while (dot(off, off) > 1.0f);
        const Vec3 c{off.x * 0.75f, 0.6f + off.y * 0.6f, off.z * 0.75f};
        const Vec3 tilt{rng.uniform(-0.6f, 0.6f), 0, rng.uniform(-0.6f, 0.6f)};
        const Vec3 n = normalize(Vec3{off.x, std::abs(off.y) + 0.3f, off.z} + tilt);
        const Vec3 t = normalize(cross(n, std::abs(n.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        const Vec3 b = cross(n, t);
        const float spin = rng.uniform(0.0f, 2.0f * kPi), len = rng.uniform(0.06f, 0.10f);
        const Vec3 along = t * std::cos(spin) + b * std::sin(spin), across = cross(n, along);
        appendCard(cards, c, across * (len * 0.55f), along * len);
    }
    return cards;
}

const float kTreeZ[] = {-24.0f, -14.0f, -4.0f, 6.0f, 16.0f, 26.0f};

std::vector<RtInstance> buildGlasshouse(const std::vector<Bed>& beds) {
    std::vector<RtInstance> v;
    auto add = [&](MeshKind mesh, Vec3 t, Vec3 s, Vec3 c, Material m, float yaw = 0.0f, float roll = 0.0f) {
        v.push_back(rtInstance(mesh, t, s, yaw, roll, c, float(m)));
    };
    const Vec3 steel{0.55f, 0.57f, 0.60f}, stone{0.58f, 0.55f, 0.50f};
    add(kBox, {0, -0.1f, 0}, {90, 0.2f, 110}, {1, 1, 1}, kGround);
    // Frame: posts, eave and mid beams, rafters, purlins, ridge.
    for (int i = 0; i <= 24; ++i) {
        const float z = -30.0f + float(i) * 2.5f;
        for (float sx : {-1.0f, 1.0f}) {
            add(kCylinder, {sx * 20.0f, 0, z}, {0.09f, 8.0f, 0.09f}, steel, kSteel);
            // Rafter from (±20, 8) to (0, 14): centre (±10, 11), length 20.88, pitch atan(6/20).
            add(kBox, {sx * 10.0f, 11.0f, z}, {20.9f, 0.18f, 0.12f}, steel, kSteel, 0.0f, -sx * std::atan(6.0f / 20.0f));
        }
    }
    for (float sx : {-1.0f, 1.0f}) {
        add(kBox, {sx * 20.0f, 8.0f, 0}, {0.25f, 0.25f, 60}, steel, kSteel);
        add(kBox, {sx * 20.0f, 4.0f, 0}, {0.15f, 0.15f, 60}, steel, kSteel);
        for (int k = 1; k < 8; ++k) {
            const float f = float(k) / 8.0f;
            add(kBox, {sx * 20.0f * (1.0f - f), 8.0f + 6.0f * f + 0.15f, 0}, {0.12f, 0.12f, 60}, steel, kSteel);
        }
    }
    add(kBox, {0, 14.15f, 0}, {0.3f, 0.3f, 60}, steel, kSteel);
    for (float sz : {-1.0f, 1.0f})
        for (int i = 0; i <= 16; ++i) add(kCylinder, {-20.0f + float(i) * 2.5f, 0, sz * 30.0f}, {0.07f, 8.0f, 0.07f}, steel, kSteel);
    // Pond with a chrome sculpture.
    add(kCylinder, {0, 0, -6}, {6.2f, 0.5f, 6.2f}, stone, kStone);
    add(kCylinder, {0, 0.05f, -6}, {5.8f, 0.4f, 5.8f}, {0.05f, 0.1f, 0.12f}, kWater);
    add(kSphere, {0, 2.4f, -6}, {1.3f, 1.3f, 1.3f}, {0.95f, 0.93f, 0.90f}, kChrome);
    add(kTorus, {0, 2.4f, -6}, {2.3f, 2.3f, 2.3f}, {0.95f, 0.80f, 0.55f}, kChrome, 0.6f, 0.55f);
    add(kCylinder, {0, 0.4f, -6}, {0.25f, 1.0f, 0.25f}, steel, kSteel);
    // Trees in planters along both sides; every tree is the same mesh, turned and scaled.
    Random rng(0x61A55u);
    for (float sx : {-1.0f, 1.0f})
        for (float z : kTreeZ) {
            const Vec3 p{sx * 13.0f, 0, z};
            add(kBox, {p.x, 0.45f, p.z}, {3.2f, 0.9f, 3.2f}, stone, kStone);
            add(kBox, {p.x, 0.9f, p.z}, {2.8f, 0.1f, 2.8f}, {0.30f, 0.20f, 0.12f}, kSoil);
            const float s = rng.uniform(0.9f, 1.15f), yaw = rng.uniform(0.0f, 2.0f * kPi);
            add(kBark, {p.x, 0.9f, p.z}, {s, s, s}, {0.35f, 0.24f, 0.15f}, kBarkMaterial, yaw);
            const Vec3 leafColor{rng.uniform(0.16f, 0.24f), rng.uniform(0.34f, 0.44f), rng.uniform(0.10f, 0.16f)};
            add(kTreeLeaves, {p.x, 0.9f, p.z}, {s, s, s}, leafColor, kLeaves, yaw);
        }
    // Grass beds (soil under the blades) with shrubs along their inner edge.
    for (const Bed& b : beds) {
        add(kBox, {b.centre.x, b.centre.y / 2, b.centre.z}, {b.half * 2 + 0.6f, b.centre.y, b.half * 2 + 0.6f}, stone, kStone);
        add(kBox, {b.centre.x, b.centre.y + 0.01f, b.centre.z}, {b.half * 2, 0.02f, b.half * 2}, {0.28f, 0.19f, 0.11f}, kSoil);
        const float inner = b.centre.x > 0 ? b.centre.x - b.half - 0.6f : b.centre.x + b.half + 0.6f;
        for (float dz : {-b.half + 0.8f, 0.0f, b.half - 0.8f}) {
            const float s = rng.uniform(0.8f, 1.2f);
            const Vec3 leafColor{rng.uniform(0.12f, 0.20f), rng.uniform(0.28f, 0.38f), rng.uniform(0.08f, 0.13f)};
            const float yaw = rng.uniform(0.0f, 2.0f * kPi);
            add(kShrubLeaves, {inner, 0, b.centre.z + dz}, {s, s, s}, leafColor, kLeaves, yaw);
        }
    }
    return v;
}

class Glasshouse final : public Test {
public:
    Glasshouse() {
        m_info.id = "scene.glasshouse";
        m_info.name = "Glasshouse";
        m_info.description = "Hybrid rendering of a steel-and-glass conservatory with leafy trees: rasterised "
                             "G-buffer with mesh-shader grass and variable-rate shading; ray-traced soft shadows, "
                             "ambient occlusion, diffuse GI and reflections through alpha-tested foliage and glass.";
        m_info.category = Category::Scene;
        m_info.group = "Ultimate";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12};
        m_info.reference = 130.0; // provisional, see docs/SCORING.md
        m_info.tags = {"ultimate", "rt", "cpu-light"};
        m_info.suites = {"standard", "extreme", "raytracing"};
        m_info.order = 50;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().rayQuery ? SupportResult::yes() : SupportResult::no("no ray queries");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const auto& f = dev.features();
        // Software renderers get less content (--param scene.full_detail=1 keeps all of it).
        const bool software = dev.adapter().isSoftware() && s.param("scene.full_detail") != "1";
        const Detail detail = detailFor(s.quality, software);
        using U = rhi::BufferUsage;
        using TU = rhi::TextureUsage;
        using BT = rhi::BindingType;

        // --- content -----------------------------------------------------------------------------
        MeshPool pool;
        auto [bark, treeLeaves] = makeTree(detail.treeLeaves, software ? 6 : 12);
        const MeshData shrubLeaves = makeShrub(detail.shrubLeaves);
        const std::vector<std::uint32_t> meshIds = {pool.add(box(1, 1, 1)), pool.add(cylinder(software ? 10 : 20, true)),
                                                    pool.add(sphere(software ? 12 : 28, software ? 8 : 16)),
                                                    pool.add(torus(software ? 20 : 48, software ? 8 : 16, 0.12f)),
                                                    pool.add(bark), pool.add(treeLeaves), pool.add(shrubLeaves)};
        pool.upload(dev, U::AccelInput);
        const std::vector<Bed> beds = {{{-7.0f, 0.35f, -25.0f}, 4.0f}, {{7.0f, 0.35f, -25.0f}, 4.0f},
                                       {{-7.0f, 0.35f, -16.0f}, 4.0f}, {{7.0f, 0.35f, -16.0f}, 4.0f},
                                       {{-7.0f, 0.35f, 8.0f}, 4.0f},   {{7.0f, 0.35f, 8.0f}, 4.0f},
                                       {{-7.0f, 0.35f, 20.0f}, 4.0f},  {{7.0f, 0.35f, 20.0f}, 4.0f}};
        std::vector<RtInstance> instances = buildGlasshouse(beds);
        std::vector<bool> alphaTested(kMeshKinds, false);
        alphaTested[kTreeLeaves] = alphaTested[kShrubLeaves] = true;
        const RtScene rs = buildRtScene(ctx, pool, meshIds, instances, alphaTested);

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
        auto albedo = tex(rhi::Format::RGBA8_UNORM, TU::RenderTarget | TU::Sampled, "glasshouse-albedo");
        auto normal = tex(rhi::Format::RGBA16_FLOAT, TU::RenderTarget | TU::Sampled, "glasshouse-normal");
        auto depth = tex(rhi::Format::D32_FLOAT, TU::DepthStencil | TU::Sampled, "glasshouse-depth");
        auto vis = tex(rhi::Format::RGBA8_UNORM, TU::Storage | TU::Sampled, "glasshouse-visibility");
        auto giRaw = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "glasshouse-gi");
        auto giA = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "glasshouse-gi-a");
        auto giB = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "glasshouse-gi-b");
        auto hdr = tex(rhi::Format::RGBA16_FLOAT, TU::Storage | TU::Sampled, "glasshouse-hdr");
        auto output = tex(rhi::Format::RGBA8_UNORM, TU::RenderTarget | TU::Sampled | TU::CopySrc, "glasshouse-output");

        // --- pipelines ---------------------------------------------------------------------------
        // --param scene.glasshouse.vertex_grass=1 forces the vertex-shader grass path (coverage).
        const bool mesh = f.meshShader && s.param("scene.glasshouse.vertex_grass") != "1";
        const bool vrs = f.variableRateShading;
        rhi::GraphicsPipelineDesc gd;
        gd.name = "glasshouse-geometry";
        gd.vertex = {"scenes/glasshouse", "vsGeometry"};
        gd.fragment = {"scenes/glasshouse", "psGeometry"};
        vertexLayout(gd);
        gd.vertexBindings.push_back({1, sizeof(RtInstance), true});
        for (std::uint32_t k = 0; k < 4; ++k) gd.attributes.push_back({3 + k, rhi::Format::RGBA32_FLOAT, 16 * k, 1});
        gd.colorFormats = {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA16_FLOAT};
        gd.depthFormat = rhi::Format::D32_FLOAT;
        gd.raster.cull = rhi::CullMode::None;
        gd.layout.bindings = {{BT::UniformBuffer, 0}};
        auto geometryPipe = dev.createGraphicsPipeline(gd);
        rhi::GraphicsPipelineDesc lfd = gd;
        lfd.name = "glasshouse-leaves";
        lfd.fragment = {"scenes/glasshouse", "psLeaves"};
        auto leavesPipe = dev.createGraphicsPipeline(lfd);
        rhi::GraphicsPipelineDesc grd;
        grd.name = mesh ? "glasshouse-grass-mesh" : "glasshouse-grass";
        if (mesh) grd.mesh = {"scenes/glasshouse", "msGrass"};
        else grd.vertex = {"scenes/glasshouse", "vsGrass"};
        grd.fragment = {"scenes/glasshouse", "psGrass"};
        grd.colorFormats = gd.colorFormats;
        grd.depthFormat = rhi::Format::D32_FLOAT;
        grd.raster.cull = rhi::CullMode::None;
        grd.variableRate = vrs;
        grd.layout.bindings = {{BT::UniformBuffer, 0}};
        auto grassPipe = dev.createGraphicsPipeline(grd);
        rhi::ComputePipelineDesc td;
        td.name = "glasshouse-trace";
        td.compute = {"scenes/glasshouse", "csTrace"};
        td.layout.bindings = {{BT::UniformBuffer, 0},   {BT::AccelerationStructure, 0}, {BT::StorageBufferRO, 1},
                              {BT::StorageBufferRO, 2}, {BT::StorageBufferRO, 3},       {BT::StorageBufferRO, 4},
                              {BT::SampledTexture, 6},  {BT::SampledTexture, 7},        {BT::StorageTexture, 1},
                              {BT::StorageTexture, 2}};
        auto tracePipe = dev.createComputePipeline(td);
        rhi::ComputePipelineDesc fd;
        fd.name = "glasshouse-filter-gi";
        fd.compute = {"scenes/glasshouse", "csFilterGi"};
        fd.layout.bindings = {{BT::UniformBuffer, 0}, {BT::SampledTexture, 6}, {BT::SampledTexture, 7},
                              {BT::SampledTexture, 10}, {BT::StorageTexture, 3}};
        fd.layout.pushConstantBytes = sizeof(FilterParams);
        auto filterPipe = dev.createComputePipeline(fd);
        rhi::ComputePipelineDesc ld;
        ld.name = "glasshouse-lighting";
        ld.compute = {"scenes/glasshouse", "csLighting"};
        ld.layout.bindings = {{BT::UniformBuffer, 0},   {BT::AccelerationStructure, 0}, {BT::StorageBufferRO, 1},
                              {BT::StorageBufferRO, 2}, {BT::StorageBufferRO, 3},       {BT::StorageBufferRO, 4},
                              {BT::SampledTexture, 5},  {BT::SampledTexture, 6},        {BT::SampledTexture, 7},
                              {BT::SampledTexture, 9},  {BT::SampledTexture, 11},       {BT::StorageTexture, 0}};
        auto lightingPipe = dev.createComputePipeline(ld);
        rhi::GraphicsPipelineDesc cd;
        cd.name = "glasshouse-composite";
        cd.vertex = {"scenes/glasshouse", "vsFullscreen"};
        cd.fragment = {"scenes/glasshouse", "psComposite"};
        cd.colorFormats = {rhi::Format::RGBA8_UNORM};
        cd.depth.test = false;
        cd.depth.write = false;
        cd.raster.cull = rhi::CullMode::None;
        cd.layout.bindings = {{BT::SampledTexture, 8}};
        auto compositePipe = dev.createGraphicsPipeline(cd);

        const std::uint32_t patchesPerRow = 8; // 1 m patches over 8 m beds
        const std::uint32_t patches = static_cast<std::uint32_t>(beds.size()) * patchesPerRow * patchesPerRow;
        const std::uint32_t bladesPerPatch = detail.bladesPerRow * detail.bladesPerRow;
        const std::uint32_t groupsPerPatch = (bladesPerPatch + 31) / 32;
        const bool gi = detail.giRays > 0;
        std::uint64_t bvhTriangles = 0, leafCards = 0;
        for (const RtInstance& in : instances) {
            const auto m = static_cast<std::size_t>(in.info[0]);
            bvhTriangles += pool[meshIds[m]].indexCount / 3;
            if (alphaTested[m]) leafCards += pool[meshIds[m]].indexCount / 6;
        }
        auto& d = ctx.details();
        d["instances"] = instances.size();
        d["bvh_triangles"] = bvhTriangles;
        d["leaf_cards"] = leafCards;
        d["grass_blades"] = patches * bladesPerPatch;
        d["grass_path"] = mesh ? "mesh shaders" : "vertex shader";
        d["variable_rate_shading"] = vrs;
        d["shadow_rays_per_pixel"] = detail.shadowRays;
        d["ao_rays_per_pixel"] = detail.aoRays;
        d["gi_rays_per_pixel"] = detail.giRays;
        d["gi_filter_passes"] = gi ? detail.giFilterPasses : 0;
        if (software) d["note"] = "software renderer: fewer leaves and grass blades, one ray of each kind";

        // --- frame -------------------------------------------------------------------------------
        const CameraPath path({{{0, 2.0f, 28}, {0, 3.0f, -6}},
                               {{-8, 1.8f, 6}, {0, 2.2f, -6}},
                               {{-6, 3.0f, -14}, {3, 2.0f, -4}},
                               {{8, 1.6f, -20}, {-4, 2.5f, -6}},
                               {{12, 9.0f, 8}, {-6, 1.0f, -10}},
                               {{30, 6.0f, 36}, {0, 5.0f, 0}}});
        const Vec3 sun = normalize({0.38f, 0.78f, -0.5f});
        const float aspect = float(w) / float(h);
        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(720);
        fl.simDuration = 30.0;
        fl.realTime = true;
        fl.output = output.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        fl.record = [&](FrameContext& fc) {
            const CameraKey cam = path.at(fc.simTime / fl.simDuration);
            FrameData fd{};
            fd.viewProj = perspective(62.0f * kPi / 180.0f, aspect, 0.1f, 300.0f) * lookAt(cam.eye, cam.target, {0, 1, 0});
            fd.invViewProj = inverse(fd.viewProj);
            fd.cameraPos[0] = cam.eye.x, fd.cameraPos[1] = cam.eye.y, fd.cameraPos[2] = cam.eye.z;
            fd.cameraPos[3] = static_cast<float>(fc.simTime);
            fd.sunDir[0] = sun.x, fd.sunDir[1] = sun.y, fd.sunDir[2] = sun.z, fd.sunDir[3] = 3.2f;
            fd.sunColor[0] = 1.0f, fd.sunColor[1] = 0.95f, fd.sunColor[2] = 0.86f, fd.sunColor[3] = 0.01f;
            fd.screen[0] = float(w), fd.screen[1] = float(h), fd.screen[2] = 1.0f / float(w), fd.screen[3] = 1.0f / float(h);
            // The noise pattern depends on the frame index; golden frames repeat it.
            fd.params[0] = fc.warmup ? 0u : fc.frame;
            fd.params[2] = detail.bladesPerRow;
            fd.params[3] = patchesPerRow;
            fd.rays[0] = detail.shadowRays;
            fd.rays[1] = detail.aoRays;
            fd.rays[2] = detail.giRays;
            fd.rays[3] = detail.reflectionShadows ? 1u : 0u;
            fd.grass[0] = 1.0f, fd.grass[1] = 0.55f, fd.grass[2] = 0.08f, fd.grass[3] = float(beds.size());
            for (std::size_t b = 0; b < beds.size(); ++b) {
                fd.beds[b][0] = beds[b].centre.x, fd.beds[b][1] = beds[b].centre.y + 0.02f, fd.beds[b][2] = beds[b].centre.z;
                fd.beds[b][3] = beds[b].half;
            }

            rhi::CommandList& cmd = fc.cmd;
            rhi::RenderPassDesc gp;
            gp.colors.push_back({albedo.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 0}});
            gp.colors.push_back({normal.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 1, 0, 0}});
            gp.depth = {depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(gp);
            cmd.setUniformData(0, &fd, sizeof(fd));
            cmd.setVertexBuffer(0, pool.vertexBuffer());
            cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
            auto drawKind = [&](int m) {
                const SubMesh& sm = pool[meshIds[std::size_t(m)]];
                if (rs.groupCount[std::size_t(m)] == 0) return;
                cmd.setVertexBuffer(1, rs.instances.get(), std::uint64_t(rs.groupFirst[std::size_t(m)]) * sizeof(RtInstance));
                cmd.drawIndexed(sm.indexCount, rs.groupCount[std::size_t(m)], sm.firstIndex, sm.baseVertex);
            };
            cmd.setPipeline(geometryPipe.get());
            for (int m = 0; m < kMeshKinds; ++m)
                if (!alphaTested[std::size_t(m)]) drawKind(m);
            cmd.setPipeline(leavesPipe.get());
            for (int m = 0; m < kMeshKinds; ++m)
                if (alphaTested[std::size_t(m)]) drawKind(m);
            cmd.setPipeline(grassPipe.get());
            if (vrs) cmd.setShadingRate(2, 2);
            if (mesh) cmd.drawMeshTasks(patches * groupsPerPatch);
            else cmd.draw(9, patches * bladesPerPatch);
            cmd.endRenderPass();

            auto bindScene = [&] {
                cmd.setAccelerationStructure(0, rs.tlas.get());
                cmd.setReadOnlyBuffer(1, rs.instances.get());
                cmd.setReadOnlyBuffer(2, rs.meshes.get());
                cmd.setReadOnlyBuffer(3, pool.vertexBuffer());
                cmd.setReadOnlyBuffer(4, pool.indexBuffer());
            };
            for (rhi::Texture* t : {albedo.get(), normal.get(), depth.get()}) cmd.transition(t, rhi::ResourceState::ShaderRead);
            for (rhi::Texture* t : {vis.get(), giRaw.get()})
                cmd.transition(t, rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(tracePipe.get());
            bindScene();
            cmd.setTexture(6, normal.get());
            cmd.setTexture(7, depth.get());
            cmd.setStorageTexture(1, vis.get());
            cmd.setStorageTexture(2, giRaw.get());
            cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            for (rhi::Texture* t : {vis.get(), giRaw.get()}) cmd.transition(t, rhi::ResourceState::ShaderRead);

            // GI wavelet filter: raw -> A -> B -> A ...
            rhi::Texture* giResult = giRaw.get();
            if (gi) {
                rhi::Texture* dst = giA.get();
                cmd.setPipeline(filterPipe.get());
                for (int i = 0; i < detail.giFilterPasses; ++i) {
                    const FilterParams fp{1 << i, 32.0f, 0.01f, 0.0f};
                    cmd.transition(dst, rhi::ResourceState::UnorderedAccess);
                    cmd.setUniformData(0, &fd, sizeof(fd));
                    cmd.pushConstants(&fp, sizeof(fp));
                    cmd.setTexture(6, normal.get());
                    cmd.setTexture(7, depth.get());
                    cmd.setTexture(10, giResult);
                    cmd.setStorageTexture(3, dst);
                    cmd.dispatch((w + 7) / 8, (h + 7) / 8);
                    cmd.transition(dst, rhi::ResourceState::ShaderRead);
                    giResult = dst;
                    dst = dst == giA.get() ? giB.get() : giA.get();
                }
            }

            cmd.transition(hdr.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(lightingPipe.get());
            cmd.setUniformData(0, &fd, sizeof(fd));
            bindScene();
            cmd.setTexture(5, albedo.get());
            cmd.setTexture(6, normal.get());
            cmd.setTexture(7, depth.get());
            cmd.setTexture(9, vis.get());
            cmd.setTexture(11, giResult);
            cmd.setStorageTexture(0, hdr.get());
            cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            cmd.transition(hdr.get(), rhi::ResourceState::ShaderRead);

            rhi::RenderPassDesc cp;
            cp.colors.push_back({output.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            cmd.beginRenderPass(cp);
            cmd.setPipeline(compositePipe.get());
            cmd.setTexture(8, hdr.get());
            cmd.draw(3);
            cmd.endRenderPass();
        };
        const FrameLoopResult r = ctx.runFrames(fl);
        ctx.setFrameResult(r);
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::Glasshouse)
