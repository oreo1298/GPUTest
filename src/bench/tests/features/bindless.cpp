// Bindless textures (feature.bindless): a table of 1024 small textures, each a distinct
// solid colour. A check kernel reads every entry with a non-uniform index and must get each
// texture's own colour back; then texture-read throughput is compared for an ordinarily
// bound texture, a table index that is uniform per workgroup, and a different table index
// per thread (the case that needs NonUniformResourceIndex and costs the most).

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <cstring>
#include <format>

namespace gt::bench {
namespace {

struct BindlessParams {
    std::uint32_t tableSize, samples, pad0, pad1;
};

class BindlessTest : public Test {
public:
    BindlessTest() {
        m_info.id = "feature.bindless";
        m_info.name = "Bindless textures";
        m_info.description = "Reads from a table of a thousand textures indexed in the shader: checks every entry, then "
                             "compares read speed for bound, uniformly indexed and per-thread indexed textures.";
        m_info.category = Category::Feature;
        m_info.group = "Resources";
        m_info.frameBased = false;
        m_info.primaryMetric = "divergent_gtexels";
        m_info.primaryUnit = "Gtexel/s";
        m_info.reference = 400.0; // provisional (docs/SCORING.md)
        m_info.tags = {"modern"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 70;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        return ctx.device().features().maxBindlessTextures >= 1024
                   ? SupportResult::yes()
                   : SupportResult::no("no descriptor indexing with non-uniform texture arrays");
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& f = dev.features();
        const bool software = dev.adapter().isSoftware();
        constexpr std::uint32_t kTable = 1024, kSize = 16;
        using U = rhi::BufferUsage;

        // --- textures and table ------------------------------------------------------------------
        ctx.progress(0.05, "creating textures");
        auto table = dev.createTextureTable(kTable);
        std::vector<std::unique_ptr<rhi::Texture>> textures;
        std::vector<std::uint32_t> texels(kSize * kSize);
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = kSize;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.name = "bindless-entry";
        for (std::uint32_t i = 0; i < kTable; ++i) {
            const std::uint32_t c = (i & 255u) | ((i >> 8) << 8) | (0x40u << 16) | (0xFFu << 24);
            std::fill(texels.begin(), texels.end(), c);
            textures.push_back(dev.createTexture(td));
            dev.uploadTexture(textures.back().get(), 0, 0, texels.data(), kSize * 4);
            table->set(i, textures.back().get());
        }
        auto sampler = dev.createSampler({});
        auto output = dev.createBuffer({kTable * 4, U::Storage | U::CopySrc, rhi::MemoryType::Device, 4, "bindless-out"});
        auto readback = dev.createBuffer({kTable * 4, U::CopyDst, rhi::MemoryType::Readback, 0, "bindless-readback"});

        auto pipe = [&](const char* entry, bool useTable) {
            rhi::ComputePipelineDesc pd;
            pd.name = entry;
            pd.compute = {"features/bindless", entry};
            pd.layout.bindings = {{rhi::BindingType::StorageBufferRW, 0}, {rhi::BindingType::Sampler, 0}};
            if (!useTable) pd.layout.bindings.push_back({rhi::BindingType::SampledTexture, 0});
            pd.layout.textureTable = useTable;
            pd.layout.pushConstantBytes = sizeof(BindlessParams);
            return dev.createComputePipeline(pd);
        };
        auto bind = [&](rhi::CommandList& cmd, rhi::Pipeline* p, const BindlessParams& params, bool useTable) {
            cmd.transition(output.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(p);
            cmd.setStorageBuffer(0, output.get());
            cmd.setSampler(0, sampler.get());
            if (useTable) cmd.setTextureTable(table.get());
            else cmd.setTexture(0, textures[7].get());
            cmd.pushConstants(&params, sizeof(params));
        };

        // --- correctness -------------------------------------------------------------------------
        ctx.progress(0.15, "checking every table entry");
        auto check = pipe("csBindlessCheck", true);
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            for (auto& t : textures) cmd.transition(t.get(), rhi::ResourceState::ShaderRead);
            bind(cmd, check.get(), {kTable, 0, 0, 0}, true);
            cmd.dispatch(kTable / 64);
            cmd.copyBuffer(readback.get(), 0, output.get(), 0, kTable * 4);
            cmd.transition(readback.get(), rhi::ResourceState::HostRead);
        });
        std::vector<std::uint32_t> got(kTable);
        std::memcpy(got.data(), readback->mapped(), kTable * 4);
        std::uint32_t wrong = 0;
        std::string example;
        for (std::uint32_t i = 0; i < kTable; ++i)
            if (got[i] != (i & 0xFFFFu)) {
                if (example.empty()) example = std::format("entry {} returned the colour of entry {}", i, got[i]);
                ++wrong;
            }

        // --- throughput --------------------------------------------------------------------------
        const std::uint32_t threads = software ? (1u << 12) : (1u << 20);
        const std::uint32_t samples = software ? 8u : 32u;
        struct Variant {
            const char* entry;
            const char* name;
            const char* label;
            bool table;
        };
        const Variant variants[] = {{"csFetchBound", "bound", "Bound texture", false},
                                    {"csFetchUniform", "uniform", "Table, uniform index", true},
                                    {"csFetchDivergent", "divergent", "Table, per-thread index", true}};
        double rate[3] = {};
        for (int v = 0; v < 3; ++v) {
            ctx.progress(0.3 + 0.2 * v, variants[v].label);
            auto p = pipe(variants[v].entry, variants[v].table);
            const BindlessParams params{kTable, samples, 0, 0};
            const MicroResult m = measureMicro(ctx, [&](rhi::CommandList& cmd, std::uint32_t) {
                bind(cmd, p.get(), params, variants[v].table);
                cmd.dispatch(threads / 64);
            });
            rate[v] = m.msPerRep > 0 ? double(threads) * samples / (m.msPerRep / 1000.0) / 1e9 : 0.0;
            ctx.addMetric(std::format("{}_gtexels", variants[v].name), variants[v].label, rate[v], "Gtexel/s");
        }
        if (rate[0] > 0) {
            ctx.addMetric("uniform_cost", "Uniform table index vs bound", rate[1] / rate[0], "x");
            ctx.addMetric("divergent_cost", "Per-thread table index vs bound", rate[2] / rate[0], "x");
        }

        auto& d = ctx.details();
        d["table_size"] = kTable;
        d["max_table_size"] = f.maxBindlessTextures;
        d["descriptor_buffer"] = f.descriptorBuffer;
        d["entries_wrong"] = wrong;
        if (!example.empty()) d["error_example"] = example;
        if (software) d["note"] = "software renderer: reduced problem size";
        if (wrong)
            ctx.setVerdict(results::Verdict::Fail,
                           std::format("{} of {} table entries returned the wrong texture ({}).", wrong, kTable, example));
        else
            ctx.setVerdict(results::Verdict::Pass,
                           std::format("All {} table entries returned their own texture (tables up to {} entries{}).",
                                       kTable, f.maxBindlessTextures, f.descriptorBuffer ? "; descriptor buffers available" : ""));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::BindlessTest)
