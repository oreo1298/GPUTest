// "Atrium" (legacy tier): a colonnaded hall with two balcony levels under an open skylight,
// rendered with classic deferred shading (shaders/src/scenes/atrium.slang): shadow pass,
// G-buffer pass, sun + ambient pass, hundreds of animated point lights drawn as light volumes,
// tone mapping. Two variants draw identical images:
//  - scene.atrium: engine-like — one draw per object and one per light;
//  - scene.atrium.lite: CPU-light — instanced objects and a single instanced draw for all lights.

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/scene.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace gt::bench {
namespace {

using namespace scene;

struct FrameData {
    Mat4 viewProj;
    Mat4 invViewProj;
    Mat4 sunViewProj;
    float cameraPos[4];
    float sunDir[4];
    float sunColor[4];
    float screen[4];
};
static_assert(sizeof(FrameData) == 256);

struct ObjectData {
    float posRot[4];
    float scaleMat[4];
    float color[4];
};

struct LightData {
    float orbit[4];
    float motion[4];
    float color[4];
};
static_assert(sizeof(LightData) == 48 && sizeof(ObjectData) == 48);

enum Material { kMarble, kStone, kWood, kBrass, kLeaves, kFabric, kPlaster, kGlow };
enum MeshKind { kBox, kCylinder, kSphere, kTorus, kMeshKinds };

struct Object {
    MeshKind mesh;
    ObjectData data;
};

std::vector<Object> buildHall() {
    std::vector<Object> objs;
    auto add = [&](MeshKind mesh, Vec3 p, Vec3 scale, Vec3 color, Material m, float rot = 0.0f) {
        objs.push_back({mesh, {{p.x, p.y, p.z, rot}, {scale.x, scale.y, scale.z, float(m)}, {color.x, color.y, color.z, 1.0f}}});
    };
    const Vec3 stone{0.62f, 0.58f, 0.52f}, plaster{0.74f, 0.66f, 0.56f}, wood{0.42f, 0.27f, 0.15f}, brass{0.78f, 0.60f, 0.30f};
    add(kBox, {0, -0.1f, 0}, {32, 0.2f, 60}, {1, 1, 1}, kMarble);
    for (float sx : {-1.0f, 1.0f}) {
        add(kBox, {sx * 16.5f, 10, 0}, {1, 20, 61}, plaster, kPlaster);
        add(kBox, {sx * 14.25f, 20.25f, 0}, {4.5f, 0.5f, 60}, stone, kStone); // roof over the balconies
        for (int i = 0; i < 15; ++i) {
            const float z = -28.0f + float(i) * 4.0f;
            add(kBox, {sx * 15.8f, 10, z}, {0.6f, 20, 1.0f}, stone, kStone); // pilasters
            for (int level = 0; level < 2; ++level) {
                const float y0 = level * 6.2f;
                add(kCylinder, {sx * 12.0f, y0, z}, {0.35f, 5.6f, 0.35f}, stone, kStone);
                add(kBox, {sx * 12.0f, y0 + 5.75f, z}, {0.9f, 0.3f, 0.9f}, stone, kStone);
            }
        }
        for (int level = 1; level <= 2; ++level) {
            const float y = level * 6.2f;
            add(kBox, {sx * 14.0f, y - 0.2f, 0}, {4.4f, 0.4f, 60}, stone, kStone); // balcony slab
            add(kBox, {sx * 12.1f, y + 1.05f, 0}, {0.18f, 0.12f, 60}, wood, kWood);   // rail
            for (int i = 0; i < 98; ++i)
                add(kCylinder, {sx * 12.1f, y, -29.1f + float(i) * 0.6f}, {0.06f, 1.0f, 0.06f}, stone, kStone);
        }
        for (int i = 0; i < 5; ++i) { // benches and potted trees on the floor
            const float z = -20.0f + float(i) * 10.0f;
            add(kBox, {sx * 6.0f, 0.25f, z}, {0.7f, 0.5f, 2.8f}, wood, kWood);
            add(kBox, {sx * 6.0f, 0.55f, z}, {0.6f, 0.1f, 2.6f}, {0.45f, 0.10f, 0.10f}, kFabric);
            add(kCylinder, {sx * 9.0f, 0, z + 5}, {0.5f, 0.8f, 0.5f}, {0.50f, 0.30f, 0.20f}, kStone);
            add(kCylinder, {sx * 9.0f, 0.8f, z + 5}, {0.08f, 2.0f, 0.08f}, wood, kWood);
            add(kSphere, {sx * 9.0f, 3.1f, z + 5}, {0.9f, 1.1f, 0.9f}, {0.20f, 0.38f, 0.16f}, kLeaves);
        }
    }
    for (float sz : {-1.0f, 1.0f}) add(kBox, {0, 10, sz * 30.5f}, {34, 20, 1}, plaster, kPlaster);
    for (int i = 0; i <= 30; ++i) add(kBox, {0, 20, -30.0f + float(i) * 2.0f}, {24.5f, 0.35f, 0.25f}, stone, kStone); // skylight
    for (int i = 0; i < 9; ++i) add(kBox, {-12.0f + float(i) * 3.0f, 20, 0}, {0.25f, 0.35f, 60}, stone, kStone);
    // Centrepiece: basin and statue; stairs at the far end.
    add(kCylinder, {0, 0, 0}, {4.0f, 0.6f, 4.0f}, stone, kStone);
    add(kCylinder, {0, 0.6f, 0}, {1.0f, 1.6f, 1.0f}, stone, kStone);
    add(kCylinder, {0, 2.2f, 0}, {0.45f, 1.8f, 0.45f}, brass, kBrass);
    add(kSphere, {0, 4.5f, 0}, {0.7f, 0.7f, 0.7f}, brass, kBrass);
    add(kTorus, {0, 4.5f, 0}, {1.2f, 1.2f, 1.2f}, brass, kBrass);
    for (int s = 0; s < 8; ++s) add(kBox, {0, 0.375f + float(s) * 0.75f, 29.5f - float(s) * 0.6f}, {8, 0.75f, 0.6f + float(s) * 1.2f}, stone, kStone);
    for (int c = 0; c < 5; ++c) { // chandeliers
        const float z = -20.0f + float(c) * 10.0f;
        add(kTorus, {0, 15.0f, z}, {2.0f, 2.0f, 2.0f}, brass, kBrass);
        add(kCylinder, {0, 15.0f, z}, {0.04f, 5.0f, 0.04f}, brass, kBrass);
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) / 8.0f * 2.0f * kPi;
            add(kSphere, {std::cos(a) * 2.0f, 15.25f, z + std::sin(a) * 2.0f}, {0.15f, 0.2f, 0.15f}, {1.0f, 0.85f, 0.55f}, kGlow);
        }
    }
    return objs;
}

std::vector<LightData> buildLights(std::uint32_t count, std::uint64_t seed) {
    std::mt19937 rng(static_cast<std::uint32_t>(seed) ^ 0xA7A7u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Vec3 palette[6] = {{1.0f, 0.55f, 0.25f}, {0.30f, 0.55f, 1.0f}, {1.0f, 0.85f, 0.6f},
                             {0.9f, 0.3f, 0.6f},  {0.4f, 1.0f, 0.6f},  {1.0f, 1.0f, 1.0f}};
    std::vector<LightData> lights(count);
    for (auto& l : lights) {
        // One draw per statement: operands of a single expression are evaluated in an unspecified
        // order, and different compilers would then build different lights.
        const Vec3 base = palette[std::min(5, int(uni(rng) * 6.0f))];
        const Vec3 c = base * (3.0f + uni(rng) * 5.0f);
        const float speedMag = 0.3f + uni(rng) * 0.9f;
        const float speed = uni(rng) < 0.5f ? -speedMag : speedMag;
        l = {{-11.0f + uni(rng) * 22.0f, 1.0f + uni(rng) * 16.0f, -28.0f + uni(rng) * 56.0f, 0.5f + uni(rng) * 2.5f},
             {speed, uni(rng) * 6.2831853f, uni(rng), 4.0f + uni(rng) * 4.0f},
             {c.x, c.y, c.z, 1.0f}};
    }
    return lights;
}

class Atrium final : public Test {
public:
    explicit Atrium(bool lite) : m_lite(lite) {
        m_info.id = lite ? "scene.atrium.lite" : "scene.atrium";
        m_info.name = lite ? "Atrium (CPU-light)" : "Atrium";
        m_info.description =
            lite ? "The Atrium scene with instanced objects and all lights in one draw, so it measures the GPU."
                 : "A colonnaded hall under a skylight, rendered with classic deferred shading: G-buffer, sun shadows "
                   "and hundreds of moving point lights, one draw call per object and per light.";
        m_info.category = Category::Scene;
        m_info.group = "Legacy";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12, rhi::Api::OpenGL};
        m_info.reference = lite ? 250.0 : 180.0; // provisional, see docs/SCORING.md
        m_info.tags = {"legacy", lite ? "cpu-light" : "cpu-heavy"};
        m_info.suites = lite ? std::vector<std::string>{"quick", "standard", "extreme", "legacy"}
                             : std::vector<std::string>{"standard", "extreme", "legacy"};
        m_info.order = lite ? 31 : 30;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();

        // --- content -----------------------------------------------------------------------------
        MeshPool pool;
        std::uint32_t meshIds[kMeshKinds];
        meshIds[kBox] = pool.add(box(1, 1, 1));
        meshIds[kCylinder] = pool.add(cylinder(software ? 8 : 16, true));
        meshIds[kSphere] = pool.add(sphere(software ? 10 : 18, software ? 6 : 10));
        meshIds[kTorus] = pool.add(torus(software ? 16 : 36, software ? 6 : 10, 0.06f));
        const std::uint32_t lightMesh = pool.add(sphere(12, 8));
        pool.upload(dev);
        std::vector<Object> objects = buildHall();
        std::stable_sort(objects.begin(), objects.end(), [](const Object& a, const Object& b) { return a.mesh < b.mesh; });
        std::vector<ObjectData> instanceData;
        std::uint32_t groupFirst[kMeshKinds] = {}, groupCount[kMeshKinds] = {};
        for (const Object& o : objects) {
            if (groupCount[o.mesh]++ == 0) groupFirst[o.mesh] = static_cast<std::uint32_t>(instanceData.size());
            instanceData.push_back(o.data);
        }
        const std::uint32_t lightCount = software ? 32u : s.quality == Quality::Low ? 64u : s.quality == Quality::Medium ? 128u
                                                     : s.quality == Quality::High ? 256u : 512u;
        const std::vector<LightData> lights = buildLights(lightCount, s.seed);
        using U = rhi::BufferUsage;
        auto objectInstances = dev.createBuffer({instanceData.size() * sizeof(ObjectData), U::Vertex | U::CopyDst,
                                                 rhi::MemoryType::Device, 0, "atrium-objects"},
                                                instanceData.data());
        auto lightInstances = dev.createBuffer({lights.size() * sizeof(LightData), U::Vertex | U::CopyDst, rhi::MemoryType::Device, 0,
                                                "atrium-lights"},
                                               lights.data());

        // --- targets -----------------------------------------------------------------------------
        auto target = [&](rhi::Format format, rhi::TextureUsage usage, const char* name, std::uint32_t w, std::uint32_t h) {
            rhi::TextureDesc td;
            td.format = format;
            td.width = w;
            td.height = h;
            td.usage = usage;
            td.name = name;
            return dev.createTexture(td);
        };
        using TU = rhi::TextureUsage;
        auto albedo = target(rhi::Format::RGBA8_UNORM, TU::RenderTarget | TU::Sampled, "atrium-albedo", s.width, s.height);
        auto normal = target(rhi::Format::RGBA16_FLOAT, TU::RenderTarget | TU::Sampled, "atrium-normal", s.width, s.height);
        auto depth = target(rhi::Format::D32_FLOAT, TU::DepthStencil | TU::Sampled, "atrium-depth", s.width, s.height);
        auto hdr = target(rhi::Format::RGBA16_FLOAT, TU::RenderTarget | TU::Sampled, "atrium-hdr", s.width, s.height);
        auto output = target(rhi::Format::RGBA8_UNORM, TU::RenderTarget | TU::Sampled | TU::CopySrc, "atrium-output", s.width, s.height);
        const std::uint32_t shadowSize = software ? 1024 : s.quality == Quality::Ultra ? 4096 : 2048;
        auto shadow = target(rhi::Format::D32_FLOAT, TU::DepthStencil | TU::Sampled, "atrium-shadow", shadowSize, shadowSize);
        rhi::SamplerDesc smp;
        smp.addressU = smp.addressV = smp.addressW = rhi::AddressMode::Clamp;
        smp.mipFilter = rhi::Filter::Nearest;
        smp.compare = rhi::CompareOp::LessEqual;
        auto shadowSampler = dev.createSampler(smp);

        // --- pipelines ---------------------------------------------------------------------------
        using BT = rhi::BindingType;
        auto make = [&](const char* name, const char* vs, const char* ps, bool instanced, std::vector<rhi::Binding> bindings,
                        const std::function<void(rhi::GraphicsPipelineDesc&)>& configure) {
            rhi::GraphicsPipelineDesc pd;
            pd.name = name;
            pd.vertex = {"scenes/atrium", vs};
            if (ps) pd.fragment = {"scenes/atrium", ps};
            pd.layout.bindings = std::move(bindings);
            configure(pd);
            if (instanced) {
                pd.vertexBindings.push_back({1, 48, true});
                for (std::uint32_t k = 0; k < 3; ++k) pd.attributes.push_back({3 + k, rhi::Format::RGBA32_FLOAT, 16 * k, 1});
            }
            return dev.createGraphicsPipeline(pd);
        };
        auto geometry = [&](rhi::GraphicsPipelineDesc& pd) {
            vertexLayout(pd);
            pd.colorFormats = {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA16_FLOAT};
            pd.depthFormat = rhi::Format::D32_FLOAT;
            pd.raster.cull = rhi::CullMode::None;
        };
        auto shadowCfg = [&](rhi::GraphicsPipelineDesc& pd) {
            vertexLayout(pd);
            pd.depthFormat = rhi::Format::D32_FLOAT;
            pd.raster.depthBias = 1.5f;
            pd.raster.depthBiasSlope = 2.0f;
            pd.raster.cull = rhi::CullMode::None;
        };
        auto fullscreen = [&](rhi::GraphicsPipelineDesc& pd) {
            pd.raster.cull = rhi::CullMode::None;
            pd.colorFormats = {rhi::Format::RGBA16_FLOAT};
            pd.depthFormat = rhi::Format::D32_FLOAT;
            pd.depth.test = false;
            pd.depth.write = false;
        };
        auto lightCfg = [&](rhi::GraphicsPipelineDesc& pd) {
            vertexLayout(pd);
            pd.colorFormats = {rhi::Format::RGBA16_FLOAT};
            pd.depthFormat = rhi::Format::D32_FLOAT;
            pd.depth.write = false;
            pd.depth.compare = rhi::CompareOp::GreaterEqual; // back faces behind the visible surface
            pd.raster.cull = rhi::CullMode::Front;
            rhi::BlendAttachment add;
            add.enable = true;
            add.srcColor = add.dstColor = add.srcAlpha = add.dstAlpha = rhi::BlendFactor::One;
            pd.blend = {add};
        };
        const std::vector<rhi::Binding> gbuffer = {{BT::SampledTexture, 0}, {BT::SampledTexture, 1}, {BT::SampledTexture, 2}};
        auto gObject = make("atrium-gbuffer-object", "vsObject", "psGBuffer", false, {{BT::UniformBuffer, 0}, {BT::UniformBuffer, 1}}, geometry);
        auto gInstanced = make("atrium-gbuffer-instanced", "vsInstanced", "psGBuffer", true, {{BT::UniformBuffer, 0}}, geometry);
        auto sObject = make("atrium-shadow-object", "vsShadowObject", nullptr, false, {{BT::UniformBuffer, 0}, {BT::UniformBuffer, 1}}, shadowCfg);
        auto sInstanced = make("atrium-shadow-instanced", "vsShadowInstanced", nullptr, true, {{BT::UniformBuffer, 0}}, shadowCfg);
        std::vector<rhi::Binding> sunBindings = gbuffer;
        sunBindings.insert(sunBindings.end(), {{BT::UniformBuffer, 0}, {BT::SampledTexture, 3}, {BT::Sampler, 0}});
        auto sunPipe = make("atrium-sun", "vsFullscreen", "psSunAmbient", false, sunBindings, fullscreen);
        std::vector<rhi::Binding> lightBindings = gbuffer;
        lightBindings.push_back({BT::UniformBuffer, 0});
        std::vector<rhi::Binding> lightObjectBindings = lightBindings;
        lightObjectBindings.push_back({BT::UniformBuffer, 2});
        auto lObject = make("atrium-light", "vsLight", "psLight", false, lightObjectBindings, lightCfg);
        auto lInstanced = make("atrium-light-instanced", "vsLightInstanced", "psLight", true, lightBindings, lightCfg);
        auto tonemap = make("atrium-tonemap", "vsFullscreen", "psTonemap", false, {{BT::UniformBuffer, 0}, {BT::SampledTexture, 4}},
                            [](rhi::GraphicsPipelineDesc& pd) {
                                pd.raster.cull = rhi::CullMode::None;
                                pd.colorFormats = {rhi::Format::RGBA8_UNORM};
                                pd.depth.test = false;
                                pd.depth.write = false;
                            });

        auto& d = ctx.details();
        d["objects"] = objects.size();
        d["lights"] = lightCount;
        d["triangles"] = pool.triangleCount();
        d["draw_calls_per_frame"] = m_lite ? 2 * kMeshKinds + 3 : 2 * objects.size() + lightCount + 2;
        d["shadow_map"] = shadowSize;
        if (software) d["note"] = "software renderer: fewer lights";

        // --- frame -------------------------------------------------------------------------------
        const CameraPath path({{{0, 2.5f, -27}, {0, 4, 0}},
                               {{-6, 3, -12}, {6, 8, 4}},
                               {{6, 2, 4}, {-12, 6, 10}},
                               {{13.8f, 7.8f, 14}, {0, 3, 0}},
                               {{13.8f, 7.8f, -10}, {-12, 12, -20}},
                               {{-8, 15, -20}, {0, 2, 10}},
                               {{0, 17, 25}, {0, 0, -10}}});
        const Vec3 sun = normalize({0.25f, 1.0f, 0.35f});
        const Mat4 sunViewProj = orthographic(-40, 40, -40, 40, 1.0f, 140.0f) * lookAt(sun * 70.0f, {0, 0, 0}, {0, 0, 1});
        const float aspect = float(s.width) / float(s.height);

        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(900);
        fl.simDuration = 30.0;
        fl.output = output.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        fl.record = [&](FrameContext& fc) {
            const CameraKey cam = path.at(fc.simTime / fl.simDuration);
            FrameData fd{};
            fd.viewProj = perspective(60.0f * kPi / 180.0f, aspect, 0.2f, 200.0f) * lookAt(cam.eye, cam.target, {0, 1, 0});
            fd.invViewProj = inverse(fd.viewProj);
            fd.sunViewProj = sunViewProj;
            fd.cameraPos[0] = cam.eye.x, fd.cameraPos[1] = cam.eye.y, fd.cameraPos[2] = cam.eye.z;
            fd.cameraPos[3] = static_cast<float>(fc.simTime);
            fd.sunDir[0] = sun.x, fd.sunDir[1] = sun.y, fd.sunDir[2] = sun.z, fd.sunDir[3] = 3.0f;
            fd.sunColor[0] = 1.0f, fd.sunColor[1] = 0.93f, fd.sunColor[2] = 0.80f, fd.sunColor[3] = 1.0f / float(shadowSize);
            fd.screen[0] = float(s.width), fd.screen[1] = float(s.height), fd.screen[2] = 1.0f / float(s.width);
            fd.screen[3] = 1.0f / float(s.height);

            rhi::CommandList& cmd = fc.cmd;
            auto drawObjects = [&](rhi::Pipeline* perObject, rhi::Pipeline* instancedPipe) {
                cmd.setVertexBuffer(0, pool.vertexBuffer());
                cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
                if (m_lite) {
                    cmd.setPipeline(instancedPipe);
                    for (int k = 0; k < kMeshKinds; ++k) {
                        const SubMesh& sm = pool[meshIds[k]];
                        cmd.setVertexBuffer(1, objectInstances.get(), std::uint64_t(groupFirst[k]) * sizeof(ObjectData));
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

            rhi::RenderPassDesc sp;
            sp.depth = {shadow.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(sp);
            cmd.setUniformData(0, &fd, sizeof(fd));
            drawObjects(sObject.get(), sInstanced.get());
            cmd.endRenderPass();

            rhi::RenderPassDesc gp;
            gp.colors.push_back({albedo.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 0}});
            gp.colors.push_back({normal.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 1, 0}});
            gp.depth = {depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            cmd.beginRenderPass(gp);
            drawObjects(gObject.get(), gInstanced.get());
            cmd.endRenderPass();

            for (rhi::Texture* t : {albedo.get(), normal.get(), shadow.get()}) cmd.transition(t, rhi::ResourceState::ShaderRead);
            rhi::RenderPassDesc lp;
            lp.colors.push_back({hdr.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            lp.depth = {depth.get(), rhi::LoadOp::Load, rhi::StoreOp::Store, 1.0f, 0, true};
            cmd.beginRenderPass(lp);
            cmd.setTexture(0, albedo.get());
            cmd.setTexture(1, normal.get());
            cmd.setTexture(2, depth.get()); // read-only depth: attached for the light-volume test and sampled
            cmd.setTexture(3, shadow.get());
            cmd.setSampler(0, shadowSampler.get());
            cmd.setPipeline(sunPipe.get());
            cmd.draw(3);
            cmd.setVertexBuffer(0, pool.vertexBuffer());
            cmd.setIndexBuffer(pool.indexBuffer(), rhi::IndexType::Uint32);
            const SubMesh& lm = pool[lightMesh];
            if (m_lite) {
                cmd.setPipeline(lInstanced.get());
                cmd.setVertexBuffer(1, lightInstances.get());
                cmd.drawIndexed(lm.indexCount, lightCount, lm.firstIndex, lm.baseVertex);
            } else {
                cmd.setPipeline(lObject.get());
                for (const LightData& l : lights) {
                    cmd.setUniformData(2, &l, sizeof(l));
                    cmd.drawIndexed(lm.indexCount, 1, lm.firstIndex, lm.baseVertex);
                }
            }
            cmd.endRenderPass();

            cmd.transition(hdr.get(), rhi::ResourceState::ShaderRead);
            rhi::RenderPassDesc tp;
            tp.colors.push_back({output.get(), 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}});
            cmd.beginRenderPass(tp);
            cmd.setPipeline(tonemap.get());
            cmd.setTexture(4, hdr.get());
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
    gt::bench::Registry::instance().add([] { return std::make_unique<gt::bench::Atrium>(false); });
    gt::bench::Registry::instance().add([] { return std::make_unique<gt::bench::Atrium>(true); });
    return true;
}();
} // namespace
