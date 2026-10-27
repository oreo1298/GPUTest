#include "video_vk.h"

#if GT_HAS_VULKAN

#include "gt/bench/video_streams.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace gt::bench::vkvideo {

std::vector<std::uint8_t> makeSource() {
    std::vector<std::uint8_t> nv12(kSourceFrame * kGop);
    // Rounds half to even, like numpy's rint in scripts/make-video-streams.py.
    auto clamp8 = [](double v) {
        return static_cast<std::uint8_t>(std::clamp(std::nearbyint(v), 16.0, 235.0));
    };
    for (std::uint32_t f = 0; f < kGop; ++f) {
        std::uint8_t* y = nv12.data() + f * kSourceFrame;
        std::uint8_t* uv = y + kSourceLuma;
        const std::uint32_t dx = 6 * f, dy = 3 * f; // the pattern pans by (6, 3) pixels per frame
        for (std::uint32_t py = 0; py < kImageHeight; ++py) {
            const std::uint32_t yi = std::min(py, kHeight - 1) + dy;
            const double v = yi;
            for (std::uint32_t px = 0; px < kImageWidth; ++px) {
                const std::uint32_t xi = px + dx;
                const double x = xi;
                const int bx = static_cast<int>(xi >> 6), by = static_cast<int>(yi >> 6);
                double g = 120.0 + 45.0 * std::sin(x * 0.011) * std::cos(v * 0.017);
                g += ((bx + by) & 1) ? 30.0 : -30.0;
                if ((bx * 7 + by * 3) % 5 == 0) g += ((xi >> 2) & 1) ? 18.0 : -18.0; // fine stripes
                // Texture noise fixed to the pattern, so inter frames can predict it.
                std::uint32_t h = (xi * 73856093u) ^ (yi * 19349663u);
                h = (h ^ (h >> 13)) * 1274126177u;
                g += static_cast<double>((h >> 24) & 15u) - 8.0;
                // Grain that changes every frame (so inter frames carry residual, as camera footage does).
                std::uint32_t t =
                    (px * 2654435761u) ^ (std::min(py, kHeight - 1) * 2246822519u) ^ (f * 3266489917u);
                t = (t ^ (t >> 15)) * 2246822519u;
                g += static_cast<double>((t >> 28) & 7u) - 4.0;
                y[std::size_t(py) * kImageWidth + px] = clamp8(g);
            }
        }
        for (std::uint32_t py = 0; py < kImageHeight / 2; ++py) {
            const double v = std::min(2 * py, kHeight - 2) + dy;
            for (std::uint32_t px = 0; px < kImageWidth / 2; ++px) {
                const double x = 2.0 * px + dx;
                std::uint8_t* c = uv + std::size_t(py) * kImageWidth + 2 * px;
                c[0] = clamp8(128.0 + 40.0 * std::sin(x * 0.006 + v * 0.002));
                c[1] = clamp8(128.0 + 40.0 * std::cos(v * 0.008 - x * 0.001));
            }
        }
    }
    return nv12;
}

double lumaPsnr(const std::uint8_t* decoded, const std::vector<std::uint8_t>& source, std::uint32_t f) {
    const std::uint8_t* src = source.data() + f * kSourceFrame;
    double sum = 0.0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const double d =
                double(decoded[std::size_t(y) * kWidth + x]) - double(src[std::size_t(y) * kImageWidth + x]);
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

namespace {

template <typename Info>
std::vector<Info> familyProperties(VkPhysicalDevice pd, VkStructureType type) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, nullptr);
    std::vector<Info> info(count, Info{type});
    std::vector<VkQueueFamilyProperties2> props(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    for (std::uint32_t i = 0; i < count; ++i)
        props[i].pNext = &info[i];
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, props.data());
    return info;
}

} // namespace

VkVideoCodecOperationFlagsKHR familyCodecs(VkPhysicalDevice pd, std::uint32_t family) {
    if (family == ~0u) return 0;
    const auto video = familyProperties<VkQueueFamilyVideoPropertiesKHR>(
        pd, VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR);
    return family < video.size() ? video[family].videoCodecOperations : 0;
}

bool familyHasStatusQueries(VkPhysicalDevice pd, std::uint32_t family) {
    if (family == ~0u) return false;
    const auto status = familyProperties<VkQueueFamilyQueryResultStatusPropertiesKHR>(
        pd, VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR);
    return family < status.size() && status[family].queryResultStatusSupport == VK_TRUE;
}

std::string codecList(VkVideoCodecOperationFlagsKHR ops) {
    std::string s;
    auto add = [&](VkVideoCodecOperationFlagsKHR bits, const char* name) {
        if (ops & bits) s += (s.empty() ? "" : ", ") + std::string(name);
    };
    add(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "H.264");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "H.265");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, "AV1");
    add(VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9");
    return s.empty() ? "none" : s;
}

std::string blocker(const rhi::NativeDevice& n) {
    if (n.api != rhi::Api::Vulkan || !n.device) return "no native Vulkan device";
    if (!n.videoEncodeQueue && !n.videoDecodeQueue) return "the driver exposes no Vulkan Video queues";
    if (!vkQueueSubmit2 || !vkCmdPipelineBarrier2 || !vkWaitSemaphores)
        return "Vulkan Video here needs Vulkan 1.3 (synchronization2, timeline semaphores)";
    if (!vkCreateVideoSessionKHR || !vkCmdBeginVideoCodingKHR || !vkGetPhysicalDeviceVideoCapabilitiesKHR)
        return "VK_KHR_video_queue is not enabled";
    return {};
}

std::optional<VkVideoFormatPropertiesKHR> pickFormat(VkPhysicalDevice pd,
                                                     const VkVideoProfileInfoKHR& profile,
                                                     VkImageUsageFlags usage, VkImageUsageFlags wantUsage) {
    VkVideoProfileListInfoKHR list{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    list.profileCount = 1;
    list.pProfiles = &profile;
    VkPhysicalDeviceVideoFormatInfoKHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    info.pNext = &list;
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
    VkVideoFormatPropertiesKHR out = *best;
    out.pNext = nullptr;
    return out;
}

VkVideoPictureResourceInfoKHR picture(VkImageView view, std::uint32_t layer, VkExtent2D coded) {
    VkVideoPictureResourceInfoKHR p{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    p.codedOffset = {0, 0};
    p.codedExtent = coded;
    p.baseArrayLayer = layer;
    p.imageViewBinding = view;
    return p;
}

// --- VideoRig ----------------------------------------------------------------------------------------

VideoRig::VideoRig(const rhi::NativeDevice& n)
    : m_n(n), m_dev(static_cast<VkDevice>(n.device)), m_pd(static_cast<VkPhysicalDevice>(n.physicalDevice)) {
    vkGetPhysicalDeviceMemoryProperties(m_pd, &m_memory);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ci.pNext = &type;
    check(vkCreateSemaphore(m_dev, &ci, nullptr, &m_timeline), "vkCreateSemaphore");
}

VideoRig::~VideoRig() {
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

VkDeviceMemory VideoRig::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags need,
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

namespace {

void uniqueFamilies(std::vector<std::uint32_t>& f) {
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end()), f.end());
}

} // namespace

VideoRig::Image VideoRig::image(const VkVideoFormatPropertiesKHR& fmt, VkImageUsageFlags usage,
                                VkImageUsageFlags viewUsage, std::uint32_t layers,
                                const VkVideoProfileInfoKHR& profile, std::vector<std::uint32_t> families) {
    uniqueFamilies(families);
    VkVideoProfileListInfoKHR list{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    list.profileCount = 1;
    list.pProfiles = &profile;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &list;
    ci.imageType = fmt.imageType;
    ci.format = fmt.format;
    ci.extent = {kImageWidth, kImageHeight, 1};
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
        allocate(req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dedicated, "video picture");
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

VideoRig::Buffer VideoRig::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                                  const VkVideoProfileInfoKHR* profile, std::vector<std::uint32_t> families) {
    uniqueFamilies(families);
    VkVideoProfileListInfoKHR list{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    list.profileCount = 1;
    list.pProfiles = profile;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.pNext = profile ? &list : nullptr;
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

void VideoRig::flush(const Buffer& b) {
    const VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, b.memory, 0, VK_WHOLE_SIZE};
    check(vkFlushMappedMemoryRanges(m_dev, 1, &r), "vkFlushMappedMemoryRanges");
}

void VideoRig::invalidate(const Buffer& b) {
    const VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, b.memory, 0, VK_WHOLE_SIZE};
    check(vkInvalidateMappedMemoryRanges(m_dev, 1, &r), "vkInvalidateMappedMemoryRanges");
}

void VideoRig::bindSessionMemory(VkVideoSessionKHR session) {
    std::uint32_t count = 0;
    check(vkGetVideoSessionMemoryRequirementsKHR(m_dev, session, &count, nullptr),
          "vkGetVideoSessionMemoryRequirementsKHR");
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(
        count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    check(vkGetVideoSessionMemoryRequirementsKHR(m_dev, session, &count, reqs.data()),
          "vkGetVideoSessionMemoryRequirementsKHR");
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    for (const auto& r : reqs) {
        VkBindVideoSessionMemoryInfoKHR b{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
        b.memoryBindIndex = r.memoryBindIndex;
        b.memory =
            allocate(r.memoryRequirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, "video session");
        b.memoryOffset = 0;
        b.memorySize = r.memoryRequirements.size;
        binds.push_back(b);
    }
    if (!binds.empty())
        check(vkBindVideoSessionMemoryKHR(m_dev, session, static_cast<std::uint32_t>(binds.size()),
                                          binds.data()),
              "vkBindVideoSessionMemoryKHR");
}

VkCommandBuffer VideoRig::begin(std::uint32_t family) {
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

void VideoRig::end(VkCommandBuffer cb) {
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

void VideoRig::submit(VkQueue queue, VkCommandBuffer cb) {
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = m_timeline;
    wait.value = m_value;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal = wait;
    signal.value = m_value + 1;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = m_value > 0 ? 1u : 0u;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    check(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
    ++m_value;
}

void VideoRig::wait() {
    if (!waitTimeline(20'000'000'000ull)) throw rhi::Error("the video engine did not finish within 20 s");
}

bool VideoRig::waitTimeline(std::uint64_t timeoutNs) {
    if (m_value == 0) return true;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &m_value;
    const VkResult r = vkWaitSemaphores(m_dev, &wi, timeoutNs);
    if (r == VK_TIMEOUT) return false;
    if (r == VK_ERROR_DEVICE_LOST) return true; // nothing can be in use any more
    check(r, "vkWaitSemaphores");
    return true;
}

double VideoRig::timeSubmissions(TestContext& ctx, VkQueue queue, VkCommandBuffer cb,
                                 std::uint32_t framesPerSubmit, double seconds) {
    submit(queue, cb); // warm-up
    wait();
    std::uint64_t frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    do {
        ctx.checkStop();
        submit(queue, cb);
        wait();
        frames += framesPerSubmit;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    } while (elapsed < seconds);
    return double(frames) / elapsed;
}

// --- Barrier -----------------------------------------------------------------------------------------

Barrier& Barrier::image(VkImage img, std::uint32_t layers, VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
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

Barrier& Barrier::global(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    memory.push_back(b);
    return *this;
}

void Barrier::record(VkCommandBuffer cb) const {
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = static_cast<std::uint32_t>(memory.size());
    di.pMemoryBarriers = memory.data();
    di.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
    di.pImageMemoryBarriers = images.data();
    vkCmdPipelineBarrier2(cb, &di);
}

namespace {

constexpr std::uint32_t kRepeats = 8; ///< GOPs per timed submission

bool extentFits(const VkVideoCapabilitiesKHR& c, VkExtent2D e) {
    return c.minCodedExtent.width <= e.width && c.minCodedExtent.height <= e.height &&
           c.maxCodedExtent.width >= e.width && c.maxCodedExtent.height >= e.height &&
           e.width <= kImageWidth && e.height <= kImageHeight;
}

} // namespace

// --- Decoder -----------------------------------------------------------------------------------------

std::string Decoder::prepare() {
    const rhi::NativeDevice& n = m_rig.native();
    const VkPhysicalDevice pd = m_rig.physicalDevice();
    const VkDevice dev = m_rig.device();
    m_decodeCaps.pNext = m_codec.capabilities();
    m_caps.pNext = &m_decodeCaps;
    const VkResult r = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &m_codec.profile(), &m_caps);
    m_caps.pNext = nullptr;
    m_decodeCaps.pNext = nullptr;
    if (r != VK_SUCCESS)
        return std::format("the decoder does not take {} ({})", m_codec.name(), resultName(r));
    const VkExtent2D coded = m_codec.codedExtent();
    if (!extentFits(m_caps, coded))
        return std::format("the {} decoder does not take {}x{} pictures", m_codec.name(), coded.width,
                           coded.height);
    if (m_caps.maxDpbSlots < kGop || m_caps.maxActiveReferencePictures < m_codec.maxActiveReferences())
        return std::format("the {} decoder has too few reference pictures ({} slots, {} active)",
                           m_codec.name(), m_caps.maxDpbSlots, m_caps.maxActiveReferencePictures);
    if (std::string why = m_codec.checkCapabilities(); !why.empty()) return why;

    std::optional<VkVideoFormatPropertiesKHR> dpbFormat, outFormat;
    const VkVideoProfileInfoKHR& profile = m_codec.profile();
    m_coincide = (m_decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    if (m_coincide) {
        dpbFormat = pickFormat(
            pd, profile, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        outFormat = dpbFormat;
    } else if (m_decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) {
        dpbFormat = pickFormat(pd, profile, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, 0);
        outFormat =
            pickFormat(pd, profile, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    if (!dpbFormat || !outFormat)
        return std::format("the {} decoder reports no picture formats", m_codec.name());
    m_readable = outFormat->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
                 (outFormat->imageUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    si.queueFamilyIndex = n.videoDecodeFamily;
    si.pVideoProfile = &profile;
    si.pictureFormat = outFormat->format;
    si.maxCodedExtent = coded;
    si.referencePictureFormat = dpbFormat->format;
    si.maxDpbSlots = kGop;
    si.maxActiveReferencePictures = std::max(m_codec.maxActiveReferences(), 1u);
    si.pStdHeaderVersion = &m_caps.stdHeaderVersion;
    check(vkCreateVideoSessionKHR(dev, &si, nullptr, &m_session), "vkCreateVideoSessionKHR (decode)");
    m_rig.onCleanup([dev, s = m_session] { vkDestroyVideoSessionKHR(dev, s, nullptr); });
    m_rig.bindSessionMemory(m_session);
    m_params = m_codec.createParameters(dev, m_session);
    m_rig.onCleanup([dev, p = m_params] { vkDestroyVideoSessionParametersKHR(dev, p, nullptr); });

    const VkImageUsageFlags read = m_readable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0;
    const std::vector<std::uint32_t> readFamilies = {n.videoDecodeFamily,
                                                     m_readable ? n.queueFamily : n.videoDecodeFamily};
    if (m_coincide) {
        const VkImageUsageFlags video =
            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
        m_dpb = m_rig.image(*dpbFormat, video | read, video, kGop, profile, readFamilies);
        m_out = m_dpb;
    } else {
        m_dpb = m_rig.image(*dpbFormat, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, kGop, profile, {n.videoDecodeFamily});
        m_out = m_rig.image(*outFormat, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | read,
                            VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, kGop, profile, readFamilies);
    }

    // Each frame's bitstream at an aligned offset, padded with zeros to the size alignment.
    const VkDeviceSize offsetAlign = std::max<VkDeviceSize>(m_caps.minBitstreamBufferOffsetAlignment, 1);
    const VkDeviceSize sizeAlign = std::max<VkDeviceSize>(m_caps.minBitstreamBufferSizeAlignment, 1);
    VkDeviceSize cursor = 0;
    for (std::uint32_t f = 0; f < m_codec.frameCount(); ++f) {
        const VkDeviceSize off = alignUp(cursor, offsetAlign);
        m_offsets.push_back(off);
        m_ranges.push_back(alignUp(m_codec.frameData(f).size(), sizeAlign));
        cursor = off + m_ranges.back();
    }
    m_stream =
        m_rig.buffer(cursor, VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, true, &profile, {n.videoDecodeFamily});
    std::memset(m_stream.mapped, 0, static_cast<std::size_t>(cursor));
    for (std::uint32_t f = 0; f < m_codec.frameCount(); ++f) {
        const auto data = m_codec.frameData(f);
        std::memcpy(m_stream.mapped + m_offsets[f], data.data(), data.size());
    }
    m_rig.flush(m_stream);

    if (familyHasStatusQueries(pd, n.videoDecodeFamily)) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.pNext = &profile;
        qi.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
        qi.queryCount = kGop;
        check(vkCreateQueryPool(dev, &qi, nullptr, &m_status), "vkCreateQueryPool (decode status)");
        m_rig.onCleanup([dev, q = m_status] { vkDestroyQueryPool(dev, q, nullptr); });
    }
    return {};
}

void Decoder::record(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query) {
    const VkExtent2D coded = m_codec.codedExtent();
    const VkVideoPictureResourceInfoKHR setupPic = picture(m_dpb.view, f, coded);
    const VkVideoPictureResourceInfoKHR outPic = m_coincide ? setupPic : picture(m_out.view, f, coded);
    const std::vector<std::uint32_t> refs = m_codec.references(f);
    const DecodeCodec::FrameInfo info = m_codec.frameInfo(f);
    std::vector<VkVideoPictureResourceInfoKHR> refPics;
    for (const std::uint32_t r : refs)
        refPics.push_back(picture(m_dpb.view, r, coded));

    // Begin: the references (active slots) and the picture being set up (bound without a slot).
    std::vector<VkVideoReferenceSlotInfoKHR> beginSlots(1 + refs.size(),
                                                        {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR});
    beginSlots[0].slotIndex = -1;
    beginSlots[0].pPictureResource = &setupPic;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        beginSlots[i + 1].slotIndex = static_cast<std::int32_t>(refs[i]);
        beginSlots[i + 1].pPictureResource = &refPics[i];
    }
    VkVideoBeginCodingInfoKHR bi{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    bi.videoSession = m_session;
    bi.videoSessionParameters = m_params;
    bi.referenceSlotCount = static_cast<std::uint32_t>(beginSlots.size());
    bi.pReferenceSlots = beginSlots.data();
    vkCmdBeginVideoCodingKHR(cb, &bi);
    if (reset) {
        VkVideoCodingControlInfoKHR ctl{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
        ctl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        vkCmdControlVideoCodingKHR(cb, &ctl);
    }

    VkVideoReferenceSlotInfoKHR setup{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup.pNext = info.setup;
    setup.slotIndex = static_cast<std::int32_t>(f);
    setup.pPictureResource = &setupPic;
    std::vector<VkVideoReferenceSlotInfoKHR> refSlots(refs.size(),
                                                      {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR});
    for (std::size_t i = 0; i < refs.size(); ++i) {
        refSlots[i].pNext = info.refs[i];
        refSlots[i].slotIndex = static_cast<std::int32_t>(refs[i]);
        refSlots[i].pPictureResource = &refPics[i];
    }
    VkVideoDecodeInfoKHR di{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    di.pNext = info.picture;
    di.srcBuffer = m_stream.buffer;
    di.srcBufferOffset = m_offsets[f];
    di.srcBufferRange = m_ranges[f];
    di.dstPictureResource = outPic;
    di.pSetupReferenceSlot = &setup;
    di.referenceSlotCount = static_cast<std::uint32_t>(refSlots.size());
    di.pReferenceSlots = refSlots.empty() ? nullptr : refSlots.data();
    if (query) vkCmdBeginQuery(cb, m_status, f, 0);
    vkCmdDecodeVideoKHR(cb, &di);
    if (query) vkCmdEndQuery(cb, m_status, f);
    VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end);
    // Later frames read this one's picture.
    Barrier()
        .global(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)
        .record(cb);
}

std::string Decoder::decodeOnce() {
    const rhi::NativeDevice& n = m_rig.native();
    VkCommandBuffer cb = m_rig.begin(n.videoDecodeFamily);
    const VkAccessFlags2 rw = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
    Barrier b;
    b.image(m_dpb.image, m_dpb.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
            VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, rw);
    if (!m_coincide)
        b.image(m_out.image, m_out.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
    b.record(cb);
    const bool query = m_status != VK_NULL_HANDLE;
    const std::uint32_t frames = m_codec.frameCount();
    if (query) vkCmdResetQueryPool(cb, m_status, 0, kGop);
    for (std::uint32_t f = 0; f < frames; ++f)
        record(cb, f, f == 0, query);
    m_rig.end(cb);
    m_rig.submit(static_cast<VkQueue>(n.videoDecodeQueue), cb);
    m_rig.wait();
    if (!query) return {};
    std::vector<std::int64_t> status(frames);
    check(vkGetQueryPoolResults(m_rig.device(), m_status, 0, frames, status.size() * sizeof(std::int64_t),
                                status.data(), sizeof(std::int64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                    VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
          "vkGetQueryPoolResults (decode status)");
    for (std::uint32_t f = 0; f < frames; ++f)
        if (status[f] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
            return std::format("the {} decoder reported status {} for frame {}", m_codec.name(), status[f],
                               f);
    return {};
}

double Decoder::time(TestContext& ctx, double seconds) {
    const rhi::NativeDevice& n = m_rig.native();
    VkCommandBuffer cb = m_rig.begin(n.videoDecodeFamily);
    // Covers the previous submission of this command buffer too (same queue, submission order).
    Barrier()
        .global(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)
        .record(cb);
    const std::uint32_t frames = m_codec.frameCount();
    for (std::uint32_t r = 0; r < kRepeats; ++r)
        for (std::uint32_t f = 0; f < frames; ++f)
            record(cb, f, false, false);
    m_rig.end(cb);
    return m_rig.timeSubmissions(ctx, static_cast<VkQueue>(n.videoDecodeQueue), cb, frames * kRepeats,
                                 seconds);
}

std::vector<std::uint8_t> Decoder::readback() {
    const rhi::NativeDevice& n = m_rig.native();
    const std::uint32_t frames = m_codec.frameCount();
    VideoRig::Buffer rb =
        m_rig.buffer(kFrameBytes * frames, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, nullptr, {n.queueFamily});
    VkCommandBuffer cb = m_rig.begin(n.queueFamily);
    Barrier()
        .image(m_out.image, m_out.layers,
               m_coincide ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT)
        .record(cb);
    std::vector<VkBufferImageCopy> copies;
    for (std::uint32_t f = 0; f < frames; ++f) {
        VkBufferImageCopy y{};
        y.bufferOffset = f * kFrameBytes;
        y.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, f, 1};
        y.imageExtent = {kWidth, kHeight, 1};
        VkBufferImageCopy c = y;
        c.bufferOffset = f * kFrameBytes + std::size_t(kWidth) * kHeight;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        c.imageExtent = {kWidth / 2, kHeight / 2, 1};
        copies.push_back(y);
        copies.push_back(c);
    }
    vkCmdCopyImageToBuffer(cb, m_out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    Barrier()
        .global(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_READ_BIT)
        .record(cb);
    m_rig.end(cb);
    m_rig.submit(static_cast<VkQueue>(n.queue), cb);
    m_rig.wait();
    m_rig.invalidate(rb);
    return std::vector<std::uint8_t>(rb.mapped, rb.mapped + kFrameBytes * frames);
}

// --- Encoder -----------------------------------------------------------------------------------------

namespace {
constexpr VkDeviceSize kStride = 4ull << 20; ///< bitstream space per encoded frame
}

std::string Encoder::prepare() {
    const rhi::NativeDevice& n = m_rig.native();
    const VkPhysicalDevice pd = m_rig.physicalDevice();
    const VkDevice dev = m_rig.device();
    const VkVideoProfileInfoKHR& profile = m_codec.profile();
    m_encodeCaps.pNext = m_codec.capabilities();
    m_caps.pNext = &m_encodeCaps;
    const VkResult r = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &profile, &m_caps);
    m_caps.pNext = nullptr;
    m_encodeCaps.pNext = nullptr;
    if (r != VK_SUCCESS)
        return std::format("the encoder does not take {} ({})", m_codec.name(), resultName(r));
    const VkExtent2D coded = m_codec.codedExtent();
    if (!extentFits(m_caps, coded))
        return std::format("the {} encoder does not take {}x{} pictures", m_codec.name(), coded.width,
                           coded.height);
    if (m_caps.maxDpbSlots < 2 || m_caps.maxActiveReferencePictures < 1)
        return std::format("the {} encoder cannot make inter frames (no reference pictures)", m_codec.name());
    constexpr VkVideoEncodeFeedbackFlagsKHR kFeedback =
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
    if ((m_encodeCaps.supportedEncodeFeedbackFlags & kFeedback) != kFeedback)
        return std::format("the {} encoder does not report how many bytes it wrote", m_codec.name());
    if (kStride % std::max<VkDeviceSize>(m_caps.minBitstreamBufferOffsetAlignment, 1) ||
        kStride % std::max<VkDeviceSize>(m_caps.minBitstreamBufferSizeAlignment, 1))
        return std::format("the {} encoder's bitstream alignment is larger than 4 MiB", m_codec.name());
    if (std::string why = m_codec.checkCapabilities(m_caps, m_encodeCaps); !why.empty()) return why;
    auto src =
        pickFormat(pd, profile, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!src || src->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
        !(src->imageUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return std::format("the {} encoder takes no NV12 input that can be uploaded to", m_codec.name());
    auto dpb = pickFormat(pd, profile, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 0);
    if (!dpb) return std::format("the {} encoder reports no reference picture format", m_codec.name());
    m_srcFormat = src->format;

    VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    si.queueFamilyIndex = n.videoEncodeFamily;
    si.pVideoProfile = &profile;
    si.pictureFormat = src->format;
    si.maxCodedExtent = coded;
    si.referencePictureFormat = dpb->format;
    si.maxDpbSlots = 2;
    si.maxActiveReferencePictures = 1;
    si.pStdHeaderVersion = &m_caps.stdHeaderVersion;
    check(vkCreateVideoSessionKHR(dev, &si, nullptr, &m_session), "vkCreateVideoSessionKHR (encode)");
    m_rig.onCleanup([dev, s = m_session] { vkDestroyVideoSessionKHR(dev, s, nullptr); });
    m_rig.bindSessionMemory(m_session);
    m_params = m_codec.createParameters(dev, m_session);
    m_rig.onCleanup([dev, p = m_params] { vkDestroyVideoSessionParametersKHR(dev, p, nullptr); });

    m_src = m_rig.image(*src, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, kGop, profile,
                        {n.queueFamily, n.videoEncodeFamily});
    m_dpb = m_rig.image(*dpb, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                        VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 2, profile, {n.videoEncodeFamily});
    // The verified pass writes host-visible memory; timed passes write elsewhere.
    m_stream = m_rig.buffer(kStride * kGop, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, true, &profile,
                            {n.videoEncodeFamily});
    m_timedStream = m_rig.buffer(kStride * kGop, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, false, &profile,
                                 {n.videoEncodeFamily});
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fb{
        VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
    fb.pNext = &profile;
    fb.encodeFeedbackFlags = kFeedback;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.pNext = &fb;
    qi.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
    qi.queryCount = kGop;
    check(vkCreateQueryPool(dev, &qi, nullptr, &m_feedback), "vkCreateQueryPool (encode feedback)");
    m_rig.onCleanup([dev, q = m_feedback] { vkDestroyQueryPool(dev, q, nullptr); });

    m_rateControlDisabled =
        (m_encodeCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) != 0;
    m_rateControl.rateControlMode = m_rateControlDisabled ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR
                                                          : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    return {};
}

void Encoder::upload(const std::vector<std::uint8_t>& nv12) {
    const rhi::NativeDevice& n = m_rig.native();
    VideoRig::Buffer staging =
        m_rig.buffer(nv12.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, nullptr, {n.queueFamily});
    std::memcpy(staging.mapped, nv12.data(), nv12.size());
    m_rig.flush(staging);
    VkCommandBuffer cb = m_rig.begin(n.queueFamily);
    Barrier()
        .image(m_src.image, kGop, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT)
        .record(cb);
    std::vector<VkBufferImageCopy> copies;
    for (std::uint32_t f = 0; f < kGop; ++f) {
        VkBufferImageCopy y{};
        y.bufferOffset = f * kSourceFrame;
        y.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, f, 1};
        y.imageExtent = {kImageWidth, kImageHeight, 1};
        VkBufferImageCopy c = y;
        c.bufferOffset = f * kSourceFrame + kSourceLuma;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        c.imageExtent = {kImageWidth / 2, kImageHeight / 2, 1};
        copies.push_back(y);
        copies.push_back(c);
    }
    vkCmdCopyBufferToImage(cb, staging.buffer, m_src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    m_rig.end(cb);
    m_rig.submit(static_cast<VkQueue>(n.queue), cb);
    m_rig.wait();
    m_uploaded = true;
}

void Encoder::record(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query, VkBuffer dst) {
    const VkExtent2D coded = m_codec.codedExtent();
    const bool idr = f == 0;
    const std::uint32_t setupSlot = f % 2, refSlot = (f + 1) % 2; // slot and layer f % 2 hold frame f
    const VkVideoPictureResourceInfoKHR src = picture(m_src.view, f, coded);
    const VkVideoPictureResourceInfoKHR setupPic = picture(m_dpb.view, setupSlot, coded);
    const VkVideoPictureResourceInfoKHR refPic = picture(m_dpb.view, refSlot, coded);

    VkVideoReferenceSlotInfoKHR beginSlots[2]{{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR},
                                              {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR}};
    beginSlots[0].slotIndex = -1;
    beginSlots[0].pPictureResource = &setupPic;
    beginSlots[1].slotIndex = static_cast<std::int32_t>(refSlot);
    beginSlots[1].pPictureResource = &refPic;
    VkVideoBeginCodingInfoKHR bi{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    bi.pNext = reset ? nullptr : &m_rateControl; // must match the session's rate control state
    bi.videoSession = m_session;
    bi.videoSessionParameters = m_params;
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

    const EncodeCodec::FrameInfo info = m_codec.frameInfo(f, m_rateControlDisabled);
    VkVideoReferenceSlotInfoKHR setup{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup.pNext = info.setup;
    setup.slotIndex = static_cast<std::int32_t>(setupSlot);
    setup.pPictureResource = &setupPic;
    VkVideoReferenceSlotInfoKHR ref{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    ref.pNext = info.ref;
    ref.slotIndex = static_cast<std::int32_t>(refSlot);
    ref.pPictureResource = &refPic;
    VkVideoEncodeInfoKHR ei{VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    ei.pNext = info.picture;
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

std::string Encoder::encodeOnce() {
    if (!m_uploaded) throw rhi::Error("Encoder::encodeOnce before upload");
    const rhi::NativeDevice& n = m_rig.native();
    VkCommandBuffer cb = m_rig.begin(n.videoEncodeFamily);
    Barrier()
        .image(m_src.image, kGop, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
               VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR)
        .image(m_dpb.image, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
               VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)
        .record(cb);
    vkCmdResetQueryPool(cb, m_feedback, 0, kGop);
    for (std::uint32_t f = 0; f < kGop; ++f)
        record(cb, f, f == 0, true, m_stream.buffer);
    Barrier()
        .global(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT)
        .record(cb);
    m_rig.end(cb);
    m_rig.submit(static_cast<VkQueue>(n.videoEncodeQueue), cb);
    m_rig.wait();

    struct Feedback {
        std::uint64_t offset, bytes;
        std::int64_t status;
    };
    std::vector<Feedback> fb(kGop);
    check(vkGetQueryPoolResults(
              m_rig.device(), m_feedback, 0, kGop, fb.size() * sizeof(Feedback), fb.data(), sizeof(Feedback),
              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
          "vkGetQueryPoolResults (encode feedback)");
    m_rig.invalidate(m_stream);
    m_output.clear();
    for (std::uint32_t f = 0; f < kGop; ++f) {
        if (fb[f].status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
            return fb[f].status == VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR
                       ? std::format("{} frame {} did not fit in 4 MiB of bitstream", m_codec.name(), f)
                       : std::format("the {} encoder reported status {} for frame {}", m_codec.name(),
                                     fb[f].status, f);
        if (fb[f].bytes == 0 || fb[f].offset + fb[f].bytes > kStride)
            return std::format("the {} encoder reported {} bytes at offset {} for frame {}", m_codec.name(),
                               fb[f].bytes, fb[f].offset, f);
        const std::uint8_t* p = m_stream.mapped + f * kStride + fb[f].offset;
        m_output.emplace_back(p, p + fb[f].bytes);
    }
    return {};
}

double Encoder::time(TestContext& ctx, double seconds) {
    const rhi::NativeDevice& n = m_rig.native();
    VkCommandBuffer cb = m_rig.begin(n.videoEncodeFamily);
    Barrier()
        .global(VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR)
        .record(cb);
    for (std::uint32_t r = 0; r < kRepeats; ++r)
        for (std::uint32_t f = 0; f < kGop; ++f)
            record(cb, f, false, false, m_timedStream.buffer);
    m_rig.end(cb);
    return m_rig.timeSubmissions(ctx, static_cast<VkQueue>(n.videoEncodeQueue), cb, kGop * kRepeats, seconds);
}

std::vector<std::uint8_t> Encoder::parameters(bool* overrides) {
    VkVideoEncodeSessionParametersGetInfoKHR gi{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR};
    gi.pNext = m_codec.parametersGetInfo();
    gi.videoSessionParameters = m_params;
    VkVideoEncodeSessionParametersFeedbackInfoKHR fb{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
    std::size_t size = 0;
    check(vkGetEncodedVideoSessionParametersKHR(m_rig.device(), &gi, &fb, &size, nullptr),
          "vkGetEncodedVideoSessionParametersKHR");
    std::vector<std::uint8_t> bytes(size);
    if (size)
        check(vkGetEncodedVideoSessionParametersKHR(m_rig.device(), &gi, nullptr, &size, bytes.data()),
              "vkGetEncodedVideoSessionParametersKHR");
    bytes.resize(size);
    if (overrides) *overrides = fb.hasOverrides == VK_TRUE;
    return bytes;
}

// --- one codec, end to end --------------------------------------------------------------------------

bool canDecode(const rhi::NativeDevice& n, const CodecHooks& c) {
    const auto pd = static_cast<VkPhysicalDevice>(n.physicalDevice);
    return n.videoDecodeQueue && c.decodeExtension && hasExtension(n, c.decodeExtension) &&
           (familyCodecs(pd, n.videoDecodeFamily) & c.decodeOp) != 0;
}

bool canEncode(const rhi::NativeDevice& n, const CodecHooks& c) {
    const auto pd = static_cast<VkPhysicalDevice>(n.physicalDevice);
    return n.videoEncodeQueue && c.encodeExtension && hasExtension(n, c.encodeExtension) &&
           (familyCodecs(pd, n.videoEncodeFamily) & c.encodeOp) != 0;
}

namespace {

void decodeReference(TestContext& ctx, const rhi::NativeDevice& n, const CodecHooks& hooks, double seconds,
                     const Progress& progress, CodecResult& res) {
    if (!canDecode(n, hooks)) {
        res.decodeNote = std::format("no Vulkan Video {} decoder", hooks.label);
        return;
    }
    const video::ReferenceStream* ref = video::referenceStream(hooks.key);
    if (!ref) {
        res.decodeNote = "the reference stream is missing from this build";
        return;
    }
    std::string error;
    std::unique_ptr<DecodeCodec> codec = hooks.reference(ref->data, &error);
    if (!codec) {
        res.failure = std::format("the {} reference stream could not be read: {}", hooks.label, error);
        return;
    }
    VideoRig rig(n);
    Decoder dec(rig, *codec);
    if (std::string why = dec.prepare(); !why.empty()) {
        res.decodeNote = why;
        return;
    }
    res.details["decode_stream"] = std::format("{} ({} frames, {} KiB, from {})", codec->name(),
                                               codec->frameCount(), ref->data.size() / 1024, "data/video");
    res.details["decode_output"] = dec.outputMode();
    progress(0.0, std::format("{}: decoding the reference stream", hooks.label));
    if (std::string failure = dec.decodeOnce(); !failure.empty()) {
        res.failure = failure;
        return;
    }
    progress(0.1, std::format("{}: measuring decode throughput", hooks.label));
    res.decodeFps = dec.time(ctx, seconds);
    if (!dec.readable()) {
        res.decodeNote = "decoded pictures cannot be copied out on this driver, so they were not compared";
        return;
    }
    const std::vector<std::uint8_t> frames = dec.readback();
    std::vector<std::uint32_t> wrong;
    nlohmann::json crcs = nlohmann::json::array();
    for (std::uint32_t f = 0; f < codec->frameCount() && f < ref->crc32.size(); ++f) {
        const std::uint32_t crc = video::crc32(std::span(frames).subspan(f * kFrameBytes, kFrameBytes));
        crcs.push_back(std::format("{:08x}", crc));
        if (crc != ref->crc32[f]) wrong.push_back(f);
    }
    res.details["decode_crc32"] = crcs;
    if (!wrong.empty()) {
        std::string list;
        for (const std::uint32_t f : wrong)
            list += (list.empty() ? "" : ", ") + std::to_string(f);
        const double p = lumaPsnr(frames.data() + wrong.front() * kFrameBytes, makeSource(), wrong.front());
        res.failure =
            std::format("{} frame(s) {} differ from the reference decoder's (decoding is bit-exact; "
                        "frame {} is {:.1f} dB from the source, the reference {:.1f} dB)",
                        hooks.label, list, wrong.front(), p, ref->psnr.at(wrong.front()));
        return;
    }
    res.decodeExact = true;
}

void encodeAndCheck(TestContext& ctx, const rhi::NativeDevice& n, const CodecHooks& hooks,
                    const std::vector<std::uint8_t>& source, double seconds, const Progress& progress,
                    CodecResult& res) {
    if (!canEncode(n, hooks)) {
        res.encodeNote = std::format("no Vulkan Video {} encoder", hooks.label);
        return;
    }
    VideoRig rig(n);
    std::vector<std::unique_ptr<EncodeCodec>> codecs =
        hooks.encoders(static_cast<VkPhysicalDevice>(n.physicalDevice));
    std::unique_ptr<Encoder> enc;
    EncodeCodec* codec = nullptr;
    std::string why;
    for (auto& c : codecs) {
        auto e = std::make_unique<Encoder>(rig, *c);
        why = e->prepare();
        if (why.empty()) {
            enc = std::move(e);
            codec = c.get();
            break;
        }
    }
    if (!enc) {
        res.encodeNote = why;
        return;
    }
    progress(0.45, std::format("{}: encoding the source frames", hooks.label));
    enc->upload(source);
    if (std::string failure = enc->encodeOnce(); !failure.empty()) {
        res.failure = failure;
        return;
    }
    std::size_t total = 0;
    for (const auto& f : enc->output())
        total += f.size();
    res.details["encode_profile"] = codec->name();
    res.details["encode_rate_control"] = enc->constantQuantizer() ? codec->quantizer() : "driver default";
    res.details["encode_gop_bytes"] = total;
    res.details["encode_mbit_per_s_at_60fps"] = double(total) * 8.0 / kGop * 60.0 / 1e6;
    progress(0.55, std::format("{}: measuring encode throughput", hooks.label));
    res.encodeFps = enc->time(ctx, seconds);

    if (!canDecode(n, hooks)) {
        res.encodeNote = "no decoder to check the encoded stream with";
        return;
    }
    bool overrides = false;
    const std::vector<std::uint8_t> params = enc->parameters(&overrides);
    res.details["encoder_overrides"] = overrides;
    std::string error;
    std::unique_ptr<DecodeCodec> rt = hooks.encoded(params, enc->output(), &error);
    if (!rt) {
        res.encodeNote = "the encoded stream could not be read back: " + error;
        return;
    }
    Decoder dec(rig, *rt);
    if (why = dec.prepare(); !why.empty()) {
        res.encodeNote = "the encoded stream was not decoded: " + why;
        return;
    }
    progress(0.8, std::format("{}: decoding the encoded stream", hooks.label));
    if (std::string failure = dec.decodeOnce(); !failure.empty()) {
        res.failure = "decoding the GPU-encoded stream: " + failure;
        return;
    }
    if (!dec.readable()) {
        res.encodeNote =
            "decoded pictures cannot be copied out on this driver, so the encoder was not checked";
        return;
    }
    const std::vector<std::uint8_t> frames = dec.readback();
    double sum = 0.0, worst = 1e9;
    nlohmann::json perFrame = nlohmann::json::array();
    for (std::uint32_t f = 0; f < rt->frameCount(); ++f) {
        const double p = lumaPsnr(frames.data() + f * kFrameBytes, source, f);
        perFrame.push_back(std::round(p * 100.0) / 100.0);
        sum += p;
        worst = std::min(worst, p);
    }
    res.details["round_trip_psnr"] = perFrame;
    res.psnr = sum / rt->frameCount();
    res.psnrMin = worst;
}

} // namespace

CodecResult runCodec(TestContext& ctx, const rhi::NativeDevice& n, const CodecHooks& hooks,
                     const std::vector<std::uint8_t>& source, double seconds, const Progress& progress) {
    CodecResult res;
    res.key = hooks.key;
    res.label = hooks.label;
    decodeReference(ctx, n, hooks, seconds, progress, res);
    ctx.checkStop();
    if (res.failure.empty()) encodeAndCheck(ctx, n, hooks, source, seconds, progress, res);
    return res;
}

} // namespace gt::bench::vkvideo

#endif // GT_HAS_VULKAN
