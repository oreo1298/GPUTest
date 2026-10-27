#pragma once
// Spatial upscaling from a scene's render resolution to the output resolution: AMD FidelityFX
// Super Resolution 1 (EASU + RCAS, shaders/src/upscale/fsr1.slang) and a bilinear baseline.

#include "gt/rhi/rhi.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace gt::bench {

enum class Upscaler : std::uint8_t { Off, Bilinear, Fsr1 };
/// FSR 1 quality modes: output-to-render resolution ratio per axis.
enum class UpscaleMode : std::uint8_t { UltraQuality, Quality, Balanced, Performance };

std::string_view upscalerName(Upscaler u);     ///< "off", "bilinear", "fsr1"
std::string_view upscaleModeName(UpscaleMode m); ///< "ultra-quality", "quality", "balanced", "performance"
std::optional<Upscaler> parseUpscaler(std::string_view s);
std::optional<UpscaleMode> parseUpscaleMode(std::string_view s);
double upscaleRatio(UpscaleMode m); ///< 1.3, 1.5, 1.7, 2.0
/// Render size for an output size: output / ratio, rounded, at least 1.
std::uint32_t renderExtent(std::uint32_t output, double ratio);

namespace fsr1 {
/// Push constants of the FSR shaders (FsrParams in fsr1.slang).
struct Constants {
    std::array<std::uint32_t, 16> con{};
    std::uint32_t outWidth = 0, outHeight = 0, pad0 = 0, pad1 = 0;
};
static_assert(sizeof(Constants) == 80);
/// FsrEasuCon: the whole input texture is the viewport.
Constants easu(std::uint32_t inWidth, std::uint32_t inHeight, std::uint32_t outWidth, std::uint32_t outHeight);
/// FsrRcasCon: `sharpnessStops` 0 is the sharpest; each stop halves the sharpening.
Constants rcas(float sharpnessStops, std::uint32_t width, std::uint32_t height);
} // namespace fsr1

/// The GPU passes of one upscaler for one input and output size.
class UpscalePass {
public:
    UpscalePass(rhi::Device& dev, Upscaler kind, std::uint32_t inWidth, std::uint32_t inHeight,
                std::uint32_t outWidth, std::uint32_t outHeight, float sharpnessStops = 0.2f);
    /// Upscales `input` (sampled, inWidth x inHeight) into output(), left in ResourceState::ShaderRead.
    /// Works on graphics and compute command lists, outside render passes.
    void record(rhi::CommandList& cmd, rhi::Texture* input);
    rhi::Texture* output() const { return m_output.get(); }
    rhi::Texture* intermediate() const { return m_intermediate.get(); } ///< FSR 1: the EASU result (before RCAS)
    Upscaler kind() const { return m_kind; }

private:
    rhi::Device& m_dev;
    Upscaler m_kind;
    std::uint32_t m_outWidth, m_outHeight;
    fsr1::Constants m_easu, m_rcas;
    std::unique_ptr<rhi::Pipeline> m_first, m_sharpen;
    std::unique_ptr<rhi::Sampler> m_sampler;
    std::unique_ptr<rhi::Texture> m_intermediate, m_output;
};

} // namespace gt::bench
