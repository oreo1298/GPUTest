#pragma once
// Thin rendering hardware interface shared by the Vulkan, D3D12, D3D11 and
// OpenGL backends. It covers what scenes and synthetic tests need — resources,
// graphics/compute pipelines with slot-based binding, render passes, barriers,
// timestamps, multiple queues, swapchains — and nothing more. Feature tests that
// need API-specific functionality get the native objects from the backend
// (see vulkan/vk_device.h etc.) instead of extending this interface.

#include "gt/rhi/types.h"

#include <functional>
#include <memory>
#include <span>

namespace gt::rhi {

// --- shader stages --------------------------------------------------------------
enum class ShaderStage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Hull = 1u << 1,
    Domain = 1u << 2,
    Geometry = 1u << 3,
    Fragment = 1u << 4,
    Compute = 1u << 5,
    Task = 1u << 6,
    Mesh = 1u << 7,
    AllGraphics = Vertex | Hull | Domain | Geometry | Fragment | Task | Mesh,
    All = AllGraphics | Compute,
};
template <>
struct EnableFlags<ShaderStage> : std::true_type {};

/// Reference to a compiled shader: either an embedded blob (module + entry, see
/// shaders/manifest.txt) or raw bytecode in the backend's native format.
struct ShaderRef {
    std::string module;
    std::string entry;
    std::span<const std::uint8_t> bytecode{}; ///< overrides module/entry when non-empty
    bool valid() const { return !bytecode.empty() || (!module.empty() && !entry.empty()); }
};

// --- buffers ---------------------------------------------------------------------
enum class BufferUsage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    CopySrc = 1u << 5,
    CopyDst = 1u << 6,
    AccelInput = 1u << 7,   ///< vertex / index data read by acceleration-structure builds
    AccelStorage = 1u << 8, ///< backing memory of an acceleration structure (backend internal)
    ShaderTable = 1u << 9,  ///< ray-tracing shader binding table (backend internal)
    Micromap = 1u << 10,    ///< opacity-micromap build input and storage (backend internal)
};
template <>
struct EnableFlags<BufferUsage> : std::true_type {};

enum class MemoryType : std::uint8_t {
    Device,   ///< device-local, not CPU visible
    Upload,   ///< CPU writes, GPU reads (persistently mapped)
    Readback, ///< GPU writes, CPU reads (persistently mapped, cached)
    DeviceUpload, ///< device-local and CPU-visible (ReBAR/SAM); falls back to Upload when unavailable
};

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryType memory = MemoryType::Device;
    std::uint32_t structStride = 0; ///< element stride for structured buffers (D3D); 0 = raw
    const char* name = "";
};

class Buffer {
public:
    virtual ~Buffer() = default;
    const BufferDesc& desc() const { return m_desc; }
    /// Persistently mapped pointer for Upload/Readback/DeviceUpload memory, nullptr otherwise.
    virtual void* mapped() = 0;
    /// True when the allocation actually landed in device-local + host-visible memory.
    virtual bool isDeviceLocalHostVisible() const { return false; }

protected:
    BufferDesc m_desc;
};

// --- textures --------------------------------------------------------------------
enum class TextureUsage : std::uint32_t {
    None = 0,
    Sampled = 1u << 0,
    Storage = 1u << 1,
    RenderTarget = 1u << 2,
    DepthStencil = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
    ShadingRate = 1u << 6, ///< R8_UINT shading-rate image (features().shadingRateImage)
};
template <>
struct EnableFlags<TextureUsage> : std::true_type {};

enum class TextureType : std::uint8_t { Tex2D, Tex2DArray, Tex3D, Cube };

struct ClearValue {
    std::array<float, 4> color{0, 0, 0, 0};
    float depth = 1.0f;
    std::uint8_t stencil = 0;
};

struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    Format format = Format::RGBA8_UNORM;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depthOrLayers = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t sampleCount = 1;
    TextureUsage usage = TextureUsage::Sampled;
    ClearValue optimizedClear{};
    bool concurrentQueues = false; ///< accessed from more than one queue family (async compute)
    const char* name = "";
};

class Texture {
public:
    virtual ~Texture() = default;
    const TextureDesc& desc() const { return m_desc; }

protected:
    TextureDesc m_desc;
};

/// Sub-range of a texture used for views / attachments. Count 0 = "all remaining".
struct Subresource {
    std::uint32_t baseMip = 0;
    std::uint32_t mipCount = 0;
    std::uint32_t baseLayer = 0;
    std::uint32_t layerCount = 0;
    bool operator==(const Subresource&) const = default;
};

// --- samplers --------------------------------------------------------------------
enum class Filter : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Wrap, Clamp, Mirror, Border };
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    AddressMode addressW = AddressMode::Wrap;
    float maxAnisotropy = 1.0f; ///< >1 enables anisotropic filtering (clamped to device limit)
    float mipLodBias = 0.0f;
    float minLod = 0.0f;
    float maxLod = 1000.0f;
    std::optional<CompareOp> compare; ///< shadow-map comparison sampler
    bool borderWhite = false;
};

class Sampler {
public:
    virtual ~Sampler() = default;
};

// --- pipeline layouts ---------------------------------------------------------------
/// Slot-based binding model. Each binding's register class follows from its type:
/// UniformBuffer -> b#, SampledTexture/StorageBufferRO/AccelerationStructure -> t#,
/// StorageTexture/StorageBufferRW -> u#, Sampler -> s#.
enum class BindingType : std::uint8_t {
    UniformBuffer,
    SampledTexture,
    StorageBufferRO,
    StorageBufferRW,
    StorageTexture,
    Sampler,
    AccelerationStructure,
};

struct Binding {
    BindingType type;
    std::uint32_t slot;
    ShaderStage stages = ShaderStage::All;
};

struct LayoutDesc {
    std::vector<Binding> bindings;
    std::uint32_t pushConstantBytes = 0; ///< <= 128 (portable minimum)
    /// Also bind a TextureTable: `Texture2D textures[] : register(t0, space1)` with
    /// `[[vk::binding(0, 1)]]` (Device::features().maxBindlessTextures > 0).
    bool textureTable = false;
};

class Texture;
/// A large array of sampled textures that shaders index dynamically (descriptor indexing,
/// "bindless"). Entries may be changed while the table is in use by recorded work.
class TextureTable {
public:
    virtual ~TextureTable() = default;
    std::uint32_t capacity() const { return m_capacity; }
    /// Textures must be in ResourceState::ShaderRead whenever shaders read them.
    virtual void set(std::uint32_t index, Texture* texture) = 0;

protected:
    std::uint32_t m_capacity = 0;
};

// --- graphics pipeline state -------------------------------------------------------
enum class PrimitiveTopology : std::uint8_t { TriangleList, TriangleStrip, LineList, LineStrip, PointList, PatchList };
enum class CullMode : std::uint8_t { None, Front, Back };
enum class BlendFactor : std::uint8_t {
    Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstColor, InvDstColor, DstAlpha, InvDstAlpha
};
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
enum class IndexType : std::uint8_t { Uint16, Uint32 };

struct VertexAttribute {
    std::uint32_t location; ///< matches [[vk::location(n)]] / semantic ATTRIBn
    Format format;
    std::uint32_t offset;
    std::uint32_t binding = 0;
};
struct VertexBinding {
    std::uint32_t binding;
    std::uint32_t stride;
    bool perInstance = false;
};

struct RasterState {
    CullMode cull = CullMode::Back;
    bool frontCounterClockwise = true;
    bool wireframe = false;
    bool depthClamp = false;
    float depthBias = 0.0f;
    float depthBiasSlope = 0.0f;
};

struct DepthState {
    bool test = true;
    bool write = true;
    CompareOp compare = CompareOp::LessEqual;
};

struct BlendAttachment {
    bool enable = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    std::uint8_t writeMask = 0xF;
    static BlendAttachment alpha() {
        return {true, BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha, BlendOp::Add,
                BlendFactor::One, BlendFactor::InvSrcAlpha, BlendOp::Add, 0xF};
    }
    static BlendAttachment additive() {
        return {true, BlendFactor::One, BlendFactor::One, BlendOp::Add, BlendFactor::One, BlendFactor::One,
                BlendOp::Add, 0xF};
    }
};

struct GraphicsPipelineDesc {
    ShaderRef vertex;
    ShaderRef fragment;
    ShaderRef hull;
    ShaderRef domain;
    ShaderRef geometry;
    ShaderRef task; ///< mesh pipelines (Device::features().meshShader): optional task/amplification stage
    ShaderRef mesh; ///< set instead of vertex; vertex input and topology are ignored
    bool variableRate = false; ///< allow CommandList::setShadingRate (features().variableRateShading)
    std::vector<VertexAttribute> attributes;
    std::vector<VertexBinding> vertexBindings;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    std::uint32_t patchControlPoints = 0;
    RasterState raster{};
    DepthState depth{};
    std::vector<BlendAttachment> blend; ///< per color target (missing entries = disabled)
    std::vector<Format> colorFormats;
    Format depthFormat = Format::Unknown;
    std::uint32_t sampleCount = 1;
    LayoutDesc layout;
    const char* name = "";
};

struct ComputePipelineDesc {
    ShaderRef compute;
    LayoutDesc layout;
    const char* name = "";
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    bool isCompute() const { return m_isCompute; }
    /// Wall-clock time the backend spent creating this pipeline (ms).
    double creationMs() const { return m_creationMs; }

protected:
    bool m_isCompute = false;
    double m_creationMs = 0.0;
};

// --- ray tracing ---------------------------------------------------------------------------
/// Opacity of one triangle for opacity micromaps: Opaque and Transparent triangles are
/// resolved by the traversal hardware; Unknown ones still reach any-hit shaders / ray-query
/// candidate processing.
enum class TriangleOpacity : std::uint8_t { Transparent = 0, Opaque = 1, Unknown = 2 };

/// Triangle geometry for a bottom-level acceleration structure. Positions are RGB32_FLOAT;
/// indices (optional) are 32-bit. Buffers need BufferUsage::AccelInput.
struct AccelTriangles {
    Buffer* vertices = nullptr;
    std::uint64_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t vertexStride = 12;
    Buffer* indices = nullptr;
    std::uint64_t indexOffset = 0;
    std::uint32_t indexCount = 0; ///< 0 = non-indexed (vertexCount / 3 triangles)
    bool opaque = true;
    /// Optional per-triangle opacity (Device::features().opacityMicromap; geometry must not be
    /// opaque). Read when the BLAS is created; one opacity micromap is built per geometry.
    std::span<const TriangleOpacity> opacity{};
};

struct BlasDesc {
    std::vector<AccelTriangles> geometries;
    bool allowUpdate = false; ///< can be refit after the vertices move (same topology)
    bool fastBuild = false;   ///< prefer build speed over trace speed
    const char* name = "";
};

class AccelerationStructure {
public:
    virtual ~AccelerationStructure() = default;
    bool isTopLevel() const { return m_topLevel; }
    std::uint64_t sizeBytes() const { return m_size; }        ///< storage size
    std::uint64_t scratchBytes() const { return m_scratch; }  ///< build scratch size

protected:
    bool m_topLevel = false;
    std::uint64_t m_size = 0;
    std::uint64_t m_scratch = 0;
};

struct AccelInstance {
    float transform[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}; ///< row-major 3x4 object-to-world
    std::uint32_t id = 0;              ///< InstanceID() in shaders (24 bits)
    std::uint8_t mask = 0xFF;
    std::uint32_t hitGroupOffset = 0;  ///< added to the hit-group index (24 bits)
    bool forceOpaque = false;
    AccelerationStructure* blas = nullptr;
};

struct RayTracingHitGroup {
    ShaderRef closestHit;
    ShaderRef anyHit;
    ShaderRef intersection; ///< procedural geometry only
};

struct RayTracingPipelineDesc {
    ShaderRef rayGen;
    std::vector<ShaderRef> miss;
    std::vector<RayTracingHitGroup> hitGroups;
    std::uint32_t maxRecursion = 1;
    std::uint32_t maxPayloadBytes = 32;   ///< largest ray payload (D3D12 needs it up front)
    std::uint32_t maxAttributeBytes = 8;  ///< largest hit attributes (8 = triangle barycentrics)
    LayoutDesc layout; ///< bindings are visible to every ray-tracing stage
    const char* name = "";
};

// --- queries ---------------------------------------------------------------------------
enum class QueryType : std::uint8_t { Timestamp, Occlusion, PipelineStatistics };

class QueryPool {
public:
    virtual ~QueryPool() = default;
    QueryType type() const { return m_type; }
    std::uint32_t count() const { return m_count; }
    /// Reads results after the submission that wrote them has completed.
    /// Timestamps are converted to nanoseconds. Returns false if any result is not available.
    virtual bool read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) = 0;

protected:
    QueryType m_type = QueryType::Timestamp;
    std::uint32_t m_count = 0;
};

// --- resource states (barriers) ---------------------------------------------------------
enum class ResourceState : std::uint8_t {
    Undefined,
    Common,
    VertexBuffer,
    IndexBuffer,
    UniformBuffer,
    ShaderRead,     ///< sampled texture / read-only storage buffer, any stage
    UnorderedAccess,///< read-write storage access
    RenderTarget,
    DepthWrite,
    DepthRead,
    CopySrc,
    CopyDst,
    IndirectArgument,
    Present,
    HostRead,       ///< after a copy into a readback buffer, before mapping
    ShadingRateSource, ///< read as a shading-rate image by the rasterizer
};

// --- render passes ---------------------------------------------------------------------
enum class LoadOp : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare };

struct ColorAttachment {
    Texture* texture = nullptr;
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    std::array<float, 4> clear{0, 0, 0, 1};
    Texture* resolveTarget = nullptr; ///< MSAA resolve destination
};

struct DepthAttachment {
    Texture* texture = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::DontCare;
    float clearDepth = 1.0f;
    std::uint8_t clearStencil = 0;
    bool readOnly = false;
};

struct RenderPassDesc {
    std::vector<ColorAttachment> colors;
    DepthAttachment depth{};
    /// Per-tile shading rates (features().shadingRateImage): an R8_UINT texture with one texel per
    /// shadingRateTileSize pixels holding shadingRateCode() values. It replaces the per-draw rate of
    /// pipelines created with variableRate while the pass is open.
    Texture* shadingRateImage = nullptr;
};

/// Shading-rate image value for a rate of width x height pixels (1, 2 or 4 each): the encoding shared by
/// D3D12_SHADING_RATE and Vulkan fragment shading rate attachments.
constexpr std::uint8_t shadingRateCode(std::uint32_t width, std::uint32_t height) {
    auto log2 = [](std::uint32_t v) { return v >= 4 ? 2u : v >= 2 ? 1u : 0u; };
    return static_cast<std::uint8_t>((log2(width) << 2) | log2(height));
}

// --- command lists -------------------------------------------------------------------------
enum class QueueType : std::uint8_t { Graphics, Compute, Copy };
std::string_view queueTypeName(QueueType q);

struct TransientAllocation {
    void* cpu = nullptr;
    Buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

class CommandList {
public:
    virtual ~CommandList() = default;
    QueueType queueType() const { return m_queue; }

    /// Starts recording. Waits for this list's previous submission (if any) to
    /// finish, then recycles its transient memory.
    virtual void begin() = 0;
    virtual void end() = 0;

    // Barriers. Resource state is tracked per resource (whole resource).
    virtual void transition(Texture* texture, ResourceState state) = 0;
    virtual void transition(Buffer* buffer, ResourceState state) = 0;
    /// Execution + memory dependency between successive UAV writes (nullptr = all resources).
    virtual void uavBarrier() = 0;

    // Render passes and fixed-function state.
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;
    /// Viewports use a top-left origin with y pointing down on every API.
    virtual void setViewport(float x, float y, float w, float h, float minDepth = 0.0f, float maxDepth = 1.0f) = 0;
    virtual void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) = 0;

    // Pipelines and bindings. Bindings persist across draws until changed and
    // are validated against the bound pipeline's layout at draw/dispatch time.
    virtual void setPipeline(Pipeline* pipeline) = 0;
    virtual void setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset = 0) = 0;
    virtual void setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset = 0) = 0;
    virtual void setUniformBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset, std::uint64_t size) = 0;
    /// Copies `size` bytes into transient memory and binds them as a uniform buffer.
    virtual void setUniformData(std::uint32_t slot, const void* data, std::uint64_t size) = 0;
    virtual void setTexture(std::uint32_t slot, Texture* texture, Subresource sub = {}) = 0;
    virtual void setStorageTexture(std::uint32_t slot, Texture* texture, std::uint32_t mip = 0) = 0;
    /// Binds a read-write structured/raw buffer (u# register, BindingType::StorageBufferRW).
    virtual void setStorageBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset = 0,
                                  std::uint64_t size = 0) = 0;
    /// Binds a read-only structured/raw buffer (t# register, BindingType::StorageBufferRO).
    virtual void setReadOnlyBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset = 0,
                                   std::uint64_t size = 0) = 0;
    virtual void setSampler(std::uint32_t slot, Sampler* sampler) = 0;
    virtual void pushConstants(const void* data, std::uint32_t size, std::uint32_t offset = 0) = 0;

    // Work.
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0,
                      std::uint32_t firstInstance = 0) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0,
                             std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0) = 0;
    virtual void drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount, std::uint32_t stride) = 0;
    virtual void drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                                     std::uint32_t stride) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y = 1, std::uint32_t z = 1) = 0;
    virtual void dispatchIndirect(Buffer* args, std::uint64_t offset) = 0;
    /// Binds the table read by pipelines created with LayoutDesc::textureTable.
    virtual void setTextureTable(TextureTable*) { throw Error("texture tables are not supported by this backend"); }
    /// Launches task (or mesh) workgroups of the bound mesh pipeline.
    virtual void drawMeshTasks(std::uint32_t, std::uint32_t = 1, std::uint32_t = 1) {
        throw Error("mesh shaders are not supported by this backend");
    }
    /// Per-draw shading rate in pixels (1, 2 or 4 per axis) for pipelines created with variableRate.
    virtual void setShadingRate(std::uint32_t /*width*/, std::uint32_t /*height*/) {
        throw Error("variable-rate shading is not supported by this backend");
    }

    // Transfers.
    virtual void copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                            std::uint64_t size) = 0;
    virtual void copyTexture(Texture* dst, Texture* src) = 0;
    /// Copies one mip/layer into a buffer with tightly packed rows of `rowPitch`
    /// bytes (must be a multiple of textureCopyPitchAlignment()).
    virtual void copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                                     std::uint32_t mip = 0, std::uint32_t layer = 0) = 0;
    virtual void copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                                     std::uint64_t srcOffset, std::uint32_t rowPitch) = 0;
    virtual void fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) = 0;
    virtual void resolveTexture(Texture* dst, Texture* src) = 0;

    // Queries.
    virtual void resetQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) = 0;
    virtual void writeTimestamp(QueryPool* pool, std::uint32_t index) = 0;
    virtual void beginQuery(QueryPool* pool, std::uint32_t index) = 0;
    virtual void endQuery(QueryPool* pool, std::uint32_t index) = 0;
    /// D3D12 needs an explicit resolve before read(); a no-op elsewhere.
    virtual void resolveQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) = 0;

    // Debug markers (visible in RenderDoc / PIX / Nsight).
    virtual void beginMarker(const char* name) = 0;
    virtual void endMarker() = 0;

    /// Linear per-list upload memory, valid until this list is re-begun.
    virtual TransientAllocation allocTransient(std::uint64_t size, std::uint64_t alignment = 256) = 0;

    // Ray tracing (Device::features().rayQuery / rayTracingPipeline). Builds use the
    // structure's own scratch memory; call accelBarrier() before reading a structure
    // that was just built (tracing, ray queries or building a TLAS over a new BLAS).
    virtual void buildBlas(AccelerationStructure*, const BlasDesc&, bool /*update*/ = false) {
        throw Error("ray tracing is not supported by this backend");
    }
    virtual void buildTlas(AccelerationStructure*, std::span<const AccelInstance>, bool /*update*/ = false) {
        throw Error("ray tracing is not supported by this backend");
    }
    virtual void accelBarrier() {}
    /// Binds a top-level structure to t# (BindingType::AccelerationStructure).
    virtual void setAccelerationStructure(std::uint32_t, AccelerationStructure*) {
        throw Error("ray tracing is not supported by this backend");
    }
    /// Launches the bound ray-tracing pipeline's ray-generation shader over width x height x depth.
    virtual void traceRays(std::uint32_t, std::uint32_t, std::uint32_t = 1) {
        throw Error("ray tracing is not supported by this backend");
    }

protected:
    QueueType m_queue = QueueType::Graphics;
};

// --- swapchain -----------------------------------------------------------------------------
struct SwapchainDesc {
    void* window = nullptr; ///< SDL_Window*
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool vsync = false;
    bool hdr = false; ///< request an HDR10 / scRGB swapchain if supported
    std::uint32_t imageCount = 3;
};

class Swapchain {
public:
    virtual ~Swapchain() = default;
    /// Acquires the next backbuffer (in ResourceState::Present). Returns nullptr
    /// if the swapchain is out of date; call resize() and retry.
    virtual Texture* acquire() = 0;
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
    virtual Format format() const = 0;
    virtual bool isHdr() const { return false; }
    virtual std::string presentModeName() const = 0;
};

// --- submission ----------------------------------------------------------------------------------
struct QueueWait {
    QueueType queue;
    std::uint64_t value; ///< submission value returned by Device::submit on that queue
};

struct SubmitInfo {
    std::span<CommandList* const> lists;
    std::span<const QueueWait> waits{};
    Swapchain* present = nullptr; ///< wait for its acquire and present it after this submission
};

/// One matrix multiply-accumulate shape the GPU runs on its matrix units (cooperative
/// matrices / wave matrix ops): Result(MxN) = A(MxK) * B(KxN) + C(MxN), per subgroup.
struct CooperativeMatrixConfig {
    std::uint32_t m = 0, n = 0, k = 0;
    std::string a, b, c, result; ///< "f16", "bf16", "f32", "f64", "e4m3", "e5m2", "s8", "u8", "s32", "u32", ...
    bool saturating = false;
};

// --- device ----------------------------------------------------------------------------------------
struct DeviceCreateInfo {
    Api api = Api::Vulkan;
    std::uint32_t adapterIndex = 0;
    bool validation = false;  ///< Vulkan validation layers / D3D debug layer / GL debug output
    bool gpuAssisted = false; ///< GPU-based validation (slow)
    void* window = nullptr;   ///< SDL_Window* — required for OpenGL (context owner), optional elsewhere
};

struct ValidationCounts {
    std::uint64_t errors = 0;
    std::uint64_t warnings = 0;
};

struct DeviceFaultReport {
    bool supported = false;           ///< the API/driver can describe device faults
    std::string description;          ///< driver's description of the fault (may be empty)
    std::vector<std::string> details; ///< faulting addresses, vendor fault codes
};

class Device {
public:
    virtual ~Device() = default;
    Api api() const { return m_api; }
    const AdapterInfo& adapter() const { return m_adapter; }
    const DeviceFeatures& features() const { return m_features; }
    /// Full capability listing for the capability explorer / system report.
    virtual std::vector<Capability> capabilities() const = 0;

    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<Sampler> createSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> createRayTracingPipeline(const RayTracingPipelineDesc&) {
        throw Error("ray-tracing pipelines are not supported by this backend");
    }
    /// Allocates a bottom-level structure (with scratch memory) sized for `desc`; build it with CommandList::buildBlas.
    virtual std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc&) {
        throw Error("ray tracing is not supported by this backend");
    }
    /// Shapes usable with subgroup-scope cooperative matrices (empty = none).
    virtual std::vector<CooperativeMatrixConfig> cooperativeMatrixConfigs() { return {}; }
    virtual std::unique_ptr<TextureTable> createTextureTable(std::uint32_t /*capacity*/) {
        throw Error("texture tables are not supported by this backend");
    }
    virtual std::unique_ptr<AccelerationStructure> createTlas(std::uint32_t /*maxInstances*/, bool /*allowUpdate*/ = false) {
        throw Error("ray tracing is not supported by this backend");
    }
    virtual std::unique_ptr<QueryPool> createQueryPool(QueryType type, std::uint32_t count) = 0;
    virtual std::unique_ptr<CommandList> createCommandList(QueueType queue) = 0;
    virtual std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) = 0;

    /// Submits command lists; returns a monotonically increasing value on that
    /// queue's timeline which other submissions and waitIdle-style calls can use.
    virtual std::uint64_t submit(QueueType queue, const SubmitInfo& info) = 0;
    /// Presents a swapchain whose image was rendered by a submission with `present` set.
    virtual void present(Swapchain* swapchain) = 0;
    virtual void wait(QueueType queue, std::uint64_t value) = 0;
    virtual bool isComplete(QueueType queue, std::uint64_t value) = 0;
    virtual void waitIdle() = 0;

    /// Uploads data to a device-local buffer/texture through a staging buffer and waits.
    virtual void uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset = 0) = 0;
    virtual void uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                               std::uint32_t rowPitch) = 0;
    /// Synchronously reads back a 2D texture mip as tightly packed rows.
    virtual std::vector<std::uint8_t> readbackTexture(Texture* src, std::uint32_t mip = 0, std::uint32_t layer = 0) = 0;

    virtual std::uint32_t textureCopyPitchAlignment() const = 0;
    virtual MemoryBudget memoryBudget() const = 0;
    virtual ValidationCounts validationCounts() const = 0;
    /// What the driver reports about a device loss (VK_EXT_device_fault, D3D12 DRED).
    /// Call after a DeviceLostError, before destroying the device.
    virtual DeviceFaultReport deviceFaultReport() { return {}; }
    /// Bit mask of the sample counts (1, 2, 4, 8, ...) usable for a render target or depth
    /// buffer of `format`. Formats differ: e.g. some drivers offer 4x but not 2x for RGBA8.
    virtual std::uint32_t renderTargetSampleCounts(Format format) const {
        (void)format;
        std::uint32_t mask = 0;
        for (std::uint32_t s = 1; s <= m_features.maxMsaaSamples; s *= 2) mask |= s;
        return mask;
    }
    virtual void setDebugName(Buffer*, const char*) {}
    virtual void setDebugName(Texture*, const char*) {}

protected:
    Api m_api = Api::Vulkan;
    AdapterInfo m_adapter;
    DeviceFeatures m_features;
};

/// Enumerates adapters for one API. Never throws; returns an empty list and
/// fills `error` if the API is unavailable.
std::vector<AdapterInfo> enumerateAdapters(Api api, std::string* error = nullptr);

/// Creates a device. Throws rhi::Error with a human-readable reason on failure.
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

} // namespace gt::rhi
