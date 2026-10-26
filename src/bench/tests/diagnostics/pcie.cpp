// PCIe link check: measures host -> GPU and GPU -> host copy bandwidth, re-reads the
// link state right after the transfers (links drop to low speeds when idle), and
// compares both with what the card supports.

#include "gt/bench/context.h"
#include "gt/bench/test.h"
#include "gt/diagnostics.h"
#include "gt/sysinfo.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

class PcieTest : public Test {
public:
    PcieTest() {
        m_info.id = "diag.pcie";
        m_info.name = "PCIe link check";
        m_info.description = "Measures transfer speed between system memory and the graphics card and checks that the "
                             "PCIe link runs at the generation and width the card supports (riser cables, wrong slots and "
                             "lane sharing often halve it).";
        m_info.category = Category::Diagnostic;
        m_info.order = 10;
        m_info.group = "Bus";
        m_info.frameBased = false;
        m_info.primaryMetric = "h2d_gbs";
        m_info.primaryUnit = "GB/s";
        m_info.tags = {"bus"};
        m_info.suites = {"diagnostics", "full"};
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
        const auto& ad = ctx.device().adapter();
        if (ctx.settings().param("diag.pcie.force") == "1") return SupportResult::yes(); // testing hook (CI)
        if (ad.isSoftware()) return SupportResult::no("software renderer: there is no PCIe link");
        if (ad.type == rhi::AdapterType::Integrated) return SupportResult::no("integrated GPU: there is no PCIe link to the GPU");
        return SupportResult::yes();
    }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        const std::uint64_t size = (s.duration == DurationPreset::Smoke ? 16 : 256) * kMiB;
        const int reps = s.duration == DurationPreset::Smoke ? 3 : 10;
        auto host = dev.createBuffer({size, rhi::BufferUsage::CopySrc, rhi::MemoryType::Upload, 0, "pcie-host-src"});
        auto device = dev.createBuffer({size, rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "pcie-device"});
        auto back = dev.createBuffer({size, rhi::BufferUsage::CopyDst, rhi::MemoryType::Readback, 0, "pcie-host-dst"});
        std::memset(host->mapped(), 0x5A, size);

        auto measure = [&](rhi::Buffer* dst, rhi::Buffer* src) {
            std::vector<double> gbs;
            ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.copyBuffer(dst, 0, src, 0, size); }); // warm up / wake the link
            for (int i = 0; i < reps; ++i) {
                ctx.checkStop();
                const double ms = ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.copyBuffer(dst, 0, src, 0, size); });
                if (ms > 0) gbs.push_back(static_cast<double>(size) / (ms / 1000.0) / 1e9);
            }
            std::sort(gbs.begin(), gbs.end());
            return gbs.empty() ? 0.0 : gbs[gbs.size() / 2];
        };
        const double h2d = measure(device.get(), host.get());
        ctx.progress(0.5, "GPU to host");
        const double d2h = measure(back.get(), device.get());
        // Link state right after the transfers, while the link is still trained up.
        ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.copyBuffer(device.get(), 0, host.get(), 0, size); });
        const results::GpuInfo g = ctx.gpuInfo ? ctx.gpuInfo() : results::GpuInfo{};

        ctx.addMetric("h2d_gbs", "Host → GPU bandwidth", h2d, "GB/s");
        ctx.addMetric("d2h_gbs", "GPU → host bandwidth", d2h, "GB/s");
        auto& d = ctx.details();
        d["transfer_mb"] = size / kMiB;
        d["resizable_bar"] = g.resizableBar;
        const bool linkKnown = g.pcieCurrentGen > 0 && g.pcieCurrentWidth > 0;
        std::vector<std::string> warnings;
        std::string link = "link state not readable on this system";
        if (linkKnown) {
            link = std::format("PCIe {} (card supports {})", sysinfo::formatLink(g.pcieCurrentGen, g.pcieCurrentWidth),
                               g.pcieMaxGen > 0 ? sysinfo::formatLink(g.pcieMaxGen, g.pcieMaxWidth) : std::string("?"));
            ctx.addMetric("link_gen", "Link generation", g.pcieCurrentGen, "");
            ctx.addMetric("link_width", "Link width", g.pcieCurrentWidth, "lanes");
            d["link"] = {{"current_gen", g.pcieCurrentGen}, {"current_width", g.pcieCurrentWidth}, {"max_gen", g.pcieMaxGen},
                         {"max_width", g.pcieMaxWidth}};
            if (g.pcieMaxWidth > 0 && g.pcieCurrentWidth < g.pcieMaxWidth)
                warnings.push_back(std::format("the link runs at x{} although the card supports x{}: check that it sits in a "
                                               "full-length x16 slot, the riser cable, and whether the board shares lanes "
                                               "with M.2 drives",
                                               g.pcieCurrentWidth, g.pcieMaxWidth));
            if (g.pcieMaxGen > 0 && g.pcieCurrentGen < g.pcieMaxGen)
                warnings.push_back(std::format("the link trained at Gen{} although the card supports Gen{}: the slot, a riser "
                                               "or a BIOS setting limits it (normal when the board or CPU only supports Gen{})",
                                               g.pcieCurrentGen, g.pcieMaxGen, g.pcieCurrentGen));
            const double expected = diag::pcieExpectedGBps(g.pcieCurrentGen, g.pcieCurrentWidth);
            if (expected > 0) {
                d["expected_gbs"] = expected;
                const double ratio = std::max(h2d, d2h) / expected;
                ctx.addMetric("link_efficiency", "Bandwidth vs link maximum", ratio * 100.0, "%");
                if (ratio < 0.5)
                    warnings.push_back(std::format("transfers reach only {:.0f} % of what {} can carry ({:.1f} GB/s)",
                                                   ratio * 100.0, sysinfo::formatLink(g.pcieCurrentGen, g.pcieCurrentWidth),
                                                   expected));
            }
        } else if (std::max(h2d, d2h) < 1.0) {
            warnings.push_back("transfers are below 1 GB/s");
        }
        std::string msg = std::format("{}; host → GPU {:.1f} GB/s, GPU → host {:.1f} GB/s; Resizable BAR {}.", link, h2d, d2h,
                                      g.resizableBar ? "on" : "off or unknown");
        if (!warnings.empty()) {
            msg += " Check: ";
            for (std::size_t i = 0; i < warnings.size(); ++i) msg += (i ? "; " : "") + warnings[i];
            msg += ".";
        }
        ctx.setVerdict(warnings.empty() ? results::Verdict::Pass : results::Verdict::Warn, msg);
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::PcieTest)
