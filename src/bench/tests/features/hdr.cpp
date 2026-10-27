// HDR output check (feature.hdr): renders an HDR10 test pattern (PQ / SMPTE ST 2084 in BT.2020 primaries,
// shaders/src/features/hdr.slang) and checks its encoded values against a CPU encoder; in a window it then
// switches the presenter to an HDR10 swapchain and shows the pattern, reporting what the display says about
// itself (colour space, luminance range — DXGI on Windows). Offscreen only the encoding is checked.

#include "gt/bench/context.h"
#include "gt/bench/presenter.h"
#include "gt/bench/test.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

constexpr float kBarNits[16] = {0.0f,   0.1f,   1.0f,    5.0f,    20.0f,   50.0f,   100.0f,  203.0f,
                                400.0f, 600.0f, 1000.0f, 1500.0f, 2000.0f, 4000.0f, 6000.0f, 10000.0f};

double pq(double nits) {
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

int code(double nits) { return static_cast<int>(std::lround(pq(nits) * 1023.0)); }

struct HdrParams {
    std::uint32_t width, height, pad0, pad1;
};

class HdrTest : public Test {
public:
    HdrTest() {
        m_info.id = "feature.hdr";
        m_info.name = "HDR output";
        m_info.description = "Checks HDR10 (PQ, BT.2020) encoding and, in a window, switches to an HDR10 swapchain "
                             "and shows a luminance and colour test pattern, reporting what the display supports.";
        m_info.category = Category::Feature;
        m_info.group = "Display";
        m_info.frameBased = false;
        m_info.primaryMetric = "pq_max_code_error";
        m_info.primaryUnit = "codes";
        m_info.higherIsBetter = false;
        m_info.reference = 0.0; // a check, not scored
        m_info.tags = {"display"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 96;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        if (ctx.device().features().maxComputeWorkgroupInvocations == 0)
            return SupportResult::no("no compute shaders");
        return SupportResult::yes();
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const bool software = dev.adapter().isSoftware();
        const std::uint32_t w = software ? 640u : 1920u, h = software ? 360u : 1080u;

        rhi::ComputePipelineDesc pd;
        pd.name = "hdr-pattern";
        pd.compute = {"features/hdr", "csPattern"};
        pd.layout.bindings = {{rhi::BindingType::StorageTexture, 0}};
        pd.layout.pushConstantBytes = sizeof(HdrParams);
        auto pipe = dev.createComputePipeline(pd);
        rhi::TextureDesc td;
        td.format = rhi::Format::RGB10A2_UNORM;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
        td.name = "hdr-pattern";
        auto pattern = dev.createTexture(td);
        ctx.timeGpu([&](rhi::CommandList& cmd) {
            const HdrParams p{w, h, 0, 0};
            cmd.transition(pattern.get(), rhi::ResourceState::UnorderedAccess);
            cmd.setPipeline(pipe.get());
            cmd.setStorageTexture(0, pattern.get());
            cmd.pushConstants(&p, sizeof(p));
            cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            cmd.transition(pattern.get(), rhi::ResourceState::ShaderRead);
        });

        // --- encoding check ------------------------------------------------------------------------
        ctx.progress(0.2, "checking the PQ encoding");
        const std::vector<std::uint8_t> px = dev.readbackTexture(pattern.get());
        auto at = [&](std::uint32_t x, std::uint32_t y) {
            std::uint32_t v;
            std::memcpy(&v, &px[(std::size_t(y) * w + x) * 4], 4);
            return std::array<int, 3>{int(v & 1023u), int((v >> 10) & 1023u), int((v >> 20) & 1023u)};
        };
        int maxError = 0;
        std::string firstError;
        auto expect = [&](const char* what, std::array<int, 3> got, std::array<double, 3> nits) {
            for (int c = 0; c < 3; ++c) {
                const int e = std::abs(got[c] - code(nits[c]));
                if (e > maxError) maxError = e;
                if (e > 1 && firstError.empty())
                    firstError = std::format("{}: channel {} is code {}, expected {} ({} nits)", what, c, got[c],
                                             code(nits[c]), nits[c]);
            }
        };
        for (std::uint32_t b = 0; b < 16; ++b) {
            const double n = kBarNits[b];
            expect(std::format("bar {} ({} nits)", b, n).c_str(), at((2 * b + 1) * w / 32, h * 35 / 100), {n, n, n});
        }
        expect("BT.2020 red patch", at(w / 12, h * 85 / 100), {100.0, 0.0, 0.0});
        expect("BT.709 red patch", at(7 * w / 12, h * 85 / 100), {62.74, 6.91, 1.64});
        ctx.addMetric("pq_max_code_error", "Largest PQ code error", maxError, "codes", false);
        auto& d = ctx.details();
        d["pattern"] = std::format("{}x{} RGB10A2, PQ (ST 2084), BT.2020", w, h);
        if (!firstError.empty()) {
            ctx.setVerdict(results::Verdict::Fail, "The HDR10 pattern is encoded wrongly: " + firstError + ".");
            return;
        }

        // --- display output ------------------------------------------------------------------------
        Presenter* presenter = ctx.presenter();
        if (!presenter) {
            d["display"] = "offscreen: not checked";
            ctx.setVerdict(results::Verdict::Pass,
                           "HDR10 encoding verified (offscreen run: the display output was not checked).");
            return;
        }
        ctx.progress(0.5, "switching to an HDR10 swapchain");
        const bool hdr = presenter->setHdr(true);
        const rhi::DisplayInfo info = presenter->swapchain()->displayInfo();
        d["display_color_space"] = info.colorSpace;
        d["swapchain"] = std::format("{} {}", rhi::formatInfo(presenter->swapchain()->format()).name,
                                     hdr ? "HDR10" : "SDR");
        if (info.known) {
            d["display_bits_per_color"] = info.bitsPerColor;
            d["display_min_nits"] = info.minNits;
            d["display_max_nits"] = info.maxNits;
            d["display_max_full_frame_nits"] = info.maxFullFrameNits;
            ctx.addMetric("display_peak_nits", "Display peak luminance (reported)", info.maxNits, "nits");
        }
        if (hdr) {
            ctx.progress(0.6, "showing the HDR10 test pattern");
            const double seconds = ctx.settings().duration == DurationPreset::Smoke ? 1.0 : 5.0;
            auto list = dev.createCommandList(rhi::QueueType::Graphics);
            const auto t0 = std::chrono::steady_clock::now();
            std::uint64_t frames = 0;
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
                ctx.checkStop();
                list->begin();
                const bool shown = presenter->record(*list, pattern.get(), {});
                list->end();
                rhi::CommandList* l[] = {list.get()};
                rhi::SubmitInfo si{l};
                if (shown) si.present = presenter->swapchain();
                dev.submit(rhi::QueueType::Graphics, si);
                if (shown) presenter->present();
                if (!presenter->pumpEvents()) break;
                ++frames;
            }
            dev.waitIdle();
            d["frames_shown"] = frames;
        }
        presenter->setHdr(false);
        if (!hdr) {
            ctx.setVerdict(results::Verdict::Warn,
                           "No HDR10 swapchain for this window: HDR is off in the system's display settings, or the "
                           "display, compositor or driver does not offer it. The encoding itself is correct.");
            return;
        }
        ctx.setVerdict(results::Verdict::Pass,
                       info.known && info.maxNits > 0
                           ? std::format("HDR10 output works ({}; the display reports {:.0f} nits peak, {:.0f} nits "
                                         "full frame). Bars brighter than the peak should look the same.",
                                         info.colorSpace, info.maxNits, info.maxFullFrameNits)
                           : std::format("HDR10 output works ({}). The display's luminance range is not reported "
                                         "on this API; bars brighter than its peak should look the same.",
                                         info.colorSpace));
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::HdrTest)
