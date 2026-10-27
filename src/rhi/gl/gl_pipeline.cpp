#include "gl_device.h"

#include "gt/shaders.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <sstream>

namespace gt::rhi::gl {
namespace {

std::string shaderSource(const ShaderRef& ref) {
    if (!ref.bytecode.empty())
        return {reinterpret_cast<const char*>(ref.bytecode.data()), ref.bytecode.size()};
    const shaders::Blob b = shaders::find(ref.module, ref.entry, shaders::Format::Glsl);
    if (!b)
        throw Error(
            std::format("shader {}:{} has no OpenGL (GLSL) variant in this build", ref.module, ref.entry));
    return std::string(b.text());
}

GLuint compile(GLenum stage, const std::string& source, const ShaderRef& ref) {
    GLuint s = gl().glCreateShader(stage);
    const char* text = source.c_str();
    gl().glShaderSource(s, 1, &text, nullptr);
    gl().glCompileShader(s);
    GLint ok = 0;
    gl().glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl().glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        gl().glGetShaderInfoLog(s, len, nullptr, log.data());
        gl().glDeleteShader(s);
        throw Error(std::format("GLSL compile error in {}:{}: {}", ref.module, ref.entry, log.c_str()));
    }
    return s;
}

/// The binding table gt_glslgen writes after #version ("// gt:<kind> <name> <args...>").
struct TableEntry {
    std::string kind, name, a, b;
};
std::vector<TableEntry> bindingTable(const std::string& source) {
    std::vector<TableEntry> out;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("// gt:", 0) != 0) {
            if (line.rfind("#version", 0) == 0) continue;
            break; // the table directly follows #version
        }
        std::istringstream l(line.substr(6));
        TableEntry e;
        l >> e.kind >> e.name >> e.a >> e.b;
        out.push_back(e);
    }
    return out;
}

GLenum primitiveOf(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList:
        return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip:
        return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:
        return GL_LINES;
    case PrimitiveTopology::LineStrip:
        return GL_LINE_STRIP;
    case PrimitiveTopology::PointList:
        return GL_POINTS;
    case PrimitiveTopology::PatchList:
        return GL_PATCHES;
    }
    return GL_TRIANGLES;
}

std::uint32_t slotOf(const std::string& s) {
    return static_cast<std::uint32_t>(std::stoul(s));
}

} // namespace

GLPipeline::GLPipeline(GLDevice& dev, const GraphicsPipelineDesc& d) : m_dev(dev) {
    const auto t0 = std::chrono::steady_clock::now();
    if (d.mesh.valid() || d.task.valid()) throw Error("mesh shaders are not available on OpenGL");
    if (d.variableRate) throw Error("variable-rate shading is not available on OpenGL");
    if ((d.hull.valid() || d.domain.valid()) && !dev.features().tessellation)
        throw Error("tessellation needs OpenGL 4.0");
    m_graphics = d;
    m_graphics.layout = d.layout;
    m_primitive = primitiveOf(d.topology);
    std::vector<std::pair<GLenum, ShaderRef>> stages;
    stages.emplace_back(GL_VERTEX_SHADER, d.vertex);
    if (d.hull.valid()) stages.emplace_back(GL_TESS_CONTROL_SHADER, d.hull);
    if (d.domain.valid()) stages.emplace_back(GL_TESS_EVALUATION_SHADER, d.domain);
    if (d.geometry.valid()) stages.emplace_back(GL_GEOMETRY_SHADER, d.geometry);
    if (d.fragment.valid()) stages.emplace_back(GL_FRAGMENT_SHADER, d.fragment);
    link(stages, d.name);
    m_creationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

GLPipeline::GLPipeline(GLDevice& dev, const ComputePipelineDesc& d) : m_dev(dev) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = true;
    m_graphics.layout = d.layout;
    link({{GL_COMPUTE_SHADER, d.compute}}, d.name);
    m_creationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

GLPipeline::~GLPipeline() {
    if (m_dev.executor().pipeline == this) m_dev.executor().pipeline = nullptr;
    gl().glDeleteProgram(m_program);
}

void GLPipeline::link(const std::vector<std::pair<GLenum, ShaderRef>>& stages, const char* name) {
    m_program = gl().glCreateProgram();
    std::vector<GLuint> shaders;
    std::vector<TableEntry> table;
    try {
        for (const auto& [stage, ref] : stages) {
            const std::string src = shaderSource(ref);
            for (const TableEntry& e : bindingTable(src)) {
                const bool known = std::any_of(table.begin(), table.end(), [&](const TableEntry& t) {
                    return t.kind == e.kind && t.name == e.name;
                });
                if (!known) table.push_back(e);
            }
            shaders.push_back(compile(stage, src, ref));
            gl().glAttachShader(m_program, shaders.back());
        }
    } catch (...) {
        for (GLuint s : shaders)
            gl().glDeleteShader(s);
        gl().glDeleteProgram(m_program);
        m_program = 0;
        throw;
    }
    gl().glLinkProgram(m_program);
    for (GLuint s : shaders)
        gl().glDeleteShader(s);
    GLint ok = 0;
    gl().glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl().glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        gl().glGetProgramInfoLog(m_program, len, nullptr, log.data());
        gl().glDeleteProgram(m_program);
        m_program = 0;
        throw Error(std::format("GLSL link error in pipeline '{}': {}", name, log.c_str()));
    }
    m_dev.label(GL_PROGRAM, m_program, name);

    // Binding points are assigned densely per program; the executor binds by these tables.
    gl().glUseProgram(m_program);
    m_dev.executor().pipelineDirty = true;
    GLuint uboBinding = 0, ssboBinding = 0;
    GLint imageUnit = 0, textureUnit = 0;
    for (const TableEntry& e : table) {
        if (e.kind == "ubo" || e.kind == "push") {
            const GLuint index = gl().glGetUniformBlockIndex(m_program, e.name.c_str());
            if (index == GL_INVALID_INDEX) continue; // optimised away
            gl().glUniformBlockBinding(m_program, index, uboBinding);
            if (e.kind == "push") {
                m_bindings.pushBinding = static_cast<int>(uboBinding);
                m_bindings.ubos.push_back(
                    0xFFFFFFFFu); // keeps ubos[k] == binding k; the executor binds push data
            } else {
                m_bindings.ubos.push_back(slotOf(e.a));
            }
            ++uboBinding;
        } else if (e.kind == "ssbo") {
            const GLuint index =
                gl().glGetProgramResourceIndex(m_program, GL_SHADER_STORAGE_BLOCK, e.name.c_str());
            if (index == GL_INVALID_INDEX) continue;
            gl().glShaderStorageBlockBinding(m_program, index, ssboBinding++);
            m_bindings.ssbos.emplace_back(e.a[0], slotOf(e.a.substr(1)));
        } else if (e.kind == "image") {
            const GLint loc = gl().glGetUniformLocation(m_program, e.name.c_str());
            if (loc < 0) continue;
            gl().glUniform1i(loc, imageUnit++);
            m_bindings.images.push_back(slotOf(e.a));
        } else if (e.kind == "tex") {
            const GLint loc = gl().glGetUniformLocation(m_program, e.name.c_str());
            if (loc < 0) continue;
            gl().glUniform1i(loc, textureUnit++);
            m_bindings.textures.push_back({slotOf(e.a), e.b == "-" ? -1 : static_cast<int>(slotOf(e.b))});
        }
    }
    m_bindings.baseVertex = gl().glGetUniformLocation(m_program, "SPIRV_Cross_BaseVertex");
    m_bindings.baseInstance = gl().glGetUniformLocation(m_program, "SPIRV_Cross_BaseInstance");
}

// --- executor: pipeline state and bindings ------------------------------------------------------------
void Executor::applyPipeline() {
    if (!pipeline) throw Error("draw/dispatch without a pipeline");
    gl().glUseProgram(pipeline->program());
    pipelineDirty = false;
    vertexDirty = true;
    if (pipeline->isCompute()) return;
    const GraphicsPipelineDesc& g = pipeline->graphics();
    const RasterState& r = g.raster;
    if (r.cull == CullMode::None) {
        gl().glDisable(GL_CULL_FACE);
    } else {
        gl().glEnable(GL_CULL_FACE);
        gl().glCullFace(r.cull == CullMode::Front ? GL_FRONT : GL_BACK);
    }
    // Rendering is not flipped in y, so GL's window-space winding is the mirror of Vulkan's/D3D's.
    gl().glFrontFace(r.frontCounterClockwise ? GL_CW : GL_CCW);
    gl().glPolygonMode(GL_FRONT_AND_BACK, r.wireframe ? GL_LINE : GL_FILL);
    if (r.depthClamp)
        gl().glEnable(GL_DEPTH_CLAMP);
    else
        gl().glDisable(GL_DEPTH_CLAMP);
    if (r.depthBias != 0.0f || r.depthBiasSlope != 0.0f) {
        gl().glEnable(GL_POLYGON_OFFSET_FILL);
        gl().glPolygonOffset(r.depthBiasSlope, r.depthBias);
    } else {
        gl().glDisable(GL_POLYGON_OFFSET_FILL);
    }
    const DepthState& ds = g.depth;
    if (g.depthFormat == Format::Unknown || (!ds.test && !ds.write)) {
        gl().glDisable(GL_DEPTH_TEST);
    } else {
        gl().glEnable(GL_DEPTH_TEST); // GL writes depth only with the test enabled
        gl().glDepthFunc(ds.test ? toGl(ds.compare) : GL_ALWAYS);
    }
    gl().glDepthMask(ds.write && !depthReadOnly ? GL_TRUE : GL_FALSE);
    const bool perTarget = gl().glBlendFuncSeparatei && gl().glBlendEquationSeparatei;
    for (std::size_t i = 0; i < std::max<std::size_t>(g.colorFormats.size(), 1); ++i) {
        const BlendAttachment b = i < g.blend.size() ? g.blend[i] : BlendAttachment{};
        const auto idx = static_cast<GLuint>(i);
        if (b.enable) {
            gl().glEnablei(GL_BLEND, idx);
            if (perTarget) {
                gl().glBlendFuncSeparatei(idx, toGl(b.srcColor), toGl(b.dstColor), toGl(b.srcAlpha),
                                          toGl(b.dstAlpha));
                gl().glBlendEquationSeparatei(idx, toGl(b.colorOp), toGl(b.alphaOp));
            } else if (i == 0) {
                gl().glBlendFuncSeparate(toGl(b.srcColor), toGl(b.dstColor), toGl(b.srcAlpha),
                                         toGl(b.dstAlpha));
                gl().glBlendEquationSeparate(toGl(b.colorOp), toGl(b.alphaOp));
            }
        } else {
            gl().glDisablei(GL_BLEND, idx);
        }
        gl().glColorMaski(idx, (b.writeMask & 1) != 0, (b.writeMask & 2) != 0, (b.writeMask & 4) != 0,
                          (b.writeMask & 8) != 0);
    }
    if (g.topology == PrimitiveTopology::PatchList && gl().glPatchParameteri)
        gl().glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(std::max(1u, g.patchControlPoints)));
}

void Executor::applyBindings() {
    const ProgramBindings& pb = pipeline->bindings();
    auto range = [](const BoundBuffer& b) {
        const std::uint64_t size = b.size ? b.size : b.buffer->desc().size - b.offset;
        return std::pair{static_cast<GLintptr>(b.offset), static_cast<GLsizeiptr>(size)};
    };
    for (std::size_t k = 0; k < pb.ubos.size(); ++k) {
        const BoundBuffer& b = static_cast<int>(k) == pb.pushBinding ? push : ubo[pb.ubos[k]];
        if (!b.buffer) continue; // validated at record time
        const auto [off, size] = range(b);
        gl().glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(k), b.buffer->name(), off, size);
    }
    for (std::size_t k = 0; k < pb.ssbos.size(); ++k) {
        const auto [cls, slot] = pb.ssbos[k];
        const BoundBuffer& b = cls == 't' ? srvBuf[slot] : uavBuf[slot];
        if (!b.buffer) continue;
        const auto [off, size] = range(b);
        gl().glBindBufferRange(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(k), b.buffer->name(), off, size);
    }
    for (std::size_t k = 0; k < pb.images.size(); ++k) {
        const BoundTexture& t = uavTex[pb.images[k]];
        if (!t.texture) continue;
        const TextureDesc& d = t.texture->desc();
        const bool layered = d.type != TextureType::Tex2D;
        gl().glBindImageTexture(static_cast<GLuint>(k), t.texture->name(), static_cast<GLint>(t.mip),
                                layered ? GL_TRUE : GL_FALSE, 0, GL_READ_WRITE, t.texture->format().internal);
    }
    for (std::size_t k = 0; k < pb.textures.size(); ++k) {
        const ProgramBindings::Tex& tex = pb.textures[k];
        BoundTexture& t = srvTex[tex.t];
        if (!t.texture) continue;
        const GLTexture::View v = t.texture->view(t.sub);
        gl().glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(k));
        gl().glBindTexture(v.target, v.name);
        GLSampler* s = tex.s >= 0 ? samplers[static_cast<std::size_t>(tex.s)] : nullptr;
        gl().glBindSampler(static_cast<GLuint>(k), s ? s->name() : 0);
    }
}

void Executor::applyVertexInput(std::int64_t baseVertexBytes) {
    (void)baseVertexBytes;
    const GraphicsPipelineDesc& g = pipeline->graphics();
    std::uint32_t mask = 0;
    for (const VertexAttribute& a : g.attributes) {
        const Vertex& vb = vertex[a.binding];
        if (!vb.buffer) continue;
        std::uint32_t stride = 0;
        bool perInstance = false;
        for (const VertexBinding& b : g.vertexBindings)
            if (b.binding == a.binding) stride = b.stride, perInstance = b.perInstance;
        const GlAttrib at = toGlAttrib(a.format);
        gl().glBindBuffer(GL_ARRAY_BUFFER, vb.buffer->name());
        const auto* ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(vb.offset + a.offset));
        if (at.integer)
            gl().glVertexAttribIPointer(a.location, at.components, at.type, static_cast<GLsizei>(stride),
                                        ptr);
        else
            gl().glVertexAttribPointer(a.location, at.components, at.type, at.normalized ? GL_TRUE : GL_FALSE,
                                       static_cast<GLsizei>(stride), ptr);
        gl().glVertexAttribDivisor(a.location, perInstance ? 1 : 0);
        mask |= 1u << a.location;
    }
    for (std::uint32_t l = 0; l < 32; ++l) {
        const bool want = (mask >> l) & 1u, have = (attribMask >> l) & 1u;
        if (want && !have) gl().glEnableVertexAttribArray(l);
        if (!want && have) gl().glDisableVertexAttribArray(l);
    }
    attribMask = mask;
    vertexDirty = false;
}

void Executor::prepareDraw(std::int32_t baseVertex, std::uint32_t baseInstance) {
    if (pipelineDirty) applyPipeline();
    applyBindings();
    if (vertexDirty) applyVertexInput();
    const ProgramBindings& pb = pipeline->bindings();
    if (pb.baseVertex >= 0) gl().glUniform1i(pb.baseVertex, baseVertex);
    if (pb.baseInstance >= 0) gl().glUniform1i(pb.baseInstance, static_cast<GLint>(baseInstance));
}

} // namespace gt::rhi::gl
