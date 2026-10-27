#include "d3d11_device.h"

#include "gt/log.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace gt::rhi::d3d11 {

D3D11Swapchain::D3D11Swapchain(D3D11Device& dev, const SwapchainDesc& desc) : m_dev(dev), m_desc(desc) {
    auto* window = static_cast<SDL_Window*>(desc.window);
    auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) throw Error("the window has no Win32 handle");
    m_format = desc.hdr ? Format::RGB10A2_UNORM : Format::BGRA8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = desc.width;
    sd.Height = desc.height;
    sd.Format = toDxgi(m_format);
    sd.SampleDesc = {1, 0};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    // Flip-discard (Windows 10), then flip-sequential (Windows 8.1), then the legacy blt model.
    const DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                                        DXGI_SWAP_EFFECT_DISCARD};
    HRESULT hr = E_FAIL;
    for (DXGI_SWAP_EFFECT effect : effects) {
        const bool flip = effect != DXGI_SWAP_EFFECT_DISCARD;
        m_tearing = flip && !desc.vsync && dev.tearingSupported();
        sd.SwapEffect = effect;
        sd.BufferCount = flip ? std::clamp(desc.imageCount, 2u, 16u) : 1u;
        sd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        hr = dev.dxgiFactory()->CreateSwapChainForHwnd(dev.d3d(), hwnd, &sd, nullptr, nullptr, m_swapchain.put());
        if (SUCCEEDED(hr)) {
            m_effect = effect;
            break;
        }
    }
    dev.check(hr, "CreateSwapChainForHwnd");
    m_flags = sd.Flags;
    dev.dxgiFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (desc.hdr) {
        auto sc3 = m_swapchain.as<IDXGISwapChain3>();
        UINT support = 0;
        const DXGI_COLOR_SPACE_TYPE hdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if (sc3 && SUCCEEDED(sc3->CheckColorSpaceSupport(hdr10, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(sc3->SetColorSpace1(hdr10)))
            m_hdr = true;
        else
            log::info("HDR10 output is not available on this display; presenting 10-bit SDR");
    }
    wrap();
}

D3D11Swapchain::~D3D11Swapchain() {
    try {
        m_dev.waitIdle();
        m_image.reset();
    } catch (const std::exception& e) {
        log::warn("swapchain shutdown: {}", e.what());
    }
}

void D3D11Swapchain::wrap() {
    // With every swap effect Direct3D 11 renders to buffer 0; the runtime rotates the flip chain.
    Com<ID3D11Texture2D> image;
    m_dev.check(m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), image.putVoid()), "IDXGISwapChain::GetBuffer");
    DXGI_SWAP_CHAIN_DESC1 sd{};
    m_swapchain->GetDesc1(&sd);
    TextureDesc td;
    td.format = m_format;
    td.width = sd.Width;
    td.height = sd.Height;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopyDst;
    td.name = "swapchain";
    m_dev.setName(image.get(), "swapchain");
    m_image = std::make_unique<D3D11Texture>(m_dev, td, std::move(image));
}

void D3D11Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_dev.waitIdle();
    m_image.reset();
    // The back buffer must have no references left: views bound to the context count too.
    m_dev.context()->OMSetRenderTargets(0, nullptr, nullptr);
    m_dev.context()->ClearState();
    m_dev.context()->Flush();
    m_dev.check(m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_flags),
                "IDXGISwapChain::ResizeBuffers");
    m_desc.width = width;
    m_desc.height = height;
    wrap();
}

std::string D3D11Swapchain::presentModeName() const {
    const char* model = m_effect == DXGI_SWAP_EFFECT_FLIP_DISCARD      ? "flip-discard"
                        : m_effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "flip-sequential"
                                                                       : "blt";
    if (m_desc.vsync) return std::string("fifo (vsync, ") + model + ")";
    return m_tearing ? std::string("immediate (tearing, ") + model + ")"
                     : std::string("no vsync (") + model + ", no tearing support)";
}

void D3D11Swapchain::present() {
    const HRESULT hr = m_swapchain->Present(m_desc.vsync ? 1 : 0, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    m_dev.check(hr, "IDXGISwapChain::Present");
}

DisplayInfo D3D11Swapchain::displayInfo() const {
    DisplayInfo info = Swapchain::displayInfo();
    Com<IDXGIOutput> output;
    if (FAILED(m_swapchain->GetContainingOutput(output.put()))) return info;
    auto o6 = output.as<IDXGIOutput6>();
    DXGI_OUTPUT_DESC1 d{};
    if (!o6 || FAILED(o6->GetDesc1(&d))) return info;
    info.known = true;
    info.bitsPerColor = d.BitsPerColor;
    info.minNits = d.MinLuminance;
    info.maxNits = d.MaxLuminance;
    info.maxFullFrameNits = d.MaxFullFrameLuminance;
    // The output's colour space says whether HDR is on in the Windows display settings.
    info.colorSpace = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "HDR10 (ST 2084, BT.2020)"
                      : d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709  ? "scRGB (linear, BT.709)"
                                                                                  : "SDR (sRGB)";
    return info;
}

} // namespace gt::rhi::d3d11
