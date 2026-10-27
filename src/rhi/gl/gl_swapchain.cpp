#include "gl_device.h"

#include "gt/log.h"

#include <SDL3/SDL.h>

#include <format>

namespace gt::rhi::gl {

// The window's default framebuffer cannot be a render target texture, so frames render into
// textures of our own and present() blits the current one, flipped: memory row 0 is the top
// of the image everywhere else, but the bottom of a GL window.
GLSwapchain::GLSwapchain(GLDevice& dev, const SwapchainDesc& desc)
    : m_dev(dev), m_window(static_cast<SDL_Window*>(desc.window)), m_desc(desc) {
    if (!m_window) throw Error("OpenGL swapchain without a window");
    m_dev.makeCurrent(m_window);
    if (!desc.vsync) {
        SDL_GL_SetSwapInterval(0);
    } else if (SDL_GL_SetSwapInterval(-1)) {
        m_adaptive = true; // adaptive vsync where available
    } else {
        SDL_GL_SetSwapInterval(1);
    }
    m_dev.makeCurrent(nullptr);
    createImages();
}

GLSwapchain::~GLSwapchain() {
    try {
        m_dev.waitIdle();
    } catch (const std::exception& e) {
        log::warn("swapchain shutdown: {}", e.what());
    }
}

void GLSwapchain::createImages() {
    m_images.clear();
    for (std::uint32_t i = 0; i < 2; ++i) {
        TextureDesc td;
        td.format = Format::RGBA8_UNORM;
        td.width = m_desc.width;
        td.height = m_desc.height;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopyDst | TextureUsage::CopySrc |
                   TextureUsage::Sampled;
        td.name = "swapchain";
        m_images.push_back(std::make_unique<GLTexture>(m_dev, td));
    }
}

Texture* GLSwapchain::acquire() {
    m_current = (m_current + 1) % static_cast<std::uint32_t>(m_images.size());
    return m_images[m_current].get();
}

void GLSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_dev.waitIdle();
    m_desc.width = width;
    m_desc.height = height;
    createImages();
}

std::string GLSwapchain::presentModeName() const {
    if (!m_desc.vsync) return "immediate (swap interval 0)";
    return m_adaptive ? "adaptive vsync (swap interval -1)" : "fifo (swap interval 1)";
}

void GLSwapchain::present() {
    m_dev.makeCurrent(m_window);
    GLTexture* image = m_images[m_current].get();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    const GLuint fbo = m_dev.readFramebuffer(image, 0, 0);
    gl().glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl().glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl().glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl().glDisable(GL_SCISSOR_TEST);
    gl().glDisable(GL_FRAMEBUFFER_SRGB);
    gl().glBlitFramebuffer(0, 0, static_cast<GLint>(m_desc.width), static_cast<GLint>(m_desc.height), 0, h, w,
                           0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl().glEnable(GL_FRAMEBUFFER_SRGB);
    if (!SDL_GL_SwapWindow(m_window)) log::warn("SDL_GL_SwapWindow: {}", SDL_GetError());
    m_dev.makeCurrent(nullptr);
    m_dev.executor().pipelineDirty = true;
}

} // namespace gt::rhi::gl
