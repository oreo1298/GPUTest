/* Upscaler plugin interface (C ABI, MIT license).
 *
 * A plugin is a shared library named "upscaler-<id>.dll" (Windows) or "libupscaler-<id>.so" / "upscaler-<id>.so"
 * in the "plugins" folder next to the benchmark runner, exporting
 *
 *     const gt_upscaler_plugin* gt_upscaler_plugin_get(void);
 *
 * The host loads plugins only when the user enables them. Plugins receive native Vulkan or Direct3D 12
 * objects and record their work into the host's command buffer / command list; every image must be left in
 * the layout (Vulkan) or resource state (D3D12) it was given in, and any pipeline state may be changed.
 * Vendor upscalers (Intel XeSS, NVIDIA DLSS, ...) ship their runtimes under their own licenses: plugins load
 * them at run time from where the user put them.
 */
#ifndef GT_UPSCALER_PLUGIN_H
#define GT_UPSCALER_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GT_UPSCALER_ABI_VERSION 1u
#define GT_UPSCALER_ENTRY "gt_upscaler_plugin_get"

enum { GT_UPSCALER_API_VULKAN = 1u, GT_UPSCALER_API_D3D12 = 2u };
enum { GT_UPSCALER_OK = 0, GT_UPSCALER_UNSUPPORTED = 1, GT_UPSCALER_ERROR = 2 };
/* Output-to-render ratio per axis: 1.3, 1.5, 1.7, 2.0. */
enum {
    GT_UPSCALER_QUALITY_ULTRA_QUALITY = 0,
    GT_UPSCALER_QUALITY_QUALITY = 1,
    GT_UPSCALER_QUALITY_BALANCED = 2,
    GT_UPSCALER_QUALITY_PERFORMANCE = 3
};

typedef struct gt_upscaler_device {
    uint32_t api;                 /* GT_UPSCALER_API_* */
    void* instance;               /* VkInstance */
    void* physical_device;        /* VkPhysicalDevice */
    void* device;                 /* VkDevice / ID3D12Device* */
    void* queue;                  /* graphics VkQueue / ID3D12CommandQueue* */
    uint32_t queue_family;        /* Vulkan graphics queue family */
    void* get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
} gt_upscaler_device;

typedef struct gt_upscaler_image {
    void* resource;  /* VkImage / ID3D12Resource* */
    void* view;      /* VkImageView of the whole image (Vulkan) */
    uint32_t format; /* VkFormat / DXGI_FORMAT */
    uint32_t layout; /* VkImageLayout / D3D12_RESOURCE_STATES the image is in (leave it so) */
    uint32_t width, height;
} gt_upscaler_image;

typedef struct gt_upscaler_create_info {
    uint32_t max_render_width, max_render_height;
    uint32_t output_width, output_height;
    uint32_t quality; /* GT_UPSCALER_QUALITY_* the host renders at */
    uint32_t color_format, output_format; /* VkFormat / DXGI_FORMAT */
} gt_upscaler_create_info;

typedef struct gt_upscaler_frame {
    void* command_buffer;     /* VkCommandBuffer / ID3D12GraphicsCommandList* (graphics, outside render passes) */
    gt_upscaler_image color;  /* shader readable, render size, RGBA8 UNORM (non-linear sRGB-like values) */
    gt_upscaler_image depth;  /* shader readable, render size, R32 float, 1 = far */
    gt_upscaler_image motion; /* shader readable, render size, RG16 float: offset in render pixels from this
                                 frame's position to the previous frame's */
    gt_upscaler_image output; /* storage image (GENERAL / UNORDERED_ACCESS), output size, RGBA8 UNORM */
    float jitter_x, jitter_y; /* offset of this frame's sample positions from the pixel centres, render
                                 pixels, in [-0.5, 0.5] (a +x jitter moves the image content by -x) */
    float frame_time_ms;
    uint32_t reset;           /* 1: no usable history (first frame, cut) */
} gt_upscaler_frame;

typedef struct gt_upscaler_plugin {
    uint32_t abi_version; /* GT_UPSCALER_ABI_VERSION */
    const char* id;       /* short id, e.g. "xess" */
    const char* name;     /* display name, e.g. "Intel XeSS" */
    const char* version;  /* plugin version, plus the loaded runtime's once known */
    uint32_t apis;        /* bit mask of GT_UPSCALER_API_* */
    uint32_t temporal;    /* 1 when it uses depth, motion vectors, jitter and history */
    /* GT_UPSCALER_OK when usable on `device`; otherwise writes the reason (UTF-8) into `message`. */
    int (*query)(const gt_upscaler_device* device, char* message, size_t message_size);
    int (*create)(const gt_upscaler_device* device, const gt_upscaler_create_info* info, void** context,
                  char* message, size_t message_size);
    /* Records the upscale of one frame into frame->command_buffer. */
    int (*evaluate)(void* context, const gt_upscaler_frame* frame, char* message, size_t message_size);
    /* Called once the GPU has finished every command recorded by evaluate(). */
    void (*destroy)(void* context);
} gt_upscaler_plugin;

typedef const gt_upscaler_plugin* (*gt_upscaler_plugin_entry)(void);

/* Plugins define GT_UPSCALER_PLUGIN before including this header and implement the entry point with
 * GT_UPSCALER_EXPORT. */
#ifdef GT_UPSCALER_PLUGIN
#if defined(_WIN32)
#define GT_UPSCALER_EXPORT __declspec(dllexport)
#else
#define GT_UPSCALER_EXPORT __attribute__((visibility("default")))
#endif
GT_UPSCALER_EXPORT const gt_upscaler_plugin* gt_upscaler_plugin_get(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GT_UPSCALER_PLUGIN_H */
