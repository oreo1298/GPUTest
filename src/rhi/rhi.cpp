#include "gt/rhi/rhi.h"

#include <format>

namespace gt::rhi {

std::string_view apiName(Api api) {
    switch (api) {
    case Api::Vulkan: return "vulkan";
    case Api::D3D12: return "d3d12";
    case Api::D3D11: return "d3d11";
    case Api::OpenGL: return "opengl";
    }
    return "?";
}

std::string_view apiDisplayName(Api api) {
    switch (api) {
    case Api::Vulkan: return "Vulkan";
    case Api::D3D12: return "Direct3D 12";
    case Api::D3D11: return "Direct3D 11";
    case Api::OpenGL: return "OpenGL";
    }
    return "?";
}

std::optional<Api> parseApi(std::string_view name) {
    if (name == "vulkan" || name == "vk") return Api::Vulkan;
    if (name == "d3d12" || name == "dx12") return Api::D3D12;
    if (name == "d3d11" || name == "dx11") return Api::D3D11;
    if (name == "opengl" || name == "gl") return Api::OpenGL;
    return std::nullopt;
}

std::vector<Api> compiledApis() {
    std::vector<Api> apis;
#ifdef GT_HAS_VULKAN
    apis.push_back(Api::Vulkan);
#endif
#ifdef GT_HAS_D3D12
    apis.push_back(Api::D3D12);
#endif
#ifdef GT_HAS_D3D11
    apis.push_back(Api::D3D11);
#endif
#ifdef GT_HAS_OPENGL
    apis.push_back(Api::OpenGL);
#endif
    return apis;
}

const FormatInfo& formatInfo(Format f) {
    // name, bytesPerBlock, blockDim, components, depth, stencil, srgb, float, int
    static const FormatInfo kTable[] = {
        {"UNKNOWN", 0, 1, 0, false, false, false, false, false},
        {"R8_UNORM", 1, 1, 1, false, false, false, false, false},
        {"RG8_UNORM", 2, 1, 2, false, false, false, false, false},
        {"RGBA8_UNORM", 4, 1, 4, false, false, false, false, false},
        {"RGBA8_SRGB", 4, 1, 4, false, false, true, false, false},
        {"BGRA8_UNORM", 4, 1, 4, false, false, false, false, false},
        {"BGRA8_SRGB", 4, 1, 4, false, false, true, false, false},
        {"R16_FLOAT", 2, 1, 1, false, false, false, true, false},
        {"RG16_FLOAT", 4, 1, 2, false, false, false, true, false},
        {"RGBA16_FLOAT", 8, 1, 4, false, false, false, true, false},
        {"RGBA16_UNORM", 8, 1, 4, false, false, false, false, false},
        {"R32_FLOAT", 4, 1, 1, false, false, false, true, false},
        {"RG32_FLOAT", 8, 1, 2, false, false, false, true, false},
        {"RGB32_FLOAT", 12, 1, 3, false, false, false, true, false},
        {"RGBA32_FLOAT", 16, 1, 4, false, false, false, true, false},
        {"R32_UINT", 4, 1, 1, false, false, false, false, true},
        {"RG32_UINT", 8, 1, 2, false, false, false, false, true},
        {"RGBA32_UINT", 16, 1, 4, false, false, false, false, true},
        {"R32_SINT", 4, 1, 1, false, false, false, false, true},
        {"R16_UINT", 2, 1, 1, false, false, false, false, true},
        {"R8_UINT", 1, 1, 1, false, false, false, false, true},
        {"R11G11B10_FLOAT", 4, 1, 3, false, false, false, true, false},
        {"RGB10A2_UNORM", 4, 1, 4, false, false, false, false, false},
        {"D16_UNORM", 2, 1, 1, true, false, false, false, false},
        {"D24_UNORM_S8_UINT", 4, 1, 2, true, true, false, false, false},
        {"D32_FLOAT", 4, 1, 1, true, false, false, true, false},
        {"D32_FLOAT_S8_UINT", 8, 1, 2, true, true, false, true, false},
        {"BC1_UNORM", 8, 4, 4, false, false, false, false, false},
        {"BC3_UNORM", 16, 4, 4, false, false, false, false, false},
        {"BC5_UNORM", 16, 4, 2, false, false, false, false, false},
        {"BC7_UNORM", 16, 4, 4, false, false, false, false, false},
    };
    static_assert(std::size(kTable) == static_cast<std::size_t>(Format::Count));
    const auto i = static_cast<std::size_t>(f);
    return i < std::size(kTable) ? kTable[i] : kTable[0];
}

std::string_view adapterTypeName(AdapterType t) {
    switch (t) {
    case AdapterType::Discrete: return "discrete";
    case AdapterType::Integrated: return "integrated";
    case AdapterType::Virtual: return "virtual";
    case AdapterType::Cpu: return "cpu";
    case AdapterType::Unknown: return "unknown";
    }
    return "unknown";
}

std::string PciAddress::toString() const {
    return std::format("{:04x}:{:02x}:{:02x}.{:x}", domain, bus, device, function);
}

std::string_view vendorName(std::uint32_t vendorId) {
    switch (vendorId) {
    case vendor::kAmd: return "AMD";
    case vendor::kNvidia: return "NVIDIA";
    case vendor::kIntel: return "Intel";
    case vendor::kMicrosoft: return "Microsoft";
    case vendor::kMesa: return "Mesa";
    case 0x13B5: return "ARM";
    case 0x5143: return "Qualcomm";
    case 0x1AE0: return "Google";
    case 0x106B: return "Apple";
    case 0x1010: return "Imagination";
    case 0x15AD: return "VMware";
    case 0x1AF4: return "Red Hat (virtio)";
    default: return "Unknown";
    }
}

std::string_view queueTypeName(QueueType q) {
    switch (q) {
    case QueueType::Graphics: return "graphics";
    case QueueType::Compute: return "compute";
    case QueueType::Copy: return "copy";
    }
    return "?";
}

// --- backend dispatch ---------------------------------------------------------------
#ifdef GT_HAS_VULKAN
namespace vk {
std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);
} // namespace vk
#endif
#ifdef GT_HAS_D3D12
namespace d3d12 {
std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);
} // namespace d3d12
#endif
#ifdef GT_HAS_D3D11
namespace d3d11 {
std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);
} // namespace d3d11
#endif
#ifdef GT_HAS_OPENGL
namespace gl {
std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);
} // namespace gl
#endif

std::vector<AdapterInfo> enumerateAdapters(Api api, std::string* error) {
    switch (api) {
#ifdef GT_HAS_VULKAN
    case Api::Vulkan: return vk::enumerateAdapters(error);
#endif
#ifdef GT_HAS_D3D12
    case Api::D3D12: return d3d12::enumerateAdapters(error);
#endif
#ifdef GT_HAS_D3D11
    case Api::D3D11: return d3d11::enumerateAdapters(error);
#endif
#ifdef GT_HAS_OPENGL
    case Api::OpenGL: return gl::enumerateAdapters(error);
#endif
    default: break;
    }
    if (error) *error = std::format("{} support is not compiled into this build", apiDisplayName(api));
    return {};
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) {
    switch (info.api) {
#ifdef GT_HAS_VULKAN
    case Api::Vulkan: return vk::createDevice(info);
#endif
#ifdef GT_HAS_D3D12
    case Api::D3D12: return d3d12::createDevice(info);
#endif
#ifdef GT_HAS_D3D11
    case Api::D3D11: return d3d11::createDevice(info);
#endif
#ifdef GT_HAS_OPENGL
    case Api::OpenGL: return gl::createDevice(info);
#endif
    default: break;
    }
    throw Error(std::format("{} support is not compiled into this build", apiDisplayName(info.api)));
}

} // namespace gt::rhi
