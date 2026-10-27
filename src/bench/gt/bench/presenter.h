#pragma once
// Shows the fixed-resolution output of a test in the benchmark window
// (letterboxed, independent of window size) with an optional HUD overlay.

#include "gt/rhi/rhi.h"

#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace gt::bench {

class Presenter {
public:
    Presenter(rhi::Device& device, SDL_Window* window, bool vsync, bool hud);
    ~Presenter();

    /// Acquires a backbuffer and records blit + HUD into `cmd`. Returns false when
    /// nothing can be presented (minimized / out of date); the frame still counts.
    bool record(rhi::CommandList& cmd, rhi::Texture* image, const std::vector<std::string>& hud);
    /// Presents the backbuffer recorded by the last successful record() (after submit).
    void present();
    rhi::Swapchain* swapchain() { return m_swapchain.get(); }
    /// Processes window events. Returns false once the user closed the window.
    bool pumpEvents();
    bool hudEnabled() const { return m_hud; }
    std::string presentMode() const { return m_swapchain ? m_swapchain->presentModeName() : "none"; }

private:
    void drawHud(rhi::CommandList& cmd, std::uint32_t width, std::uint32_t height, const std::vector<std::string>& lines);
    void createSamplerAndFont();
    void createHud(const rhi::GraphicsPipelineDesc& blit);
    rhi::Device& m_device;
    SDL_Window* m_window;
    bool m_hud;
    bool m_closed = false;
    bool m_resized = false;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<rhi::Pipeline> m_blit;
    std::unique_ptr<rhi::Pipeline> m_text;
    std::unique_ptr<rhi::Pipeline> m_rect;
    std::unique_ptr<rhi::Sampler> m_sampler;
    std::unique_ptr<rhi::Texture> m_font;
};

} // namespace gt::bench
