#include "vk_common.h"

namespace gt::rhi::vk {

std::string_view resultName(VkResult r) {
    switch (r) {
#define GT_CASE(x) \
    case x: return #x;
        GT_CASE(VK_SUCCESS)
        GT_CASE(VK_NOT_READY)
        GT_CASE(VK_TIMEOUT)
        GT_CASE(VK_EVENT_SET)
        GT_CASE(VK_EVENT_RESET)
        GT_CASE(VK_INCOMPLETE)
        GT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        GT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        GT_CASE(VK_ERROR_INITIALIZATION_FAILED)
        GT_CASE(VK_ERROR_DEVICE_LOST)
        GT_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        GT_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        GT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        GT_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        GT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        GT_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        GT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        GT_CASE(VK_ERROR_FRAGMENTED_POOL)
        GT_CASE(VK_ERROR_UNKNOWN)
        GT_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        GT_CASE(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        GT_CASE(VK_ERROR_FRAGMENTATION)
        GT_CASE(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        GT_CASE(VK_ERROR_SURFACE_LOST_KHR)
        GT_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        GT_CASE(VK_SUBOPTIMAL_KHR)
        GT_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        GT_CASE(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR)
        GT_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
        GT_CASE(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
#undef GT_CASE
    default: return "VK_RESULT_UNKNOWN";
    }
}

void throwResult(VkResult r, const char* what) {
    std::string msg = std::format("{} failed: {}", what, resultName(r));
    if (r == VK_ERROR_DEVICE_LOST) throw DeviceLostError(msg);
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) throw OutOfMemoryError(msg);
    throw Error(msg);
}

VkFormat toVk(Format f) {
    switch (f) {
    case Format::Unknown: return VK_FORMAT_UNDEFINED;
    case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case Format::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case Format::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
    case Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::R32_SINT: return VK_FORMAT_R32_SINT;
    case Format::R16_UINT: return VK_FORMAT_R16_UINT;
    case Format::R8_UINT: return VK_FORMAT_R8_UINT;
    case Format::R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::D16_UNORM: return VK_FORMAT_D16_UNORM;
    case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    case Format::D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::Count: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format fromVk(VkFormat f) {
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(Format::Count); ++i) {
        if (toVk(static_cast<Format>(i)) == f) return static_cast<Format>(i);
    }
    return Format::Unknown;
}

VkCompareOp toVk(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkSampleCountFlagBits toVkSamples(std::uint32_t count) {
    switch (count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

std::string versionString(std::uint32_t v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

std::string decodeDriverVersion(std::uint32_t vendorId, VkDriverId driverId, std::uint32_t v) {
    // Proprietary NVIDIA: 10.8.8.6 bits
    if (vendorId == vendor::kNvidia && driverId != VK_DRIVER_ID_MESA_NVK) {
        return std::format("{}.{:02}.{:02}.{:02}", (v >> 22) & 0x3ff, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);
    }
#ifdef _WIN32
    // Intel Windows: 18.14 bits
    if (vendorId == vendor::kIntel) return std::format("{}.{}", v >> 14, v & 0x3fff);
#endif
    return versionString(v);
}

} // namespace gt::rhi::vk
