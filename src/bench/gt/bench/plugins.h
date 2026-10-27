#pragma once
// Host side of the upscaler plugin interface (gt/bench/upscaler_plugin.h): finds and loads plugins from the
// "plugins" folder next to the executable, and drives one plugin for one input/output size.

#include "gt/bench/upscale.h"
#include "gt/bench/upscaler_plugin.h"
#include "gt/rhi/rhi.h"

#include <memory>
#include <string>
#include <vector>

namespace gt::bench {

struct UpscalerPlugin {
    std::string path;
    const gt_upscaler_plugin* api = nullptr; ///< null when the library could not be used (see error)
    std::string error;
};

/// Loads every plugin once (libraries stay loaded for the process). Only call this when the user enabled
/// plugins.
const std::vector<UpscalerPlugin>& upscalerPlugins();

/// Empty if `plugin` can run on `device`; otherwise the reason.
std::string upscalerPluginSupport(rhi::Device& device, const gt_upscaler_plugin& plugin);

/// One plugin instance for one size. Textures passed to record() must be in ResourceState::ShaderRead.
class PluginUpscalePass {
public:
    PluginUpscalePass(rhi::Device& dev, const gt_upscaler_plugin& plugin, std::uint32_t inWidth,
                      std::uint32_t inHeight, std::uint32_t outWidth, std::uint32_t outHeight, UpscaleMode mode);
    ~PluginUpscalePass();
    PluginUpscalePass(const PluginUpscalePass&) = delete;
    PluginUpscalePass& operator=(const PluginUpscalePass&) = delete;
    /// `color` RGBA8, `depth` R32_FLOAT, `motion` RG16_FLOAT (render pixels to the previous frame); output()
    /// ends in ResourceState::ShaderRead.
    void record(rhi::CommandList& cmd, rhi::Texture* color, rhi::Texture* depth, rhi::Texture* motion,
                float jitterX, float jitterY, bool reset);
    rhi::Texture* output() const { return m_output.get(); }

private:
    rhi::Device& m_dev;
    const gt_upscaler_plugin& m_plugin;
    void* m_context = nullptr;
    std::unique_ptr<rhi::Texture> m_output;
};

} // namespace gt::bench
