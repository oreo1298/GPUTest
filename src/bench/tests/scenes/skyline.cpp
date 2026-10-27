// "Skyline" (modern tier): a procedural city at dusk (shaders/src/scenes/skyline*.slang).
// GPU-driven and CPU-light by design: per frame the CPU records a culling dispatch and a fixed
// set of indirect draws; a compute pass frustum-culls every instance for the camera and four
// shadow cascades. PBR forward shading with cascaded shadows writes HDR colour, normals and
// depth; ambient occlusion and a bloom chain run on the async compute queue, overlapping the
// next frame's graphics work; a composite pass tone-maps the result.

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cmath>

namespace gt::bench {
namespace {

using namespace scene;

constexpr std::uint32_t kMeshes = 4, kViews = 5, kCascades = 4, kBloomLevels = 5;
enum MeshKind { kBox, kCylinder, kSphere, kCone };
enum Material { kConcrete, kGlassTower, kBrick, kRoad, kFoliage, kCarPaint, kMetal, kLight };

struct Instance {
    float posRot[4];
    float scaleMesh[4];
    float color[4];
};
struct MeshInfo {
    std::uint32_t indexCount, firstIndex;
    std::int32_t baseVertex;
    std::uint32_t instanceBase;
    float sphere[4];
};
struct FrameData {
    Mat4 viewProj;
    Mat4 invViewProj;
    Mat4 cascadeViewProj[4];
    float cascadeSplits[4];
    float cameraPos[4];
    float cameraFwd[4];
    float sunDir[4];
    float sunColor[4];
    float screen[4];
    float views[5][6][4];
    std::uint32_t counts[4];
};
static_assert(sizeof(FrameData) == 6 * 64 + 6 * 16 + 5 * 96 + 16);
struct DrawConstants {
    std::uint32_t visibleBase, view, pad0, pad1;
};
struct PostParams {
    std::uint32_t size[2];
    float invSrc[2];
    float threshold, strength;
    std::uint32_t frameIndex, aoSamples;
};

/// What there is to draw at each quality.
struct Detail {
    int side;                 ///< city blocks per side
    int ledgeStep;            ///< a ledge around the buildings every n floors (0: none)
    std::uint32_t round;      ///< segments of cylinders, spheres, cones
    std::uint32_t tile;       ///< shadow cascade tile size (the atlas holds 2 x 2)
    std::uint32_t aoSamples;
};

Detail detailFor(Quality q, bool software) {
    if (software) return {6, 0, 8, 512, 8};
    switch (q) {
    case Quality::Low: return {16, 0, 12, 1024, 4};
    case Quality::Medium: return {24, 3, 16, 1536, 8};
    case Quality::High: return {40, 2, 24, 2048, 16};
    case Quality::Ultra: return {60, 1, 32, 4096, 32};
    }
    return {40, 2, 24, 2048, 16};
}

/// The city: blocks of buildings with rooftop details, streets with cars and lamps, parks.
std::vector<Instance> buildCity(int side, int ledgeStep, std::uint64_t seed) {
    std::vector<Instance> out;
    Random rng(seed ^ 0x51C7u);
    auto uni = [](Random& r) { return r.uniform(); };
    auto add = [&](MeshKind mesh, Vec3 p, float rot, Vec3 s, Vec3 c, Material m) {
        out.push_back({{p.x, p.y, p.z, rot}, {s.x, s.y, s.z, float(mesh)}, {c.x, c.y, c.z, float(m)}});
    };
    const float half = float(side) * 30.0f;
    add(kBox, {0, -0.5f, 0}, 0, {half * 2.0f + 400.0f, 1.0f, half * 2.0f + 400.0f}, {0.3f, 0.3f, 0.3f}, kRoad);
    const Vec3 concrete[4] = {{0.62f, 0.60f, 0.56f}, {0.52f, 0.50f, 0.48f}, {0.70f, 0.66f, 0.58f}, {0.44f, 0.45f, 0.47f}};
    const Vec3 brick[3] = {{0.55f, 0.30f, 0.22f}, {0.48f, 0.34f, 0.28f}, {0.62f, 0.42f, 0.30f}};
    const Vec3 paint[6] = {{0.7f, 0.08f, 0.06f}, {0.08f, 0.1f, 0.3f}, {0.85f, 0.85f, 0.82f}, {0.05f, 0.05f, 0.05f},
                           {0.6f, 0.6f, 0.62f}, {0.2f, 0.35f, 0.2f}};
    for (int bz = 0; bz < side; ++bz)
        for (int bx = 0; bx < side; ++bx) {
            const float cx = -half + float(bx) * 60.0f + 30.0f, cz = -half + float(bz) * 60.0f + 30.0f;
            const float centre = 1.0f - std::min(1.0f, std::sqrt(cx * cx + cz * cz) / (half * 1.1f));
            if (uni(rng) < 0.08f) { // park
                add(kBox, {cx, 0.05f, cz}, 0, {44, 0.1f, 44}, {0.18f, 0.30f, 0.12f}, kFoliage);
                for (int t = 0; t < 14; ++t) {
                    const float tx = cx + (uni(rng) - 0.5f) * 38.0f, tz = cz + (uni(rng) - 0.5f) * 38.0f, h = 4 + uni(rng) * 4;
                    add(kCylinder, {tx, 0, tz}, 0, {0.25f, h, 0.25f}, {0.3f, 0.2f, 0.12f}, kConcrete);
                    add(kSphere, {tx, h + 1.0f, tz}, 0, {2.2f, 2.6f, 2.2f}, {0.16f, 0.30f, 0.12f}, kFoliage);
                }
                continue;
            }
            const int lots = 1 + int(uni(rng) * 4.0f);
            for (int l = 0; l < lots; ++l) {
                const float w = lots == 1 ? 34.0f + uni(rng) * 8.0f : 16.0f + uni(rng) * 4.0f;
                const float dpt = lots == 1 ? 34.0f + uni(rng) * 8.0f : 16.0f + uni(rng) * 4.0f;
                const float ox = lots == 1 ? 0.0f : ((l & 1) ? 10.5f : -10.5f), oz = lots <= 2 ? 0.0f : ((l & 2) ? 10.5f : -10.5f);
                const float tall = std::pow(uni(rng), 2.2f) * (40.0f + 200.0f * centre * centre) + 12.0f + 30.0f * centre;
                const float r = uni(rng);
                const Material m = tall > 90.0f && r < 0.5f ? kGlassTower : r < 0.35f ? kBrick : kConcrete;
                const Vec3 c = m == kBrick ? brick[int(uni(rng) * 2.99f)] : m == kGlassTower ? Vec3{0.20f, 0.28f, 0.34f}
                                                                                              : concrete[int(uni(rng) * 3.99f)];
                const Vec3 p{cx + ox, 0, cz + oz};
                add(kBox, {p.x, tall / 2, p.z}, 0, {w, tall, dpt}, c, m);
                // Ledges at the floor lines (3.4 m, as the shader's window grid).
                if (ledgeStep > 0 && m != kGlassTower)
                    for (float y = 3.4f * float(ledgeStep); y < tall - 1.0f; y += 3.4f * float(ledgeStep))
                        add(kBox, {p.x, y, p.z}, 0, {w + 0.5f, 0.22f, dpt + 0.5f}, c * 0.85f, m);
                float top = tall;
                if (tall > 60.0f && uni(rng) < 0.6f) { // setback
                    const float t2 = tall * (0.15f + uni(rng) * 0.3f);
                    add(kBox, {p.x, tall + t2 / 2, p.z}, 0, {w * 0.65f, t2, dpt * 0.65f}, c, m);
                    top += t2;
                }
                for (int u = 0; u < 3; ++u) { // rooftop units (separate statements fix the draw order)
                    const Vec3 at{p.x + (uni(rng) - 0.5f) * w * 0.4f, top + 1.2f, p.z + (uni(rng) - 0.5f) * dpt * 0.4f};
                    const Vec3 size{2.5f + uni(rng) * 2, 2.4f, 2 + uni(rng) * 2};
                    add(kBox, at, 0, size, {0.5f, 0.5f, 0.52f}, kMetal);
                }
                if (m != kGlassTower && uni(rng) < 0.4f) {
                    const Vec3 t{p.x + w * 0.25f, top, p.z - dpt * 0.25f};
                    add(kCylinder, t, 0, {1.8f, 3.5f, 1.8f}, {0.42f, 0.30f, 0.22f}, kConcrete);
                    add(kCone, {t.x, top + 3.5f, t.z}, 0, {2.0f, 1.4f, 2.0f}, {0.3f, 0.25f, 0.2f}, kConcrete);
                }
                if (tall > 150.0f) add(kCylinder, {p.x, top, p.z}, 0, {0.35f, 25.0f, 0.35f}, {0.7f, 0.7f, 0.7f}, kMetal);
            }
            // Street furniture on the block's west and south streets: cars and lamps.
            for (int k = 0; k < 4; ++k) {
                const float along = (uni(rng) - 0.5f) * 44.0f;
                const bool ns = k < 2;
                const float lane = (k & 1) ? 3.5f : -3.5f;
                const Vec3 cp = ns ? Vec3{cx - 30.0f + lane, 0.0f, cz + along} : Vec3{cx + along, 0.0f, cz - 30.0f + lane};
                const float rot = ns ? 0.0f : 1.5707963f;
                const Vec3 col = paint[int(uni(rng) * 5.99f)];
                add(kBox, {cp.x, 0.75f, cp.z}, rot, {1.9f, 0.9f, 4.4f}, col, kCarPaint);
                add(kBox, {cp.x, 1.5f, cp.z - (ns ? 0.3f : 0.0f) }, rot, {1.7f, 0.65f, 2.3f}, {0.06f, 0.07f, 0.08f}, kMetal);
            }
            for (int k = 0; k < 2; ++k) {
                const Vec3 lp{cx - 23.5f + float(k) * 47.0f, 0, cz - 23.5f};
                add(kCylinder, lp, 0, {0.12f, 7.0f, 0.12f}, {0.2f, 0.2f, 0.22f}, kMetal);
                add(kSphere, {lp.x, 7.1f, lp.z}, 0, {0.35f, 0.25f, 0.35f}, {1.0f, 0.72f, 0.4f}, kLight);
            }
        }
    return out;
}

class Skyline final : public Test {
public:
    Skyline() {
        m_info.id = "scene.skyline";
        m_info.name = "Skyline";
        m_info.description = "A city at dusk with tens of thousands of objects, culled on the GPU and drawn indirectly: "
                             "PBR shading, cascaded shadows, ambient occlusion and bloom on the async compute queue.";
        m_info.category = Category::Scene;
        m_info.group = "Modern";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12};
        m_info.reference = 120.0; // provisional, see docs/SCORING.md
        m_info.tags = {"modern", "cpu-light", "async-compute"};
        m_info.suites = {"quick", "standard", "extreme"};
        m_info.order = 40;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().multiDrawIndirect ? SupportResult::yes()
                                                         : SupportResult::no("no multi-draw indirect");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        // Software renderers get less content (--param scene.full_detail=1 keeps all of it).
        const bool software = dev.adapter().isSoftware() && s.param("scene.full_detail") != "1";
        const Detail detail = detailFor(s.quality, software);
        using U = rhi::BufferUsage;
        using TU = rhi::TextureUsage;
        using BT = rhi::BindingType;

        // --- content -----------------------------------------------------------------------------
        MeshPool pool;
        const std::uint32_t round = detail.round;
        const std::uint32_t meshIds[kMeshes] = {pool.add(box(1, 1, 1)), pool.add(cylinder(round, true)),
                                                pool.add(sphere(round, round * 5 / 8)), pool.add(cone(round))};
        pool.upload(dev);
        const int side = detail.side;
        std::vector<Instance> city = buildCity(side, detail.ledgeStep, s.seed);
        const auto count = static_cast<std::uint32_t>(city.size());
        std::uint32_t perMesh[kMeshes] = {};
        for (const Instance& i : city) ++perMesh[static_cast<int>(i.scaleMesh[3])];
        MeshInfo meshes[kMeshes];
        std::uint32_t base = 0;
        for (std::uint32_t m = 0; m < kMeshes; ++m) {
            const SubMesh& sm = pool[meshIds[m]];
            meshes[m] = {sm.indexCount, sm.firstIndex, sm.baseVertex, base, {sm.center.x, sm.center.y, sm.center.z, sm.radius}};
            base += perMesh[m];
        }
        auto instances = dev.createBuffer({city.size() * sizeof(Instance), U::Storage | U::CopyDst, rhi::MemoryType::Device,
                                           sizeof(Instance), "skyline-instances"},
                                          city.data());
        auto meshBuf = dev.createBuffer({sizeof(meshes), U::Storage | U::CopyDst, rhi::MemoryType::Device, sizeof(MeshInfo), "skyline-meshes"},
                                        meshes);
        auto visible = dev.createBuffer({std::uint64_t(kViews) * count * 4, U::Storage, rhi::MemoryType::Device, 4, "skyline-visible"});
        auto args = dev.createBuffer({kViews * kMeshes * 20, U::Storage | U::Indirect, rhi::MemoryType::Device, 4, "skyline-args"});

        // --- targets (colour, normals, depth, AO and bloom per frame in flight) -------------------
        auto tex = [&](rhi::Format f, std::uint32_t w, std::uint32_t h, TU usage, const char* name, std::uint32_t mips = 1) {
            rhi::TextureDesc td;
            td.format = f;
            td.width = w;
            td.height = h;
            td.mipLevels = mips;
            td.usage = usage;
            td.concurrentQueues = true; // written on one queue, read on the other
            td.name = name;
            return dev.createTexture(td);
        };
        struct Slot {
            std::unique_ptr<rhi::Texture> hdr, normal, depth, ao, bloom;
            FrameData frame{};
        };
        Slot slots[kFramesInFlight];
        const std::uint32_t w = s.width, h = s.height;
        for (Slot& sl : slots) {
            sl.hdr = tex(rhi::Format::RGBA16_FLOAT, w, h, TU::RenderTarget | TU::Sampled, "skyline-hdr");
            sl.normal = tex(rhi::Format::RGBA8_UNORM, w, h, TU::RenderTarget | TU::Sampled, "skyline-normal");
            sl.depth = tex(rhi::Format::D32_FLOAT, w, h, TU::DepthStencil | TU::Sampled, "skyline-depth");
            sl.ao = tex(rhi::Format::R32_FLOAT, w, h, TU::Storage | TU::Sampled, "skyline-ao");
            sl.bloom = tex(rhi::Format::RGBA16_FLOAT, std::max(1u, w / 2), std::max(1u, h / 2), TU::Storage | TU::Sampled,
                           "skyline-bloom", kBloomLevels);
        }
        rhi::TextureDesc od;
        od.format = rhi::Format::RGBA8_UNORM;
        od.width = w;
        od.height = h;
        od.usage = TU::RenderTarget | TU::Sampled | TU::CopySrc;
        od.name = "skyline-output";
        auto output = dev.createTexture(od);
        const std::uint32_t tile = detail.tile;
        rhi::TextureDesc sd;
        sd.format = rhi::Format::D32_FLOAT;
        sd.width = sd.height = tile * 2;
        sd.usage = TU::DepthStencil | TU::Sampled;
        sd.name = "skyline-shadow-atlas";
        auto atlas = dev.createTexture(sd);
        rhi::SamplerDesc cmp;
        cmp.addressU = cmp.addressV = cmp.addressW = rhi::AddressMode::Clamp;
        cmp.mipFilter = rhi::Filter::Nearest;
        cmp.compare = rhi::CompareOp::LessEqual;
        auto shadowSampler = dev.createSampler(cmp);
        rhi::SamplerDesc lin;
        lin.addressU = lin.addressV = lin.addressW = rhi::AddressMode::Clamp;
        lin.mipFilter = rhi::Filter::Nearest;
        auto linearClamp = dev.createSampler(lin);

        // --- pipelines ---------------------------------------------------------------------------
        auto compute = [&](const char* module, const char* entry, std::vector<rhi::Binding> b, std::uint32_t push) {
            rhi::ComputePipelineDesc pd;
            pd.name = entry;
            pd.compute = {module, entry};
            pd.layout.bindings = std::move(b);
            pd.layout.pushConstantBytes = push;
            return dev.createComputePipeline(pd);
        };
        const std::vector<rhi::Binding> cullBindings = {{BT::UniformBuffer, 0}, {BT::StorageBufferRO, 0}, {BT::StorageBufferRO, 1},
                                                        {BT::StorageBufferRW, 0}, {BT::StorageBufferRW, 1}};
        auto resetPipe = compute("scenes/skyline", "csResetArgs", {{BT::StorageBufferRO, 1}, {BT::StorageBufferRW, 1}}, 0);
        auto cullPipe = compute("scenes/skyline", "csCull", cullBindings, 0);
        auto aoPipe = compute("scenes/skyline_post", "csAmbientOcclusion",
                              {{BT::UniformBuffer, 0}, {BT::SampledTexture, 4}, {BT::SampledTexture, 5}, {BT::StorageTexture, 2}},
                              sizeof(PostParams));
        const std::vector<rhi::Binding> bloomBindings = {{BT::SampledTexture, 6}, {BT::Sampler, 1}, {BT::StorageTexture, 3}};
        auto downPipe = compute("scenes/skyline_post", "csBloomDown", bloomBindings, sizeof(PostParams));
        std::vector<rhi::Binding> upBindings = bloomBindings;
        upBindings.push_back({BT::SampledTexture, 7});
        auto upPipe = compute("scenes/skyline_post", "csBloomUp", upBindings, sizeof(PostParams));

        rhi::GraphicsPipelineDesc gd;
        gd.name = "skyline-scene";
        gd.vertex = {"scenes/skyline", "vsScene"};
        gd.fragment = {"scenes/skyline", "psScene"};
        vertexLayout(gd);
        gd.colorFormats = {rhi::Format::RGBA16_FLOAT, rhi::Format::RGBA8_UNORM};
        gd.depthFormat = rhi::Format::D32_FLOAT;
        gd.raster.cull = rhi::CullMode::Back;
        gd.layout.bindings = {{BT::UniformBuffer, 0}, {BT::StorageBufferRO, 0}, {BT::StorageBufferRO, 2}, {BT::SampledTexture, 3},
                              {BT::Sampler, 0}};
        gd.layout.pushConstantBytes = sizeof(DrawConstants);
        auto scenePipe = dev.createGraphicsPipeline(gd);
        rhi::GraphicsPipelineDesc shd = gd;
        shd.name = "skyline-shadow";
        shd.vertex = {"scenes/skyline", "vsShadow"};
        shd.fragment = {};
        shd.colorFormats = {};
        shd.raster.depthBias = 2.0f;
        shd.raster.depthBiasSlope = 2.5f;
        shd.raster.cull = rhi::CullMode::None;
        shd.layout.bindings = {{BT::UniformBuffer, 0}, {BT::StorageBufferRO, 0}, {BT::StorageBufferRO, 2}};
        auto shadowPipe = dev.createGraphicsPipeline(shd);
        rhi::GraphicsPipelineDesc skd;
        skd.name = "skyline-sky";
        skd.vertex = {"scenes/skyline", "vsFullscreen"};
        skd.fragment = {"scenes/skyline", "psSky"};
        skd.colorFormats = gd.colorFormats;
        skd.depthFormat = rhi::Format::D32_FLOAT;
        skd.depth.write = false;
        skd.depth.compare = rhi::CompareOp::LessEqual;
        skd.raster.cull = rhi::CullMode::None;
        skd.layout.bindings = {{BT::UniformBuffer, 0}};
        auto skyPipe = dev.createGraphicsPipeline(skd);
        rhi::GraphicsPipelineDesc cd;
        cd.name = "skyline-composite";
        cd.vertex = {"scenes/skyline", "vsFullscreen"};
        cd.fragment = {"scenes/skyline", "psComposite"};
        cd.colorFormats = {rhi::Format::RGBA8_UNORM};
        cd.depth.test = false;
        cd.depth.write = false;
        cd.raster.cull = rhi::CullMode::None;
        cd.layout.bindings = {{BT::UniformBuffer, 0}, {BT::SampledTexture, 8}, {BT::SampledTexture, 9}, {BT::SampledTexture, 10},
                              {BT::Sampler, 1}};
        auto compositePipe = dev.createGraphicsPipeline(cd);

        auto& d = ctx.details();
        d["instances"] = count;
        d["triangles_in_scene"] = [&] {
            std::uint64_t t = 0;
            for (std::uint32_t m = 0; m < kMeshes; ++m) t += std::uint64_t(perMesh[m]) * pool[meshIds[m]].indexCount / 3;
            return t;
        }();
        d["shadow_cascades"] = kCascades;
        d["shadow_tile"] = tile;
        d["ao_samples"] = detail.aoSamples;
        d["async_compute_queue"] = dev.features().asyncComputeQueue;
        if (software) d["note"] = "software renderer: smaller city";

        // --- camera, sun, cascades ------------------------------------------------------------------
        const float E = float(side) * 30.0f;
        const CameraPath path({{{-E * 0.95f, 190, -E * 0.95f}, {0, 40, 0}},
                               {{-E * 0.35f, 95, -E * 0.55f}, {E * 0.2f, 30, 0}},
                               {{0.5f, 6, -E * 0.45f}, {0.5f, 9, -E * 0.45f + 100}},
                               {{0.5f, 7, E * 0.05f}, {60, 22, E * 0.3f}},
                               {{E * 0.45f, 130, E * 0.55f}, {0, 0, 0}}});
        const Vec3 sun = normalize({-0.62f, 0.2f, 0.52f});
        const float aspect = float(w) / float(h);
        const float nearZ = 0.5f, farZ = std::min(1400.0f, E * 2.6f);
        float splits[kCascades];
        for (std::uint32_t c = 0; c < kCascades; ++c) {
            const float t = float(c + 1) / float(kCascades);
            const float logS = nearZ * std::pow(farZ * 0.6f / nearZ, t), linS = nearZ + (farZ * 0.6f - nearZ) * t;
            splits[c] = 0.8f * logS + 0.2f * linS;
        }
        const float fovY = 60.0f * kPi / 180.0f;

        auto makeFrame = [&](double simTime, double total) {
            const CameraKey cam = path.at(simTime / total);
            FrameData fd{};
            const Mat4 view = lookAt(cam.eye, cam.target, {0, 1, 0});
            fd.viewProj = perspective(fovY, aspect, nearZ, farZ) * view;
            fd.invViewProj = inverse(fd.viewProj);
            const Vec3 fwd = normalize(cam.target - cam.eye);
            const Vec3 right = normalize(cross(fwd, {0, 1, 0})), up = cross(right, fwd);
            float prev = nearZ;
            for (std::uint32_t c = 0; c < kCascades; ++c) {
                // Bounding sphere of the frustum slice, snapped to shadow texels for stable edges.
                const float th = std::tan(fovY / 2);
                Vec3 centre{};
                Vec3 corners[8];
                int k = 0;
                for (float z : {prev, splits[c]})
                    for (float sy : {-1.0f, 1.0f})
                        for (float sx : {-1.0f, 1.0f}) {
                            corners[k] = cam.eye + fwd * z + up * (sy * th * z) + right * (sx * th * aspect * z);
                            centre = centre + corners[k++];
                        }
                centre = centre * (1.0f / 8.0f);
                float radius = 0.0f;
                for (const Vec3& p : corners) radius = std::max(radius, std::sqrt(dot(p - centre, p - centre)));
                radius = std::ceil(radius);
                const float texel = 2.0f * radius / float(tile);
                const Vec3 sx = normalize(cross({0, 1, 0}, sun)), sy = cross(sun, sx);
                const float u = std::floor(dot(centre, sx) / texel) * texel, v = std::floor(dot(centre, sy) / texel) * texel;
                centre = sx * u + sy * v + sun * dot(centre, sun);
                const float back = 600.0f; // casters behind the slice (tall buildings, low sun)
                fd.cascadeViewProj[c] = orthographic(-radius, radius, -radius, radius, 0.0f, back + radius * 2.0f) *
                                        lookAt(centre + sun * (back + radius), centre, {0, 1, 0});
                fd.cascadeSplits[c] = splits[c];
                prev = splits[c];
            }
            fd.cameraPos[0] = cam.eye.x, fd.cameraPos[1] = cam.eye.y, fd.cameraPos[2] = cam.eye.z;
            fd.cameraPos[3] = static_cast<float>(simTime);
            fd.cameraFwd[0] = fwd.x, fd.cameraFwd[1] = fwd.y, fd.cameraFwd[2] = fwd.z;
            fd.sunDir[0] = sun.x, fd.sunDir[1] = sun.y, fd.sunDir[2] = sun.z, fd.sunDir[3] = 3.5f;
            fd.sunColor[0] = 1.0f, fd.sunColor[1] = 0.62f, fd.sunColor[2] = 0.38f, fd.sunColor[3] = 0.5f / float(tile);
            fd.screen[0] = float(w), fd.screen[1] = float(h), fd.screen[2] = 1.0f / float(w), fd.screen[3] = 1.0f / float(h);
            auto store = [&](int view, const Mat4& m) {
                const auto planes = frustumPlanes(m);
                for (int p = 0; p < 6; ++p)
                    for (int c = 0; c < 4; ++c) fd.views[view][p][c] = planes[static_cast<std::size_t>(p)][static_cast<std::size_t>(c)];
            };
            store(0, fd.viewProj);
            for (std::uint32_t c = 0; c < kCascades; ++c) store(int(c) + 1, fd.cascadeViewProj[c]);
            fd.counts[0] = count;
            fd.counts[1] = count;
            return fd;
        };

        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(900);
        fl.simDuration = 36.0;
        fl.realTime = true;
        fl.output = output.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        std::uint32_t frameCounter = 0;

        fl.record = [&](FrameContext& fc) {
            Slot& sl = slots[fc.slot];
            sl.frame = makeFrame(fc.simTime, fl.simDuration);
            rhi::CommandList& cmd = fc.cmd;
            // GPU culling for all five views.
            cmd.transition(args.get(), rhi::ResourceState::UnorderedAccess);
            cmd.transition(visible.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(resetPipe.get());
            cmd.setReadOnlyBuffer(1, meshBuf.get());
            cmd.setStorageBuffer(1, args.get());
            cmd.dispatch(1);
            cmd.uavBarrier();
            cmd.setPipeline(cullPipe.get());
            cmd.setUniformData(0, &sl.frame, sizeof(FrameData));
            cmd.setReadOnlyBuffer(0, instances.get());
            cmd.setStorageBuffer(0, visible.get());
            cmd.dispatch((count + 63) / 64);
            cmd.transition(args.get(), rhi::ResourceState::IndirectArgument);
            cmd.transition(visible.get(), rhi::ResourceState::ShaderRead);

            auto drawView = [&](std::uint32_t view) {
                cmd.setVertexBuffer(0, pool.vertexBuffer());
                cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
                cmd.setReadOnlyBuffer(0, instances.get());
                cmd.setReadOnlyBuffer(2, visible.get());
                for (std::uint32_t m = 0; m < kMeshes; ++m) {
                    const DrawConstants dc{view * count + meshes[m].instanceBase, view, 0, 0};
                    cmd.pushConstants(&dc, sizeof(dc));
                    cmd.drawIndexedIndirect(args.get(), std::uint64_t(view * kMeshes + m) * 20, 1, 20);
                }
            };
            // Shadow cascades into a 2 x 2 atlas.
            rhi::RenderPassDesc sp;
            sp.depth = {atlas.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(sp);
            cmd.setPipeline(shadowPipe.get());
            for (std::uint32_t c = 0; c < kCascades; ++c) {
                const float x = float((c & 1) * tile), y = float((c >> 1) * tile);
                cmd.setViewport(x, y, float(tile), float(tile));
                cmd.setScissor(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), tile, tile);
                drawView(c + 1);
            }
            cmd.endRenderPass();
            cmd.transition(atlas.get(), rhi::ResourceState::ShaderRead);
            // Main pass.
            rhi::RenderPassDesc mp;
            mp.colors.push_back({sl.hdr.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            mp.colors.push_back({sl.normal.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            mp.depth = {sl.depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(mp);
            cmd.setPipeline(scenePipe.get());
            cmd.setTexture(3, atlas.get());
            cmd.setSampler(0, shadowSampler.get());
            drawView(0);
            cmd.setPipeline(skyPipe.get());
            cmd.draw(3);
            cmd.endRenderPass();
            // Attachment layouts change here, on the graphics queue: a compute-only queue cannot wait on
            // attachment pipeline stages.
            for (rhi::Texture* t : {sl.hdr.get(), sl.normal.get(), sl.depth.get()}) cmd.transition(t, rhi::ResourceState::ShaderRead);
        };

        fl.recordAsync = [&](FrameContext& fc) {
            Slot& sl = slots[fc.slot];
            rhi::CommandList& cmd = fc.cmd;
            cmd.transition(sl.ao.get(), rhi::ResourceState::UnorderedAccess);
            cmd.transition(sl.bloom.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setUniformData(0, &sl.frame, sizeof(FrameData));
            // Ambient occlusion.
            PostParams pp{{w, h}, {1.0f / float(w), 1.0f / float(h)}, 0.0f, 0.0f, frameCounter++, detail.aoSamples};
            cmd.setPipeline(aoPipe.get());
            cmd.setTexture(4, sl.depth.get());
            cmd.setTexture(5, sl.normal.get());
            cmd.setStorageTexture(2, sl.ao.get());
            cmd.pushConstants(&pp, sizeof(pp));
            cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            // Bloom: downsample chain (bright-pass on the first level), then tent upsampling.
            cmd.setSampler(1, linearClamp.get());
            std::uint32_t lw[kBloomLevels], lh[kBloomLevels];
            for (std::uint32_t l = 0; l < kBloomLevels; ++l) {
                lw[l] = std::max(1u, (w / 2) >> l);
                lh[l] = std::max(1u, (h / 2) >> l);
            }
            cmd.setPipeline(downPipe.get());
            for (std::uint32_t l = 0; l < kBloomLevels; ++l) {
                const float sw = l == 0 ? float(w) : float(lw[l - 1]), sh = l == 0 ? float(h) : float(lh[l - 1]);
                PostParams bp{{lw[l], lh[l]}, {1.0f / sw, 1.0f / sh}, l == 0 ? 1.2f : 0.0f, 0.0f, 0, 0};
                if (l == 0) cmd.setTexture(6, sl.hdr.get());
                else cmd.setTexture(6, sl.bloom.get(), rhi::Subresource{l - 1, 1, 0, 0});
                cmd.setStorageTexture(3, sl.bloom.get(), l);
                cmd.pushConstants(&bp, sizeof(bp));
                cmd.dispatch((lw[l] + 7) / 8, (lh[l] + 7) / 8);
                cmd.uavBarrier();
            }
            cmd.setPipeline(upPipe.get());
            for (std::uint32_t l = kBloomLevels - 1; l-- > 0;) {
                PostParams bp{{lw[l], lh[l]}, {1.0f / float(lw[l + 1]), 1.0f / float(lh[l + 1])}, 0.0f, 0.8f, 0, 0};
                cmd.setTexture(6, sl.bloom.get(), rhi::Subresource{l, 1, 0, 0});
                cmd.setTexture(7, sl.bloom.get(), rhi::Subresource{l + 1, 1, 0, 0});
                cmd.setStorageTexture(3, sl.bloom.get(), l);
                cmd.pushConstants(&bp, sizeof(bp));
                cmd.dispatch((lw[l] + 7) / 8, (lh[l] + 7) / 8);
                cmd.uavBarrier();
            }
        };

        fl.recordPost = [&](FrameContext& fc) {
            Slot& sl = slots[fc.slot];
            rhi::CommandList& cmd = fc.cmd;
            cmd.transition(sl.ao.get(), rhi::ResourceState::ShaderRead);
            cmd.transition(sl.bloom.get(), rhi::ResourceState::ShaderRead);
            rhi::RenderPassDesc cp;
            cp.colors.push_back({output.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            cmd.beginRenderPass(cp);
            cmd.setPipeline(compositePipe.get());
            cmd.setUniformData(0, &sl.frame, sizeof(FrameData));
            cmd.setTexture(8, sl.hdr.get());
            cmd.setTexture(9, sl.ao.get());
            cmd.setTexture(10, sl.bloom.get(), rhi::Subresource{0, 1, 0, 0});
            cmd.setSampler(1, linearClamp.get());
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

GT_REGISTER_TEST(gt::bench::Skyline)
