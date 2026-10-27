#include "gl_device.h"

#include "gt/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <format>

// Not in every glext.h revision.
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif

namespace gt::rhi::gl {
namespace {

std::string str(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(gl().glGetString(name));
    return s ? s : "";
}

bool contains(const std::string& s, const char* needle) {
    std::string a = s, b = needle;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

std::uint32_t vendorFromString(const std::string& vendor, const std::string& renderer) {
    if (contains(vendor, "nvidia")) return vendor::kNvidia;
    if (contains(vendor, "amd") || contains(vendor, "ati ")) return vendor::kAmd;
    if (contains(vendor, "intel")) return vendor::kIntel;
    if (contains(vendor, "microsoft")) return vendor::kMicrosoft;
    if (contains(renderer, "llvmpipe") || contains(renderer, "softpipe") || contains(vendor, "mesa"))
        return vendor::kMesa;
    return 0;
}

bool isSoftwareRenderer(const std::string& renderer) {
    return contains(renderer, "llvmpipe") || contains(renderer, "softpipe") ||
           contains(renderer, "swiftshader") || contains(renderer, "basic render") ||
           contains(renderer, "gdi generic") || contains(renderer, "lavapipe");
}

/// Initialises SDL video, falling back to the offscreen (EGL) driver without a display.
void initVideo() {
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_InitSubSystem(SDL_INIT_VIDEO); // balance the SDL_QuitSubSystem in the destructor
        return;
    }
#ifdef __linux__
    // Headless: skip the X11/Wayland probes (libwayland complains on stderr) and use EGL directly.
    if (!SDL_getenv("DISPLAY") && !SDL_getenv("WAYLAND_DISPLAY") && !SDL_GetHint(SDL_HINT_VIDEO_DRIVER))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) return;
    const std::string first = SDL_GetError();
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        throw Error(std::format("cannot initialise video for OpenGL ({}; offscreen driver: {})", first,
                                SDL_GetError()));
}

void GLAPIENTRY debugCallback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity,
                              GLsizei /*length*/, const GLchar* message, const void* user) {
    static_cast<GLDevice*>(const_cast<void*>(user))->countMessage(type, severity, message);
}

} // namespace

// --- arena ----------------------------------------------------------------------------------------
void* CommandArena::allocate(std::size_t bytes) {
    bytes = (bytes + kAlign - 1) / kAlign * kAlign;
    if (bytes > kBlock) {
        m_large.push_back(std::make_unique<std::byte[]>(bytes + kAlign));
        auto p = reinterpret_cast<std::uintptr_t>(m_large.back().get());
        return reinterpret_cast<void*>((p + kAlign - 1) / kAlign * kAlign);
    }
    if (m_used + bytes > kBlock) {
        if (m_blocks.empty() || m_block + 1 >= m_blocks.size()) {
            m_blocks.push_back(std::make_unique<std::byte[]>(kBlock + kAlign));
            m_block = m_blocks.size() - 1;
        } else {
            ++m_block;
        }
        m_used = 0;
    }
    auto base = reinterpret_cast<std::uintptr_t>(m_blocks[m_block].get());
    base = (base + kAlign - 1) / kAlign * kAlign;
    void* p = reinterpret_cast<void*>(base + m_used);
    m_used += bytes;
    return p;
}

void CommandArena::execute(Executor& e) const {
    for (Header* h = m_head; h; h = h->next)
        h->run(h + 1, e);
}

void CommandArena::reset() {
    for (Header* h = m_head; h; h = h->next)
        if (h->destroy) h->destroy(h + 1);
    m_head = m_tail = nullptr;
    m_count = 0;
    m_large.clear();
    m_block = 0;
    m_used = m_blocks.empty() ? kBlock : 0;
}

// --- adapters ---------------------------------------------------------------------------------------
namespace {
struct ProbeResult {
    bool done = false;
    std::vector<AdapterInfo> adapters;
    std::string error;
};

ProbeResult& probeCache() {
    static ProbeResult r;
    return r;
}
} // namespace

std::vector<AdapterInfo> enumerateAdapters(std::string* error) {
    ProbeResult& r = probeCache();
    if (!r.done) {
        r.done = true;
        try {
            DeviceCreateInfo ci;
            ci.api = Api::OpenGL;
            GLDevice dev(ci);
            r.adapters.push_back(dev.adapter());
        } catch (const std::exception& e) {
            r.error = e.what();
        }
    }
    if (error) *error = r.error;
    return r.adapters;
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) {
    auto dev = std::make_unique<GLDevice>(info);
    log::info("OpenGL device: {} ({}, GL {}){}", dev->adapter().name, dev->adapter().driverName,
              dev->adapter().driverInfo, info.validation ? ", debug output on" : "");
    return dev;
}

// --- device ---------------------------------------------------------------------------------------------
GLDevice::GLDevice(const DeviceCreateInfo& info) {
    m_api = Api::OpenGL;
    m_validation = info.validation;
    createContext(info);
    const std::string missing = glMutable().load();
    if (!gl().glGetString || !gl().glFenceSync || !gl().glBindBufferRange)
        throw Error("the OpenGL context lacks core 3.3 entry points (" + missing + ")");

    GLint major = 0, minor = 0, n = 0;
    gl().glGetIntegerv(GL_MAJOR_VERSION, &major);
    gl().glGetIntegerv(GL_MINOR_VERSION, &minor);
    m_version = major * 10 + minor;
    gl().glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const auto* e =
            reinterpret_cast<const char*>(gl().glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e) m_extensions.emplace_back(e);
    }
    if (m_version < 33)
        throw Error(std::format("OpenGL 3.3 is required (the context is {}.{})", major, minor));
    if (m_version < 44 && !hasExtension("GL_ARB_buffer_storage"))
        throw Error("OpenGL 4.4 or GL_ARB_buffer_storage is required (persistently mapped buffers)");
    m_textureViews = (m_version >= 43 || hasExtension("GL_ARB_texture_view")) && gl().glTextureView &&
                     (m_version >= 42 || hasExtension("GL_ARB_texture_storage"));

    const std::string vendorStr = str(GL_VENDOR), renderer = str(GL_RENDERER), versionStr = str(GL_VERSION);
    m_adapter.api = Api::OpenGL;
    m_adapter.index = 0;
    m_adapter.name = renderer;
    m_adapter.vendorId = vendorFromString(vendorStr, renderer);
    m_adapter.type = isSoftwareRenderer(renderer) ? AdapterType::Cpu : AdapterType::Unknown;
    m_adapter.driverName = vendorStr;
    m_adapter.driverInfo = versionStr;
    // "4.6.0 NVIDIA 566.36", "4.5 (Core Profile) Mesa 25.0.7", "4.6.0 Compatibility Profile Context 23.10.2":
    // the driver version is the last word.
    const std::size_t sp = versionStr.find_last_of(' ');
    m_adapter.driverVersion = sp == std::string::npos ? versionStr : versionStr.substr(sp + 1);
    m_adapter.apiVersion = std::format("{}.{}", major, minor);
    if (hasExtension("GL_NVX_gpu_memory_info")) {
        GLint kb = 0;
        gl().glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kb);
        m_adapter.dedicatedMemory = static_cast<std::uint64_t>(kb) * 1024;
        if (m_adapter.type == AdapterType::Unknown) m_adapter.type = AdapterType::Discrete;
    }

    if (m_validation && gl().glDebugMessageCallback) {
        gl().glEnable(GL_DEBUG_OUTPUT);
        gl().glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        gl().glDebugMessageCallback(debugCallback, this);
        gl().glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr,
                                   GL_FALSE);
    }
    gl().glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_uniformAlignment);
    m_uniformAlignment = std::max(m_uniformAlignment, 16);
    gl().glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    gl().glEnable(GL_FRAMEBUFFER_SRGB);
    m_exec = std::make_unique<Executor>(*this);
    gl().glGenVertexArrays(1, &m_exec->vao);
    gl().glBindVertexArray(m_exec->vao);
    fillFeatures();
}

GLDevice::~GLDevice() {
    try {
        if (m_context) {
            makeCurrent(m_window);
            waitIdle();
            m_uploadList.reset();
            for (auto& [key, fbo] : m_framebuffers)
                gl().glDeleteFramebuffers(1, &fbo);
            m_framebuffers.clear();
            if (m_exec && m_exec->vao) gl().glDeleteVertexArrays(1, &m_exec->vao);
            for (Fence& f : m_fences)
                gl().glDeleteSync(f.sync);
        }
    } catch (const std::exception& e) {
        log::warn("OpenGL shutdown: {}", e.what());
    }
    if (m_context) SDL_GL_DestroyContext(m_context);
    if (m_ownWindow) SDL_DestroyWindow(m_ownWindow);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void GLDevice::createContext(const DeviceCreateInfo& /*info*/) {
    initVideo();
    // The context lives on a hidden window of our own and is made current on a swapchain's window
    // only to present: the runner may destroy its window (e.g. when presenting fails) at any time.
    SDL_GL_ResetAttributes();
    m_ownWindow = SDL_CreateWindow("gl-context", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!m_ownWindow) throw Error(std::format("cannot create a window for OpenGL: {}", SDL_GetError()));
    m_window = m_ownWindow;
    static const int kVersions[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}};
    std::string lastError;
    for (const auto& v : kVersions) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v[0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v[1]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, m_validation ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
        m_context = SDL_GL_CreateContext(m_window);
        if (m_context) break;
        lastError = SDL_GetError();
    }
    if (!m_context) throw Error(std::format("cannot create an OpenGL 3.3+ core context: {}", lastError));
    SDL_GL_MakeCurrent(m_window, m_context);
}

void GLDevice::makeCurrent(SDL_Window* window) {
    if (!window) window = m_ownWindow;
    if (window != m_window) {
        if (!SDL_GL_MakeCurrent(window, m_context))
            throw Error(std::format("SDL_GL_MakeCurrent: {}", SDL_GetError()));
        m_window = window;
    }
}

bool GLDevice::hasExtension(const char* name) const {
    return std::find(m_extensions.begin(), m_extensions.end(), name) != m_extensions.end();
}

void GLDevice::fillFeatures() {
    DeviceFeatures& f = m_features;
    auto geti = [](GLenum e) {
        GLint v = 0;
        gl().glGetIntegerv(e, &v);
        return v;
    };
    f.timestamps = gl().glQueryCounter != nullptr;
    f.timestampsOnComputeQueue = f.timestamps;
    f.timestampPeriodNs = 1.0;
    f.asyncComputeQueue = false;
    f.copyQueue = false;
    f.geometryShader = m_version >= 32;
    f.tessellation = m_version >= 40 || hasExtension("GL_ARB_tessellation_shader");
    f.wireframe = true;
    f.anisotropicFiltering = m_version >= 46 || hasExtension("GL_EXT_texture_filter_anisotropic") ||
                             hasExtension("GL_ARB_texture_filter_anisotropic");
    f.textureCompressionBC = hasExtension("GL_EXT_texture_compression_s3tc") &&
                             (m_version >= 42 || hasExtension("GL_ARB_texture_compression_bptc"));
    f.shaderFloat64 = m_version >= 40 || hasExtension("GL_ARB_gpu_shader_fp64");
    f.shaderInt64 = hasExtension("GL_ARB_gpu_shader_int64");
    f.subgroupOps = hasExtension("GL_KHR_shader_subgroup");
    f.multiDrawIndirect = m_version >= 43 || hasExtension("GL_ARB_multi_draw_indirect");
    f.drawIndirectCount = false;
    f.pipelineStatistics = false;
    f.memoryBudget = hasExtension("GL_NVX_gpu_memory_info") || hasExtension("GL_ATI_meminfo");
    f.maxTexture2D = static_cast<std::uint32_t>(geti(GL_MAX_TEXTURE_SIZE));
    f.maxMsaaSamples = static_cast<std::uint32_t>(std::max(1, geti(GL_MAX_SAMPLES)));
    if (hasCompute()) {
        f.maxComputeSharedMemory = static_cast<std::uint32_t>(geti(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE));
        for (GLuint i = 0; i < 3; ++i) {
            GLint v = 0;
            gl().glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &v);
            f.maxComputeWorkgroupSize[i] = static_cast<std::uint32_t>(v);
        }
        f.maxComputeWorkgroupInvocations =
            static_cast<std::uint32_t>(geti(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS));
        GLint64 ssbo = 0;
        gl().glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssbo);
        f.maxStorageBufferRange = static_cast<std::uint64_t>(ssbo);
    }
    f.maxBufferSize = std::max<std::uint64_t>(f.maxStorageBufferRange, 256ull << 20);
}

std::vector<Capability> GLDevice::capabilities() const {
    std::vector<Capability> c;
    auto add = [&](const std::string& g, const std::string& n, const std::string& v, bool ok = true) {
        c.push_back({g, n, v, ok});
    };
    auto yes = [&](const std::string& g, const std::string& n, bool v) { add(g, n, v ? "yes" : "no", v); };
    auto geti = [](GLenum e) {
        GLint v = 0;
        gl().glGetIntegerv(e, &v);
        return std::to_string(v);
    };
    add("Device", "Renderer", m_adapter.name);
    add("Device", "Vendor", m_adapter.driverName);
    add("Device", "Version", m_adapter.driverInfo);
    add("Device", "Shading language", str(GL_SHADING_LANGUAGE_VERSION));
    const std::string g = "Features";
    yes(g, "Compute shaders (4.3)", hasCompute());
    yes(g, "Tessellation (4.0)", m_features.tessellation);
    yes(g, "Geometry shaders (3.2)", m_features.geometryShader);
    yes(g, "Texture views", m_textureViews);
    yes(g, "Multi-draw indirect", m_features.multiDrawIndirect);
    yes(g, "Base instance (4.2)", hasBaseInstance());
    yes(g, "Shader draw parameters", m_version >= 46 || hasExtension("GL_ARB_shader_draw_parameters"));
    yes(g, "Anisotropic filtering", m_features.anisotropicFiltering);
    yes(g, "BC texture compression", m_features.textureCompressionBC);
    yes(g, "64-bit floats in shaders", m_features.shaderFloat64);
    yes(g, "Subgroup operations (KHR_shader_subgroup)", m_features.subgroupOps);
    yes(g, "Debug output (KHR_debug)", gl().glDebugMessageCallback != nullptr);
    yes(g, "SPIR-V shaders (4.6)", m_version >= 46 || hasExtension("GL_ARB_gl_spirv"));
    add("Limits", "Max texture size", geti(GL_MAX_TEXTURE_SIZE));
    add("Limits", "Max samples", geti(GL_MAX_SAMPLES));
    add("Limits", "Max texture units (combined)", geti(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS));
    add("Limits", "Max uniform block size", geti(GL_MAX_UNIFORM_BLOCK_SIZE));
    add("Limits", "Uniform buffer offset alignment", std::to_string(m_uniformAlignment));
    if (hasCompute()) {
        add("Limits", "Max storage blocks (combined)", geti(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS));
        add("Limits", "Max compute shared memory", geti(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE));
    }
    for (const std::string& e : m_extensions)
        add("Extensions", e, "yes");
    return c;
}

std::unique_ptr<Buffer> GLDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    return std::make_unique<GLBuffer>(*this, desc, initialData);
}
std::unique_ptr<Texture> GLDevice::createTexture(const TextureDesc& desc) {
    return std::make_unique<GLTexture>(*this, desc);
}
std::unique_ptr<Sampler> GLDevice::createSampler(const SamplerDesc& desc) {
    return std::make_unique<GLSampler>(*this, desc);
}
std::unique_ptr<Pipeline> GLDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    return std::make_unique<GLPipeline>(*this, desc);
}
std::unique_ptr<Pipeline> GLDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    if (!hasCompute()) throw Error("compute shaders need OpenGL 4.3");
    return std::make_unique<GLPipeline>(*this, desc);
}
std::unique_ptr<QueryPool> GLDevice::createQueryPool(QueryType type, std::uint32_t count) {
    return std::make_unique<GLQueryPool>(*this, type, count);
}
std::unique_ptr<CommandList> GLDevice::createCommandList(QueueType queue) {
    return std::make_unique<GLCommandList>(*this, queue);
}
std::unique_ptr<Swapchain> GLDevice::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<GLSwapchain>(*this, desc);
}

// --- submission --------------------------------------------------------------------------------------
std::uint64_t GLDevice::submit(QueueType /*queue*/, const SubmitInfo& info) {
    for (CommandList* l : info.lists) {
        auto* list = static_cast<GLCommandList*>(l);
        m_exec->pipelineDirty = m_exec->vertexDirty = true;
        list->commands().execute(*m_exec);
    }
    // Queue waits need nothing: there is one ordered command stream.
    GLsync sync = gl().glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl().glFlush();
    m_fences.push_back({++m_submitted, sync});
    for (CommandList* l : info.lists)
        static_cast<GLCommandList*>(l)->setLastSubmit(m_submitted);
    if (m_validation) checkErrors("submit");
    return m_submitted;
}

void GLDevice::present(Swapchain* swapchain) {
    static_cast<GLSwapchain*>(swapchain)->present();
}

void GLDevice::wait(QueueType /*queue*/, std::uint64_t value) {
    while (!m_fences.empty() && m_fences.front().value <= value) {
        Fence f = m_fences.front();
        for (;;) {
            const GLenum r = gl().glClientWaitSync(f.sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) break;
            if (r == GL_WAIT_FAILED) throw DeviceLostError("glClientWaitSync failed (context lost?)");
        }
        gl().glDeleteSync(f.sync);
        m_completed = f.value;
        m_fences.erase(m_fences.begin());
    }
    m_completed = std::max(m_completed, std::min(value, m_submitted));
}

bool GLDevice::isComplete(QueueType /*queue*/, std::uint64_t value) {
    while (!m_fences.empty()) {
        Fence f = m_fences.front();
        const GLenum r = gl().glClientWaitSync(f.sync, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) break;
        gl().glDeleteSync(f.sync);
        m_completed = f.value;
        m_fences.erase(m_fences.begin());
    }
    if (m_fences.empty()) m_completed = m_submitted;
    return value <= m_completed;
}

void GLDevice::waitIdle() {
    wait(QueueType::Graphics, m_submitted);
    gl().glFinish();
}

void GLDevice::checkErrors(const char* where) {
    for (int i = 0; i < 8; ++i) {
        const GLenum e = gl().glGetError();
        if (e == GL_NO_ERROR) break;
        ++m_validationCounts.errors;
        log::warn("[gl] {} after {}", glErrorName(e), where);
    }
}

void GLDevice::countMessage(GLenum type, GLenum severity, const char* message) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_ERROR) {
        ++m_validationCounts.errors;
        log::warn("[gl-debug] error: {}", message);
    } else if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM) {
        ++m_validationCounts.warnings;
        log::debug("[gl-debug] {}", message);
    }
}

void GLDevice::label(GLenum identifier, GLuint name, const char* text) const {
    if (m_validation && gl().glObjectLabel && text && *text) gl().glObjectLabel(identifier, name, -1, text);
}

void GLDevice::setDebugName(Buffer* buffer, const char* name) {
    label(GL_BUFFER, static_cast<GLBuffer*>(buffer)->name(), name);
}
void GLDevice::setDebugName(Texture* texture, const char* name) {
    label(GL_TEXTURE, static_cast<GLTexture*>(texture)->name(), name);
}

MemoryBudget GLDevice::memoryBudget() const {
    MemoryBudget b;
    if (hasExtension("GL_NVX_gpu_memory_info")) {
        GLint total = 0, avail = 0;
        gl().glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
        gl().glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
        b.localBudget = static_cast<std::uint64_t>(total) * 1024;
        b.localUsage = static_cast<std::uint64_t>(std::max(0, total - avail)) * 1024;
        b.valid = true;
    }
    return b;
}

std::uint32_t GLDevice::renderTargetSampleCounts(Format format) const {
    std::uint32_t mask = 1;
    if (!gl().glGetInternalformativ) {
        for (std::uint32_t s = 2; s <= m_features.maxMsaaSamples; s *= 2)
            mask |= s;
        return mask;
    }
    GLint n = 0;
    const GLenum internal = toGl(format).internal;
    gl().glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, internal, GL_NUM_SAMPLE_COUNTS, 1, &n);
    std::vector<GLint> counts(static_cast<std::size_t>(std::max(n, 0)));
    if (n > 0) gl().glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, internal, GL_SAMPLES, n, counts.data());
    for (GLint s : counts)
        if (s > 1 && s <= 64) mask |= static_cast<std::uint32_t>(s);
    return mask;
}

// --- framebuffers ---------------------------------------------------------------------------------------
namespace {
void attach(GLenum attachment, GLTexture* t, std::uint32_t mip, std::uint32_t layer) {
    const TextureDesc& d = t->desc();
    if (d.type == TextureType::Cube)
        gl().glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer,
                                    t->name(), static_cast<GLint>(mip));
    else if (d.type == TextureType::Tex2DArray || d.type == TextureType::Tex3D)
        gl().glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, t->name(), static_cast<GLint>(mip),
                                       static_cast<GLint>(layer));
    else
        gl().glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, t->target(), t->name(),
                                    static_cast<GLint>(mip));
}

GLenum depthAttachment(Format f) {
    return formatInfo(f).stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
}
} // namespace

GLuint GLDevice::framebuffer(const std::vector<std::tuple<GLTexture*, std::uint32_t, std::uint32_t>>& colors,
                             GLTexture* depth) {
    std::vector<GLuint> key;
    key.reserve(colors.size() * 3 + 2);
    for (auto& [t, mip, layer] : colors)
        key.insert(key.end(), {t->name(), mip, layer});
    key.push_back(0xFFFFFFFFu);
    key.push_back(depth ? depth->name() : 0);
    if (auto it = m_framebuffers.find(key); it != m_framebuffers.end()) {
        gl().glBindFramebuffer(GL_FRAMEBUFFER, it->second);
        return it->second;
    }
    GLuint fbo = 0;
    gl().glGenFramebuffers(1, &fbo);
    gl().glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    std::vector<GLenum> drawBuffers;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        auto& [t, mip, layer] = colors[i];
        attach(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), t, mip, layer);
        drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
    }
    if (depth) attach(depthAttachment(depth->desc().format), depth, 0, 0);
    if (drawBuffers.empty())
        gl().glDrawBuffers(1, std::array<GLenum, 1>{GL_NONE}.data());
    else
        gl().glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
    const GLenum status = gl().glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gl().glDeleteFramebuffers(1, &fbo);
        throw Error(std::format("framebuffer incomplete ({:#x})", status));
    }
    m_framebuffers.emplace(std::move(key), fbo);
    return fbo;
}

GLuint GLDevice::readFramebuffer(GLTexture* texture, std::uint32_t mip, std::uint32_t layer) {
    if (formatInfo(texture->desc().format).depth) return framebuffer({}, texture); // mip 0 / layer 0 only
    return framebuffer({{texture, mip, layer}}, nullptr);
}

void GLDevice::forgetTexture(GLuint name) {
    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end();) {
        if (std::find(it->first.begin(), it->first.end(), name) != it->first.end()) {
            gl().glDeleteFramebuffers(1, &it->second);
            it = m_framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

// --- uploads and readbacks ------------------------------------------------------------------------------
void GLDevice::uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) {
    auto* b = static_cast<GLBuffer*>(dst);
    if (b->mapped()) {
        std::memcpy(static_cast<std::byte*>(b->mapped()) + dstOffset, data, size);
        return;
    }
    gl().glBindBuffer(GL_COPY_WRITE_BUFFER, b->name());
    gl().glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(dstOffset),
                         static_cast<GLsizeiptr>(size), data);
}

void GLDevice::uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                             std::uint32_t rowPitch) {
    auto* t = static_cast<GLTexture*>(dst);
    const TextureDesc& d = t->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const GlFormat& f = t->format();
    const auto w = static_cast<GLsizei>(std::max(1u, d.width >> mip));
    const auto h = static_cast<GLsizei>(std::max(1u, d.height >> mip));
    gl().glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl().glBindTexture(t->target(), t->name());
    gl().glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!f.compressed)
        gl().glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(rowPitch / fi.bytesPerBlock));
    const auto level = static_cast<GLint>(mip);
    const GLsizei rows = (h + static_cast<GLsizei>(fi.blockDim) - 1) / static_cast<GLsizei>(fi.blockDim);
    const auto bytes = static_cast<GLsizei>(rowPitch) * rows;
    if (d.type == TextureType::Tex2DArray || d.type == TextureType::Tex3D) {
        if (f.compressed)
            gl().glCompressedTexSubImage3D(t->target(), level, 0, 0, static_cast<GLint>(layer), w, h, 1,
                                           f.internal, bytes, data);
        else
            gl().glTexSubImage3D(t->target(), level, 0, 0, static_cast<GLint>(layer), w, h, 1, f.format,
                                 f.type, data);
    } else {
        const GLenum target =
            d.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer : t->target();
        if (f.compressed)
            gl().glCompressedTexSubImage2D(target, level, 0, 0, w, h, f.internal, bytes, data);
        else
            gl().glTexSubImage2D(target, level, 0, 0, w, h, f.format, f.type, data);
    }
    gl().glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl().glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

std::vector<std::uint8_t> GLDevice::readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) {
    auto* t = static_cast<GLTexture*>(src);
    const TextureDesc& d = t->desc();
    const FormatInfo& fi = formatInfo(d.format);
    if (t->format().compressed) throw Error("readbackTexture: compressed formats cannot be read back");
    const std::uint32_t w = std::max(1u, d.width >> mip), h = std::max(1u, d.height >> mip);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * fi.bytesPerBlock);
    GLuint prevFbo = 0;
    gl().glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&prevFbo));
    if (d.sampleCount > 1) throw Error("readbackTexture: resolve multisampled textures first");
    readFramebuffer(t, mip, layer);
    if (!fi.depth) gl().glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl().glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl().glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl().glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), t->format().format,
                      t->format().type, out.data());
    gl().glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl().glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    return out;
}

} // namespace gt::rhi::gl
