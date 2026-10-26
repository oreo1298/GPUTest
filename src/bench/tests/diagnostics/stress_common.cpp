#include "stress_common.h"

#include <algorithm>
#include <cstring>

namespace gt::bench::diagnostics {
namespace {

struct StressParams {
    float time;
    std::uint32_t steps, shadowSteps, width, height, checkSlot, pad0, pad1;
};
static_assert(sizeof(StressParams) == 32);

} // namespace

StressRenderer::StressRenderer(TestContext& ctx, std::uint32_t width, std::uint32_t height)
    : m_ctx(ctx), m_dev(ctx.device()), m_width(width), m_height(height) {
    switch (ctx.settings().quality) {
    case Quality::Low: m_steps = 48, m_shadowSteps = 16; break;
    case Quality::Medium: m_steps = 64, m_shadowSteps = 24; break;
    case Quality::High: m_steps = 96, m_shadowSteps = 32; break;
    case Quality::Ultra: m_steps = 128, m_shadowSteps = 48; break;
    }
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = width;
    td.height = height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
    td.name = "stress-color";
    m_color = m_dev.createTexture(td);
    td.name = "stress-reference";
    m_reference = m_dev.createTexture(td);
    td.name = "stress-check";
    m_check = m_dev.createTexture(td);

    rhi::GraphicsPipelineDesc gd;
    gd.name = "stress";
    gd.vertex = {"diagnostics/stress", "vsStress"};
    gd.fragment = {"diagnostics/stress", "psStress"};
    gd.colorFormats = {td.format};
    gd.raster.cull = rhi::CullMode::None;
    gd.depth.test = false;
    gd.depth.write = false;
    gd.layout.pushConstantBytes = sizeof(StressParams);
    m_draw = m_dev.createGraphicsPipeline(gd);
    rhi::ComputePipelineDesc cd;
    cd.name = "stress-compare";
    cd.compute = {"diagnostics/stress", "csCompare"};
    cd.layout.bindings = {{rhi::BindingType::SampledTexture, 0},
                          {rhi::BindingType::SampledTexture, 1},
                          {rhi::BindingType::StorageBufferRW, 0}};
    cd.layout.pushConstantBytes = sizeof(StressParams);
    m_compare = m_dev.createComputePipeline(cd);

    const std::uint64_t bytes = (3 + kSlots) * 4;
    m_counters = m_dev.createBuffer({bytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
                                     rhi::MemoryType::Device, 0, "stress-counters"});
    m_readback = m_dev.createBuffer({bytes, rhi::BufferUsage::CopyDst, rhi::MemoryType::Readback, 0, "stress-readback"});
    m_ctx.timeGpu([&](rhi::CommandList& cmd) { cmd.fillBuffer(m_counters.get(), 0, bytes, 0); });
}

void StressRenderer::render(rhi::CommandList& cmd, rhi::Texture* target, float time) {
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {0, 0, 0, 1}});
    cmd.beginRenderPass(rp);
    cmd.setPipeline(m_draw.get());
    const StressParams p{time, m_steps, m_shadowSteps, m_width, m_height, 0, 0, 0};
    cmd.pushConstants(&p, sizeof(p));
    cmd.draw(3);
    cmd.endRenderPass();
}

void StressRenderer::recordFrame(rhi::CommandList& cmd, float time) { render(cmd, m_color.get(), time); }

void StressRenderer::captureReference() {
    m_ctx.timeGpu([&](rhi::CommandList& cmd) { render(cmd, m_reference.get(), kReferenceTime); });
}

void StressRenderer::recordCheck(rhi::CommandList& cmd, bool perturb) {
    render(cmd, m_check.get(), perturb ? kReferenceTime + 0.01f : kReferenceTime);
    cmd.transition(m_reference.get(), rhi::ResourceState::ShaderRead);
    cmd.transition(m_check.get(), rhi::ResourceState::ShaderRead);
    cmd.transition(m_counters.get(), rhi::ResourceState::UnorderedAccess);
    cmd.setPipeline(m_compare.get());
    cmd.setTexture(0, m_reference.get());
    cmd.setTexture(1, m_check.get());
    cmd.setStorageBuffer(0, m_counters.get());
    const StressParams p{0.0f, 0, 0, m_width, m_height, m_checks % kSlots, 0, 0};
    cmd.pushConstants(&p, sizeof(p));
    cmd.dispatch((m_width + 7) / 8, (m_height + 7) / 8);
    ++m_checks;
}

StressRenderer::CheckSummary StressRenderer::readChecks() {
    const std::uint64_t bytes = (3 + kSlots) * 4;
    m_ctx.timeGpu([&](rhi::CommandList& cmd) {
        cmd.copyBuffer(m_readback.get(), 0, m_counters.get(), 0, bytes);
        cmd.transition(m_readback.get(), rhi::ResourceState::HostRead);
    });
    std::vector<std::uint32_t> c(bytes / 4);
    std::memcpy(c.data(), m_readback->mapped(), bytes);
    CheckSummary s;
    s.checks = m_checks;
    s.mismatchedPixels = c[0];
    s.firstX = c[1];
    s.firstY = c[2];
    for (std::uint32_t i = 0; i < std::min(m_checks, kSlots); ++i)
        if (c[3 + i]) ++s.failedChecks;
    return s;
}

} // namespace gt::bench::diagnostics
