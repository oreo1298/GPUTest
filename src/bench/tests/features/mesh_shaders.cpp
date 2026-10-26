// Mesh shaders (feature.mesh): one field of rotating spheres drawn with the classic
// indexed vertex pipeline, with mesh shaders (one workgroup per meshlet per instance) and
// with task + mesh shaders that cull meshlets facing away from the camera or outside the
// view (shaders/src/features/mesh.slang). Reports triangle throughput of each path and
// checks that all three produce the same image.

#include "rt_common.h"

#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace gt::bench {
namespace {

struct SceneData {
    Mat4 viewProj;
    float planes[5][4];
    float cameraPos[4];
    float lightDir[4];
    std::uint32_t grid[4];
    float layout[4];
};
static_assert(sizeof(SceneData) == 208);

struct Meshlet {
    std::uint32_t vertexOffset, vertexCount, primOffset, primCount;
    float sphere[4];
    float cone[4];
};
static_assert(sizeof(Meshlet) == 48);

constexpr std::uint32_t kMaxVerts = 64, kMaxPrims = 124;

struct Meshlets {
    std::vector<Meshlet> meshlets;
    std::vector<std::uint32_t> vertices; ///< global vertex index per meshlet-local vertex
    std::vector<std::uint32_t> prims;    ///< packed local indices
};

/// Greedy meshlet builder in index order (the icosphere's subdivision order is spatially coherent).
Meshlets buildMeshlets(const rt::Mesh& mesh) {
    Meshlets out;
    std::unordered_map<std::uint32_t, std::uint32_t> local;
    Meshlet cur{};
    std::vector<std::array<std::uint32_t, 3>> curTris;
    auto pos = [&](std::uint32_t v) { return Vec3{mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]}; };
    auto flush = [&] {
        if (curTris.empty()) return;
        cur.primOffset = static_cast<std::uint32_t>(out.prims.size());
        cur.primCount = static_cast<std::uint32_t>(curTris.size());
        // Bounding sphere around the vertex centroid; normal cone as in meshoptimizer.
        Vec3 c{};
        for (std::uint32_t i = 0; i < cur.vertexCount; ++i) c = c + pos(out.vertices[cur.vertexOffset + i]);
        c = c * (1.0f / float(cur.vertexCount));
        float r = 0.0f;
        for (std::uint32_t i = 0; i < cur.vertexCount; ++i) {
            const Vec3 d = pos(out.vertices[cur.vertexOffset + i]) - c;
            r = std::max(r, std::sqrt(dot(d, d)));
        }
        Vec3 axis{};
        std::vector<Vec3> normals;
        for (const auto& t : curTris) {
            const Vec3 a = pos(out.vertices[cur.vertexOffset + t[0]]), b = pos(out.vertices[cur.vertexOffset + t[1]]),
                       cc = pos(out.vertices[cur.vertexOffset + t[2]]);
            normals.push_back(normalize(cross(b - a, cc - a)));
            axis = axis + normals.back();
        }
        axis = normalize(axis);
        float minDot = 1.0f;
        for (const Vec3& n : normals) minDot = std::min(minDot, dot(n, axis));
        const float cutoff = minDot <= 0.1f ? 1.0f : std::sqrt(1.0f - minDot * minDot);
        cur.sphere[0] = c.x, cur.sphere[1] = c.y, cur.sphere[2] = c.z, cur.sphere[3] = r;
        cur.cone[0] = axis.x, cur.cone[1] = axis.y, cur.cone[2] = axis.z, cur.cone[3] = cutoff;
        for (const auto& t : curTris) out.prims.push_back(t[0] | t[1] << 8 | t[2] << 16);
        out.meshlets.push_back(cur);
        cur = {};
        cur.vertexOffset = static_cast<std::uint32_t>(out.vertices.size());
        curTris.clear();
        local.clear();
    };
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        std::uint32_t fresh = 0;
        for (int k = 0; k < 3; ++k) fresh += local.count(mesh.indices[i + k]) ? 0 : 1;
        if (cur.vertexCount + fresh > kMaxVerts || curTris.size() + 1 > kMaxPrims) flush();
        std::array<std::uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh.indices[i + k];
            auto it = local.find(v);
            if (it == local.end()) {
                it = local.emplace(v, cur.vertexCount++).first;
                out.vertices.push_back(v);
            }
            tri[static_cast<std::size_t>(k)] = it->second;
        }
        curTris.push_back(tri);
    }
    flush();
    return out;
}

class MeshShaderTest : public Test {
public:
    MeshShaderTest() {
        m_info.id = "feature.mesh";
        m_info.name = "Mesh shaders";
        m_info.description = "Draws the same scene with the classic vertex pipeline, with mesh shaders, and with task "
                             "shaders that cull hidden meshlets; compares triangle throughput and checks the images match.";
        m_info.category = Category::Feature;
        m_info.group = "Geometry";
        m_info.frameBased = false;
        m_info.primaryMetric = "mesh_best_gtris";
        m_info.primaryUnit = "Gtri/s";
        m_info.reference = 20.0; // provisional (docs/SCORING.md)
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 30;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().meshShader ? SupportResult::yes()
                                                  : SupportResult::no("no mesh shaders (VK_EXT_mesh_shader / D3D12 mesh shader tier)");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const RunSettings& s = ctx.settings();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t width = software ? 320 : s.width, height = software ? 180 : s.height;
        const std::uint32_t side = software ? 6 : 48;
        const float spacing = 2.6f;
        const rt::Mesh sphere = rt::icosphere(3);
        const Meshlets ml = buildMeshlets(sphere);
        const auto meshletCount = static_cast<std::uint32_t>(ml.meshlets.size());
        if (meshletCount > 64) throw rhi::Error(std::format("{} meshlets per sphere (the task shader handles 64)", meshletCount));
        const std::uint32_t instances = side * side;
        const std::uint64_t trisPerFrame = std::uint64_t(sphere.triangleCount()) * instances;

        // Positions as float4 so the vertex pipeline and the mesh shaders read the same buffer.
        std::vector<float> pos4;
        for (std::size_t i = 0; i < sphere.positions.size(); i += 3)
            pos4.insert(pos4.end(), {sphere.positions[i], sphere.positions[i + 1], sphere.positions[i + 2], 1.0f});
        using U = rhi::BufferUsage;
        auto buffer = [&](const void* data, std::size_t bytes, U usage, std::uint32_t stride, const char* name) {
            return dev.createBuffer({bytes, usage | U::CopyDst, rhi::MemoryType::Device, stride, name}, data);
        };
        auto positions = buffer(pos4.data(), pos4.size() * 4, U::Vertex | U::Storage, 16, "mesh-positions");
        auto indices = buffer(sphere.indices.data(), sphere.indices.size() * 4, U::Index, 0, "mesh-indices");
        auto meshletBuf = buffer(ml.meshlets.data(), ml.meshlets.size() * sizeof(Meshlet), U::Storage, sizeof(Meshlet), "meshlets");
        auto meshletVerts = buffer(ml.vertices.data(), ml.vertices.size() * 4, U::Storage, 4, "meshlet-vertices");
        auto meshletPrims = buffer(ml.prims.data(), ml.prims.size() * 4, U::Storage, 4, "meshlet-prims");
        auto stats = dev.createBuffer({256, U::Storage | U::CopySrc | U::CopyDst, rhi::MemoryType::Device, 4, "mesh-stats"});
        auto statsReadback = dev.createBuffer({256, U::CopyDst, rhi::MemoryType::Readback, 0, "mesh-stats-readback"});

        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA8_UNORM;
        cd.width = width;
        cd.height = height;
        cd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        cd.name = "mesh-color";
        auto color = dev.createTexture(cd);
        rhi::TextureDesc dd = cd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.usage = rhi::TextureUsage::DepthStencil;
        dd.name = "mesh-depth";
        auto depth = dev.createTexture(dd);

        rhi::GraphicsPipelineDesc pd;
        pd.fragment = {"features/mesh", "psSpheres"};
        pd.colorFormats = {cd.format};
        pd.depthFormat = dd.format;
        pd.raster.cull = rhi::CullMode::None;
        pd.layout.bindings = {{rhi::BindingType::UniformBuffer, 0},
                              {rhi::BindingType::StorageBufferRO, 0},
                              {rhi::BindingType::StorageBufferRO, 1},
                              {rhi::BindingType::StorageBufferRO, 2},
                              {rhi::BindingType::StorageBufferRO, 3},
                              {rhi::BindingType::StorageBufferRW, 0}};
        rhi::GraphicsPipelineDesc vpd = pd;
        vpd.name = "spheres-vertex";
        vpd.vertex = {"features/mesh", "vsSpheres"};
        vpd.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}};
        vpd.vertexBindings = {{0, 16}};
        auto vertexPipe = dev.createGraphicsPipeline(vpd);
        rhi::GraphicsPipelineDesc mpd = pd;
        mpd.name = "spheres-mesh";
        mpd.mesh = {"features/mesh", "msDirect"};
        auto meshPipe = dev.createGraphicsPipeline(mpd);
        std::unique_ptr<rhi::Pipeline> taskPipe;
        const bool taskShaders = dev.features().taskShader;
        if (taskShaders) {
            rhi::GraphicsPipelineDesc tpd = pd;
            tpd.name = "spheres-task";
            tpd.task = {"features/mesh", "tsCull"};
            tpd.mesh = {"features/mesh", "msFromTask"};
            taskPipe = dev.createGraphicsPipeline(tpd);
        }

        // Camera: above the near edge of the field, looking across it (the sides fall out of view).
        const float extent = spacing * float(side);
        const Vec3 eye{0.0f, extent * 0.3f + 2.0f, -extent * 0.55f}, target{0.0f, 0.0f, extent * 0.1f};
        SceneData sd{};
        sd.viewProj = perspective(50.0f * kPi / 180.0f, float(width) / float(height), 0.5f, extent * 3.0f) *
                      lookAt(eye, target, {0, 1, 0});
        const Mat4& m = sd.viewProj;
        auto row = [&](int r) { return std::array<float, 4>{m.m[0][r], m.m[1][r], m.m[2][r], m.m[3][r]}; };
        const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const std::array<float, 4> planes[5] = {
            {r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]},
            {r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]},
            {r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]},
            {r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]},
            r2};
        for (int i = 0; i < 5; ++i) {
            const float l = std::sqrt(planes[i][0] * planes[i][0] + planes[i][1] * planes[i][1] + planes[i][2] * planes[i][2]);
            for (int k = 0; k < 4; ++k) sd.planes[i][k] = planes[i][static_cast<std::size_t>(k)] / l;
        }
        sd.cameraPos[0] = eye.x, sd.cameraPos[1] = eye.y, sd.cameraPos[2] = eye.z, sd.cameraPos[3] = 1.3f;
        const Vec3 light = normalize({0.4f, 0.8f, -0.3f});
        sd.lightDir[0] = light.x, sd.lightDir[1] = light.y, sd.lightDir[2] = light.z;
        sd.grid[0] = side;
        sd.grid[1] = meshletCount;
        sd.layout[0] = spacing;

        enum Path { Vertex, Mesh, Task };
        auto record = [&](rhi::CommandList& cmd, Path path) {
            cmd.transition(stats.get(), rhi::ResourceState::UnorderedAccess);
            rhi::RenderPassDesc rp;
            rp.colors.push_back({color.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.55f, 0.62f, 0.72f, 1.0f}});
            rp.depth = {depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f};
            cmd.beginRenderPass(rp);
            cmd.setPipeline(path == Vertex ? vertexPipe.get() : path == Mesh ? meshPipe.get() : taskPipe.get());
            cmd.setUniformData(0, &sd, sizeof(sd));
            cmd.setReadOnlyBuffer(0, positions.get());
            cmd.setReadOnlyBuffer(1, meshletBuf.get());
            cmd.setReadOnlyBuffer(2, meshletVerts.get());
            cmd.setReadOnlyBuffer(3, meshletPrims.get());
            cmd.setStorageBuffer(0, stats.get());
            if (path == Vertex) {
                cmd.setVertexBuffer(0, positions.get());
                cmd.setIndexBuffer(indices.get(), rhi::IndexType::Uint32);
                cmd.drawIndexed(static_cast<std::uint32_t>(sphere.indices.size()), instances);
            } else if (path == Mesh) {
                cmd.drawMeshTasks(meshletCount, instances, 1);
            } else {
                cmd.drawMeshTasks(instances, 1, 1);
            }
            cmd.endRenderPass();
        };

        struct Result {
            const char* name;
            const char* label;
            double gtris = 0.0;
            double ms = 0.0;
            std::string signature;
        };
        auto& d = ctx.details();
        std::vector<Result> results;
        const Path paths[] = {Vertex, Mesh, Task};
        const char* names[] = {"vertex", "mesh", "task"};
        const char* labels[] = {"Vertex pipeline", "Mesh shaders", "Task + mesh shaders (meshlet culling)"};
        for (Path path : paths) {
            if (path == Task && !taskPipe) continue;
            ctx.progress(0.1 + 0.3 * double(path), labels[path]);
            const MicroResult mr = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) { record(cmd, path); });
            Result r{names[path], labels[path]};
            r.ms = mr.msPerRep;
            r.gtris = mr.msPerRep > 0 ? double(trisPerFrame) / (mr.msPerRep / 1000.0) / 1e9 : 0.0;
            ctx.timeGpu([&](rhi::CommandList& cmd) {
                cmd.fillBuffer(stats.get(), 0, 4, 0);
                record(cmd, path);
                cmd.copyBuffer(statsReadback.get(), 0, stats.get(), 0, 4);
                cmd.transition(statsReadback.get(), rhi::ResourceState::HostRead);
            });
            if (path == Task) {
                std::uint32_t visible = 0;
                std::memcpy(&visible, statsReadback->mapped(), 4);
                const double total = double(meshletCount) * instances;
                d["meshlets_visible"] = visible;
                d["meshlets_culled_pct"] = total > 0 ? 100.0 * (1.0 - double(visible) / total) : 0.0;
            }
            std::vector<std::uint8_t> px = dev.readbackTexture(color.get());
            for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            r.signature = imageSignature(width, height, px.data());
            if (s.dumpFrames) writePng(std::format("{}/feature.mesh_{}.png", s.dumpDir, r.name), width, height, px.data());
            results.push_back(std::move(r));
        }

        double bestMesh = 0.0;
        for (const Result& r : results) {
            ctx.addMetric(std::format("{}_gtris", r.name), r.label, r.gtris, "Gtri/s");
            d[std::format("{}_ms", r.name)] = r.ms;
            if (r.name != names[Vertex]) bestMesh = std::max(bestMesh, r.gtris);
        }
        ctx.addMetric("mesh_best_gtris", "Best mesh-shader path", bestMesh, "Gtri/s");
        if (results[0].gtris > 0) {
            ctx.addMetric("mesh_speedup", "Mesh shaders vs vertex pipeline", results[1].gtris / results[0].gtris, "x");
            if (results.size() > 2)
                ctx.addMetric("task_speedup", "Task culling vs vertex pipeline", results[2].gtris / results[0].gtris, "x");
        }
        d["instances"] = instances;
        d["triangles_per_frame"] = trisPerFrame;
        d["meshlets_per_mesh"] = meshletCount;
        d["triangles_per_meshlet"] = double(sphere.triangleCount()) / meshletCount;
        d["width"] = width;
        d["height"] = height;
        if (!taskPipe) d["note_task"] = "no task shaders on this GPU";
        if (software) d["note"] = "software renderer: reduced problem size";

        int worst = 0;
        for (std::size_t i = 1; i < results.size(); ++i)
            worst = std::max(worst, compareSignatures(results[0].signature, results[i].signature));
        d["image_max_diff"] = worst;
        if (worst < 0 || worst > 4)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("The mesh-shader paths rendered a different image from the vertex pipeline "
                                       "(max cell difference {}).",
                                       worst));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("All {} paths rendered the same image ({} meshlets per sphere).", results.size(),
                                       meshletCount));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::MeshShaderTest)
