#pragma once
// OpenGL backend. GL has one implicit queue and immediate state, so command lists record into an
// arena of small closures that the device replays at submit on the context's thread; an
// executor keeps the RHI's slot tables (b/t/u/s registers) and, at each draw or dispatch, binds
// what the current program's binding table (emitted by gt_glslgen) asks for. Every queue type
// maps to the same GL stream; submissions are ordered and fenced with glFenceSync.

#include "command_arena.h"
#include "gl_common.h"

#include <SDL3/SDL_video.h>

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace gt::rhi::gl {

class GLDevice;
struct Executor;

std::vector<AdapterInfo> enumerateAdapters(std::string* error);
std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

// --- resources ------------------------------------------------------------------------------------
class GLBuffer final : public Buffer {
public:
    GLBuffer(GLDevice& dev, const BufferDesc& desc, const void* data);
    ~GLBuffer() override;
    void* mapped() override { return m_mapped; }
    GLuint name() const { return m_name; }

private:
    GLDevice& m_dev;
    GLuint m_name = 0;
    void* m_mapped = nullptr;
};

class GLTexture final : public Texture {
public:
    GLTexture(GLDevice& dev, const TextureDesc& desc);
    ~GLTexture() override;
    GLuint name() const { return m_name; }
    GLenum target() const { return m_target; }
    const GlFormat& format() const { return m_format; }
    struct View {
        GLuint name;
        GLenum target;
    };
    /// Texture to sample for a sub-range: the texture itself, or a cached texture view.
    View view(Subresource sub);

private:
    GLDevice& m_dev;
    GLuint m_name = 0;
    GLenum m_target = GL_TEXTURE_2D;
    GlFormat m_format;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, View> m_views;
};

class GLSampler final : public Sampler {
public:
    GLSampler(GLDevice& dev, const SamplerDesc& desc);
    ~GLSampler() override;
    GLuint name() const { return m_name; }

private:
    GLuint m_name = 0;
};

class GLQueryPool final : public QueryPool {
public:
    GLQueryPool(GLDevice& dev, QueryType type, std::uint32_t count);
    ~GLQueryPool() override;
    bool read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) override;
    GLuint query(std::uint32_t index) const { return m_names[index]; }
    GLenum target() const;

private:
    GLDevice& m_dev;
    std::vector<GLuint> m_names;
};

// --- pipelines --------------------------------------------------------------------------------------
/// Resources a linked program uses, in the GL binding order assigned at link time.
struct ProgramBindings {
    std::vector<std::uint32_t> ubos;                   ///< b slot per uniform-block binding 0..n-1
    int pushBinding = -1;                              ///< uniform-block binding of the push constants
    std::vector<std::pair<char, std::uint32_t>> ssbos; ///< ('t'|'u', slot) per storage-block binding
    std::vector<std::uint32_t> images;                 ///< u slot per image unit
    struct Tex {
        std::uint32_t t;
        int s; ///< sampler slot, -1 = none (texelFetch)
    };
    std::vector<Tex> textures;                ///< per texture unit
    GLint baseVertex = -1, baseInstance = -1; ///< SPIRV-Cross base uniforms (no ARB_shader_draw_parameters)
};

class GLPipeline final : public Pipeline {
public:
    GLPipeline(GLDevice& dev, const GraphicsPipelineDesc& desc);
    GLPipeline(GLDevice& dev, const ComputePipelineDesc& desc);
    ~GLPipeline() override;
    GLuint program() const { return m_program; }
    const ProgramBindings& bindings() const { return m_bindings; }
    const GraphicsPipelineDesc& graphics() const { return m_graphics; }
    GLenum primitive() const { return m_primitive; }

private:
    void link(const std::vector<std::pair<GLenum, ShaderRef>>& stages, const char* name);
    GLDevice& m_dev;
    GLuint m_program = 0;
    ProgramBindings m_bindings;
    GraphicsPipelineDesc m_graphics; ///< state applied at bind (compute pipelines leave it default)
    GLenum m_primitive = GL_TRIANGLES;
};

// --- execution state ----------------------------------------------------------------------------
struct BoundBuffer {
    GLBuffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
struct BoundTexture {
    GLTexture* texture = nullptr;
    Subresource sub{};
    std::uint32_t mip = 0;
};

/// Replay state: what the recorded commands have set so far, and what GL currently has bound.
struct Executor {
    GLDevice& dev;
    explicit Executor(GLDevice& d) : dev(d) {}
    static constexpr std::uint32_t kSlots = 16;
    std::array<BoundBuffer, kSlots> ubo{}, srvBuf{}, uavBuf{};
    std::array<BoundTexture, kSlots> srvTex{}, uavTex{};
    std::array<GLSampler*, kSlots> samplers{};
    BoundBuffer push{}; ///< push-constant data (transient memory)
    struct Vertex {
        GLBuffer* buffer = nullptr;
        std::uint64_t offset = 0;
    };
    std::array<Vertex, 16> vertex{};
    GLBuffer* indexBuffer = nullptr;
    std::uint64_t indexOffset = 0;
    GLenum indexType = GL_UNSIGNED_INT;
    GLPipeline* pipeline = nullptr;
    bool pipelineDirty = true, vertexDirty = true;
    bool depthReadOnly = false;   ///< current render pass attaches depth read-only
    std::uint32_t attribMask = 0; ///< enabled vertex attribute arrays
    GLuint vao = 0;
    std::uint32_t passHeight = 0;

    void applyPipeline();
    void applyBindings();
    void applyVertexInput(std::int64_t baseVertexBytes = 0);
    void prepareDraw(std::int32_t baseVertex, std::uint32_t baseInstance);
};

// --- command list -------------------------------------------------------------------------------------
class GLCommandList final : public CommandList {
public:
    GLCommandList(GLDevice& dev, QueueType queue);
    ~GLCommandList() override;
    void setLastSubmit(std::uint64_t v) { m_lastSubmit = v; }
    const CommandArena<Executor>& commands() const { return m_cmds; }

    void begin() override;
    void end() override;
    void transition(Texture* texture, ResourceState state) override;
    void transition(Buffer* buffer, ResourceState state) override;
    void uavBarrier() override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(float x, float y, float w, float h, float minDepth, float maxDepth) override;
    void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) override;
    void setPipeline(Pipeline* pipeline) override;
    void setVertexBuffer(std::uint32_t binding, Buffer* buffer, std::uint64_t offset) override;
    void setIndexBuffer(Buffer* buffer, IndexType type, std::uint64_t offset) override;
    void setUniformBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                          std::uint64_t size) override;
    void setUniformData(std::uint32_t slot, const void* data, std::uint64_t size) override;
    void setTexture(std::uint32_t slot, Texture* texture, Subresource sub) override;
    void setStorageTexture(std::uint32_t slot, Texture* texture, std::uint32_t mip) override;
    void setStorageBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                          std::uint64_t size) override;
    void setReadOnlyBuffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset,
                           std::uint64_t size) override;
    void setSampler(std::uint32_t slot, Sampler* sampler) override;
    void pushConstants(const void* data, std::uint32_t size, std::uint32_t offset) override;
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                     std::int32_t vertexOffset, std::uint32_t firstInstance) override;
    void drawIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                      std::uint32_t stride) override;
    void drawIndexedIndirect(Buffer* args, std::uint64_t offset, std::uint32_t drawCount,
                             std::uint32_t stride) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void dispatchIndirect(Buffer* args, std::uint64_t offset) override;
    void copyBuffer(Buffer* dst, std::uint64_t dstOffset, Buffer* src, std::uint64_t srcOffset,
                    std::uint64_t size) override;
    void copyTexture(Texture* dst, Texture* src) override;
    void copyTextureToBuffer(Buffer* dst, std::uint64_t dstOffset, std::uint32_t rowPitch, Texture* src,
                             std::uint32_t mip, std::uint32_t layer) override;
    void copyBufferToTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, Buffer* src,
                             std::uint64_t srcOffset, std::uint32_t rowPitch) override;
    void fillBuffer(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override;
    void resolveTexture(Texture* dst, Texture* src) override;
    void resetQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) override;
    void writeTimestamp(QueryPool* pool, std::uint32_t index) override;
    void beginQuery(QueryPool* pool, std::uint32_t index) override;
    void endQuery(QueryPool* pool, std::uint32_t index) override;
    void resolveQueries(QueryPool* pool, std::uint32_t first, std::uint32_t count) override;
    void beginMarker(const char* name) override;
    void endMarker() override;
    TransientAllocation allocTransient(std::uint64_t size, std::uint64_t alignment) override;

private:
    void requireRecording(const char* what) const;
    /// Throws when the bound pipeline uses a slot nothing is bound to; uploads dirty push constants.
    void beforeWork(const char* what);
    void barrierIfNeeded();
    GLDevice& m_dev;
    CommandArena<Executor> m_cmds;
    // Record-time view of the bindings (validation; the executor holds the real ones at replay).
    GLPipeline* m_pipe = nullptr;
    std::uint32_t m_uboMask = 0, m_srvTexMask = 0, m_srvBufMask = 0, m_uavTexMask = 0, m_uavBufMask = 0,
                  m_samplerMask = 0;
    bool m_pushDirty = false;
    bool m_recording = false;
    bool m_inRenderPass = false;
    bool m_uavPending = false; ///< UAV writes since the last memory barrier
    std::uint64_t m_lastSubmit = 0;
    std::vector<std::pair<GLTexture*, GLTexture*>> m_pendingResolves;
    std::array<std::uint32_t, 32> m_push{};
    struct Chunk {
        std::unique_ptr<GLBuffer> buffer; ///< persistently mapped upload memory
        std::uint64_t used = 0;
    };
    std::vector<Chunk> m_chunks;
    std::size_t m_chunk = 0;
};

// --- swapchain ------------------------------------------------------------------------------------------
class GLSwapchain final : public Swapchain {
public:
    GLSwapchain(GLDevice& dev, const SwapchainDesc& desc);
    ~GLSwapchain() override;
    Texture* acquire() override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Format format() const override { return Format::RGBA8_UNORM; }
    std::string presentModeName() const override;
    void present();
    SDL_Window* window() const { return m_window; }

private:
    void createImages();
    GLDevice& m_dev;
    SDL_Window* m_window;
    SwapchainDesc m_desc;
    std::vector<std::unique_ptr<GLTexture>> m_images;
    std::uint32_t m_current = 0;
    bool m_adaptive = false;
};

// --- device ---------------------------------------------------------------------------------------------
class GLDevice final : public Device {
public:
    explicit GLDevice(const DeviceCreateInfo& info);
    ~GLDevice() override;

    std::vector<Capability> capabilities() const override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    std::unique_ptr<Sampler> createSampler(const SamplerDesc& desc) override;
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override;
    std::unique_ptr<QueryPool> createQueryPool(QueryType type, std::uint32_t count) override;
    std::unique_ptr<CommandList> createCommandList(QueueType queue) override;
    std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
    std::uint64_t submit(QueueType queue, const SubmitInfo& info) override;
    void present(Swapchain* swapchain) override;
    void wait(QueueType queue, std::uint64_t value) override;
    bool isComplete(QueueType queue, std::uint64_t value) override;
    void waitIdle() override;
    void uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) override;
    void uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                       std::uint32_t rowPitch) override;
    std::vector<std::uint8_t> readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) override;
    std::uint32_t textureCopyPitchAlignment() const override { return 4; }
    MemoryBudget memoryBudget() const override;
    ValidationCounts validationCounts() const override { return m_validationCounts; }
    std::uint32_t renderTargetSampleCounts(Format format) const override;
    void setDebugName(Buffer* buffer, const char* name) override;
    void setDebugName(Texture* texture, const char* name) override;

    // --- internal
    /// GL version as major * 10 + minor (33, 45, 46, ...).
    int version() const { return m_version; }
    bool hasExtension(const char* name) const;
    bool hasCompute() const { return m_version >= 43; }
    bool hasTextureViews() const { return m_textureViews; }
    bool hasClearBuffer() const { return gl().glClearBufferSubData != nullptr; }
    bool hasBaseInstance() const { return m_version >= 42; }
    std::int32_t uniformAlignment() const { return m_uniformAlignment; }
    /// Framebuffer for a set of attachments (cached; dropped when one of the textures goes away).
    GLuint framebuffer(const std::vector<std::tuple<GLTexture*, std::uint32_t, std::uint32_t>>& colors,
                       GLTexture* depth);
    GLuint readFramebuffer(GLTexture* texture, std::uint32_t mip, std::uint32_t layer);
    void forgetTexture(GLuint name);
    void label(GLenum identifier, GLuint name, const char* text) const;
    void countMessage(GLenum type, GLenum severity, const char* message);
    Executor& executor() { return *m_exec; }
    void makeCurrent(SDL_Window* window);
    SDL_Window* contextWindow() const { return m_window; }

private:
    void createContext(const DeviceCreateInfo& info);
    void fillFeatures();
    void checkErrors(const char* where);

    SDL_Window* m_window = nullptr;    ///< window the context is current on
    SDL_Window* m_ownWindow = nullptr; ///< hidden window created for offscreen use
    SDL_GLContext m_context = nullptr;
    int m_version = 0;
    std::vector<std::string> m_extensions;
    bool m_textureViews = false;
    bool m_validation = false;
    std::int32_t m_uniformAlignment = 256;
    ValidationCounts m_validationCounts;
    std::unique_ptr<Executor> m_exec;
    struct Fence {
        std::uint64_t value;
        GLsync sync;
    };
    std::vector<Fence> m_fences; ///< outstanding submissions, oldest first
    std::uint64_t m_submitted = 0, m_completed = 0;
    std::map<std::vector<GLuint>, GLuint> m_framebuffers; ///< key: attachment texture names + mips/layers
    std::unique_ptr<GLCommandList> m_uploadList;
};

} // namespace gt::rhi::gl
