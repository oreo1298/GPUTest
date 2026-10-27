// Video engines (feature.video): throughput of the GPU's fixed-function video encoder and decoder.
//
// Vulkan Video, H.264: eight synthetic 1080p frames (NV12, a panning pattern of gradients and hard-edged
// blocks) are encoded as one IDR + seven P-frames at a constant QP on the video encode queue. The stream the
// encoder wrote is decoded on the video decode queue with the parameter sets and slice headers the encoder
// actually produced (read back and parsed, gt/bench/h264_params.h: encoders may override what they were
// asked for), and the decoded luma is compared with the source (PSNR). Throughput is the wall time of
// back-to-back encodes and decodes of that group of pictures. Every step is gated on what the driver
// reports (queues, extensions, profiles, formats, capabilities), so the test skips with a reason wherever
// one of them is missing (lavapipe, WARP, older GPUs), and the codecs the video queues offer are listed.

#include "gt/bench/context.h"
#include "gt/bench/h264_params.h"
#include "gt/bench/test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#if GT_HAS_VULKAN
#include <volk.h>
#endif

namespace gt::bench {
namespace {

#if GT_HAS_VULKAN

constexpr std::uint32_t kWidth = 1920, kHeight = 1080;           ///< shown picture
constexpr std::uint32_t kCodedWidth = 1920, kCodedHeight = 1088; ///< whole 16x16 macroblocks
constexpr std::uint32_t kGop = 8;                                ///< frames per group of pictures
constexpr std::size_t kLumaBytes = std::size_t(kCodedWidth) * kCodedHeight;
constexpr std::size_t kFrameBytes = kLumaBytes * 3 / 2; ///< NV12

/// The source frames, NV12 at the coded size (the rows below 1080 repeat the last visible row).
std::vector<std::uint8_t> makeSource() {
    std::vector<std::uint8_t> nv12(kFrameBytes * kGop);
    auto clamp8 = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 16l, 235l)); };
    for (std::uint32_t f = 0; f < kGop; ++f) {
        std::uint8_t* y = nv12.data() + f * kFrameBytes;
        std::uint8_t* uv = y + kLumaBytes;
        const double dx = 6.0 * f, dy = 3.0 * f; // the pattern pans by (6, 3) pixels per frame
        for (std::uint32_t py = 0; py < kCodedHeight; ++py) {
            const double v = std::min(py, kHeight - 1) + dy;
            for (std::uint32_t px = 0; px < kCodedWidth; ++px) {
                const double x = px + dx;
                const int bx = static_cast<int>(x) >> 6, by = static_cast<int>(v) >> 6;
                double g = 120.0 + 45.0 * std::sin(x * 0.011) * std::cos(v * 0.017);
                g += ((bx + by) & 1) ? 30.0 : -30.0;
                if ((bx * 7 + by * 3) % 5 == 0)
                    g += ((static_cast<int>(x) >> 2) & 1) ? 18.0 : -18.0; // fine stripes
                y[std::size_t(py) * kCodedWidth + px] = clamp8(g);
            }
        }
        for (std::uint32_t py = 0; py < kCodedHeight / 2; ++py) {
            const double v = std::min(2 * py, kHeight - 2) + dy;
            for (std::uint32_t px = 0; px < kCodedWidth / 2; ++px) {
                const double x = 2.0 * px + dx;
                std::uint8_t* c = uv + std::size_t(py) * kCodedWidth + 2 * px;
                c[0] = clamp8(128.0 + 40.0 * std::sin(x * 0.006 + v * 0.002));
                c[1] = clamp8(128.0 + 40.0 * std::cos(v * 0.008 - x * 0.001));
            }
        }
    }
    return nv12;
}

/// Luma PSNR over the shown 1920x1080 area (both planes have the coded row pitch).
double lumaPsnr(const std::uint8_t* a, const std::uint8_t* b) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const double d =
                double(a[std::size_t(y) * kCodedWidth + x]) - double(b[std::size_t(y) * kCodedWidth + x]);
            sum += d * d;
        }
    const double mse = sum / (double(kWidth) * kHeight);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::string resultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR:
        return "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
    case VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR:
        return "VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR";
    default:
        return std::format("VkResult {}", static_cast<int>(r));
    }
}

void check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    const std::string msg = std::format("{} failed: {}", what, resultName(r));
    if (r == VK_ERROR_DEVICE_LOST) throw rhi::DeviceLostError(msg);
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY) throw rhi::OutOfMemoryError(msg);
    throw rhi::Error(msg);
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return a > 1 ? (v + a - 1) / a * a : v;
}

bool hasExtension(const rhi::NativeDevice& n, std::string_view name) {
    return std::find(n.extensions.begin(), n.extensions.end(), name) != n.extensions.end();
}

/// Codec operations each queue family offers, for the capability listing.
std::string codecList(VkVideoCodecOperationFlagsKHR ops) {
    std::vector<std::string> names;
    auto add = [&](VkVideoCodecOperationFlagsKHR bits, const char* name) {
        if (ops & bits) names.emplace_back(name);
    };
    add(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H.265");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, "AV1");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9");
    std::string s;
    for (const auto& n : names)
        s += (s.empty() ? "" : ", ") + n;
    return s.empty() ? "none" : s;
}

VkVideoCodecOperationFlagsKHR familyCodecs(VkPhysicalDevice pd, std::uint32_t family) {
    if (family == ~0u) return 0;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, nullptr);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(count,
                                                       {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
    std::vector<VkQueueFamilyProperties2> props(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    for (std::uint32_t i = 0; i < count; ++i)
        props[i].pNext = &video[i];
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, props.data());
    return family < count ? video[family].videoCodecOperations : 0;
}

bool familyHasStatusQueries(VkPhysicalDevice pd, std::uint32_t family) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, nullptr);
    std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> status(
        count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR});
    std::vector<VkQueueFamilyProperties2> props(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    for (std::uint32_t i = 0; i < count; ++i)
        props[i].pNext = &status[i];
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, props.data());
    return family < count && status[family].queryResultStatusSupport == VK_TRUE;
}

/// Picks a format the profile list supports for `usage`: NV12 if offered (needed to upload or read back),
/// optimal tiling first. `wantUsage` are extra usages (transfers) preferred in the returned properties.
std::optional<VkVideoFormatPropertiesKHR> pickFormat(VkPhysicalDevice pd,
                                                     const VkVideoProfileListInfoKHR& profiles,
                                                     VkImageUsageFlags usage, VkImageUsageFlags wantUsage,
                                                     bool needNv12) {
    VkPhysicalDeviceVideoFormatInfoKHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    info.pNext = &profiles;
    info.imageUsage = usage;
    std::uint32_t count = 0;
    if (vkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &info, &count, nullptr) != VK_SUCCESS || count == 0)
        return std::nullopt;
    std::vector<VkVideoFormatPropertiesKHR> props(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    if (vkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &info, &count, props.data()) < VK_SUCCESS)
        return std::nullopt;
    props.resize(count);
    auto score = [&](const VkVideoFormatPropertiesKHR& p) {
        int s = 0;
        if (p.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) s += 4;
        if ((p.imageUsageFlags & wantUsage) == wantUsage) s += 2;
        if (p.imageTiling == VK_IMAGE_TILING_OPTIMAL) s += 1;
        return s;
    };
    auto best = std::max_element(props.begin(), props.end(),
                                 [&](const auto& a, const auto& b) { return score(a) < score(b); });
    if (needNv12 && best->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) return std::nullopt;
    return *best;
}

/// Device memory, images, buffers, command buffers and cross-queue ordering for native video work. All
/// submissions are chained through one timeline semaphore, so each waits for the one before it whatever
/// queue it runs on.
class VideoRig {
public:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t layers = 0;
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    explicit VideoRig(const rhi::NativeDevice& n)
        : m_dev(static_cast<VkDevice>(n.device)), m_pd(static_cast<VkPhysicalDevice>(n.physicalDevice)) {
        vkGetPhysicalDeviceMemoryProperties(m_pd, &m_memory);
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &type;
        check(vkCreateSemaphore(m_dev, &ci, nullptr, &m_timeline), "vkCreateSemaphore");
    }
    ~VideoRig() {
        // Never free what the GPU may still use: after a timeout the objects are leaked instead.
        try {
            if (!waitTimeline(20'000'000'000ull)) return;
        } catch (const std::exception&) {
            return;
        }
        for (auto it = m_cleanup.rbegin(); it != m_cleanup.rend(); ++it)
            (*it)();
        for (auto& [family, pool] : m_pools)
            vkDestroyCommandPool(m_dev, pool, nullptr);
        vkDestroySemaphore(m_dev, m_timeline, nullptr);
    }
    VideoRig(const VideoRig&) = delete;
    VideoRig& operator=(const VideoRig&) = delete;

    VkDevice device() const { return m_dev; }
    VkPhysicalDevice physicalDevice() const { return m_pd; }
    void onCleanup(std::function<void()> f) { m_cleanup.push_back(std::move(f)); }

    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags need,
                            VkMemoryPropertyFlags want, const void* pNext, const char* what) {
        std::uint32_t type = ~0u;
        for (std::uint32_t i = 0; i < m_memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = m_memory.memoryTypes[i].propertyFlags;
            if (!(req.memoryTypeBits & (1u << i)) || (f & need) != need) continue;
            if ((f & want) == want) {
                type = i;
                break;
            }
            if (type == ~0u) type = i;
        }
        if (type == ~0u) throw rhi::Error(std::format("no memory type fits the {}", what));
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = pNext;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        check(vkAllocateMemory(m_dev, &ai, nullptr, &mem), "vkAllocateMemory");
        onCleanup([this, mem] { vkFreeMemory(m_dev, mem, nullptr); });
        return mem;
    }

    /// A video picture image at the coded size; `families` > 1 makes it shared (concurrent).
    Image image(const VkVideoFormatPropertiesKHR& fmt, VkImageUsageFlags usage, VkImageUsageFlags viewUsage,
                std::uint32_t layers, const VkVideoProfileListInfoKHR& profiles,
                std::vector<std::uint32_t> families) {
        std::sort(families.begin(), families.end());
        families.erase(std::unique(families.begin(), families.end()), families.end());
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &profiles;
        ci.imageType = fmt.imageType;
        ci.format = fmt.format;
        ci.extent = {kCodedWidth, kCodedHeight, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = fmt.imageTiling;
        ci.usage = usage;
        ci.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount = families.size() > 1 ? static_cast<std::uint32_t>(families.size()) : 0;
        ci.pQueueFamilyIndices = families.size() > 1 ? families.data() : nullptr;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Image img;
        img.layers = layers;
        check(vkCreateImage(m_dev, &ci, nullptr, &img.image), "vkCreateImage (video picture)");
        const VkImage handle = img.image;
        onCleanup([this, handle] { vkDestroyImage(m_dev, handle, nullptr); });
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_dev, img.image, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = img.image;
        const VkDeviceMemory mem =
            allocate(req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dedicated, "video picture image");
        check(vkBindImageMemory(m_dev, img.image, mem, 0), "vkBindImageMemory");

        VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        vu.usage = viewUsage;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.pNext = &vu;
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        check(vkCreateImageView(m_dev, &vi, nullptr, &img.view), "vkCreateImageView (video picture)");
        const VkImageView view = img.view;
        onCleanup([this, view] { vkDestroyImageView(m_dev, view, nullptr); });
        return img;
    }

    /// A buffer; `profiles` is required for bitstream usages. Host-visible buffers stay mapped.
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                  const VkVideoProfileListInfoKHR* profiles, std::vector<std::uint32_t> families) {
        std::sort(families.begin(), families.end());
        families.erase(std::unique(families.begin(), families.end()), families.end());
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.pNext = profiles;
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount = families.size() > 1 ? static_cast<std::uint32_t>(families.size()) : 0;
        ci.pQueueFamilyIndices = families.size() > 1 ? families.data() : nullptr;
        Buffer b;
        b.size = size;
        check(vkCreateBuffer(m_dev, &ci, nullptr, &b.buffer), "vkCreateBuffer");
        const VkBuffer handle = b.buffer;
        onCleanup([this, handle] { vkDestroyBuffer(m_dev, handle, nullptr); });
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_dev, b.buffer, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = b.buffer;
        b.memory = hostVisible
                       ? allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                  &dedicated, "host-visible buffer")
                       : allocate(req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dedicated, "buffer");
        check(vkBindBufferMemory(m_dev, b.buffer, b.memory, 0), "vkBindBufferMemory");
        if (hostVisible) {
            void* p = nullptr;
            check(vkMapMemory(m_dev, b.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
            b.mapped = static_cast<std::uint8_t*>(p);
        }
        return b;
    }
    void flush(const Buffer& b) {
        const VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, b.memory, 0,
                                    VK_WHOLE_SIZE};
        check(vkFlushMappedMemoryRanges(m_dev, 1, &r), "vkFlushMappedMemoryRanges");
    }
    void invalidate(const Buffer& b) {
        const VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, b.memory, 0,
                                    VK_WHOLE_SIZE};
        check(vkInvalidateMappedMemoryRanges(m_dev, 1, &r), "vkInvalidateMappedMemoryRanges");
    }

    /// Starts recording a command buffer for `family` (reusable: it may be submitted again once finished).
    VkCommandBuffer begin(std::uint32_t family) {
        VkCommandPool& pool = m_pools[family];
        if (!pool) {
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.queueFamilyIndex = family;
            check(vkCreateCommandPool(m_dev, &ci, nullptr, &pool), "vkCreateCommandPool");
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(m_dev, &ai, &cb), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
        return cb;
    }
    void end(VkCommandBuffer cb) { check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); }

    /// Submits after everything submitted before (on any queue) has finished.
    void submit(VkQueue queue, VkCommandBuffer cb) {
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = m_timeline;
        wait.value = m_value;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo signal = wait;
        signal.value = ++m_value;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cb;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = m_value > 1 ? 1u : 0u;
        si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        check(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
    }
    /// Waits for the last submission; throws if the GPU takes longer than 20 s.
    void wait() {
        if (!waitTimeline(20'000'000'000ull)) throw rhi::Error("the video engine did not finish within 20 s");
    }

private:
    bool waitTimeline(std::uint64_t timeoutNs) {
        if (m_value == 0) return true;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_timeline;
        wi.pValues = &m_value;
        const VkResult r = vkWaitSemaphores(m_dev, &wi, timeoutNs);
        if (r == VK_TIMEOUT) return false;
        if (r != VK_SUCCESS) {
            // Device lost: nothing can be in use any more.
            if (r == VK_ERROR_DEVICE_LOST) return true;
            check(r, "vkWaitSemaphores");
        }
        return true;
    }

    VkDevice m_dev;
    VkPhysicalDevice m_pd;
    VkPhysicalDeviceMemoryProperties m_memory{};
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    std::uint64_t m_value = 0;
    std::map<std::uint32_t, VkCommandPool> m_pools;
    std::vector<std::function<void()>> m_cleanup;
};

/// Records a sync2 barrier: global memory dependency and/or image layout transitions.
struct Barrier {
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkMemoryBarrier2> memory;
    Barrier& image(VkImage img, std::uint32_t layers, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                   VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        images.push_back(b);
        return *this;
    }
    Barrier& global(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                    VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        memory.push_back(b);
        return *this;
    }
    void record(VkCommandBuffer cb) const {
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = static_cast<std::uint32_t>(memory.size());
        di.pMemoryBarriers = memory.data();
        di.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
        di.pImageMemoryBarriers = images.data();
        vkCmdPipelineBarrier2(cb, &di);
    }
};

VkVideoPictureResourceInfoKHR picture(VkImageView view, std::uint32_t layer) {
    VkVideoPictureResourceInfoKHR p{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    p.codedOffset = {0, 0};
    p.codedExtent = {kCodedWidth, kCodedHeight};
    p.baseArrayLayer = layer;
    p.imageViewBinding = view;
    return p;
}

/// Binds memory to a video session (one allocation per bind point, device-local preferred).
void bindSessionMemory(VideoRig& rig, VkVideoSessionKHR session) {
    std::uint32_t count = 0;
    check(vkGetVideoSessionMemoryRequirementsKHR(rig.device(), session, &count, nullptr),
          "vkGetVideoSessionMemoryRequirementsKHR");
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(
        count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    check(vkGetVideoSessionMemoryRequirementsKHR(rig.device(), session, &count, reqs.data()),
          "vkGetVideoSessionMemoryRequirementsKHR");
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    for (const auto& r : reqs) {
        VkBindVideoSessionMemoryInfoKHR b{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
        b.memoryBindIndex = r.memoryBindIndex;
        b.memory = rig.allocate(r.memoryRequirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr,
                                "video session");
        b.memoryOffset = 0;
        b.memorySize = r.memoryRequirements.size;
        binds.push_back(b);
    }
    if (!binds.empty())
        check(vkBindVideoSessionMemoryKHR(rig.device(), session, static_cast<std::uint32_t>(binds.size()),
                                          binds.data()),
              "vkBindVideoSessionMemoryKHR");
}

// --- H.264 ---------------------------------------------------------------------------------------------

StdVideoH264LevelIdc levelEnum(std::uint32_t levelIdc) {
    switch (levelIdc) {
    case 9:
    case 10:
        return STD_VIDEO_H264_LEVEL_IDC_1_0;
    case 11:
        return STD_VIDEO_H264_LEVEL_IDC_1_1;
    case 12:
        return STD_VIDEO_H264_LEVEL_IDC_1_2;
    case 13:
        return STD_VIDEO_H264_LEVEL_IDC_1_3;
    case 20:
        return STD_VIDEO_H264_LEVEL_IDC_2_0;
    case 21:
        return STD_VIDEO_H264_LEVEL_IDC_2_1;
    case 22:
        return STD_VIDEO_H264_LEVEL_IDC_2_2;
    case 30:
        return STD_VIDEO_H264_LEVEL_IDC_3_0;
    case 31:
        return STD_VIDEO_H264_LEVEL_IDC_3_1;
    case 32:
        return STD_VIDEO_H264_LEVEL_IDC_3_2;
    case 40:
        return STD_VIDEO_H264_LEVEL_IDC_4_0;
    case 41:
        return STD_VIDEO_H264_LEVEL_IDC_4_1;
    case 42:
        return STD_VIDEO_H264_LEVEL_IDC_4_2;
    case 50:
        return STD_VIDEO_H264_LEVEL_IDC_5_0;
    case 51:
        return STD_VIDEO_H264_LEVEL_IDC_5_1;
    case 52:
        return STD_VIDEO_H264_LEVEL_IDC_5_2;
    case 60:
        return STD_VIDEO_H264_LEVEL_IDC_6_0;
    case 61:
        return STD_VIDEO_H264_LEVEL_IDC_6_1;
    case 62:
        return STD_VIDEO_H264_LEVEL_IDC_6_2;
    default:
        return STD_VIDEO_H264_LEVEL_IDC_INVALID;
    }
}

const char* profileName(StdVideoH264ProfileIdc p) {
    switch (p) {
    case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
        return "Constrained Baseline";
    case STD_VIDEO_H264_PROFILE_IDC_MAIN:
        return "Main";
    case STD_VIDEO_H264_PROFILE_IDC_HIGH:
        return "High";
    default:
        return "other";
    }
}

/// The encode and decode profiles of one H.264 profile_idc, and the lists images and buffers name them in.
struct H264Profiles {
    VkVideoEncodeH264ProfileInfoKHR encodeH264{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    VkVideoDecodeH264ProfileInfoKHR decodeH264{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR encode{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR decode{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR both[2]{};
    VkVideoProfileListInfoKHR encodeList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkVideoProfileListInfoKHR decodeList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkVideoProfileListInfoKHR bothList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};

    explicit H264Profiles(StdVideoH264ProfileIdc idc) {
        encodeH264.stdProfileIdc = idc;
        decodeH264.stdProfileIdc = idc;
        decodeH264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        for (VkVideoProfileInfoKHR* p : {&encode, &decode}) {
            p->chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
            p->lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            p->chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        }
        encode.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        encode.pNext = &encodeH264;
        decode.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        decode.pNext = &decodeH264;
        both[0] = encode;
        both[1] = decode;
        encodeList.profileCount = 1;
        encodeList.pProfiles = &encode;
        decodeList.profileCount = 1;
        decodeList.pProfiles = &decode;
        bothList.profileCount = 2;
        bothList.pProfiles = both;
    }
    H264Profiles(const H264Profiles&) = delete;
    H264Profiles& operator=(const H264Profiles&) = delete;
};

/// What the driver offers for the H.264 round trip; `reason` says why it cannot run (empty = it can).
struct H264Plan {
    std::string reason;
    StdVideoH264ProfileIdc profile = STD_VIDEO_H264_PROFILE_IDC_INVALID;
    VkVideoCapabilitiesKHR encodeCaps{}, decodeCaps{};
    VkVideoEncodeCapabilitiesKHR encode{};
    VkVideoEncodeH264CapabilitiesKHR encodeH264{};
    VkVideoDecodeCapabilitiesKHR decode{};
    VkVideoDecodeH264CapabilitiesKHR decodeH264{};
    VkVideoFormatPropertiesKHR srcFormat{}, encodeDpbFormat{}, decodeDpbFormat{}, decodeOutFormat{};
    bool canDecode = false;
    std::string decodeNote; ///< why decoding is not measured
    bool coincide = false;  ///< decoded pictures are written to the DPB images themselves
    bool readable = false;  ///< decoded pictures can be copied out (NV12 + transfer source)
    bool statusQueries = false;
};

bool extentFits(const VkVideoCapabilitiesKHR& c) {
    return c.minCodedExtent.width <= kCodedWidth && c.minCodedExtent.height <= kCodedHeight &&
           c.maxCodedExtent.width >= kCodedWidth && c.maxCodedExtent.height >= kCodedHeight;
}

H264Plan planH264(const rhi::NativeDevice& n) {
    H264Plan plan;
    const auto pd = static_cast<VkPhysicalDevice>(n.physicalDevice);
    const VkVideoCodecOperationFlagsKHR encOps = familyCodecs(pd, n.videoEncodeFamily);
    const VkVideoCodecOperationFlagsKHR decOps = familyCodecs(pd, n.videoDecodeFamily);
    const bool decodeAvailable = n.videoDecodeQueue &&
                                 hasExtension(n, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME) &&
                                 (decOps & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0;
    if (!n.videoEncodeQueue || !hasExtension(n, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME) ||
        !(encOps & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)) {
        plan.reason = decodeAvailable
                          ? "the driver has a Vulkan Video H.264 decoder but no encoder; this test makes "
                            "its stream with the GPU's own encoder"
                          : "no Vulkan Video H.264 encoder (VK_KHR_video_encode_h264 and a video "
                            "encode queue)";
        return plan;
    }

    // Encoder: the richest profile it supports at 1080p.
    for (const StdVideoH264ProfileIdc idc : {STD_VIDEO_H264_PROFILE_IDC_HIGH, STD_VIDEO_H264_PROFILE_IDC_MAIN,
                                             STD_VIDEO_H264_PROFILE_IDC_BASELINE}) {
        H264Profiles prof(idc);
        VkVideoEncodeH264CapabilitiesKHR h264{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
        VkVideoEncodeCapabilitiesKHR enc{VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR};
        enc.pNext = &h264;
        VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
        caps.pNext = &enc;
        if (vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &prof.encode, &caps) != VK_SUCCESS ||
            !extentFits(caps))
            continue;
        plan.profile = idc;
        plan.encodeCaps = caps;
        plan.encode = enc;
        plan.encodeH264 = h264;
        plan.encodeCaps.pNext = plan.encode.pNext = plan.encodeH264.pNext = nullptr;
        break;
    }
    if (plan.profile == STD_VIDEO_H264_PROFILE_IDC_INVALID) {
        plan.reason = "the H.264 encoder supports no 8-bit 4:2:0 High, Main or Baseline profile at 1920x1088";
        return plan;
    }
    if (plan.encodeH264.maxLevelIdc < STD_VIDEO_H264_LEVEL_IDC_4_0) {
        plan.reason = "the H.264 encoder's highest level is below 4.0 (1080p)";
        return plan;
    }
    if (plan.encodeCaps.maxDpbSlots < 2 || plan.encodeCaps.maxActiveReferencePictures < 1 ||
        plan.encodeH264.maxPPictureL0ReferenceCount < 1) {
        plan.reason = "the H.264 encoder cannot make P-frames (no reference pictures)";
        return plan;
    }
    constexpr VkVideoEncodeFeedbackFlagsKHR kFeedback =
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
    if ((plan.encode.supportedEncodeFeedbackFlags & kFeedback) != kFeedback) {
        plan.reason = "the H.264 encoder does not report how many bytes it wrote";
        return plan;
    }
    H264Profiles prof(plan.profile);
    auto src = pickFormat(pd, prof.encodeList, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
    if (!src || !(src->imageUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        plan.reason = "the H.264 encoder takes no NV12 input image that can be uploaded to";
        return plan;
    }
    auto dpb = pickFormat(pd, prof.encodeList, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 0, false);
    if (!dpb) {
        plan.reason = "the H.264 encoder reports no reference picture format";
        return plan;
    }
    plan.srcFormat = *src;
    plan.encodeDpbFormat = *dpb;
    plan.srcFormat.pNext = plan.encodeDpbFormat.pNext = nullptr;

    // Decoder (optional): same profile.
    if (!decodeAvailable) {
        plan.decodeNote = "no Vulkan Video H.264 decoder";
        return plan;
    }
    VkVideoDecodeH264CapabilitiesKHR dh{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR dec{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    dec.pNext = &dh;
    VkVideoCapabilitiesKHR dcaps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    dcaps.pNext = &dec;
    if (vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &prof.decode, &dcaps) != VK_SUCCESS ||
        !extentFits(dcaps)) {
        plan.decodeNote =
            std::format("the H.264 decoder does not take {} profile at 1920x1088", profileName(plan.profile));
        return plan;
    }
    if (dcaps.maxDpbSlots < 2 || dcaps.maxActiveReferencePictures < 1) {
        plan.decodeNote = "the H.264 decoder offers no reference pictures";
        return plan;
    }
    plan.decodeCaps = dcaps;
    plan.decode = dec;
    plan.decodeH264 = dh;
    plan.decodeCaps.pNext = plan.decode.pNext = plan.decodeH264.pNext = nullptr;
    plan.coincide = (dec.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    std::optional<VkVideoFormatPropertiesKHR> out, ddpb;
    if (plan.coincide) {
        ddpb = pickFormat(pd, prof.decodeList,
                          VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
        out = ddpb;
    } else if (dec.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) {
        ddpb = pickFormat(pd, prof.decodeList, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, 0, false);
        out = pickFormat(pd, prof.decodeList, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
    }
    if (!ddpb || !out) {
        plan.decodeNote = "the H.264 decoder reports no usable picture formats";
        return plan;
    }
    plan.decodeDpbFormat = *ddpb;
    plan.decodeOutFormat = *out;
    plan.decodeDpbFormat.pNext = plan.decodeOutFormat.pNext = nullptr;
    plan.readable = out->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
                    (out->imageUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    plan.statusQueries = familyHasStatusQueries(pd, n.videoDecodeFamily);
    plan.canDecode = true;
    return plan;
}

struct H264Result {
    double encodeFps = 0.0, decodeFps = 0.0;
    double psnrMean = 0.0, psnrMin = 0.0;
    bool decoded = false, verified = false;
    std::string decodeNote; ///< why decoding was not measured, or why the decoded frames were not compared
    std::string failure;    ///< set when the round trip produced a wrong result
};

/// The H.264 round trip: encode the GOP, read the encoder's parameter sets, decode the stream, compare.
class H264RoundTrip {
public:
    H264RoundTrip(TestContext& ctx, const rhi::NativeDevice& n, H264Plan plan)
        : m_ctx(ctx), m_n(n), m_plan(std::move(plan)), m_prof(m_plan.profile), m_rig(n) {}

    H264Result run(double seconds) {
        H264Result res;
        auto& d = m_ctx.details();
        createEncoder();
        upload();
        m_ctx.checkStop();
        m_ctx.progress(0.15, "H.264: encoding the reference GOP");
        if (!encodeReference(res)) return res;
        m_ctx.checkStop();
        m_ctx.progress(0.25, "H.264: measuring encode throughput");
        res.encodeFps = timeEncode(seconds);
        d["h264_encode_frames_per_submit"] = kGop * kRepeats;

        if (!m_plan.canDecode) {
            res.decodeNote = m_plan.decodeNote;
            return res;
        }
        std::string why;
        if (!readParameters(why) || !createDecoder(why)) {
            res.decodeNote = why;
            return res;
        }
        m_ctx.checkStop();
        m_ctx.progress(0.55, "H.264: decoding the stream");
        if (!decodeReference(res)) return res;
        m_ctx.checkStop();
        m_ctx.progress(0.65, "H.264: measuring decode throughput");
        res.decodeFps = timeDecode(seconds);
        res.decoded = true;
        if (!m_plan.readable) {
            res.decodeNote =
                "decoded pictures cannot be copied out on this driver, so they were not compared";
            return res;
        }
        m_ctx.progress(0.9, "H.264: comparing decoded frames with the source");
        compare(res);
        return res;
    }

private:
    static constexpr std::uint32_t kRepeats = 8;        ///< GOPs per timed submission
    static constexpr VkDeviceSize kStride = 4ull << 20; ///< bitstream space per frame
    static constexpr std::int32_t kQp = 26;

    std::uint32_t graphicsFamily() const { return m_n.queueFamily; }
    VkQueue graphicsQueue() const { return static_cast<VkQueue>(m_n.queue); }
    VkQueue encodeQueue() const { return static_cast<VkQueue>(m_n.videoEncodeQueue); }
    VkQueue decodeQueue() const { return static_cast<VkQueue>(m_n.videoDecodeQueue); }
    bool rateControlDisabled() const {
        return (m_plan.encode.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) != 0;
    }

    // --- encoder -----------------------------------------------------------------------------------
    void createEncoder() {
        VkDevice dev = m_rig.device();
        const auto& caps = m_plan.encodeCaps;
        if (kStride % std::max<VkDeviceSize>(caps.minBitstreamBufferOffsetAlignment, 1) ||
            kStride % std::max<VkDeviceSize>(caps.minBitstreamBufferSizeAlignment, 1))
            throw rhi::Error("the encoder's bitstream alignment is larger than 4 MiB");

        VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
        si.queueFamilyIndex = m_n.videoEncodeFamily;
        si.pVideoProfile = &m_prof.encode;
        si.pictureFormat = m_plan.srcFormat.format;
        si.maxCodedExtent = {kCodedWidth, kCodedHeight};
        si.referencePictureFormat = m_plan.encodeDpbFormat.format;
        si.maxDpbSlots = 2;
        si.maxActiveReferencePictures = 1;
        si.pStdHeaderVersion = &caps.stdHeaderVersion;
        check(vkCreateVideoSessionKHR(dev, &si, nullptr, &m_encSession),
              "vkCreateVideoSessionKHR (H.264 encode)");
        m_rig.onCleanup([dev, s = m_encSession] { vkDestroyVideoSessionKHR(dev, s, nullptr); });
        bindSessionMemory(m_rig, m_encSession);

        // Parameter sets: 1920x1088 coded, cropped to 1080 lines, one reference frame, POC type 0.
        const VkVideoEncodeH264StdFlagsKHR syntax = m_plan.encodeH264.stdSyntaxFlags;
        StdVideoH264SequenceParameterSet sps{};
        sps.flags.direct_8x8_inference_flag = 1;
        sps.flags.frame_mbs_only_flag = 1;
        sps.flags.frame_cropping_flag = 1;
        sps.profile_idc = m_plan.profile;
        sps.level_idc = std::min(STD_VIDEO_H264_LEVEL_IDC_4_1, m_plan.encodeH264.maxLevelIdc);
        sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        sps.log2_max_frame_num_minus4 = 0;
        sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 2;
        sps.max_num_ref_frames = 1;
        sps.pic_width_in_mbs_minus1 = kCodedWidth / 16 - 1;
        sps.pic_height_in_map_units_minus1 = kCodedHeight / 16 - 1;
        sps.frame_crop_bottom_offset = (kCodedHeight - kHeight) / 2; // in 2-line units for 4:2:0 frames
        StdVideoH264PictureParameterSet pps{};
        pps.flags.entropy_coding_mode_flag =
            m_plan.profile != STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
            (syntax & VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR);
        pps.flags.transform_8x8_mode_flag =
            m_plan.profile == STD_VIDEO_H264_PROFILE_IDC_HIGH &&
            (syntax & VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR);
        pps.weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;
        VkVideoEncodeH264SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoEncodeH264SessionParametersCreateInfoKHR h264{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h264.maxStdSPSCount = 1;
        h264.maxStdPPSCount = 1;
        h264.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h264;
        pi.videoSession = m_encSession;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &m_encParams),
              "vkCreateVideoSessionParametersKHR (H.264 encode)");
        m_rig.onCleanup([dev, p = m_encParams] { vkDestroyVideoSessionParametersKHR(dev, p, nullptr); });

        m_src = m_rig.image(m_plan.srcFormat,
                            VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, kGop, m_prof.encodeList,
                            {graphicsFamily(), m_n.videoEncodeFamily});
        m_encDpb = m_rig.image(m_plan.encodeDpbFormat, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                               VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 2, m_prof.encodeList,
                               {m_n.videoEncodeFamily});
        // The reference stream is read on the host and by the decoder; timed encodes write elsewhere.
        const bool shared = m_plan.canDecode;
        VkBufferUsageFlags streamUsage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
        if (shared) streamUsage |= VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
        m_stream =
            m_rig.buffer(kStride * kGop, streamUsage, true, shared ? &m_prof.bothList : &m_prof.encodeList,
                         {m_n.videoEncodeFamily, shared ? m_n.videoDecodeFamily : m_n.videoEncodeFamily});
        m_timedStream = m_rig.buffer(kStride * kGop, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, false,
                                     &m_prof.encodeList, {m_n.videoEncodeFamily});

        VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fb{
            VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
        fb.pNext = &m_prof.encode;
        fb.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                 VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.pNext = &fb;
        qi.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
        qi.queryCount = kGop;
        check(vkCreateQueryPool(dev, &qi, nullptr, &m_feedback), "vkCreateQueryPool (encode feedback)");
        m_rig.onCleanup([dev, q = m_feedback] { vkDestroyQueryPool(dev, q, nullptr); });

        m_rateControl = VkVideoEncodeRateControlInfoKHR{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
        m_rateControl.rateControlMode = rateControlDisabled()
                                            ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR
                                            : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
        m_qp = std::clamp(kQp, m_plan.encodeH264.minQp, m_plan.encodeH264.maxQp);
    }

    void upload() {
        const std::vector<std::uint8_t> nv12 = makeSource();
        m_source = nv12;
        VideoRig::Buffer staging =
            m_rig.buffer(nv12.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, nullptr, {graphicsFamily()});
        std::memcpy(staging.mapped, nv12.data(), nv12.size());
        m_rig.flush(staging);
        VkCommandBuffer cb = m_rig.begin(graphicsFamily());
        Barrier()
            .image(m_src.image, kGop, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT)
            .record(cb);
        std::vector<VkBufferImageCopy> copies;
        for (std::uint32_t f = 0; f < kGop; ++f) {
            VkBufferImageCopy y{};
            y.bufferOffset = f * kFrameBytes;
            y.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, f, 1};
            y.imageExtent = {kCodedWidth, kCodedHeight, 1};
            VkBufferImageCopy c = y;
            c.bufferOffset = f * kFrameBytes + kLumaBytes;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            c.imageExtent = {kCodedWidth / 2, kCodedHeight / 2, 1};
            copies.push_back(y);
            copies.push_back(c);
        }
        vkCmdCopyBufferToImage(cb, staging.buffer, m_src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        m_rig.end(cb);
        m_rig.submit(graphicsQueue(), cb);
        m_rig.wait();
    }

    /// Records one frame of the GOP: an IDR first, then P-frames each referencing the frame before.
    /// DPB slot (and layer) f % 2 holds frame f.
    void recordEncode(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query, VkBuffer dst) {
        const bool idr = f == 0;
        const std::uint32_t setupSlot = f % 2, refSlot = (f + 1) % 2;
        const VkVideoPictureResourceInfoKHR src = picture(m_src.view, f);
        const VkVideoPictureResourceInfoKHR setupPic = picture(m_encDpb.view, setupSlot);
        const VkVideoPictureResourceInfoKHR refPic = picture(m_encDpb.view, refSlot);

        // Begin: the reference picture (active slot) and the picture being set up (not yet active).
        VkVideoReferenceSlotInfoKHR beginSlots[2]{{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR},
                                                  {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR}};
        beginSlots[0].slotIndex = -1;
        beginSlots[0].pPictureResource = &setupPic;
        beginSlots[1].slotIndex = static_cast<std::int32_t>(refSlot);
        beginSlots[1].pPictureResource = &refPic;
        VkVideoBeginCodingInfoKHR bi{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
        bi.pNext = reset ? nullptr : &m_rateControl; // must match the session's rate control state
        bi.videoSession = m_encSession;
        bi.videoSessionParameters = m_encParams;
        bi.referenceSlotCount = idr ? 1 : 2;
        bi.pReferenceSlots = beginSlots;
        vkCmdBeginVideoCodingKHR(cb, &bi);
        if (reset) {
            VkVideoCodingControlInfoKHR ctl{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
            ctl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
            vkCmdControlVideoCodingKHR(cb, &ctl);
            ctl.pNext = &m_rateControl;
            ctl.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
            vkCmdControlVideoCodingKHR(cb, &ctl);
        }

        const std::uint32_t frameNum = f;
        const std::int32_t poc = static_cast<std::int32_t>(2 * f);
        StdVideoEncodeH264ReferenceListsInfo lists{};
        std::fill(std::begin(lists.RefPicList0), std::end(lists.RefPicList0),
                  STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        std::fill(std::begin(lists.RefPicList1), std::end(lists.RefPicList1),
                  STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        lists.RefPicList0[0] = static_cast<std::uint8_t>(refSlot);
        StdVideoEncodeH264PictureInfo pic{};
        pic.flags.IdrPicFlag = idr;
        pic.flags.is_reference = 1;
        pic.primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        pic.frame_num = frameNum;
        pic.PicOrderCnt = poc;
        pic.pRefLists = idr ? nullptr : &lists;
        StdVideoEncodeH264SliceHeader slice{};
        slice.slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
        slice.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
        slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;
        VkVideoEncodeH264NaluSliceInfoKHR nalu{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
        nalu.constantQp = rateControlDisabled() ? m_qp : 0;
        nalu.pStdSliceHeader = &slice;
        VkVideoEncodeH264PictureInfoKHR picInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
        picInfo.naluSliceEntryCount = 1;
        picInfo.pNaluSliceEntries = &nalu;
        picInfo.pStdPictureInfo = &pic;

        StdVideoEncodeH264ReferenceInfo setupRef{};
        setupRef.primary_pic_type = pic.primary_pic_type;
        setupRef.FrameNum = frameNum;
        setupRef.PicOrderCnt = poc;
        VkVideoEncodeH264DpbSlotInfoKHR setupInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
        setupInfo.pStdReferenceInfo = &setupRef;
        VkVideoReferenceSlotInfoKHR setup{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        setup.pNext = &setupInfo;
        setup.slotIndex = static_cast<std::int32_t>(setupSlot);
        setup.pPictureResource = &setupPic;

        StdVideoEncodeH264ReferenceInfo prevRef{};
        prevRef.primary_pic_type = f == 1 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        prevRef.FrameNum = f - 1;
        prevRef.PicOrderCnt = poc - 2;
        VkVideoEncodeH264DpbSlotInfoKHR refInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
        refInfo.pStdReferenceInfo = &prevRef;
        VkVideoReferenceSlotInfoKHR ref{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        ref.pNext = &refInfo;
        ref.slotIndex = static_cast<std::int32_t>(refSlot);
        ref.pPictureResource = &refPic;

        VkVideoEncodeInfoKHR ei{VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
        ei.pNext = &picInfo;
        ei.dstBuffer = dst;
        ei.dstBufferOffset = f * kStride;
        ei.dstBufferRange = kStride;
        ei.srcPictureResource = src;
        ei.pSetupReferenceSlot = &setup;
        ei.referenceSlotCount = idr ? 0 : 1;
        ei.pReferenceSlots = idr ? nullptr : &ref;
        if (query) vkCmdBeginQuery(cb, m_feedback, f, 0);
        vkCmdEncodeVideoKHR(cb, &ei);
        if (query) vkCmdEndQuery(cb, m_feedback, f);
        VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
        vkCmdEndVideoCodingKHR(cb, &end);
        // The next frame reads this one's reconstructed picture.
        Barrier()
            .global(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)
            .record(cb);
    }

    bool encodeReference(H264Result& res) {
        VkCommandBuffer cb = m_rig.begin(m_n.videoEncodeFamily);
        Barrier()
            .image(m_src.image, kGop, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                   VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR)
            .image(m_encDpb.image, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
                   VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                   VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)
            .record(cb);
        vkCmdResetQueryPool(cb, m_feedback, 0, kGop);
        for (std::uint32_t f = 0; f < kGop; ++f)
            recordEncode(cb, f, f == 0, true, m_stream.buffer);
        Barrier()
            .global(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT)
            .record(cb);
        m_rig.end(cb);
        m_rig.submit(encodeQueue(), cb);
        m_rig.wait();

        struct Feedback {
            std::uint64_t offset, bytes;
            std::int64_t status;
        };
        std::vector<Feedback> fb(kGop);
        check(vkGetQueryPoolResults(m_rig.device(), m_feedback, 0, kGop, fb.size() * sizeof(Feedback),
                                    fb.data(), sizeof(Feedback),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                        VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
              "vkGetQueryPoolResults (encode feedback)");
        m_rig.invalidate(m_stream);
        std::uint64_t total = 0;
        for (std::uint32_t f = 0; f < kGop; ++f) {
            if (fb[f].status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
                res.failure =
                    fb[f].status == VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR
                        ? std::format("frame {} did not fit in 4 MiB of bitstream", f)
                        : std::format("the encoder reported status {} for frame {}", fb[f].status, f);
                return false;
            }
            if (fb[f].bytes == 0 || fb[f].offset + fb[f].bytes > kStride) {
                res.failure = std::format("the encoder reported {} bytes at offset {} for frame {}",
                                          fb[f].bytes, fb[f].offset, f);
                return false;
            }
            m_frames[f].offset = fb[f].offset;
            m_frames[f].bytes = fb[f].bytes;
            total += fb[f].bytes;
        }
        auto& d = m_ctx.details();
        d["h264_profile"] = profileName(m_plan.profile);
        d["h264_rate_control"] =
            rateControlDisabled() ? std::format("constant QP {}", m_qp) : "driver default";
        d["h264_gop_bytes"] = total;
        d["h264_mbit_per_s_at_60fps"] = double(total) * 8.0 / kGop * 60.0 / 1e6;
        return true;
    }

    double timeEncode(double seconds) {
        VkCommandBuffer cb = m_rig.begin(m_n.videoEncodeFamily);
        // Covers the previous submission of this command buffer too (same queue, submission order).
        Barrier()
            .global(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)
            .record(cb);
        for (std::uint32_t r = 0; r < kRepeats; ++r)
            for (std::uint32_t f = 0; f < kGop; ++f)
                recordEncode(cb, f, false, false, m_timedStream.buffer);
        m_rig.end(cb);
        return timeSubmissions(encodeQueue(), cb, seconds);
    }

    double timeSubmissions(VkQueue queue, VkCommandBuffer cb, double seconds) {
        m_rig.submit(queue, cb); // warm-up
        m_rig.wait();
        std::uint64_t frames = 0;
        const auto t0 = std::chrono::steady_clock::now();
        double elapsed = 0.0;
        do {
            m_ctx.checkStop();
            m_rig.submit(queue, cb);
            m_rig.wait();
            frames += std::uint64_t(kGop) * kRepeats;
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        } while (elapsed < seconds);
        return double(frames) / elapsed;
    }

    // --- decoder -----------------------------------------------------------------------------------
    /// Reads the parameter sets the encoder wrote and the slice headers of each frame.
    bool readParameters(std::string& why) {
        VkVideoEncodeH264SessionParametersGetInfoKHR h264{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
        h264.writeStdSPS = VK_TRUE;
        h264.writeStdPPS = VK_TRUE;
        VkVideoEncodeSessionParametersGetInfoKHR gi{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR};
        gi.pNext = &h264;
        gi.videoSessionParameters = m_encParams;
        VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264Fb{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
        VkVideoEncodeSessionParametersFeedbackInfoKHR fb{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
        fb.pNext = &h264Fb;
        std::size_t size = 0;
        check(vkGetEncodedVideoSessionParametersKHR(m_rig.device(), &gi, &fb, &size, nullptr),
              "vkGetEncodedVideoSessionParametersKHR");
        std::vector<std::uint8_t> bytes(size);
        if (size)
            check(vkGetEncodedVideoSessionParametersKHR(m_rig.device(), &gi, nullptr, &size, bytes.data()),
                  "vkGetEncodedVideoSessionParametersKHR");
        bytes.resize(size);
        m_ctx.details()["h264_encoder_overrides"] = fb.hasOverrides == VK_TRUE;
        std::string error;
        auto ps = h264::parse(bytes, &error);
        if (!ps) {
            why = "the encoder's parameter sets could not be read: " + error;
            return false;
        }
        const h264::Sps& sps = *ps->sps;
        if (sps.profileIdc != static_cast<std::uint32_t>(m_plan.profile) || sps.chromaFormatIdc != 1 ||
            sps.bitDepthLumaMinus8 || sps.bitDepthChromaMinus8 || !sps.frameMbsOnly ||
            sps.widthInMbsMinus1 + 1 != kCodedWidth / 16 ||
            sps.heightInMapUnitsMinus1 + 1 != kCodedHeight / 16) {
            why = std::format("the encoder wrote an unexpected SPS (profile {}, {}x{} macroblocks)",
                              sps.profileIdc, sps.widthInMbsMinus1 + 1, sps.heightInMapUnitsMinus1 + 1);
            return false;
        }
        if (levelEnum(sps.levelIdc) == STD_VIDEO_H264_LEVEL_IDC_INVALID ||
            levelEnum(sps.levelIdc) > m_plan.decodeH264.maxLevelIdc) {
            why = std::format("the decoder does not support the stream's level ({})", sps.levelIdc);
            return false;
        }
        m_params = *ps;
        m_ctx.details()["h264_entropy_coding"] = ps->pps->entropyCodingMode ? "CABAC" : "CAVLC";
        m_ctx.details()["h264_level"] = std::format("{}.{}", sps.levelIdc / 10, sps.levelIdc % 10);

        h264::PocCounter poc;
        for (std::uint32_t f = 0; f < kGop; ++f) {
            Frame& fr = m_frames[f];
            const std::span<const std::uint8_t> data(m_stream.mapped + f * kStride + fr.offset, fr.bytes);
            for (const h264::NalUnit& nal : h264::nalUnits(data)) {
                if (nal.type != 1 && nal.type != 5) continue;
                if (fr.slices.empty()) {
                    auto h =
                        h264::parseSliceHeader(data.subspan(nal.payload, nal.end - nal.payload), *ps, &error);
                    if (!h) {
                        why = std::format("frame {}'s slice header could not be read: {}", f, error);
                        return false;
                    }
                    fr.header = *h;
                }
                fr.slices.push_back(static_cast<std::uint32_t>(fr.offset + nal.offset));
            }
            if (fr.slices.empty()) {
                why = std::format("the encoder's output for frame {} has no slice", f);
                return false;
            }
            if (fr.header.idr() != (f == 0)) {
                why = std::format("frame {} came out as {} picture", f,
                                  fr.header.idr() ? "an IDR" : "a non-IDR");
                return false;
            }
            fr.poc = poc.next(fr.header, sps);
        }
        return true;
    }

    std::uint32_t decodeLayers() const { return m_plan.coincide ? kGop : 2; }
    /// The DPB picture frame f is reconstructed into (one layer per frame when DPB and output coincide).
    VkVideoPictureResourceInfoKHR decodeDpbPicture(std::uint32_t f) const {
        return picture(m_decDpb.view, m_plan.coincide ? f : f % 2);
    }

    bool createDecoder(std::string& why) {
        VkDevice dev = m_rig.device();
        const auto& caps = m_plan.decodeCaps;
        if (kStride % std::max<VkDeviceSize>(caps.minBitstreamBufferOffsetAlignment, 1) ||
            kStride % std::max<VkDeviceSize>(caps.minBitstreamBufferSizeAlignment, 1)) {
            why = "the decoder's bitstream alignment is larger than 4 MiB";
            return false;
        }
        VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
        si.queueFamilyIndex = m_n.videoDecodeFamily;
        si.pVideoProfile = &m_prof.decode;
        si.pictureFormat = m_plan.decodeOutFormat.format;
        si.maxCodedExtent = {kCodedWidth, kCodedHeight};
        si.referencePictureFormat = m_plan.decodeDpbFormat.format;
        si.maxDpbSlots = 2;
        si.maxActiveReferencePictures = 1;
        si.pStdHeaderVersion = &caps.stdHeaderVersion;
        check(vkCreateVideoSessionKHR(dev, &si, nullptr, &m_decSession),
              "vkCreateVideoSessionKHR (H.264 decode)");
        m_rig.onCleanup([dev, s = m_decSession] { vkDestroyVideoSessionKHR(dev, s, nullptr); });
        bindSessionMemory(m_rig, m_decSession);

        // Parameter sets exactly as the encoder wrote them.
        const h264::Sps& s = *m_params.sps;
        const h264::Pps& p = *m_params.pps;
        StdVideoH264SequenceParameterSet sps{};
        sps.flags.constraint_set0_flag = s.constraintFlags & 1u;
        sps.flags.constraint_set1_flag = (s.constraintFlags >> 1) & 1u;
        sps.flags.constraint_set2_flag = (s.constraintFlags >> 2) & 1u;
        sps.flags.constraint_set3_flag = (s.constraintFlags >> 3) & 1u;
        sps.flags.constraint_set4_flag = (s.constraintFlags >> 4) & 1u;
        sps.flags.constraint_set5_flag = (s.constraintFlags >> 5) & 1u;
        sps.flags.direct_8x8_inference_flag = s.direct8x8Inference;
        sps.flags.mb_adaptive_frame_field_flag = s.mbAdaptiveFrameField;
        sps.flags.frame_mbs_only_flag = s.frameMbsOnly;
        sps.flags.gaps_in_frame_num_value_allowed_flag = s.gapsInFrameNumAllowed;
        sps.flags.qpprime_y_zero_transform_bypass_flag = s.qpprimeYZeroTransformBypass;
        sps.flags.frame_cropping_flag = s.frameCropping;
        sps.profile_idc = static_cast<StdVideoH264ProfileIdc>(s.profileIdc);
        sps.level_idc = levelEnum(s.levelIdc);
        sps.chroma_format_idc = static_cast<StdVideoH264ChromaFormatIdc>(s.chromaFormatIdc);
        sps.seq_parameter_set_id = static_cast<std::uint8_t>(s.spsId);
        sps.bit_depth_luma_minus8 = static_cast<std::uint8_t>(s.bitDepthLumaMinus8);
        sps.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(s.bitDepthChromaMinus8);
        sps.log2_max_frame_num_minus4 = static_cast<std::uint8_t>(s.log2MaxFrameNumMinus4);
        sps.pic_order_cnt_type = static_cast<StdVideoH264PocType>(s.pocType);
        sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(s.log2MaxPocLsbMinus4);
        sps.max_num_ref_frames = static_cast<std::uint8_t>(s.maxNumRefFrames);
        sps.pic_width_in_mbs_minus1 = s.widthInMbsMinus1;
        sps.pic_height_in_map_units_minus1 = s.heightInMapUnitsMinus1;
        sps.frame_crop_left_offset = s.cropLeft;
        sps.frame_crop_right_offset = s.cropRight;
        sps.frame_crop_top_offset = s.cropTop;
        sps.frame_crop_bottom_offset = s.cropBottom;
        StdVideoH264PictureParameterSet pps{};
        pps.flags.transform_8x8_mode_flag = p.transform8x8Mode;
        pps.flags.redundant_pic_cnt_present_flag = p.redundantPicCntPresent;
        pps.flags.constrained_intra_pred_flag = p.constrainedIntraPred;
        pps.flags.deblocking_filter_control_present_flag = p.deblockingFilterControlPresent;
        pps.flags.weighted_pred_flag = p.weightedPred;
        pps.flags.bottom_field_pic_order_in_frame_present_flag = p.bottomFieldPicOrderInFramePresent;
        pps.flags.entropy_coding_mode_flag = p.entropyCodingMode;
        pps.seq_parameter_set_id = static_cast<std::uint8_t>(p.spsId);
        pps.pic_parameter_set_id = static_cast<std::uint8_t>(p.ppsId);
        pps.num_ref_idx_l0_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL0DefaultMinus1);
        pps.num_ref_idx_l1_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL1DefaultMinus1);
        pps.weighted_bipred_idc = static_cast<StdVideoH264WeightedBipredIdc>(p.weightedBipredIdc);
        pps.pic_init_qp_minus26 = static_cast<std::int8_t>(p.picInitQpMinus26);
        pps.pic_init_qs_minus26 = static_cast<std::int8_t>(p.picInitQsMinus26);
        pps.chroma_qp_index_offset = static_cast<std::int8_t>(p.chromaQpIndexOffset);
        pps.second_chroma_qp_index_offset = static_cast<std::int8_t>(p.secondChromaQpIndexOffset);
        VkVideoDecodeH264SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoDecodeH264SessionParametersCreateInfoKHR h264{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h264.maxStdSPSCount = 1;
        h264.maxStdPPSCount = 1;
        h264.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h264;
        pi.videoSession = m_decSession;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &m_decParams),
              "vkCreateVideoSessionParametersKHR (H.264 decode)");
        m_rig.onCleanup([dev, q = m_decParams] { vkDestroyVideoSessionParametersKHR(dev, q, nullptr); });

        // Decoded pictures are read back on the graphics queue.
        const VkImageUsageFlags readUsage = m_plan.readable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0;
        if (m_plan.coincide) {
            const VkImageUsageFlags video =
                VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
            m_decDpb = m_rig.image(
                m_plan.decodeDpbFormat, video | readUsage, video, kGop, m_prof.decodeList,
                {m_n.videoDecodeFamily, m_plan.readable ? graphicsFamily() : m_n.videoDecodeFamily});
            m_decOut = m_decDpb;
        } else {
            m_decDpb = m_rig.image(m_plan.decodeDpbFormat, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                                   VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, 2, m_prof.decodeList,
                                   {m_n.videoDecodeFamily});
            m_decOut = m_rig.image(
                m_plan.decodeOutFormat, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | readUsage,
                VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, kGop, m_prof.decodeList,
                {m_n.videoDecodeFamily, m_plan.readable ? graphicsFamily() : m_n.videoDecodeFamily});
        }
        if (m_plan.statusQueries) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.pNext = &m_prof.decode;
            qi.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
            qi.queryCount = kGop;
            check(vkCreateQueryPool(dev, &qi, nullptr, &m_decStatus), "vkCreateQueryPool (decode status)");
            m_rig.onCleanup([dev, q = m_decStatus] { vkDestroyQueryPool(dev, q, nullptr); });
        }
        return true;
    }

    void recordDecode(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query) {
        const Frame& fr = m_frames[f];
        const VkVideoPictureResourceInfoKHR setupPic = decodeDpbPicture(f);
        const VkVideoPictureResourceInfoKHR refPic = decodeDpbPicture(f ? f - 1 : 0);
        const VkVideoPictureResourceInfoKHR outPic = m_plan.coincide ? setupPic : picture(m_decOut.view, f);
        const std::uint32_t setupSlot = f % 2, refSlot = (f + 1) % 2;
        const bool hasRef = f > 0;

        VkVideoReferenceSlotInfoKHR beginSlots[2]{{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR},
                                                  {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR}};
        beginSlots[0].slotIndex = -1;
        beginSlots[0].pPictureResource = &setupPic;
        beginSlots[1].slotIndex = static_cast<std::int32_t>(refSlot);
        beginSlots[1].pPictureResource = &refPic;
        VkVideoBeginCodingInfoKHR bi{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
        bi.videoSession = m_decSession;
        bi.videoSessionParameters = m_decParams;
        bi.referenceSlotCount = hasRef ? 2 : 1;
        bi.pReferenceSlots = beginSlots;
        vkCmdBeginVideoCodingKHR(cb, &bi);
        if (reset) {
            VkVideoCodingControlInfoKHR ctl{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
            ctl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
            vkCmdControlVideoCodingKHR(cb, &ctl);
        }

        const h264::SliceHeader& h = fr.header;
        StdVideoDecodeH264PictureInfo pic{};
        pic.flags.is_intra = h.intra();
        pic.flags.IdrPicFlag = h.idr();
        pic.flags.is_reference = h.nalRefIdc != 0;
        pic.seq_parameter_set_id = static_cast<std::uint8_t>(m_params.sps->spsId);
        pic.pic_parameter_set_id = static_cast<std::uint8_t>(m_params.pps->ppsId);
        pic.frame_num = static_cast<std::uint16_t>(h.frameNum);
        pic.idr_pic_id = static_cast<std::uint16_t>(h.idrPicId);
        pic.PicOrderCnt[0] = fr.poc[0];
        pic.PicOrderCnt[1] = fr.poc[1];
        // Slice offsets are relative to the frame's bitstream range, which starts at its 4 MiB slot.
        VkVideoDecodeH264PictureInfoKHR picInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
        picInfo.pStdPictureInfo = &pic;
        picInfo.sliceCount = static_cast<std::uint32_t>(fr.slices.size());
        picInfo.pSliceOffsets = fr.slices.data();

        StdVideoDecodeH264ReferenceInfo setupRef{};
        setupRef.FrameNum = static_cast<std::uint16_t>(h.frameNum);
        setupRef.PicOrderCnt[0] = fr.poc[0];
        setupRef.PicOrderCnt[1] = fr.poc[1];
        VkVideoDecodeH264DpbSlotInfoKHR setupInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
        setupInfo.pStdReferenceInfo = &setupRef;
        VkVideoReferenceSlotInfoKHR setup{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        setup.pNext = &setupInfo;
        setup.slotIndex = static_cast<std::int32_t>(setupSlot);
        setup.pPictureResource = &setupPic;

        StdVideoDecodeH264ReferenceInfo prevRef{};
        if (hasRef) {
            const Frame& prev = m_frames[f - 1];
            prevRef.FrameNum = static_cast<std::uint16_t>(prev.header.frameNum);
            prevRef.PicOrderCnt[0] = prev.poc[0];
            prevRef.PicOrderCnt[1] = prev.poc[1];
        }
        VkVideoDecodeH264DpbSlotInfoKHR refInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
        refInfo.pStdReferenceInfo = &prevRef;
        VkVideoReferenceSlotInfoKHR ref{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        ref.pNext = &refInfo;
        ref.slotIndex = static_cast<std::int32_t>(refSlot);
        ref.pPictureResource = &refPic;

        VkVideoDecodeInfoKHR di{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
        di.pNext = &picInfo;
        di.srcBuffer = m_stream.buffer;
        di.srcBufferOffset = f * kStride;
        di.srcBufferRange = std::min<VkDeviceSize>(
            kStride, alignUp(fr.offset + fr.bytes,
                             std::max<VkDeviceSize>(m_plan.decodeCaps.minBitstreamBufferSizeAlignment, 1)));
        di.dstPictureResource = outPic;
        di.pSetupReferenceSlot = &setup;
        di.referenceSlotCount = hasRef ? 1 : 0;
        di.pReferenceSlots = hasRef ? &ref : nullptr;
        if (query) vkCmdBeginQuery(cb, m_decStatus, f, 0);
        vkCmdDecodeVideoKHR(cb, &di);
        if (query) vkCmdEndQuery(cb, m_decStatus, f);
        VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
        vkCmdEndVideoCodingKHR(cb, &end);
        Barrier()
            .global(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)
            .record(cb);
    }

    bool decodeReference(H264Result& res) {
        VkCommandBuffer cb = m_rig.begin(m_n.videoDecodeFamily);
        Barrier b;
        const VkAccessFlags2 rw =
            VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        b.image(m_decDpb.image, m_decDpb.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, rw);
        if (!m_plan.coincide)
            b.image(m_decOut.image, m_decOut.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        b.record(cb);
        const bool query = m_decStatus != VK_NULL_HANDLE;
        if (query) vkCmdResetQueryPool(cb, m_decStatus, 0, kGop);
        for (std::uint32_t f = 0; f < kGop; ++f)
            recordDecode(cb, f, f == 0, query);
        m_rig.end(cb);
        m_rig.submit(decodeQueue(), cb);
        m_rig.wait();
        if (query) {
            std::int64_t status[kGop]{};
            check(vkGetQueryPoolResults(m_rig.device(), m_decStatus, 0, kGop, sizeof(status), status,
                                        sizeof(std::int64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                            VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
                  "vkGetQueryPoolResults (decode status)");
            for (std::uint32_t f = 0; f < kGop; ++f)
                if (status[f] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
                    res.failure = std::format("the decoder reported status {} for frame {}", status[f], f);
                    return false;
                }
        }
        return true;
    }

    double timeDecode(double seconds) {
        VkCommandBuffer cb = m_rig.begin(m_n.videoDecodeFamily);
        Barrier()
            .global(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)
            .record(cb);
        for (std::uint32_t r = 0; r < kRepeats; ++r)
            for (std::uint32_t f = 0; f < kGop; ++f)
                recordDecode(cb, f, false, false);
        m_rig.end(cb);
        return timeSubmissions(decodeQueue(), cb, seconds);
    }

    /// Copies the decoded luma planes out (graphics queue) and compares them with the source.
    void compare(H264Result& res) {
        VideoRig::Buffer rb = m_rig.buffer(kLumaBytes * kGop, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, nullptr,
                                           {graphicsFamily()});
        VkCommandBuffer cb = m_rig.begin(graphicsFamily());
        Barrier()
            .image(m_decOut.image, m_decOut.layers,
                   m_plan.coincide ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR
                                   : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT)
            .record(cb);
        std::vector<VkBufferImageCopy> copies;
        for (std::uint32_t f = 0; f < kGop; ++f) {
            VkBufferImageCopy c{};
            c.bufferOffset = f * kLumaBytes;
            c.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, f, 1};
            c.imageExtent = {kCodedWidth, kCodedHeight, 1};
            copies.push_back(c);
        }
        vkCmdCopyImageToBuffer(cb, m_decOut.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        Barrier()
            .global(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT)
            .record(cb);
        m_rig.end(cb);
        m_rig.submit(graphicsQueue(), cb);
        m_rig.wait();
        m_rig.invalidate(rb);
        double sum = 0.0, worst = 1e9;
        nlohmann::json perFrame = nlohmann::json::array();
        for (std::uint32_t f = 0; f < kGop; ++f) {
            const double p = lumaPsnr(rb.mapped + f * kLumaBytes, m_source.data() + f * kFrameBytes);
            perFrame.push_back(std::round(p * 100.0) / 100.0);
            sum += p;
            worst = std::min(worst, p);
        }
        m_ctx.details()["h264_psnr_per_frame"] = perFrame;
        res.psnrMean = sum / kGop;
        res.psnrMin = worst;
        res.verified = true;
    }

    struct Frame {
        std::uint64_t offset = 0, bytes = 0; ///< within the frame's 4 MiB slot
        std::vector<std::uint32_t> slices;   ///< start-code offsets of its slices, from the slot start
        h264::SliceHeader header;
        std::array<std::int32_t, 2> poc{};
    };

    TestContext& m_ctx;
    const rhi::NativeDevice& m_n;
    H264Plan m_plan;
    H264Profiles m_prof;
    VideoRig m_rig;
    std::vector<std::uint8_t> m_source;
    VkVideoSessionKHR m_encSession = VK_NULL_HANDLE, m_decSession = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR m_encParams = VK_NULL_HANDLE, m_decParams = VK_NULL_HANDLE;
    VideoRig::Image m_src, m_encDpb, m_decDpb, m_decOut;
    VideoRig::Buffer m_stream, m_timedStream;
    VkQueryPool m_feedback = VK_NULL_HANDLE, m_decStatus = VK_NULL_HANDLE;
    VkVideoEncodeRateControlInfoKHR m_rateControl{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
    std::int32_t m_qp = kQp;
    h264::ParameterSets m_params;
    std::array<Frame, kGop> m_frames{};
};

/// Why native Vulkan video work cannot run at all on this device (empty = it can be tried).
std::string vulkanVideoBlocker(const rhi::NativeDevice& n) {
    if (!n.device) return "no native Vulkan device";
    if (!n.videoEncodeQueue && !n.videoDecodeQueue) return "the driver exposes no Vulkan Video queues";
    if (!vkQueueSubmit2 || !vkCmdPipelineBarrier2 || !vkWaitSemaphores)
        return "Vulkan Video here needs Vulkan 1.3 (synchronization2, timeline semaphores)";
    if (!vkCreateVideoSessionKHR || !vkCmdBeginVideoCodingKHR || !vkGetPhysicalDeviceVideoCapabilitiesKHR)
        return "VK_KHR_video_queue is not enabled";
    return {};
}

#endif // GT_HAS_VULKAN

class VideoTest : public Test {
public:
    VideoTest() {
        m_info.id = "feature.video";
        m_info.name = "Video engines";
        m_info.description =
            "Encodes 1080p H.264 on the GPU's video encoder, decodes the stream on its video decoder "
            "and checks the result against the source, reporting frames per second for both and "
            "the codecs the video engines offer.";
        m_info.category = Category::Feature;
        m_info.group = "Video";
        m_info.apis = {rhi::Api::Vulkan};
        m_info.frameBased = false;
        m_info.primaryMetric = "h264_encode_fps";
        m_info.primaryUnit = "fps";
        m_info.reference = 0.0; // fixed-function throughput: shown, not scored
        m_info.tags = {"video"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 97;
    }
    const TestInfo& info() const override { return m_info; }

    SupportResult checkSupport(TestContext& ctx) override {
#if GT_HAS_VULKAN
        if (ctx.api() != rhi::Api::Vulkan)
            return SupportResult::no("video engines are tested through Vulkan Video");
        const rhi::NativeDevice n = ctx.device().nativeDevice();
        if (std::string why = vulkanVideoBlocker(n); !why.empty()) return SupportResult::no(why);
        if (H264Plan plan = planH264(n); !plan.reason.empty()) return SupportResult::no(plan.reason);
        return SupportResult::yes();
#else
        (void)ctx;
        return SupportResult::no("built without Vulkan");
#endif
    }

    void run(TestContext& ctx) override {
#if GT_HAS_VULKAN
        rhi::Device& dev = ctx.device();
        const rhi::NativeDevice n = dev.nativeDevice();
        auto& d = ctx.details();
        const auto pd = static_cast<VkPhysicalDevice>(n.physicalDevice);
        d["decode_codecs"] = codecList(familyCodecs(pd, n.videoDecodeFamily));
        d["encode_codecs"] = codecList(familyCodecs(pd, n.videoEncodeFamily));
        H264Plan plan = planH264(n);
        if (!plan.reason.empty()) throw rhi::Error(plan.reason);
        d["h264_decode_output"] = !plan.canDecode ? "n/a"
                                  : plan.coincide ? "DPB and output coincide"
                                                  : "distinct output";

        const DurationPreset dur = ctx.settings().duration;
        const double seconds = dur == DurationPreset::Smoke     ? 0.5
                               : dur == DurationPreset::Quick   ? 2.0
                               : dur == DurationPreset::Extreme ? 8.0
                                                                : 4.0;
        dev.waitIdle();
        H264Result r;
        {
            H264RoundTrip trip(ctx, n, std::move(plan));
            r = trip.run(seconds);
        }
        dev.waitIdle();

        if (!r.failure.empty()) {
            ctx.setVerdict(results::Verdict::Fail, "H.264 round trip failed: " + r.failure + ".");
            return;
        }
        ctx.addMetric("h264_encode_fps", "H.264 1080p encode", r.encodeFps, "fps");
        if (r.decoded) ctx.addMetric("h264_decode_fps", "H.264 1080p decode", r.decodeFps, "fps");
        if (r.verified) {
            ctx.addMetric("h264_psnr", "H.264 round-trip quality (luma PSNR)", r.psnrMean, "dB");
            ctx.addMetric("h264_psnr_min", "H.264 worst frame (luma PSNR)", r.psnrMin, "dB");
        }
        const std::string speeds =
            r.decoded
                ? std::format("H.264 1080p: encode {:.0f} fps, decode {:.0f} fps", r.encodeFps, r.decodeFps)
                : std::format("H.264 1080p: encode {:.0f} fps", r.encodeFps);
        if (!r.decoded || !r.verified) {
            ctx.setVerdict(results::Verdict::Pass, speeds + " (" + r.decodeNote + ").");
            return;
        }
        if (r.psnrMin < 25.0) {
            ctx.setVerdict(
                results::Verdict::Fail,
                std::format("{}, but the decoded frames do not match the source (worst frame {:.1f} dB "
                            "PSNR): the encoder or decoder produced wrong pictures.",
                            speeds, r.psnrMin));
            return;
        }
        if (r.psnrMin < 30.0) {
            ctx.setVerdict(
                results::Verdict::Warn,
                std::format("{}; the decoded frames are of low quality (worst frame {:.1f} dB PSNR).", speeds,
                            r.psnrMin));
            return;
        }
        ctx.setVerdict(
            results::Verdict::Pass,
            std::format("{}; the decoded frames match the source ({:.1f} dB PSNR).", speeds, r.psnrMean));
#else
        (void)ctx;
#endif
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::VideoTest)
