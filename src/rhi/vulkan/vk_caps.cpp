// Capability listing for the capability explorer and the system report.
#include "vk_device.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if __has_include("vk_caps_gen.inc")
#include "vk_caps_gen.inc"
#define GT_HAVE_VK_CAPS_GEN 1
#endif

namespace gt::rhi::vk {
namespace {

std::string sizeString(std::uint64_t bytes) {
    if (bytes >= (1ull << 30)) return std::format("{:.2f} GiB", static_cast<double>(bytes) / (1ull << 30));
    if (bytes >= (1ull << 20)) return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1ull << 20));
    return std::format("{} B", bytes);
}

std::string memoryFlags(VkMemoryPropertyFlags f) {
    std::string s;
    auto add = [&](VkMemoryPropertyFlags bit, const char* name) {
        if (f & bit) s += s.empty() ? name : std::string(" | ") + name;
    };
    add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL");
    add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE");
    add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT");
    add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED");
    add(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED");
    add(VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED");
    return s.empty() ? "none" : s;
}

std::string queueFlags(VkQueueFlags f) {
    std::string s;
    auto add = [&](VkQueueFlags bit, const char* name) {
        if (f & bit) s += s.empty() ? name : std::string(" | ") + name;
    };
    add(VK_QUEUE_GRAPHICS_BIT, "GRAPHICS");
    add(VK_QUEUE_COMPUTE_BIT, "COMPUTE");
    add(VK_QUEUE_TRANSFER_BIT, "TRANSFER");
    add(VK_QUEUE_SPARSE_BINDING_BIT, "SPARSE");
    add(VK_QUEUE_PROTECTED_BIT, "PROTECTED");
    add(VK_QUEUE_VIDEO_DECODE_BIT_KHR, "VIDEO_DECODE");
    add(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "VIDEO_ENCODE");
    add(VK_QUEUE_OPTICAL_FLOW_BIT_NV, "OPTICAL_FLOW");
    return s.empty() ? "none" : s;
}

} // namespace

std::vector<Capability> VulkanDevice::capabilities() const {
    std::vector<Capability> out;
    auto add = [&](std::string group, std::string name, std::string value, bool supported = true) {
        out.push_back({std::move(group), std::move(name), std::move(value), supported});
    };

    add("Device", "Name", m_adapter.name);
    add("Device", "API version", versionString(m_props.apiVersion));
    add("Device", "Driver", std::format("{} {}", m_adapter.driverName, m_adapter.driverVersion));
    add("Device", "Driver info", m_adapter.driverInfo);
    add("Device", "Vendor ID", std::format("0x{:04x} ({})", m_adapter.vendorId, vendorName(m_adapter.vendorId)));
    add("Device", "Device ID", std::format("0x{:04x}", m_adapter.deviceId));
    add("Device", "Type", std::string(adapterTypeName(m_adapter.type)));
    if (m_adapter.pci) add("Device", "PCI address", m_adapter.pci->toString());
    add("Device", "Push descriptors", m_pushDescriptors ? "yes" : "no", m_pushDescriptors);
    add("Device", "Timeline semaphores", m_timeline ? "yes" : "no", m_timeline);
    add("Device", "Resizable BAR (large host-visible VRAM heap)", m_rebar ? "yes" : "no", m_rebar);

    // Extensions: every supported one, marked when we enabled it.
    for (const auto& e : m_supportedExtensions) {
        const bool enabled = extensionEnabled(e.extensionName);
        add("Extensions", e.extensionName,
            std::format("rev {}{}", e.specVersion, enabled ? ", enabled" : ""), true);
    }

    for (std::uint32_t i = 0; i < m_memProps.memoryHeapCount; ++i) {
        const auto& h = m_memProps.memoryHeaps[i];
        add("Memory heaps", std::format("Heap {}", i),
            std::format("{}{}", sizeString(h.size), (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", device-local" : ""));
    }
    for (std::uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        const auto& t = m_memProps.memoryTypes[i];
        add("Memory types", std::format("Type {} (heap {})", i, t.heapIndex), memoryFlags(t.propertyFlags));
    }

    std::uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, fams.data());
    for (std::uint32_t i = 0; i < qcount; ++i) {
        add("Queue families", std::format("Family {}", i),
            std::format("{} x{}, timestamp bits {}", queueFlags(fams[i].queueFlags), fams[i].queueCount,
                        fams[i].timestampValidBits));
    }

#ifdef GT_HAVE_VK_CAPS_GEN
    const ScopedValidationMute mute(m_validationHeaderLimit != 0);
    auto hasExt = [this](const char* n) { return extensionSupported(n); };
    gen::collectFeatures(m_physical, m_apiVersion, hasExt, out);
    gen::collectProperties(m_physical, m_apiVersion, hasExt, out);
#endif
    return out;
}

} // namespace gt::rhi::vk
