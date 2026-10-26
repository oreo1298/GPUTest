#include "vk_device.h"

#include "gt/log.h"

#include <algorithm>

namespace gt::rhi::vk {

VulkanSwapchain::VulkanSwapchain(VulkanDevice& dev, const SwapchainDesc& desc) : m_dev(dev), m_desc(desc) {
    m_surface = dev.createSurface(desc.window);
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(dev.vkPhysicalDevice(), dev.queue(QueueType::Graphics).family, m_surface,
                                         &supported);
    if (!supported) {
        vkDestroySurfaceKHR(dev.vkInstance(), m_surface, nullptr);
        throw Error("the graphics queue cannot present to this window");
    }
    create(desc.width, desc.height);
}

VulkanSwapchain::~VulkanSwapchain() {
    m_dev.waitIdle();
    destroySwapchain();
    vkDestroySurfaceKHR(m_dev.vkInstance(), m_surface, nullptr);
}

void VulkanSwapchain::destroySwapchain() {
    VkDevice d = m_dev.vkDevice();
    m_images.clear();
    m_dev.waitIdle();
    for (VkSemaphore s : m_acquireSems) vkDestroySemaphore(d, s, nullptr);
    for (VkSemaphore s : m_presentSems) vkDestroySemaphore(d, s, nullptr);
    m_acquireSems.clear();
    m_presentSems.clear();
    m_acquireUse.clear();
    if (m_swapchain) vkDestroySwapchainKHR(d, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

std::string VulkanSwapchain::presentModeName() const {
    switch (m_presentMode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo (vsync)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "other";
    }
}

void VulkanSwapchain::create(std::uint32_t width, std::uint32_t height) {
    VkPhysicalDevice pd = m_dev.vkPhysicalDevice();
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, m_surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, m_surface, &n, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, m_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, m_surface, &n, modes.data());

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    m_hdr = false;
    bool found = false;
    if (m_desc.hdr) {
        for (const auto& f : formats) {
            if (f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
                chosen = f;
                m_hdr = found = true;
                break;
            }
        }
    }
    if (!found) {
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            for (const auto& f : formats) {
                if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    chosen = f;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }
    m_format = fromVk(chosen.format);
    if (chosen.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) m_format = Format::RGB10A2_UNORM;

    auto hasMode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_desc.vsync) {
        if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) m_presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR)) m_presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else log::warn("tearing-free present only: vsync cannot be disabled on this surface");
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    std::uint32_t imageCount = std::max(caps.minImageCount + 1, m_desc.imageCount);
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha &
                                                                       -caps.supportedCompositeAlpha);
    ci.presentMode = m_presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(m_dev.vkDevice(), &ci, nullptr, &m_swapchain), "vkCreateSwapchainKHR");

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_dev.vkDevice(), m_swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(m_dev.vkDevice(), m_swapchain, &count, images.data());
    TextureDesc td;
    td.format = m_format;
    td.width = extent.width;
    td.height = extent.height;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopyDst;
    td.name = "backbuffer";
    for (VkImage img : images) m_images.push_back(std::make_unique<VulkanTexture>(m_dev, td, img));

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_acquireSems.resize(count + 1);
    m_acquireUse.assign(count + 1, 0);
    for (auto& s : m_acquireSems) check(vkCreateSemaphore(m_dev.vkDevice(), &sci, nullptr, &s), "vkCreateSemaphore");
    m_presentSems.resize(count);
    for (auto& s : m_presentSems) check(vkCreateSemaphore(m_dev.vkDevice(), &sci, nullptr, &s), "vkCreateSemaphore");
    m_acquireSlot = 0;
    m_outOfDate = false;
    log::info("swapchain {}x{} {} images, format {}, present mode {}{}", extent.width, extent.height, count,
              formatInfo(m_format).name, presentModeName(), m_hdr ? ", HDR10" : "");
}

void VulkanSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    destroySwapchain();
    create(width, height);
}

Texture* VulkanSwapchain::acquire() {
    if (m_outOfDate) return nullptr;
    m_acquireSlot = (m_acquireSlot + 1) % static_cast<std::uint32_t>(m_acquireSems.size());
    if (m_acquireUse[m_acquireSlot]) m_dev.wait(QueueType::Graphics, m_acquireUse[m_acquireSlot]);
    VkResult r = vkAcquireNextImageKHR(m_dev.vkDevice(), m_swapchain, UINT64_MAX, m_acquireSems[m_acquireSlot],
                                       VK_NULL_HANDLE, &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_outOfDate = true;
        return nullptr;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throwResult(r, "vkAcquireNextImageKHR");
    return m_images[m_imageIndex].get();
}

} // namespace gt::rhi::vk
