#include "gl_device.h"

#include <algorithm>
#include <cstring>
#include <format>

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

namespace gt::rhi::gl {

// --- buffers ----------------------------------------------------------------------------------------
GLBuffer::GLBuffer(GLDevice& dev, const BufferDesc& desc, const void* data) : m_dev(dev) {
    m_desc = desc;
    const auto size = static_cast<GLsizeiptr>(std::max<std::uint64_t>(desc.size, 4));
    gl().glGenBuffers(1, &m_name);
    gl().glBindBuffer(GL_COPY_WRITE_BUFFER, m_name);
    GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
    switch (desc.memory) {
    case MemoryType::Device:
        break;
    case MemoryType::Upload:
    case MemoryType::DeviceUpload:
        flags |= GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        break;
    case MemoryType::Readback:
        flags |= GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT;
        break;
    }
    gl().glBufferStorage(GL_COPY_WRITE_BUFFER, size, data, flags);
    if (desc.memory != MemoryType::Device) {
        const GLbitfield access =
            flags & (GL_MAP_WRITE_BIT | GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        m_mapped = gl().glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size, access);
        if (!m_mapped) throw Error(std::format("cannot map buffer '{}' ({} bytes)", desc.name, desc.size));
    }
    dev.label(GL_BUFFER, m_name, desc.name);
}

GLBuffer::~GLBuffer() {
    if (m_mapped) {
        gl().glBindBuffer(GL_COPY_WRITE_BUFFER, m_name);
        gl().glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    }
    gl().glDeleteBuffers(1, &m_name);
}

// --- textures --------------------------------------------------------------------------------------
GLTexture::GLTexture(GLDevice& dev, const TextureDesc& desc) : m_dev(dev) {
    m_desc = desc;
    m_format = toGl(desc.format);
    if (!m_format.internal)
        throw Error(std::format("texture '{}': format not supported on OpenGL", desc.name));
    if (any(desc.usage & TextureUsage::ShadingRate))
        throw Error(std::format("texture '{}': shading-rate images are not available on OpenGL", desc.name));
    const auto w = static_cast<GLsizei>(desc.width), h = static_cast<GLsizei>(desc.height);
    const auto mips = static_cast<GLsizei>(std::max(1u, desc.mipLevels));
    switch (desc.type) {
    case TextureType::Tex2D:
        m_target = desc.sampleCount > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
        break;
    case TextureType::Tex2DArray:
        m_target = GL_TEXTURE_2D_ARRAY;
        break;
    case TextureType::Tex3D:
        m_target = GL_TEXTURE_3D;
        break;
    case TextureType::Cube:
        m_target = GL_TEXTURE_CUBE_MAP;
        break;
    }
    gl().glGenTextures(1, &m_name);
    gl().glBindTexture(m_target, m_name);
    const bool storage = gl().glTexStorage2D && gl().glTexStorage3D;
    if (m_target == GL_TEXTURE_2D_MULTISAMPLE) {
        gl().glTexImage2DMultisample(m_target, static_cast<GLsizei>(desc.sampleCount), m_format.internal, w,
                                     h, GL_TRUE);
    } else if (m_target == GL_TEXTURE_2D || m_target == GL_TEXTURE_CUBE_MAP) {
        if (storage) {
            gl().glTexStorage2D(m_target, mips, m_format.internal, w, h);
        } else {
            for (GLint m = 0; m < mips; ++m) {
                const GLsizei mw = std::max(1, w >> m), mh = std::max(1, h >> m);
                if (m_target == GL_TEXTURE_CUBE_MAP)
                    for (GLenum f = 0; f < 6; ++f)
                        gl().glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, m,
                                          static_cast<GLint>(m_format.internal), mw, mh, 0, m_format.format,
                                          m_format.type, nullptr);
                else
                    gl().glTexImage2D(m_target, m, static_cast<GLint>(m_format.internal), mw, mh, 0,
                                      m_format.format, m_format.type, nullptr);
            }
        }
    } else {
        const auto d = static_cast<GLsizei>(std::max(1u, desc.depthOrLayers));
        if (storage) {
            gl().glTexStorage3D(m_target, mips, m_format.internal, w, h, d);
        } else {
            for (GLint m = 0; m < mips; ++m)
                gl().glTexImage3D(m_target, m, static_cast<GLint>(m_format.internal), std::max(1, w >> m),
                                  std::max(1, h >> m), m_target == GL_TEXTURE_3D ? std::max(1, d >> m) : d, 0,
                                  m_format.format, m_format.type, nullptr);
        }
    }
    if (m_target != GL_TEXTURE_2D_MULTISAMPLE) {
        gl().glTexParameteri(m_target, GL_TEXTURE_BASE_LEVEL, 0);
        gl().glTexParameteri(m_target, GL_TEXTURE_MAX_LEVEL, mips - 1);
    }
    dev.label(GL_TEXTURE, m_name, desc.name);
}

GLTexture::~GLTexture() {
    m_dev.forgetTexture(m_name);
    for (auto& [key, v] : m_views) {
        m_dev.forgetTexture(v.name);
        gl().glDeleteTextures(1, &v.name);
    }
    gl().glDeleteTextures(1, &m_name);
}

GLTexture::View GLTexture::view(Subresource sub) {
    const std::uint32_t mips = std::max(1u, m_desc.mipLevels);
    const std::uint32_t layers = m_desc.type == TextureType::Tex3D  ? 1u
                                 : m_desc.type == TextureType::Cube ? 6u
                                                                    : std::max(1u, m_desc.depthOrLayers);
    const std::uint32_t mipCount = sub.mipCount ? sub.mipCount : mips - sub.baseMip;
    const std::uint32_t layerCount = sub.layerCount ? sub.layerCount : layers - sub.baseLayer;
    if (sub.baseMip == 0 && mipCount == mips && sub.baseLayer == 0 && layerCount == layers)
        return {m_name, m_target};
    const auto key = std::make_tuple(sub.baseMip, mipCount, sub.baseLayer, layerCount);
    if (auto it = m_views.find(key); it != m_views.end()) return it->second;
    if (!m_dev.hasTextureViews())
        throw Error(
            std::format("texture '{}': sampling a sub-range needs texture views (OpenGL 4.3)", m_desc.name));
    GLenum target = m_target;
    if (m_target == GL_TEXTURE_2D_ARRAY && layerCount == 1) target = GL_TEXTURE_2D;
    if (m_target == GL_TEXTURE_CUBE_MAP && layerCount != 6)
        target = layerCount == 1 ? GL_TEXTURE_2D : GL_TEXTURE_2D_ARRAY;
    GLuint v = 0;
    gl().glGenTextures(1, &v);
    gl().glTextureView(v, target, m_name, m_format.internal, sub.baseMip, mipCount, sub.baseLayer,
                       layerCount);
    m_views.emplace(key, View{v, target});
    return {v, target};
}

// --- samplers --------------------------------------------------------------------------------------
GLSampler::GLSampler(GLDevice& dev, const SamplerDesc& d) {
    gl().glGenSamplers(1, &m_name);
    const bool linearMin = d.minFilter == Filter::Linear, linearMip = d.mipFilter == Filter::Linear;
    const GLint minFilter = linearMin ? (linearMip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                      : (linearMip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    gl().glSamplerParameteri(m_name, GL_TEXTURE_MIN_FILTER, minFilter);
    gl().glSamplerParameteri(m_name, GL_TEXTURE_MAG_FILTER,
                             d.magFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
    gl().glSamplerParameteri(m_name, GL_TEXTURE_WRAP_S, static_cast<GLint>(toGlWrap(d.addressU)));
    gl().glSamplerParameteri(m_name, GL_TEXTURE_WRAP_T, static_cast<GLint>(toGlWrap(d.addressV)));
    gl().glSamplerParameteri(m_name, GL_TEXTURE_WRAP_R, static_cast<GLint>(toGlWrap(d.addressW)));
    gl().glSamplerParameterf(m_name, GL_TEXTURE_LOD_BIAS, d.mipLodBias);
    gl().glSamplerParameterf(m_name, GL_TEXTURE_MIN_LOD, d.minLod);
    gl().glSamplerParameterf(m_name, GL_TEXTURE_MAX_LOD, d.maxLod);
    if (d.maxAnisotropy > 1.0f && dev.features().anisotropicFiltering) {
        GLfloat maxAniso = 1.0f;
        gl().glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        gl().glSamplerParameterf(m_name, GL_TEXTURE_MAX_ANISOTROPY, std::min(d.maxAnisotropy, maxAniso));
    }
    if (d.compare) {
        gl().glSamplerParameteri(m_name, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl().glSamplerParameteri(m_name, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(toGl(*d.compare)));
    }
    const GLfloat border[4] = {d.borderWhite ? 1.0f : 0.0f, d.borderWhite ? 1.0f : 0.0f,
                               d.borderWhite ? 1.0f : 0.0f, d.borderWhite ? 1.0f : 0.0f};
    gl().glSamplerParameterfv(m_name, GL_TEXTURE_BORDER_COLOR, border);
}

GLSampler::~GLSampler() {
    gl().glDeleteSamplers(1, &m_name);
}

// --- queries --------------------------------------------------------------------------------------------
GLQueryPool::GLQueryPool(GLDevice& dev, QueryType type, std::uint32_t count) : m_dev(dev) {
    if (type == QueryType::PipelineStatistics)
        throw Error("pipeline statistics queries are not available on OpenGL");
    m_type = type;
    m_count = count;
    m_names.resize(count);
    gl().glGenQueries(static_cast<GLsizei>(count), m_names.data());
}

GLQueryPool::~GLQueryPool() {
    gl().glDeleteQueries(static_cast<GLsizei>(m_names.size()), m_names.data());
}

GLenum GLQueryPool::target() const {
    return m_type == QueryType::Timestamp ? GL_TIMESTAMP : GL_SAMPLES_PASSED;
}

bool GLQueryPool::read(std::uint32_t first, std::uint32_t count, std::span<std::uint64_t> out) {
    for (std::uint32_t i = 0; i < count; ++i) {
        GLuint available = 0;
        gl().glGetQueryObjectuiv(m_names[first + i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return false;
        GLuint64 v = 0;
        gl().glGetQueryObjectui64v(m_names[first + i], GL_QUERY_RESULT, &v);
        out[i] = v; // timestamps are already in nanoseconds
    }
    return true;
}

} // namespace gt::rhi::gl
