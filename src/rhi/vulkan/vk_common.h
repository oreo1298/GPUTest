#pragma once

#include "gt/rhi/rhi.h"

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS 1
#endif
#include <volk.h>

#include <vk_mem_alloc.h>

#include <format>
#include <string>

namespace gt::rhi::vk {

std::string_view resultName(VkResult r);

/// Throws DeviceLostError / OutOfMemoryError / Error for a failed VkResult.
[[noreturn]] void throwResult(VkResult r, const char* what);

inline void check(VkResult r, const char* what) {
    if (r < VK_SUCCESS) throwResult(r, what);
}

VkFormat toVk(Format f);
Format fromVk(VkFormat f);
VkCompareOp toVk(CompareOp op);
VkSampleCountFlagBits toVkSamples(std::uint32_t count);

std::string decodeDriverVersion(std::uint32_t vendorId, VkDriverId driverId, std::uint32_t version);
std::string versionString(std::uint32_t apiVersion);

/// Shared instance (created on first use, lives until process exit).
struct Instance {
    VkInstance instance = VK_NULL_HANDLE;
    std::uint32_t apiVersion = VK_API_VERSION_1_1;
    bool debugUtils = false;
    bool validation = false;
    bool surfaceExtensions = false;
    bool swapchainColorspace = false;
    /// Header version (VK_HEADER_VERSION-style patch, e.g. 275) of the validation layer when it
    /// is older than our headers; extensions newer than it are not enabled while validating.
    std::uint32_t validationHeaderLimit = 0;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

/// Returns the instance, creating it with/without validation on first call.
/// Requesting validation after a non-validated instance exists recreates it.
Instance& instance(bool validation);
ValidationCounts globalValidationCounts();

/// While alive, validation messages are dropped. Only used around purely informational
/// queries (capability listing) when the installed layer is older than our headers and
/// would flag every structure it does not know.
class ScopedValidationMute {
public:
    explicit ScopedValidationMute(bool active);
    ~ScopedValidationMute();
    ScopedValidationMute(const ScopedValidationMute&) = delete;
    ScopedValidationMute& operator=(const ScopedValidationMute&) = delete;

private:
    bool m_active;
};

} // namespace gt::rhi::vk
