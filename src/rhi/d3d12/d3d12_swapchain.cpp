#include "d3d12_device.h"

#include "gt/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <format>

namespace gt::rhi::d3d12 {

D3D12Swapchain::D3D12Swapchain(D3D12Device& dev, const SwapchainDesc& desc) : m_dev(dev), m_desc(desc) {
    auto* window = static_cast<SDL_Window*>(desc.window);
    auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) throw Error("the window has no Win32 handle");
    m_tearing = !desc.vsync && dev.tearingSupported();
    m_format = desc.hdr ? Format::RGB10A2_UNORM : Format::BGRA8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = desc.width;
    sd.Height = desc.height;
    sd.Format = toDxgi(m_format);
    sd.SampleDesc = {1, 0};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = std::clamp(desc.imageCount, 2u, 16u);
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    Com<IDXGISwapChain1> sc1;
    dev.check(dev.dxgiFactory()->CreateSwapChainForHwnd(dev.queue(QueueType::Graphics), hwnd, &sd, nullptr, nullptr,
                                                        sc1.put()),
              "CreateSwapChainForHwnd");
    m_swapchain = sc1.as<IDXGISwapChain3>();
    if (!m_swapchain) throw Error("IDXGISwapChain3 is not available");
    dev.dxgiFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (desc.hdr) {
        UINT support = 0;
        const DXGI_COLOR_SPACE_TYPE hdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if (SUCCEEDED(m_swapchain->CheckColorSpaceSupport(hdr10, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(m_swapchain->SetColorSpace1(hdr10)))
            m_hdr = true;
        else
            log::info("HDR10 output is not available on this display; presenting 10-bit SDR");
    }
    wrapImages();
}

D3D12Swapchain::~D3D12Swapchain() {
    try {
        m_dev.waitIdle();
        m_images.clear();
        m_dev.waitIdle(); // releases the deferred back-buffer references
    } catch (const std::exception& e) {
        log::warn("swapchain shutdown: {}", e.what());
    }
}

void D3D12Swapchain::wrapImages() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    m_swapchain->GetDesc1(&sd);
    for (UINT i = 0; i < sd.BufferCount; ++i) {
        Com<ID3D12Resource> image;
        m_dev.check(m_swapchain->GetBuffer(i, __uuidof(ID3D12Resource), image.putVoid()), "IDXGISwapChain::GetBuffer");
        TextureDesc td;
        td.format = m_format;
        td.width = sd.Width;
        td.height = sd.Height;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopyDst;
        td.name = "swapchain";
        m_dev.setName(image.get(), "swapchain");
        m_images.push_back(std::make_unique<D3D12Texture>(m_dev, td, std::move(image)));
    }
}

Texture* D3D12Swapchain::acquire() { return m_images[m_swapchain->GetCurrentBackBufferIndex()].get(); }

void D3D12Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_dev.waitIdle();
    m_images.clear();
    m_dev.waitIdle(); // the back buffers must have no references left
    m_dev.check(m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                           m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                "IDXGISwapChain::ResizeBuffers");
    m_desc.width = width;
    m_desc.height = height;
    wrapImages();
}

std::string D3D12Swapchain::presentModeName() const {
    if (m_desc.vsync) return "fifo (vsync)";
    return m_tearing ? "immediate (tearing)" : "flip (no tearing support)";
}

void D3D12Swapchain::present() {
    const HRESULT hr = m_swapchain->Present(m_desc.vsync ? 1 : 0, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    m_dev.check(hr, "IDXGISwapChain::Present");
}

} // namespace gt::rhi::d3d12
