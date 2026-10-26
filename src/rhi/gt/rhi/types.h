#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gt::rhi {

enum class Api : std::uint8_t { Vulkan, D3D12, D3D11, OpenGL };
inline constexpr std::array<Api, 4> kAllApis{Api::Vulkan, Api::D3D12, Api::D3D11, Api::OpenGL};
std::string_view apiName(Api api);        ///< "vulkan", "d3d12", "d3d11", "opengl"
std::string_view apiDisplayName(Api api); ///< "Vulkan", "Direct3D 12", ...
std::optional<Api> parseApi(std::string_view name);
/// Backends compiled into this build.
std::vector<Api> compiledApis();

// --- errors -------------------------------------------------------------------
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
/// Device removed / lost (TDR, driver reset, hang). The device must be recreated.
struct DeviceLostError : Error {
    using Error::Error;
};
/// Out of device memory while creating a resource.
struct OutOfMemoryError : Error {
    using Error::Error;
};

// --- bit flags helper ---------------------------------------------------------
template <typename E>
struct EnableFlags : std::false_type {};
template <typename E>
    requires EnableFlags<E>::value
constexpr E operator|(E a, E b) {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}
template <typename E>
    requires EnableFlags<E>::value
constexpr E operator&(E a, E b) {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}
template <typename E>
    requires EnableFlags<E>::value
constexpr bool any(E e) {
    return static_cast<std::underlying_type_t<E>>(e) != 0;
}

// --- formats ------------------------------------------------------------------
enum class Format : std::uint16_t {
    Unknown,
    R8_UNORM, RG8_UNORM, RGBA8_UNORM, RGBA8_SRGB, BGRA8_UNORM, BGRA8_SRGB,
    R16_FLOAT, RG16_FLOAT, RGBA16_FLOAT, RGBA16_UNORM,
    R32_FLOAT, RG32_FLOAT, RGB32_FLOAT, RGBA32_FLOAT,
    R32_UINT, RG32_UINT, RGBA32_UINT, R32_SINT, R16_UINT, R8_UINT,
    R11G11B10_FLOAT, RGB10A2_UNORM,
    D16_UNORM, D24_UNORM_S8_UINT, D32_FLOAT, D32_FLOAT_S8_UINT,
    BC1_UNORM, BC3_UNORM, BC5_UNORM, BC7_UNORM,
    Count
};

struct FormatInfo {
    const char* name;
    std::uint32_t bytesPerBlock; ///< bytes per pixel, or per 4x4 block when compressed
    std::uint32_t blockDim;      ///< 1, or 4 for block-compressed formats
    std::uint32_t components;
    bool depth;
    bool stencil;
    bool srgb;
    bool isFloat;
    bool isInteger;
};
const FormatInfo& formatInfo(Format f);
inline bool isDepthFormat(Format f) { return formatInfo(f).depth; }

// --- adapters -----------------------------------------------------------------
enum class AdapterType : std::uint8_t { Discrete, Integrated, Virtual, Cpu, Unknown };
std::string_view adapterTypeName(AdapterType t);

struct PciAddress {
    std::uint32_t domain = 0, bus = 0, device = 0, function = 0;
    std::string toString() const; ///< "0000:01:00.0"
    bool operator==(const PciAddress&) const = default;
};

namespace vendor {
inline constexpr std::uint32_t kAmd = 0x1002;
inline constexpr std::uint32_t kNvidia = 0x10DE;
inline constexpr std::uint32_t kIntel = 0x8086;
inline constexpr std::uint32_t kMicrosoft = 0x1414; // WARP / basic render
inline constexpr std::uint32_t kMesa = 0x10005;     // lavapipe / llvmpipe (VK_VENDOR_ID_MESA)
} // namespace vendor
std::string_view vendorName(std::uint32_t vendorId);

struct AdapterInfo {
    Api api = Api::Vulkan;
    std::uint32_t index = 0; ///< index in this API's enumeration order
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subsysId = 0; ///< 0 if the API does not expose it
    std::uint32_t revision = 0;
    AdapterType type = AdapterType::Unknown;
    std::string driverName;    ///< e.g. "NVIDIA", "radv", "llvmpipe"
    std::string driverInfo;    ///< free-form driver string
    std::string driverVersion; ///< human-readable, vendor-decoded
    std::string apiVersion;    ///< "1.4.318", "12_2", "4.6"
    std::uint64_t dedicatedMemory = 0;
    std::uint64_t sharedMemory = 0;
    std::optional<PciAddress> pci;
    std::optional<std::array<std::uint8_t, 8>> luid;
    std::optional<std::array<std::uint8_t, 16>> uuid;
    bool isSoftware() const { return type == AdapterType::Cpu || vendorId == vendor::kMesa || vendorId == vendor::kMicrosoft; }
};

/// One row of the capability explorer: grouped name/value pairs.
struct Capability {
    std::string group;  ///< "Extensions", "Features: Vulkan 1.2", "Limits", ...
    std::string name;
    std::string value;  ///< "yes"/"no" for booleans, numbers or text otherwise
    bool supported = true;
};

/// Features the shared (RHI-level) tests rely on. Feature tests query native
/// APIs directly for anything more specific.
struct DeviceFeatures {
    bool timestamps = false;
    bool timestampsOnComputeQueue = false;
    bool asyncComputeQueue = false; ///< a separate compute-capable queue exists
    bool copyQueue = false;         ///< a separate transfer-only queue exists
    bool geometryShader = false;
    bool tessellation = false;
    bool wireframe = false;
    bool anisotropicFiltering = false;
    bool textureCompressionBC = false;
    bool shaderFloat64 = false;
    bool shaderFloat16 = false;
    bool shaderInt64 = false;
    bool shaderInt16 = false;
    bool shaderInt8 = false;
    bool storage16Bit = false;
    bool storage8Bit = false;
    bool integerDotProduct = false;
    bool cooperativeMatrix = false; ///< VK_KHR_cooperative_matrix (see Device::cooperativeMatrixConfigs)
    bool cooperativeVector = false; ///< VK_NV_cooperative_vector (reported only)
    bool subgroupOps = false;
    bool atomicInt64 = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    std::uint32_t maxBindlessTextures = 0; ///< TextureTable capacity limit (0 = no texture tables)
    bool descriptorBuffer = false;         ///< VK_EXT_descriptor_buffer (reported only)
    bool rayQuery = false;
    bool rayTracingPipeline = false;
    bool opacityMicromap = false; ///< VK_EXT_opacity_micromap
    bool shaderExecutionReordering = false; ///< VK_EXT_ray_tracing_invocation_reorder (HitObject / ReorderThread)
    bool shaderExecutionReorderingNv = false; ///< only the NVIDIA-specific SER extension is available
    bool meshShader = false;
    bool taskShader = false; ///< mesh pipelines may have a task (amplification) stage
    bool variableRateShading = false;
    std::uint32_t maxShadingRate = 1; ///< largest square per-draw shading rate (2 or 4) with variableRateShading
    bool multiDrawIndirect = false;
    bool drawIndirectCount = false;
    bool pipelineStatistics = false;
    bool memoryBudget = false;
    bool resizableBar = false; ///< a device-local, host-visible heap larger than 256 MiB exists
    std::uint32_t subgroupSize = 0;
    std::uint32_t minSubgroupSize = 0; ///< range selectable with subgroup size control (= subgroupSize without it)
    std::uint32_t maxSubgroupSize = 0;
    std::uint32_t maxTexture2D = 0;
    std::uint32_t maxMsaaSamples = 1;
    std::uint32_t maxComputeSharedMemory = 0;
    std::array<std::uint32_t, 3> maxComputeWorkgroupSize{};
    std::uint32_t maxComputeWorkgroupInvocations = 0;
    std::uint64_t maxBufferSize = 0;
    std::uint64_t maxStorageBufferRange = 0;
    double timestampPeriodNs = 0.0;
};

struct MemoryBudget {
    std::uint64_t localUsage = 0;
    std::uint64_t localBudget = 0;
    std::uint64_t nonLocalUsage = 0;
    std::uint64_t nonLocalBudget = 0;
    bool valid = false;
};

} // namespace gt::rhi
