// "Courtyard" (legacy tier): an arcaded stone plaza with a fountain on rolling, wooded terrain,
// lit by the sun with a shadow map, forward shaded (shaders/src/scenes/courtyard.slang).
// Two variants draw identical images:
//  - scene.courtyard: engine-like — one draw call with its own constants per object, twice
//    (shadow and main pass), the way a straightforward legacy renderer works;
//  - scene.courtyard.lite: CPU-light — one instanced draw per mesh type, so it measures the GPU.
// Tessellated terrain detail where the GPU supports it (not on software renderers).

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>

namespace gt::bench {
namespace {

using namespace scene;

struct FrameData {
    Mat4 viewProj;
    Mat4 sunViewProj;
    float cameraPos[4];
    float sunDir[4];
    float sunColor[4];
    float fog[4];
    float tess[4];
};
static_assert(sizeof(FrameData) == 208);

struct ObjectData {
    float posRot[4];
    float scaleMat[4];
    float color[4];
};
static_assert(sizeof(ObjectData) == 48);

enum Material { kStone, kPlaster, kBark, kLeaves, kWater, kRoof, kTerrain, kMetal, kLamp };
enum MeshKind { kBox, kCylinder, kSphere, kCone, kTorus, kMeshKinds };

struct Object {
    MeshKind mesh;
    ObjectData data;
};

float terrainHeight(float x, float z) {
    const float r = std::sqrt(x * x + z * z);
    const float hills = fbm2(x * 0.018f + 3.1f, z * 0.018f + 7.7f, 5) * 16.0f - 6.0f + r * 0.04f;
    const float t = std::clamp((r - 32.0f) / 16.0f, 0.0f, 1.0f);
    const float s = t * t * (3.0f - 2.0f * t);
    return -0.45f * (1.0f - s) + hills * s;
}

/// Builds the courtyard's objects (deterministic for a seed).
std::vector<Object> buildObjects(Quality q, std::uint64_t seed) {
    std::vector<Object> objs;
    auto add = [&](MeshKind mesh, Vec3 p, float rot, Vec3 scale, Vec3 color, Material m) {
        objs.push_back({mesh, {{p.x, p.y, p.z, rot}, {scale.x, scale.y, scale.z, float(m)}, {color.x, color.y, color.z, 1.0f}}});
    };
    const Vec3 stone{0.52f, 0.49f, 0.44f}, darkStone{0.40f, 0.38f, 0.35f};
    add(kBox, {0, -0.2f, 0}, 0, {56, 0.4f, 56}, stone, kStone);
    const Vec3 plaster[4] = {{0.86f, 0.74f, 0.58f}, {0.80f, 0.62f, 0.50f}, {0.88f, 0.82f, 0.70f}, {0.76f, 0.68f, 0.60f}};
    for (int side = 0; side < 4; ++side) {
        const float rot = float(side) * kPi / 2.0f;
        const float c = std::cos(rot), s = std::sin(rot);
        auto at = [&](float along, float out, float y) { return Vec3{c * along + s * out, y, -s * along + c * out}; };
        for (int i = 0; i < 12; ++i) {
            const float along = -22.0f + float(i) * 4.0f;
            add(kBox, at(along, 20.0f, 0.2f), rot, {1.2f, 0.4f, 1.2f}, darkStone, kStone);
            add(kCylinder, at(along, 20.0f, 0.4f), rot, {0.42f, 4.6f, 0.42f}, stone, kStone);
            add(kBox, at(along, 20.0f, 5.15f), rot, {1.1f, 0.35f, 1.1f}, darkStone, kStone);
        }
        add(kBox, at(0, 20.0f, 5.75f), rot, {48.0f, 0.9f, 1.2f}, stone, kStone);              // lintel
        add(kBox, at(0, 22.4f, 6.35f), rot, {50.0f, 0.3f, 5.6f}, {0.55f, 0.30f, 0.22f}, kRoof); // arcade roof
        add(kBox, at(0, 24.6f, 5.0f), rot, {50.0f, 10.0f, 0.8f}, plaster[side], kPlaster);      // back wall
        add(kBox, at(0, 24.6f, 10.2f), rot, {50.8f, 0.5f, 1.2f}, darkStone, kStone);           // cornice
        for (int i = 0; i < 12; ++i) {
            const float along = -22.0f + float(i) * 4.0f;
            add(kBox, at(along, 24.15f, 7.6f), rot, {1.2f, 1.9f, 0.1f}, {0.10f, 0.12f, 0.15f}, kMetal); // windows
            if (i % 3 == 1) add(kBox, at(along, 24.15f, 1.6f), rot, {1.8f, 3.2f, 0.1f}, {0.35f, 0.22f, 0.14f}, kBark);
        }
    }
    // Fountain.
    add(kCylinder, {0, 0, 0}, 0, {5.0f, 0.8f, 5.0f}, stone, kStone);
    add(kCylinder, {0, 0.05f, 0}, 0, {4.6f, 0.62f, 4.6f}, {0.18f, 0.32f, 0.38f}, kWater);
    add(kCylinder, {0, 0, 0}, 0, {0.6f, 3.2f, 0.6f}, stone, kStone);
    add(kTorus, {0, 2.4f, 0}, 0, {1.6f, 1.6f, 1.6f}, stone, kStone);
    add(kSphere, {0, 3.5f, 0}, 0, {0.5f, 0.5f, 0.5f}, darkStone, kStone);
    // Lamps and benches around the fountain.
    for (int i = 0; i < 16; ++i) {
        const float a = float(i) / 16.0f * 2.0f * kPi;
        add(kCylinder, {std::cos(a) * 12.0f, 0, std::sin(a) * 12.0f}, 0, {0.08f, 3.5f, 0.08f}, {0.12f, 0.12f, 0.13f}, kMetal);
        add(kSphere, {std::cos(a) * 12.0f, 3.65f, std::sin(a) * 12.0f}, 0, {0.25f, 0.25f, 0.25f}, {1.0f, 0.82f, 0.55f}, kLamp);
        if (i % 2 == 0)
            add(kBox, {std::cos(a + 0.2f) * 9.0f, 0.22f, std::sin(a + 0.2f) * 9.0f}, -a, {0.6f, 0.45f, 2.0f}, {0.42f, 0.28f, 0.16f},
                kBark);
    }
    // Trees and rocks on the terrain around the plaza.
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const int trees = q == Quality::Low ? 150 : q == Quality::Medium ? 300 : q == Quality::High ? 600 : 1000;
    for (int i = 0; i < trees; ++i) {
        const float a = uni(rng) * 2.0f * kPi, r = 38.0f + uni(rng) * 38.0f;
        const float x = std::cos(a) * r, z = std::sin(a) * r, y = terrainHeight(x, z) - 0.2f;
        const float h = 3.0f + uni(rng) * 3.0f;
        add(kCylinder, {x, y, z}, 0, {0.22f, h, 0.22f}, {0.32f, 0.22f, 0.14f}, kBark);
        const Vec3 leaf{0.20f + 0.1f * uni(rng), 0.36f + 0.12f * uni(rng), 0.12f};
        if (uni(rng) < 0.5f) {
            for (int k = 0; k < 3; ++k)
                add(kCone, {x, y + h * 0.45f + float(k) * h * 0.25f, z}, 0, {1.8f - 0.45f * k, h * 0.4f, 1.8f - 0.45f * k},
                    leaf * 0.8f, kLeaves);
        } else {
            for (int k = 0; k < 3; ++k) {
                const float ox = (uni(rng) - 0.5f) * 1.6f, oz = (uni(rng) - 0.5f) * 1.6f, rr = 1.3f + uni(rng) * 0.8f;
                add(kSphere, {x + ox, y + h + float(k) * 0.6f - 0.3f, z + oz}, 0, {rr, rr * 0.85f, rr}, leaf, kLeaves);
            }
        }
    }
    for (int i = 0; i < trees / 3; ++i) {
        const float a = uni(rng) * 2.0f * kPi, r = 33.0f + uni(rng) * 45.0f;
        const float x = std::cos(a) * r, z = std::sin(a) * r, sc = 0.4f + uni(rng) * 1.1f;
        add(kSphere, {x, terrainHeight(x, z) - 0.2f * sc, z}, uni(rng) * 6.0f, {sc * 1.4f, sc * 0.7f, sc}, {0.45f, 0.43f, 0.40f}, kStone);
    }
    return objs;
}

class Courtyard final : public Test {
public:
    explicit Courtyard(bool lite) : m_lite(lite) {
        m_info.id = lite ? "scene.courtyard.lite" : "scene.courtyard";
        m_info.name = lite ? "Courtyard (CPU-light)" : "Courtyard";
        m_info.description =
            lite ? "The Courtyard scene with one instanced draw per mesh type, so it measures the GPU rather than the CPU."
                 : "An arcaded plaza on wooded hills: forward shading, a sun shadow map, fog and tessellated terrain where "
                   "available, drawn the way a classic renderer does — one draw call per object.";
        m_info.category = Category::Scene;
        m_info.group = "Legacy";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12};
        m_info.reference = lite ? 300.0 : 200.0; // provisional, see docs/SCORING.md
        m_info.tags = {"legacy", lite ? "cpu-light" : "cpu-heavy"};
        m_info.suites = lite ? std::vector<std::string>{"quick", "standard", "extreme", "legacy"}
                             : std::vector<std::string>{"standard", "extreme", "legacy"};
        m_info.order = lite ? 21 : 20;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();

        // --- content -----------------------------------------------------------------------------
        MeshPool pool;
        std::uint32_t meshIds[kMeshKinds];
        meshIds[kBox] = pool.add(box(1, 1, 1));
        meshIds[kCylinder] = pool.add(cylinder(software ? 10 : 20, true));
        meshIds[kSphere] = pool.add(sphere(software ? 10 : 20, software ? 6 : 12));
        meshIds[kCone] = pool.add(cone(software ? 10 : 18));
        meshIds[kTorus] = pool.add(torus(software ? 16 : 40, software ? 8 : 14, 0.25f));
        const std::uint32_t cells = software ? 48 : s.quality == Quality::Low ? 128 : s.quality == Quality::Medium ? 192
                                                    : s.quality == Quality::High ? 256 : 384;
        const std::uint32_t terrain = pool.add(heightfield(cells, 170.0f, terrainHeight));
        pool.upload(dev);
        std::vector<Object> objects = buildObjects(software ? Quality::Low : s.quality, s.seed);
        // Group by mesh for instancing (the engine-like variant draws them in the same order).
        std::stable_sort(objects.begin(), objects.end(), [](const Object& a, const Object& b) { return a.mesh < b.mesh; });
        std::vector<ObjectData> instanceData;
        std::uint32_t groupFirst[kMeshKinds] = {}, groupCount[kMeshKinds] = {};
        for (const Object& o : objects) {
            if (groupCount[o.mesh]++ == 0) groupFirst[o.mesh] = static_cast<std::uint32_t>(instanceData.size());
            instanceData.push_back(o.data);
        }
        auto instances = dev.createBuffer({instanceData.size() * sizeof(ObjectData), rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                                           rhi::MemoryType::Device, 0, "courtyard-instances"},
                                          instanceData.data());

        // --- targets -----------------------------------------------------------------------------
        std::uint32_t samples = s.quality == Quality::Low ? 1 : s.quality == Quality::Medium ? 2 : 4;
        const std::uint32_t msaaMask =
            dev.renderTargetSampleCounts(rhi::Format::RGBA8_UNORM) & dev.renderTargetSampleCounts(rhi::Format::D32_FLOAT);
        while (samples > 1 && !(msaaMask & samples)) samples /= 2;
        const std::uint32_t shadowSize = software ? 1024 : s.quality == Quality::Ultra ? 4096 : s.quality == Quality::Low ? 1024 : 2048;
        rhi::TextureDesc od;
        od.format = rhi::Format::RGBA8_UNORM;
        od.width = s.width;
        od.height = s.height;
        od.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
        od.name = "courtyard-output";
        auto output = dev.createTexture(od);
        rhi::TextureDesc cd = od;
        cd.sampleCount = samples;
        cd.usage = rhi::TextureUsage::RenderTarget;
        cd.name = "courtyard-color-msaa";
        auto colorMs = samples > 1 ? dev.createTexture(cd) : nullptr;
        rhi::TextureDesc dd = cd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.usage = rhi::TextureUsage::DepthStencil;
        dd.name = "courtyard-depth";
        auto depth = dev.createTexture(dd);
        rhi::TextureDesc sd;
        sd.format = rhi::Format::D32_FLOAT;
        sd.width = sd.height = shadowSize;
        sd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        sd.name = "courtyard-shadow";
        auto shadow = dev.createTexture(sd);
        rhi::SamplerDesc smp;
        smp.addressU = smp.addressV = smp.addressW = rhi::AddressMode::Clamp;
        smp.mipFilter = rhi::Filter::Nearest;
        smp.compare = rhi::CompareOp::LessEqual;
        auto shadowSampler = dev.createSampler(smp);

        // --- pipelines ---------------------------------------------------------------------------
        const bool tess = f.tessellation && !software;
        using BT = rhi::BindingType;
        auto pipeline = [&](const char* name, const char* vs, const char* ps, std::vector<rhi::Binding> bindings,
                            bool instanced, bool shadowPass) {
            rhi::GraphicsPipelineDesc pd;
            pd.name = name;
            pd.vertex = {"scenes/courtyard", vs};
            if (ps) pd.fragment = {"scenes/courtyard", ps};
            vertexLayout(pd);
            if (instanced) {
                pd.vertexBindings.push_back({1, sizeof(ObjectData), true});
                pd.attributes.push_back({3, rhi::Format::RGBA32_FLOAT, 0, 1});
                pd.attributes.push_back({4, rhi::Format::RGBA32_FLOAT, 16, 1});
                pd.attributes.push_back({5, rhi::Format::RGBA32_FLOAT, 32, 1});
            }
            if (shadowPass) {
                pd.depthFormat = rhi::Format::D32_FLOAT;
                pd.raster.depthBias = 1.5f;
                pd.raster.depthBiasSlope = 2.0f;
                pd.raster.cull = rhi::CullMode::None;
            } else {
                pd.colorFormats = {rhi::Format::RGBA8_UNORM};
                pd.depthFormat = rhi::Format::D32_FLOAT;
                pd.sampleCount = samples;
                pd.raster.cull = rhi::CullMode::None; // foliage and thin boxes are seen from both sides
            }
            pd.layout.bindings = std::move(bindings);
            return dev.createGraphicsPipeline(pd);
        };
        const std::vector<rhi::Binding> mainBindings = {{BT::UniformBuffer, 0}, {BT::SampledTexture, 0}, {BT::Sampler, 0}};
        auto objectPipe = pipeline("courtyard-object", "vsObject", "psScene",
                                   {{BT::UniformBuffer, 0}, {BT::UniformBuffer, 1}, {BT::SampledTexture, 0}, {BT::Sampler, 0}}, false, false);
        auto instancedPipe = pipeline("courtyard-instanced", "vsInstanced", "psScene", mainBindings, true, false);
        auto shadowObjectPipe = pipeline("courtyard-shadow-object", "vsShadowObject", nullptr,
                                         {{BT::UniformBuffer, 0}, {BT::UniformBuffer, 1}}, false, true);
        auto shadowInstancedPipe = pipeline("courtyard-shadow-instanced", "vsShadowInstanced", nullptr, {{BT::UniformBuffer, 0}}, true, true);
        auto shadowTerrainPipe = pipeline("courtyard-shadow-terrain", "vsShadowTerrain", nullptr, {{BT::UniformBuffer, 0}}, false, true);
        std::unique_ptr<rhi::Pipeline> terrainPipe;
        if (tess) {
            rhi::GraphicsPipelineDesc pd;
            pd.name = "courtyard-terrain-tess";
            pd.vertex = {"scenes/courtyard", "vsTerrainTess"};
            pd.hull = {"scenes/courtyard", "hsTerrain"};
            pd.domain = {"scenes/courtyard", "dsTerrain"};
            pd.fragment = {"scenes/courtyard", "psScene"};
            vertexLayout(pd);
            pd.topology = rhi::PrimitiveTopology::PatchList;
            pd.patchControlPoints = 3;
            pd.colorFormats = {rhi::Format::RGBA8_UNORM};
            pd.depthFormat = rhi::Format::D32_FLOAT;
            pd.sampleCount = samples;
            pd.layout.bindings = mainBindings;
            terrainPipe = dev.createGraphicsPipeline(pd);
        } else {
            terrainPipe = pipeline("courtyard-terrain", "vsTerrain", "psScene", mainBindings, false, false);
        }
        rhi::GraphicsPipelineDesc skyDesc;
        skyDesc.name = "courtyard-sky";
        skyDesc.vertex = {"scenes/courtyard", "vsSky"};
        skyDesc.fragment = {"scenes/courtyard", "psSky"};
        skyDesc.colorFormats = {rhi::Format::RGBA8_UNORM};
        skyDesc.depthFormat = rhi::Format::D32_FLOAT;
        skyDesc.sampleCount = samples;
        skyDesc.depth.write = false;
        skyDesc.raster.cull = rhi::CullMode::None; // full-screen triangle
        skyDesc.depth.compare = rhi::CompareOp::LessEqual;
        skyDesc.layout.bindings = {{BT::UniformBuffer, 0}, {BT::UniformBuffer, 2}};
        auto skyPipe = dev.createGraphicsPipeline(skyDesc);

        auto& d = ctx.details();
        d["objects"] = objects.size();
        d["triangles"] = pool.triangleCount();
        d["draw_calls_per_frame"] = m_lite ? 2 * kMeshKinds + 3 : 2 * objects.size() + 3;
        d["msaa"] = samples;
        d["shadow_map"] = shadowSize;
        d["terrain_tessellation"] = tess;
        if (software) d["note"] = "software renderer: reduced detail";

        // --- frame -------------------------------------------------------------------------------
        const CameraPath path({{{62, 20, 58}, {0, 2, 0}},
                               {{36, 9, 14}, {0, 3, 0}},
                               {{21.6f, 2.4f, -12}, {21.6f, 2.6f, 10}},
                               {{21.6f, 2.4f, 13}, {6, 2.2f, 6}},
                               {{8, 3.2f, 9}, {0, 2.2f, 0}},
                               {{-9, 4, -5}, {-22, 4.5f, -20}},
                               {{-32, 26, -36}, {0, 0, 0}},
                               {{-62, 15, 22}, {0, 3, 0}}});
        const Vec3 sun = normalize({0.55f, 0.6f, 0.3f});
        const Mat4 sunViewProj = orthographic(-85, 85, -85, 85, 1.0f, 260.0f) * lookAt(sun * 120.0f, {0, 0, 0}, {0, 1, 0});
        const float aspect = float(s.width) / float(s.height);
        const float maxTess = s.quality == Quality::Low ? 4.0f : s.quality == Quality::Medium ? 8.0f : s.quality == Quality::High ? 12.0f : 16.0f;

        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(900);
        fl.simDuration = 30.0;
        fl.output = output.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        fl.record = [&](FrameContext& fc) {
            const CameraKey cam = path.at(fc.simTime / fl.simDuration);
            FrameData fd{};
            fd.viewProj = perspective(55.0f * kPi / 180.0f, aspect, 0.3f, 400.0f) * lookAt(cam.eye, cam.target, {0, 1, 0});
            fd.sunViewProj = sunViewProj;
            fd.cameraPos[0] = cam.eye.x, fd.cameraPos[1] = cam.eye.y, fd.cameraPos[2] = cam.eye.z;
            fd.cameraPos[3] = static_cast<float>(fc.simTime);
            fd.sunDir[0] = sun.x, fd.sunDir[1] = sun.y, fd.sunDir[2] = sun.z, fd.sunDir[3] = 1.7f;
            fd.sunColor[0] = 1.0f, fd.sunColor[1] = 0.94f, fd.sunColor[2] = 0.84f, fd.sunColor[3] = 1.0f / float(shadowSize);
            fd.fog[0] = 0.70f, fd.fog[1] = 0.76f, fd.fog[2] = 0.84f, fd.fog[3] = 0.006f;
            fd.tess[0] = maxTess, fd.tess[1] = 12.0f, fd.tess[2] = 70.0f, fd.tess[3] = 0.9f;
            const Mat4 invViewProj = inverse(fd.viewProj);

            rhi::CommandList& cmd = fc.cmd;
            auto drawObjects = [&](rhi::Pipeline* perObject, rhi::Pipeline* instancedPipe) {
                cmd.setVertexBuffer(0, pool.vertexBuffer());
                cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
                if (m_lite) {
                    cmd.setPipeline(instancedPipe);
                    for (int k = 0; k < kMeshKinds; ++k) {
                        if (!groupCount[k]) continue;
                        const SubMesh& sm = pool[meshIds[k]];
                        cmd.setVertexBuffer(1, instances.get(), std::uint64_t(groupFirst[k]) * sizeof(ObjectData));
                        cmd.drawIndexed(sm.indexCount, groupCount[k], sm.firstIndex, sm.baseVertex);
                    }
                } else {
                    cmd.setPipeline(perObject);
                    for (const Object& o : objects) {
                        const SubMesh& sm = pool[meshIds[o.mesh]];
                        cmd.setUniformData(1, &o.data, sizeof(ObjectData));
                        cmd.drawIndexed(sm.indexCount, 1, sm.firstIndex, sm.baseVertex);
                    }
                }
            };
            auto drawTerrain = [&](rhi::Pipeline* pipe) {
                const SubMesh& sm = pool[terrain];
                cmd.setPipeline(pipe);
                cmd.setVertexBuffer(0, pool.vertexBuffer());
                cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
                cmd.drawIndexed(sm.indexCount, 1, sm.firstIndex, sm.baseVertex);
            };

            // Shadow pass.
            rhi::RenderPassDesc sp;
            sp.depth = {shadow.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(sp);
            cmd.setUniformData(0, &fd, sizeof(fd));
            drawTerrain(shadowTerrainPipe.get());
            drawObjects(shadowObjectPipe.get(), shadowInstancedPipe.get());
            cmd.endRenderPass();
            cmd.transition(shadow.get(), rhi::ResourceState::ShaderRead);

            // Main pass.
            rhi::RenderPassDesc mp;
            rhi::ColorAttachment ca{colorMs ? colorMs.get() : output.get(), 0, 0, rhi::LoadOp::Clear,
                                    colorMs ? rhi::StoreOp::DontCare : rhi::StoreOp::Store, {0.7f, 0.76f, 0.84f, 1.0f}};
            if (colorMs) ca.resolveTarget = output.get();
            mp.colors.push_back(ca);
            mp.depth = {depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f};
            cmd.beginRenderPass(mp);
            cmd.setUniformData(0, &fd, sizeof(fd));
            cmd.setTexture(0, shadow.get());
            cmd.setSampler(0, shadowSampler.get());
            drawTerrain(terrainPipe.get());
            drawObjects(objectPipe.get(), instancedPipe.get());
            cmd.setPipeline(skyPipe.get());
            cmd.setUniformData(2, &invViewProj, sizeof(invViewProj));
            cmd.draw(3);
            cmd.endRenderPass();
        };
        const FrameLoopResult r = ctx.runFrames(fl);
        ctx.setFrameResult(r);
    }

private:
    bool m_lite;
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

namespace {
[[maybe_unused]] const bool kRegistered = [] {
    gt::bench::Registry::instance().add([] { return std::make_unique<gt::bench::Courtyard>(false); });
    gt::bench::Registry::instance().add([] { return std::make_unique<gt::bench::Courtyard>(true); });
    return true;
}();
} // namespace
