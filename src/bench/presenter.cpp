#include "gt/bench/presenter.h"

#include "gt/log.h"
#include "hud_font.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace gt::bench {

Presenter::Presenter(rhi::Device& device, SDL_Window* window, bool vsync, bool hud)
    : m_device(device), m_window(window), m_hud(hud) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    rhi::SwapchainDesc sd;
    sd.window = window;
    sd.width = static_cast<std::uint32_t>(std::max(w, 1));
    sd.height = static_cast<std::uint32_t>(std::max(h, 1));
    sd.vsync = vsync;
    m_swapchain = device.createSwapchain(sd);
    const rhi::Format fmt = m_swapchain->format();

    rhi::GraphicsPipelineDesc blit;
    blit.name = "present-blit";
    blit.vertex = {"common/blit", "vsFullscreen"};
    blit.fragment = {"common/blit", "psBlit"};
    blit.raster.cull = rhi::CullMode::None;
    blit.depth.test = blit.depth.write = false;
    blit.colorFormats = {fmt};
    blit.layout.bindings = {{rhi::BindingType::SampledTexture, 0, rhi::ShaderStage::Fragment},
                            {rhi::BindingType::Sampler, 0, rhi::ShaderStage::Fragment}};
    blit.layout.pushConstantBytes = 32;
    m_blit = device.createGraphicsPipeline(blit);
    createSamplerAndFont();

    // The HUD reads glyphs from a storage buffer; without one (OpenGL 3.3) frames show without it.
    try {
        createHud(blit);
    } catch (const std::exception& e) {
        log::info("HUD unavailable on this API/GPU: {}", e.what());
        m_text.reset();
        m_rect.reset();
        m_hud = false;
    }
}

void Presenter::createHud(const rhi::GraphicsPipelineDesc& blit) {
    rhi::Device& device = m_device;
    rhi::GraphicsPipelineDesc text = blit;
    text.name = "hud-text";
    text.vertex = {"common/hud", "vsText"};
    text.fragment = {"common/hud", "psText"};
    text.topology = rhi::PrimitiveTopology::TriangleStrip;
    text.blend = {rhi::BlendAttachment::alpha()};
    text.layout.bindings = {{rhi::BindingType::StorageBufferRO, 0, rhi::ShaderStage::Vertex},
                            {rhi::BindingType::SampledTexture, 1, rhi::ShaderStage::Fragment}};
    text.layout.pushConstantBytes = 16;
    m_text = device.createGraphicsPipeline(text);

    rhi::GraphicsPipelineDesc rect = text;
    rect.name = "hud-rect";
    rect.vertex = {"common/hud", "vsRect"};
    rect.fragment = {"common/hud", "psRect"};
    rect.layout.bindings = {{rhi::BindingType::StorageBufferRO, 2, rhi::ShaderStage::Vertex}};
    m_rect = device.createGraphicsPipeline(rect);
}

void Presenter::createSamplerAndFont() {
    rhi::Device& device = m_device;
    rhi::SamplerDesc s;
    s.addressU = s.addressV = s.addressW = rhi::AddressMode::Clamp;
    m_sampler = device.createSampler(s);

    // 16x6 glyph atlas, 8x8 pixels per glyph.
    rhi::TextureDesc fd;
    fd.format = rhi::Format::R8_UNORM;
    fd.width = 128;
    fd.height = 48;
    fd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    fd.name = "hud-font";
    m_font = device.createTexture(fd);
    std::vector<std::uint8_t> pixels(128 * 48, 0);
    for (int g = 0; g < 96; ++g) {
        const int cx = (g % 16) * 8, cy = (g / 16) * 8;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if (kHudFont8x8[g][y] & (1u << x)) pixels[static_cast<std::size_t>((cy + y) * 128 + cx + x)] = 255;
    }
    device.uploadTexture(m_font.get(), 0, 0, pixels.data(), 128);
}

Presenter::~Presenter() {
    try {
        m_device.waitIdle();
    } catch (...) {
    }
}

bool Presenter::pumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: m_closed = true; break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) m_closed = true;
            if ((e.key.key == SDLK_F1 || e.key.key == SDLK_H) && m_text) m_hud = !m_hud;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: m_resized = true; break;
        default: break;
        }
    }
    return !m_closed;
}

bool Presenter::record(rhi::CommandList& cmd, rhi::Texture* image, const std::vector<std::string>& hud) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED)) return false;
    if (m_resized) {
        m_swapchain->resize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        m_resized = false;
    }
    rhi::Texture* bb = m_swapchain->acquire();
    if (!bb) {
        m_swapchain->resize(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        bb = m_swapchain->acquire();
        if (!bb) return false;
    }
    const auto bw = static_cast<float>(bb->desc().width);
    const auto bh = static_cast<float>(bb->desc().height);
    if (image) cmd.transition(image, rhi::ResourceState::ShaderRead);
    rhi::RenderPassDesc rp;
    rp.colors.push_back({bb, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.02f, 0.02f, 0.025f, 1.0f}});
    cmd.beginRenderPass(rp);
    if (image) {
        // Letterbox: keep the internal render resolution's aspect ratio.
        const float iw = static_cast<float>(image->desc().width);
        const float ih = static_cast<float>(image->desc().height);
        const float scale = std::min(bw / iw, bh / ih);
        const float vw = iw * scale, vh = ih * scale;
        cmd.setViewport((bw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh);
        cmd.setPipeline(m_blit.get());
        cmd.setTexture(0, image);
        cmd.setSampler(0, m_sampler.get());
        const float pc[8] = {1, 1, 0, 0, 1, 1, 1, 1};
        cmd.pushConstants(pc, sizeof(pc));
        cmd.draw(3);
    }
    if (m_hud && !hud.empty()) {
        cmd.setViewport(0, 0, bw, bh);
        drawHud(cmd, bb->desc().width, bb->desc().height, hud);
    }
    cmd.endRenderPass();
    cmd.transition(bb, rhi::ResourceState::Present);
    return true;
}

void Presenter::present() { m_device.present(m_swapchain.get()); }

void Presenter::drawHud(rhi::CommandList& cmd, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::string>& lines) {
    struct Glyph {
        float x, y;
        std::uint32_t ch, color;
    };
    struct Rect {
        float x, y, w, h;
        std::uint32_t color, pad[3];
    };
    const float scale = height >= 1400 ? 3.0f : height >= 900 ? 2.0f : 1.0f;
    const float glyph = 8.0f * scale;
    const float pad = 6.0f * scale;
    std::size_t maxLen = 0;
    for (const auto& l : lines) maxLen = std::max(maxLen, l.size());
    std::vector<Glyph> glyphs;
    float y = pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        float x = pad;
        const std::uint32_t color = i == 0 ? 0xFF4FC3FFu : 0xFFF0F0F0u; // ABGR: first line amber
        for (char c : lines[i]) {
            if (c != ' ') glyphs.push_back({x, y, static_cast<std::uint32_t>(static_cast<unsigned char>(c)), color});
            x += glyph;
        }
        y += glyph + 2.0f * scale;
    }
    if (glyphs.empty()) return;
    Rect bg{0, 0, pad * 2 + glyph * static_cast<float>(maxLen), y + pad - 2.0f * scale, 0xB0000000u, {0, 0, 0}};
    const float pc[4] = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), scale, 0.0f};

    rhi::TransientAllocation ra = cmd.allocTransient(sizeof(Rect), 16);
    std::memcpy(ra.cpu, &bg, sizeof(Rect));
    cmd.setPipeline(m_rect.get());
    cmd.setReadOnlyBuffer(2, ra.buffer, ra.offset, sizeof(Rect));
    cmd.pushConstants(pc, sizeof(pc));
    cmd.draw(4, 1);

    const std::uint64_t bytes = glyphs.size() * sizeof(Glyph);
    rhi::TransientAllocation ga = cmd.allocTransient(bytes, 16);
    std::memcpy(ga.cpu, glyphs.data(), bytes);
    cmd.setPipeline(m_text.get());
    cmd.setReadOnlyBuffer(0, ga.buffer, ga.offset, bytes);
    cmd.setTexture(1, m_font.get());
    cmd.pushConstants(pc, sizeof(pc));
    cmd.draw(4, static_cast<std::uint32_t>(glyphs.size()));
}

} // namespace gt::bench
