#include "vk_device.h"

#include "gt/app_info.h"
#include "gt/log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace gt::rhi::vk {
namespace {

std::mutex g_instanceMutex;
Instance g_instance;
bool g_volkLoaded = false;
std::atomic<std::uint64_t> g_validationErrors{0};
std::atomic<std::uint64_t> g_validationWarnings{0};
std::atomic<int> g_muted{0};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (g_muted.load(std::memory_order_relaxed) > 0) return VK_FALSE;
    const char* id = data && data->pMessageIdName ? data->pMessageIdName : "";
    const char* msg = data && data->pMessage ? data->pMessage : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validationErrors.fetch_add(1, std::memory_order_relaxed);
        log::error("[vulkan-validation] {} {}", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        // Performance warnings are informative, not correctness problems.
        if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
            log::debug("[vulkan-perf] {} {}", id, msg);
        } else {
            g_validationWarnings.fetch_add(1, std::memory_order_relaxed);
            log::warn("[vulkan-validation] {} {}", id, msg);
        }
    } else {
        log::trace("[vulkan] {}", msg);
    }
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

void destroyLocked() {
    if (g_instance.instance == VK_NULL_HANDLE) return;
    if (g_instance.messenger) vkDestroyDebugUtilsMessengerEXT(g_instance.instance, g_instance.messenger, nullptr);
    vkDestroyInstance(g_instance.instance, nullptr);
    g_instance = Instance{};
}

void createLocked(bool validation) {
    if (!g_volkLoaded) {
        if (volkInitialize() != VK_SUCCESS) throw Error("Vulkan loader (libvulkan / vulkan-1.dll) not found");
        g_volkLoaded = true;
    }
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1)
        throw Error(std::format("Vulkan loader {} is too old (1.1 required)", versionString(loaderVersion)));

    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());

    std::vector<const char*> enabled;
    auto want = [&](const char* name) {
        if (hasExtension(exts, name)) {
            enabled.push_back(name);
            return true;
        }
        return false;
    };
    Instance inst;
    inst.debugUtils = want(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    inst.surfaceExtensions = want(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    want("VK_KHR_win32_surface");
#else
    want("VK_KHR_xlib_surface");
    want("VK_KHR_xcb_surface");
    want("VK_KHR_wayland_surface");
#endif
    want(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    inst.swapchainColorspace = want(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    want(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const bool portability = want(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (validation) {
        std::uint32_t lcount = 0;
        vkEnumerateInstanceLayerProperties(&lcount, nullptr);
        std::vector<VkLayerProperties> props(lcount);
        vkEnumerateInstanceLayerProperties(&lcount, props.data());
        auto it = std::find_if(props.begin(), props.end(), [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        const bool found = it != props.end();
        if (found && (VK_API_VERSION_MINOR(it->specVersion) < 4 || VK_API_VERSION_PATCH(it->specVersion) < VK_HEADER_VERSION)) {
            // An old layer reports structures/extensions it does not know as errors. Validate the
            // subset it understands instead (CI uses an up-to-date layer).
            inst.validationHeaderLimit = VK_API_VERSION_MINOR(it->specVersion) * 1000 + VK_API_VERSION_PATCH(it->specVersion);
            log::warn("validation layer {} is older than the Vulkan headers {}.{}; limiting the API to what it knows",
                      versionString(it->specVersion), 4, VK_HEADER_VERSION);
        }
        if (found) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            inst.validation = true;
            if (!inst.debugUtils) {
                // The validation layer itself provides debug utils.
                enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                inst.debugUtils = true;
            }
        } else {
            log::warn("Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = app::kDisplayName;
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = app::kId;
    app.engineVersion = app.applicationVersion;
    app.apiVersion = VK_API_VERSION_1_4;
    if (inst.validationHeaderLimit && inst.validationHeaderLimit < 4000) app.apiVersion = VK_API_VERSION_1_3;
    inst.apiVersion = app.apiVersion;

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    ci.ppEnabledExtensionNames = enabled.data();
    ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (portability) ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    if (inst.validation) ci.pNext = &dbg;

    VkResult r = vkCreateInstance(&ci, nullptr, &inst.instance);
    if (r != VK_SUCCESS) throwResult(r, "vkCreateInstance");
    volkLoadInstance(inst.instance);
    if (inst.validation && vkCreateDebugUtilsMessengerEXT) {
        vkCreateDebugUtilsMessengerEXT(inst.instance, &dbg, nullptr, &inst.messenger);
    }
    g_instance = inst;
}

} // namespace

Instance& instance(bool validation) {
    std::lock_guard lock(g_instanceMutex);
    if (g_instance.instance != VK_NULL_HANDLE && validation && !g_instance.validation) destroyLocked();
    if (g_instance.instance == VK_NULL_HANDLE) createLocked(validation);
    return g_instance;
}

ScopedValidationMute::ScopedValidationMute(bool active) : m_active(active) {
    if (m_active) g_muted.fetch_add(1);
}
ScopedValidationMute::~ScopedValidationMute() {
    if (m_active) g_muted.fetch_sub(1);
}

ValidationCounts globalValidationCounts() {
    return {g_validationErrors.load(std::memory_order_relaxed), g_validationWarnings.load(std::memory_order_relaxed)};
}

// --- adapter enumeration --------------------------------------------------------------
AdapterInfo describePhysicalDevice(VkPhysicalDevice pd, std::uint32_t index);

AdapterInfo describePhysicalDevice(VkPhysicalDevice pd, std::uint32_t index) {
    std::uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());

    VkPhysicalDeviceProperties base{};
    vkGetPhysicalDeviceProperties(pd, &base);

    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceIDProperties idProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties driverProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDevicePCIBusInfoPropertiesEXT pciProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    props.pNext = &idProps;
    void** tail = &idProps.pNext;
    const bool hasDriverProps =
        base.apiVersion >= VK_API_VERSION_1_2 || hasExtension(exts, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    if (hasDriverProps) {
        *tail = &driverProps;
        tail = &driverProps.pNext;
    }
    const bool hasPci = hasExtension(exts, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    if (hasPci) {
        *tail = &pciProps;
        tail = &pciProps.pNext;
    }
    vkGetPhysicalDeviceProperties2(pd, &props);

    AdapterInfo a;
    a.api = Api::Vulkan;
    a.index = index;
    a.name = base.deviceName;
    a.vendorId = base.vendorID;
    a.deviceId = base.deviceID;
    switch (base.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: a.type = AdapterType::Discrete; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: a.type = AdapterType::Integrated; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: a.type = AdapterType::Virtual; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: a.type = AdapterType::Cpu; break;
    default: a.type = AdapterType::Unknown; break;
    }
    a.apiVersion = versionString(base.apiVersion);
    VkDriverId driverId = hasDriverProps ? driverProps.driverID : static_cast<VkDriverId>(0);
    a.driverVersion = decodeDriverVersion(base.vendorID, driverId, base.driverVersion);
    if (hasDriverProps) {
        a.driverName = driverProps.driverName;
        a.driverInfo = driverProps.driverInfo;
    }
    if (idProps.deviceLUIDValid) {
        std::array<std::uint8_t, 8> luid{};
        std::memcpy(luid.data(), idProps.deviceLUID, 8);
        a.luid = luid;
    }
    std::array<std::uint8_t, 16> uuid{};
    std::memcpy(uuid.data(), idProps.deviceUUID, 16);
    a.uuid = uuid;
    if (hasPci) a.pci = PciAddress{pciProps.pciDomain, pciProps.pciBus, pciProps.pciDevice, pciProps.pciFunction};

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) a.dedicatedMemory += mem.memoryHeaps[i].size;
        else a.sharedMemory += mem.memoryHeaps[i].size;
    }
    return a;
}

std::vector<AdapterInfo> enumerateAdapters(std::string* error) {
    std::vector<AdapterInfo> out;
    try {
        Instance& inst = instance(false);
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(inst.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> pds(count);
        check(vkEnumeratePhysicalDevices(inst.instance, &count, pds.data()), "vkEnumeratePhysicalDevices");
        for (std::uint32_t i = 0; i < count; ++i) out.push_back(describePhysicalDevice(pds[i], i));
        if (out.empty() && error) *error = "no Vulkan devices found";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return out;
}

} // namespace gt::rhi::vk
