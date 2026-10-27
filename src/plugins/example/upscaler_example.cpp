// Example upscaler plugin: a linear-filtered vkCmdBlitImage from the render-size colour image to the output.
// It exists so CI can exercise the plugin interface end to end (loading, native handles, layout handover);
// it is not packaged.

#define GT_UPSCALER_PLUGIN
#include "upscaler_plugin.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <new>

namespace {

struct Context {
    PFN_vkCmdBlitImage blit = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
};

void say(char* message, size_t size, const char* text) {
    if (message && size) std::snprintf(message, size, "%s", text);
}

int query(const gt_upscaler_device* device, char* message, size_t size) {
    if (device->api != GT_UPSCALER_API_VULKAN) {
        say(message, size, "the example plugin is Vulkan-only");
        return GT_UPSCALER_UNSUPPORTED;
    }
    return GT_UPSCALER_OK;
}

int create(const gt_upscaler_device* device, const gt_upscaler_create_info*, void** context, char* message,
           size_t size) {
    if (query(device, message, size) != GT_UPSCALER_OK) return GT_UPSCALER_UNSUPPORTED;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(device->get_instance_proc_addr);
    auto instance = static_cast<VkInstance>(device->instance);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    auto dev = static_cast<VkDevice>(device->device);
    auto* c = new (std::nothrow) Context;
    if (!c || !gdpa) {
        delete c;
        say(message, size, "vkGetDeviceProcAddr is not available");
        return GT_UPSCALER_ERROR;
    }
    c->blit = reinterpret_cast<PFN_vkCmdBlitImage>(gdpa(dev, "vkCmdBlitImage"));
    c->barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(dev, "vkCmdPipelineBarrier"));
    if (!c->blit || !c->barrier) {
        delete c;
        say(message, size, "missing Vulkan entry points");
        return GT_UPSCALER_ERROR;
    }
    *context = c;
    return GT_UPSCALER_OK;
}

VkImageMemoryBarrier imageBarrier(const gt_upscaler_image& img, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = static_cast<VkImage>(img.resource);
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

int evaluate(void* context, const gt_upscaler_frame* f, char*, size_t) {
    const auto* c = static_cast<const Context*>(context);
    auto cmd = static_cast<VkCommandBuffer>(f->command_buffer);
    const auto colorLayout = static_cast<VkImageLayout>(f->color.layout);
    const auto outLayout = static_cast<VkImageLayout>(f->output.layout);
    // Into transfer layouts, blit, and back to what the host gave us.
    VkImageMemoryBarrier pre[2] = {
        imageBarrier(f->color, colorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT),
        imageBarrier(f->output, outLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
    c->barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
               pre);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(f->color.width), static_cast<int32_t>(f->color.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(f->output.width), static_cast<int32_t>(f->output.height), 1};
    c->blit(cmd, static_cast<VkImage>(f->color.resource), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            static_cast<VkImage>(f->output.resource), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
            VK_FILTER_LINEAR);
    VkImageMemoryBarrier post[2] = {
        imageBarrier(f->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorLayout, 0, VK_ACCESS_MEMORY_READ_BIT),
        imageBarrier(f->output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, outLayout, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)};
    c->barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2,
               post);
    return GT_UPSCALER_OK;
}

void destroy(void* context) { delete static_cast<Context*>(context); }

const gt_upscaler_plugin kPlugin = {GT_UPSCALER_ABI_VERSION,
                                    "example",
                                    "Example (Vulkan blit)",
                                    "1.0",
                                    GT_UPSCALER_API_VULKAN,
                                    0,
                                    query,
                                    create,
                                    evaluate,
                                    destroy};

} // namespace

extern "C" GT_UPSCALER_EXPORT const gt_upscaler_plugin* gt_upscaler_plugin_get(void) { return &kPlugin; }
