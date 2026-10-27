// "Lattice" — vertical-slice scene: thousands of instanced, procedurally animated
// blocks over a procedural floor. Instance transforms are computed in the vertex
// shader, so each frame is just two draws (CPU-light by construction).
#include "gt/bench/context.h"
#include "gt/bench/math.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gt::bench {
namespace {

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct SceneParams {
    Mat4 viewProj;
    float cameraPos[4];
    float lightDir[4];
    std::uint32_t grid[4];
    float fog[4];
};
static_assert(sizeof(SceneParams) == 128);

/// The block: a cube over [-1, 1]³, each face split `subdiv` x `subdiv` times; past one split its edges are
/// rounded (radius 0.25), with the grid packed towards them.
void buildCube(std::vector<Vertex>& v, std::vector<std::uint16_t>& idx, std::uint32_t subdiv) {
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float inner = subdiv > 1 ? 0.75f : 1.0f;
    auto coord = [&](std::uint32_t i) {
        const float x = -1.0f + 2.0f * float(i) / float(subdiv);
        return subdiv > 1 ? std::sin(x * kPi * 0.5f) : x;
    };
    for (const Vec3& n : normals) {
        const Vec3 t = n.y != 0.0f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 b = cross(n, t); // t x b == n, so (t, b) order is counter-clockwise seen from outside
        const auto base = static_cast<std::uint16_t>(v.size());
        for (std::uint32_t j = 0; j <= subdiv; ++j)
            for (std::uint32_t i = 0; i <= subdiv; ++i) {
                const Vec3 p = n + t * coord(i) + b * coord(j);
                const Vec3 q{std::clamp(p.x, -inner, inner), std::clamp(p.y, -inner, inner),
                             std::clamp(p.z, -inner, inner)};
                const Vec3 nr = normalize(p - q);
                const Vec3 r = q + nr * (1.0f - inner);
                v.push_back({r.x, r.y, r.z, nr.x, nr.y, nr.z});
            }
        for (std::uint32_t j = 0; j < subdiv; ++j)
            for (std::uint32_t i = 0; i < subdiv; ++i) {
                const std::uint32_t a = j * (subdiv + 1) + i, c = a + subdiv + 1;
                for (std::uint32_t k : {a, a + 1, c + 1, a, c + 1, c}) idx.push_back(static_cast<std::uint16_t>(base + k));
            }
    }
}

class Lattice final : public Test {
public:
    Lattice() {
        m_info.id = "scene.lattice";
        m_info.name = "Lattice";
        m_info.description = "Thousands of instanced, individually animated blocks over a procedural floor. "
                             "A pure rasterization workload with per-pixel noise shading; CPU-light by design.";
        m_info.category = Category::Scene;
        m_info.group = "Rasterization";
        m_info.apis = {rhi::Api::Vulkan, rhi::Api::D3D12, rhi::Api::D3D11, rhi::Api::OpenGL};
        m_info.reference = 600.0; // provisional, see docs/SCORING.md
        m_info.tags = {"cpu-light", "modern"};
        m_info.suites = {"smoke", "quick", "standard", "extreme", "legacy"};
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        // Blocks (x, y, z), pattern octaves and face splits per quality; software renderers get the low grid
        // (--param scene.full_detail=1 keeps the quality's).
        std::array<std::uint32_t, 4> grid{};
        std::uint32_t subdiv = 1;
        const bool software = dev.adapter().isSoftware() && s.param("scene.full_detail") != "1";
        switch (software ? Quality::Low : s.quality) {
        case Quality::Low: grid = {16, 3, 16, 2}, subdiv = 1; break;
        case Quality::Medium: grid = {32, 4, 32, 3}, subdiv = 2; break;
        case Quality::High: grid = {64, 6, 64, 4}, subdiv = 3; break;
        case Quality::Ultra: grid = {96, 8, 96, 5}, subdiv = 4; break;
        }
        const std::uint32_t instances = grid[0] * grid[1] * grid[2];

        std::vector<Vertex> cubeV;
        std::vector<std::uint16_t> cubeI;
        buildCube(cubeV, cubeI, subdiv);
        ctx.details()["instances"] = instances;
        ctx.details()["triangles"] = std::uint64_t(instances) * (cubeI.size() / 3);
        const std::vector<Vertex> floorV = {{-1, 0, -1, 0, 1, 0}, {1, 0, -1, 0, 1, 0}, {1, 0, 1, 0, 1, 0}, {-1, 0, 1, 0, 1, 0}};
        const std::vector<std::uint16_t> floorI = {0, 2, 1, 0, 3, 2};

        auto makeBuffer = [&](const void* data, std::size_t bytes, rhi::BufferUsage usage, const char* name) {
            return dev.createBuffer({bytes, usage | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, name}, data);
        };
        auto cubeVB = makeBuffer(cubeV.data(), cubeV.size() * sizeof(Vertex), rhi::BufferUsage::Vertex, "cube-vb");
        auto cubeIB = makeBuffer(cubeI.data(), cubeI.size() * 2, rhi::BufferUsage::Index, "cube-ib");
        auto floorVB = makeBuffer(floorV.data(), floorV.size() * sizeof(Vertex), rhi::BufferUsage::Vertex, "floor-vb");
        auto floorIB = makeBuffer(floorI.data(), floorI.size() * 2, rhi::BufferUsage::Index, "floor-ib");

        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA8_UNORM;
        cd.width = s.width;
        cd.height = s.height;
        cd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
        cd.name = "lattice-color";
        auto color = dev.createTexture(cd);
        rhi::TextureDesc dd = cd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.usage = rhi::TextureUsage::DepthStencil;
        dd.name = "lattice-depth";
        auto depth = dev.createTexture(dd);

        rhi::GraphicsPipelineDesc pd;
        pd.name = "lattice";
        pd.vertex = {"scenes/lattice", "vsLattice"};
        pd.fragment = {"scenes/lattice", "psLattice"};
        pd.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}, {1, rhi::Format::RGB32_FLOAT, 12}};
        pd.vertexBindings = {{0, sizeof(Vertex)}};
        pd.colorFormats = {cd.format};
        pd.depthFormat = dd.format;
        pd.layout.bindings = {{rhi::BindingType::UniformBuffer, 0}};
        auto lattice = dev.createGraphicsPipeline(pd);
        pd.name = "lattice-floor";
        pd.vertex = {"scenes/lattice", "vsFloor"};
        auto floor = dev.createGraphicsPipeline(pd);
        ctx.addMetric("pipeline_ms", "Pipeline creation time", lattice->creationMs() + floor->creationMs(), "ms", false);

        const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height);
        const float extent = static_cast<float>(std::max(grid[0], grid[2])) * 2.2f;
        FrameLoopDesc fl;
        fl.framesPerPass = s.framesPerPass(600);
        fl.simDuration = 20.0;
        fl.realTime = true;
        fl.output = color.get();
        fl.goldenFrames = {0, fl.framesPerPass / 2};
        fl.record = [&](FrameContext& f) {
            const float t = static_cast<float>(f.simTime);
            const float angle = 0.6f + t * 0.18f;
            const float radius = extent * (0.75f + 0.1f * std::sin(t * 0.3f));
            const Vec3 eye{std::cos(angle) * radius, extent * 0.35f + 4.0f * std::sin(t * 0.21f), std::sin(angle) * radius};
            const Vec3 target{0.0f, 4.0f, 0.0f};
            SceneParams p{};
            p.viewProj = perspective(55.0f * kPi / 180.0f, aspect, 0.5f, extent * 4.0f) * lookAt(eye, target, {0, 1, 0});
            p.cameraPos[0] = eye.x;
            p.cameraPos[1] = eye.y;
            p.cameraPos[2] = eye.z;
            p.cameraPos[3] = t;
            const Vec3 l = normalize({0.4f, 0.8f, 0.3f});
            p.lightDir[0] = l.x;
            p.lightDir[1] = l.y;
            p.lightDir[2] = l.z;
            p.lightDir[3] = 1.1f;
            for (int i = 0; i < 4; ++i) p.grid[i] = grid[static_cast<std::size_t>(i)];
            p.fog[0] = 0.62f;
            p.fog[1] = 0.70f;
            p.fog[2] = 0.80f;
            p.fog[3] = 0.6f / extent;

            rhi::CommandList& cmd = f.cmd;
            rhi::RenderPassDesc rp;
            rp.colors.push_back({color.get(), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.62f, 0.70f, 0.80f, 1.0f}});
            rp.depth = {depth.get(), rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f};
            cmd.beginRenderPass(rp);
            cmd.setPipeline(floor.get());
            cmd.setUniformData(0, &p, sizeof(p));
            cmd.setVertexBuffer(0, floorVB.get());
            cmd.setIndexBuffer(floorIB.get(), rhi::IndexType::Uint16);
            cmd.drawIndexed(6);
            cmd.setPipeline(lattice.get());
            cmd.setVertexBuffer(0, cubeVB.get());
            cmd.setIndexBuffer(cubeIB.get(), rhi::IndexType::Uint16);
            cmd.drawIndexed(static_cast<std::uint32_t>(cubeI.size()), instances);
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

GT_REGISTER_TEST(gt::bench::Lattice)
