#include "gt/bench/upscale.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace gt::bench {

std::string_view upscalerName(Upscaler u) {
    switch (u) {
    case Upscaler::Off: return "off";
    case Upscaler::Bilinear: return "bilinear";
    case Upscaler::Fsr1: return "fsr1";
    }
    return "off";
}

std::string_view upscaleModeName(UpscaleMode m) {
    switch (m) {
    case UpscaleMode::UltraQuality: return "ultra-quality";
    case UpscaleMode::Quality: return "quality";
    case UpscaleMode::Balanced: return "balanced";
    case UpscaleMode::Performance: return "performance";
    }
    return "quality";
}

std::optional<Upscaler> parseUpscaler(std::string_view s) {
    for (Upscaler u : {Upscaler::Off, Upscaler::Bilinear, Upscaler::Fsr1})
        if (s == upscalerName(u)) return u;
    if (s == "fsr") return Upscaler::Fsr1;
    return std::nullopt;
}

std::optional<UpscaleMode> parseUpscaleMode(std::string_view s) {
    for (UpscaleMode m : {UpscaleMode::UltraQuality, UpscaleMode::Quality, UpscaleMode::Balanced, UpscaleMode::Performance})
        if (s == upscaleModeName(m)) return m;
    return std::nullopt;
}

double upscaleRatio(UpscaleMode m) {
    switch (m) {
    case UpscaleMode::UltraQuality: return 1.3;
    case UpscaleMode::Quality: return 1.5;
    case UpscaleMode::Balanced: return 1.7;
    case UpscaleMode::Performance: return 2.0;
    }
    return 1.5;
}

std::uint32_t renderExtent(std::uint32_t output, double ratio) {
    return std::max(1u, static_cast<std::uint32_t>(std::lround(double(output) / std::max(ratio, 1.0))));
}

namespace fsr1 {
namespace {
std::uint32_t u(float f) { return std::bit_cast<std::uint32_t>(f); }
} // namespace

Constants easu(std::uint32_t inWidth, std::uint32_t inHeight, std::uint32_t outWidth, std::uint32_t outHeight) {
    const float vx = float(inWidth), vy = float(inHeight); // viewport = whole input image
    const float ix = 1.0f / float(inWidth), iy = 1.0f / float(inHeight);
    Constants c;
    c.con = {// output pixel -> input viewport pixel
             u(vx / float(outWidth)), u(vy / float(outHeight)), u(0.5f * vx / float(outWidth) - 0.5f),
             u(0.5f * vy / float(outHeight) - 0.5f),
             // viewport pixel -> normalized position of the first gather
             u(ix), u(iy), u(1.0f * ix), u(-1.0f * iy),
             // the other gathers, from the first
             u(-1.0f * ix), u(2.0f * iy), u(1.0f * ix), u(2.0f * iy), u(0.0f), u(4.0f * iy), 0, 0};
    c.outWidth = outWidth;
    c.outHeight = outHeight;
    return c;
}

Constants rcas(float sharpnessStops, std::uint32_t width, std::uint32_t height) {
    Constants c;
    c.con[0] = u(std::exp2(-sharpnessStops));
    c.outWidth = width;
    c.outHeight = height;
    return c;
}
} // namespace fsr1

UpscalePass::UpscalePass(rhi::Device& dev, Upscaler kind, std::uint32_t inWidth, std::uint32_t inHeight,
                         std::uint32_t outWidth, std::uint32_t outHeight, float sharpnessStops)
    : m_dev(dev), m_kind(kind), m_outWidth(outWidth), m_outHeight(outHeight) {
    if (kind == Upscaler::Off) throw rhi::Error("UpscalePass: no upscaler selected");
    m_easu = fsr1::easu(inWidth, inHeight, outWidth, outHeight);
    m_rcas = fsr1::rcas(sharpnessStops, outWidth, outHeight);
    auto pipeline = [&](const char* entry) {
        rhi::ComputePipelineDesc pd;
        pd.name = entry;
        pd.compute = {"upscale/fsr1", entry};
        pd.layout.bindings = {{rhi::BindingType::SampledTexture, 0},
                              {rhi::BindingType::Sampler, 0},
                              {rhi::BindingType::StorageTexture, 0}};
        pd.layout.pushConstantBytes = sizeof(fsr1::Constants);
        return dev.createComputePipeline(pd);
    };
    m_first = pipeline(kind == Upscaler::Fsr1 ? "csEasu" : "csBilinear");
    if (kind == Upscaler::Fsr1) m_sharpen = pipeline("csRcas");
    rhi::SamplerDesc sd;
    sd.addressU = sd.addressV = sd.addressW = rhi::AddressMode::Clamp;
    sd.mipFilter = rhi::Filter::Nearest;
    m_sampler = dev.createSampler(sd);
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = outWidth;
    td.height = outHeight;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc;
    td.name = "upscaled";
    m_output = dev.createTexture(td);
    if (kind == Upscaler::Fsr1) {
        td.name = "fsr-easu";
        m_intermediate = dev.createTexture(td);
    }
}

void UpscalePass::record(rhi::CommandList& cmd, rhi::Texture* input) {
    const std::uint32_t gx = (m_outWidth + 7) / 8, gy = (m_outHeight + 7) / 8;
    rhi::Texture* first = m_kind == Upscaler::Fsr1 ? m_intermediate.get() : m_output.get();
    cmd.transition(input, rhi::ResourceState::ShaderRead);
    cmd.transition(first, rhi::ResourceState::UnorderedAccess);
    cmd.setPipeline(m_first.get());
    cmd.setTexture(0, input);
    cmd.setSampler(0, m_sampler.get());
    cmd.setStorageTexture(0, first);
    cmd.pushConstants(&m_easu, sizeof(m_easu));
    cmd.dispatch(gx, gy);
    cmd.transition(first, rhi::ResourceState::ShaderRead);
    if (m_kind == Upscaler::Fsr1) {
        cmd.transition(m_output.get(), rhi::ResourceState::UnorderedAccess);
        cmd.setPipeline(m_sharpen.get());
        cmd.setTexture(0, first);
        cmd.setSampler(0, m_sampler.get());
        cmd.setStorageTexture(0, m_output.get());
        cmd.pushConstants(&m_rcas, sizeof(m_rcas));
        cmd.dispatch(gx, gy);
        cmd.transition(m_output.get(), rhi::ResourceState::ShaderRead);
    }
}

} // namespace gt::bench
