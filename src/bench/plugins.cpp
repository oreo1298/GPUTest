#include "gt/bench/plugins.h"

#include "dynlib.h"
#include "gt/log.h"
#include "gt/paths.h"

#include <algorithm>
#include <filesystem>
#include <format>

namespace gt::bench {
namespace {

gt_upscaler_device toPlugin(const rhi::NativeDevice& n) {
    gt_upscaler_device d{};
    d.api = n.api == rhi::Api::D3D12 ? std::uint32_t{GT_UPSCALER_API_D3D12} : std::uint32_t{GT_UPSCALER_API_VULKAN};
    d.instance = n.instance;
    d.physical_device = n.physicalDevice;
    d.device = n.device;
    d.queue = n.queue;
    d.queue_family = n.queueFamily;
    d.get_instance_proc_addr = n.getInstanceProcAddr;
    return d;
}

gt_upscaler_image image(rhi::Device& dev, rhi::Texture* t, rhi::ResourceState state) {
    const rhi::NativeTexture n = dev.nativeTexture(t, state);
    return {n.resource, n.view, n.format, n.layout, t->desc().width, t->desc().height};
}

bool isPluginFile(const std::filesystem::path& p) {
    std::string name = p.filename().string();
    if (name.starts_with("lib")) name = name.substr(3);
#ifdef _WIN32
    const char* ext = ".dll";
#else
    const char* ext = ".so";
#endif
    return name.starts_with("upscaler-") && p.extension() == ext;
}

} // namespace

const std::vector<UpscalerPlugin>& upscalerPlugins() {
    static const std::vector<UpscalerPlugin> plugins = [] {
        std::vector<UpscalerPlugin> out;
        static std::vector<std::unique_ptr<telemetry::DynLib>> libraries; // kept loaded for the process
        const std::filesystem::path dir = paths::executableDir() / "plugins";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return out;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file() && isPluginFile(e.path())) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            UpscalerPlugin p;
            p.path = f.string();
            auto lib = std::make_unique<telemetry::DynLib>();
            gt_upscaler_plugin_entry entry = nullptr;
            if (!lib->open(p.path)) {
                p.error = lib->error();
            } else if (!lib->load(entry, GT_UPSCALER_ENTRY)) {
                p.error = std::format("no {} entry point", GT_UPSCALER_ENTRY);
            } else if (const gt_upscaler_plugin* api = entry(); !api) {
                p.error = "the plugin returned no interface";
            } else if (api->abi_version != GT_UPSCALER_ABI_VERSION) {
                p.error = std::format("interface version {} (this build supports {})", api->abi_version,
                                      GT_UPSCALER_ABI_VERSION);
            } else if (!api->query || !api->create || !api->evaluate || !api->destroy || !api->id || !api->name) {
                p.error = "the plugin's interface is incomplete";
            } else {
                p.api = api;
            }
            if (p.api)
                log::info("upscaler plugin: {} ({}, {}) from {}", p.api->name, p.api->id,
                          p.api->version ? p.api->version : "?", p.path);
            else
                log::warn("upscaler plugin {} not usable: {}", p.path, p.error);
            libraries.push_back(std::move(lib));
            out.push_back(std::move(p));
        }
        return out;
    }();
    return plugins;
}

std::string upscalerPluginSupport(rhi::Device& device, const gt_upscaler_plugin& plugin) {
    const rhi::Api api = device.api();
    std::uint32_t bit = 0;
    if (api == rhi::Api::Vulkan) bit = GT_UPSCALER_API_VULKAN;
    if (api == rhi::Api::D3D12) bit = GT_UPSCALER_API_D3D12;
    if (!(plugin.apis & bit))
        return std::format("{} does not support {}", plugin.name, rhi::apiDisplayName(api));
    const gt_upscaler_device d = toPlugin(device.nativeDevice());
    char message[512] = {};
    if (plugin.query(&d, message, sizeof(message)) != GT_UPSCALER_OK)
        return message[0] ? std::string(message) : std::format("{} is not supported on this GPU", plugin.name);
    return {};
}

PluginUpscalePass::PluginUpscalePass(rhi::Device& dev, const gt_upscaler_plugin& plugin, std::uint32_t inWidth,
                                     std::uint32_t inHeight, std::uint32_t outWidth, std::uint32_t outHeight,
                                     UpscaleMode mode)
    : m_dev(dev), m_plugin(plugin) {
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = outWidth;
    td.height = outHeight;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc |
               rhi::TextureUsage::CopyDst;
    td.name = "plugin-upscaled";
    m_output = dev.createTexture(td);
    const gt_upscaler_device d = toPlugin(dev.nativeDevice());
    const rhi::NativeTexture out = dev.nativeTexture(m_output.get(), rhi::ResourceState::UnorderedAccess);
    gt_upscaler_create_info ci{};
    ci.max_render_width = inWidth;
    ci.max_render_height = inHeight;
    ci.output_width = outWidth;
    ci.output_height = outHeight;
    ci.quality = static_cast<std::uint32_t>(mode); // UpscaleMode follows GT_UPSCALER_QUALITY_*
    ci.color_format = out.format;
    ci.output_format = out.format;
    char message[512] = {};
    if (plugin.create(&d, &ci, &m_context, message, sizeof(message)) != GT_UPSCALER_OK || !m_context)
        throw rhi::Error(std::format("{}: {}", plugin.name, message[0] ? message : "could not be created"));
}

PluginUpscalePass::~PluginUpscalePass() {
    try {
        m_dev.waitIdle();
    } catch (const std::exception& e) {
        log::warn("upscaler plugin shutdown: {}", e.what());
    }
    if (m_context) m_plugin.destroy(m_context);
}

void PluginUpscalePass::record(rhi::CommandList& cmd, rhi::Texture* color, rhi::Texture* depth,
                               rhi::Texture* motion, float jitterX, float jitterY, bool reset) {
    using S = rhi::ResourceState;
    for (rhi::Texture* t : {color, depth, motion}) cmd.transition(t, S::ShaderRead);
    cmd.transition(m_output.get(), S::UnorderedAccess);
    gt_upscaler_frame f{};
    f.color = image(m_dev, color, S::ShaderRead);
    f.depth = image(m_dev, depth, S::ShaderRead);
    f.motion = image(m_dev, motion, S::ShaderRead);
    f.output = image(m_dev, m_output.get(), S::UnorderedAccess);
    f.jitter_x = jitterX;
    f.jitter_y = jitterY;
    f.frame_time_ms = 16.667f;
    f.reset = reset ? 1u : 0u;
    f.command_buffer = cmd.beginNativeCommands();
    char message[512] = {};
    const int r = m_plugin.evaluate(m_context, &f, message, sizeof(message));
    cmd.endNativeCommands();
    if (r != GT_UPSCALER_OK)
        throw rhi::Error(std::format("{}: {}", m_plugin.name, message[0] ? message : "evaluation failed"));
    cmd.transition(m_output.get(), S::ShaderRead);
}

} // namespace gt::bench
