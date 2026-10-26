// Host <-> GPU transfers: copy-engine uploads and downloads, and CPU writes straight
// into GPU memory through the BAR (Resizable BAR / Smart Access Memory when the whole
// VRAM is mappable).

#include "gt/bench/context.h"
#include "gt/bench/micro.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

namespace gt::bench {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

class TransferTest : public Test {
public:
    TransferTest() {
        m_info.id = "synth.transfer";
        m_info.name = "Host ↔ GPU transfers";
        m_info.description = "Upload and download speed through the copy engine, and how fast the CPU can write "
                             "directly into graphics memory (shows whether Resizable BAR / Smart Access Memory is active).";
        m_info.category = Category::Synthetic;
        m_info.group = "Bus";
        m_info.frameBased = false;
        m_info.primaryMetric = "upload_gbs";
        m_info.primaryUnit = "GB/s";
        m_info.reference = 25.0;
        m_info.tags = {"bus"};
        m_info.suites = {"standard", "extreme", "compute"};
        m_info.order = 30;
    }
    const TestInfo& info() const override { return m_info; }

    void run(TestContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const auto& s = ctx.settings();
        const std::uint64_t size = (s.duration == DurationPreset::Smoke || dev.adapter().isSoftware() ? 16 : 256) * kMiB;
        auto host = dev.createBuffer({size, rhi::BufferUsage::CopySrc, rhi::MemoryType::Upload, 0, "xfer-host"});
        auto gpu = dev.createBuffer({size, rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst, rhi::MemoryType::Device, 0, "xfer-gpu"});
        auto back = dev.createBuffer({size, rhi::BufferUsage::CopyDst, rhi::MemoryType::Readback, 0, "xfer-back"});
        std::memset(host->mapped(), 0x5A, size);
        const rhi::QueueType queue = dev.features().copyQueue ? rhi::QueueType::Copy : rhi::QueueType::Graphics;
        auto rate = [&](rhi::Buffer* dst, rhi::Buffer* src) {
            const MicroResult m = measureMicro(
                ctx, [&](rhi::CommandList& cmd, std::uint32_t) { cmd.copyBuffer(dst, 0, src, 0, size); }, 20.0, queue);
            return m.msPerRep > 0 ? static_cast<double>(size) / (m.msPerRep / 1000.0) / 1e9 : 0.0;
        };
        ctx.addMetric("upload_gbs", "Upload (host → GPU)", rate(gpu.get(), host.get()), "GB/s");
        ctx.progress(0.4, "download");
        ctx.addMetric("download_gbs", "Download (GPU → host)", rate(back.get(), gpu.get()), "GB/s");
        ctx.progress(0.7, "CPU writes to GPU memory");

        // CPU writes into device-local, host-visible memory (falls back to plain upload memory).
        auto bar = dev.createBuffer({size, rhi::BufferUsage::CopySrc, rhi::MemoryType::DeviceUpload, 0, "xfer-bar"});
        const bool deviceLocal = bar->isDeviceLocalHostVisible();
        std::vector<std::uint8_t> src(size, 0xA5);
        double best = 0.0;
        for (std::uint32_t i = 0; i < microSamples(s); ++i) {
            ctx.checkStop();
            const auto t0 = std::chrono::steady_clock::now();
            std::memcpy(bar->mapped(), src.data(), size);
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (sec > 0) best = std::max(best, static_cast<double>(size) / sec / 1e9);
        }
        ctx.addMetric("cpu_write_gbs", deviceLocal ? "CPU writes into VRAM (BAR)" : "CPU writes into upload memory", best, "GB/s");
        auto& d = ctx.details();
        d["transfer_mb"] = size / kMiB;
        d["copy_queue"] = queue == rhi::QueueType::Copy;
        d["resizable_bar"] = dev.features().resizableBar;
        d["bar_write_is_device_local"] = deviceLocal;
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::TransferTest)
