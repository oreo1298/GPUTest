#pragma once
// OpenGL backend internals: function table (loaded through SDL_GL_GetProcAddress, so nothing
// links against libGL/opengl32), format tables and small helpers. Targets OpenGL 3.3 core and
// uses newer functionality (compute, storage buffers/images, multi-draw indirect, clip control,
// texture views) when the context provides it.

#include "gt/rhi/rhi.h"

#include <SDL3/SDL_opengl.h>

#include <string>

namespace gt::rhi::gl {

// OpenGL 1.0/1.1 functions have no PFN typedefs in glext.h; decltype does not link against them.
#define GT_GL_PFN(name) decltype(&::name)

// X(name, PFN type). Everything goes through the table, even OpenGL 1.1 entry points.
#define GT_GL_FUNCTIONS(X)                                                                                   \
    X(glGetString, GT_GL_PFN(glGetString))                                                                   \
    X(glGetStringi, PFNGLGETSTRINGIPROC)                                                                     \
    X(glGetIntegerv, GT_GL_PFN(glGetIntegerv))                                                               \
    X(glGetIntegeri_v, PFNGLGETINTEGERI_VPROC)                                                               \
    X(glGetInteger64v, PFNGLGETINTEGER64VPROC)                                                               \
    X(glGetFloatv, GT_GL_PFN(glGetFloatv))                                                                   \
    X(glGetError, GT_GL_PFN(glGetError))                                                                     \
    X(glEnable, GT_GL_PFN(glEnable))                                                                         \
    X(glDisable, GT_GL_PFN(glDisable))                                                                       \
    X(glEnablei, PFNGLENABLEIPROC)                                                                           \
    X(glDisablei, PFNGLDISABLEIPROC)                                                                         \
    X(glViewport, GT_GL_PFN(glViewport))                                                                     \
    X(glScissor, GT_GL_PFN(glScissor))                                                                       \
    X(glDepthRange, GT_GL_PFN(glDepthRange))                                                                 \
    X(glClearBufferfv, PFNGLCLEARBUFFERFVPROC)                                                               \
    X(glClearBufferfi, PFNGLCLEARBUFFERFIPROC)                                                               \
    X(glClearBufferuiv, PFNGLCLEARBUFFERUIVPROC)                                                             \
    X(glClearBufferiv, PFNGLCLEARBUFFERIVPROC)                                                               \
    X(glColorMaski, PFNGLCOLORMASKIPROC)                                                                     \
    X(glBlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC)                                                       \
    X(glBlendEquationSeparate, PFNGLBLENDEQUATIONSEPARATEPROC)                                               \
    X(glBlendFuncSeparatei, PFNGLBLENDFUNCSEPARATEIPROC)                                                     \
    X(glBlendEquationSeparatei, PFNGLBLENDEQUATIONSEPARATEIPROC)                                             \
    X(glDepthFunc, GT_GL_PFN(glDepthFunc))                                                                   \
    X(glDepthMask, GT_GL_PFN(glDepthMask))                                                                   \
    X(glCullFace, GT_GL_PFN(glCullFace))                                                                     \
    X(glFrontFace, GT_GL_PFN(glFrontFace))                                                                   \
    X(glPolygonMode, GT_GL_PFN(glPolygonMode))                                                               \
    X(glPolygonOffset, GT_GL_PFN(glPolygonOffset))                                                           \
    X(glPatchParameteri, PFNGLPATCHPARAMETERIPROC)                                                           \
    X(glGenBuffers, PFNGLGENBUFFERSPROC)                                                                     \
    X(glDeleteBuffers, PFNGLDELETEBUFFERSPROC)                                                               \
    X(glBindBuffer, PFNGLBINDBUFFERPROC)                                                                     \
    X(glBufferData, PFNGLBUFFERDATAPROC)                                                                     \
    X(glBufferSubData, PFNGLBUFFERSUBDATAPROC)                                                               \
    X(glBufferStorage, PFNGLBUFFERSTORAGEPROC)                                                               \
    X(glBindBufferRange, PFNGLBINDBUFFERRANGEPROC)                                                           \
    X(glBindBufferBase, PFNGLBINDBUFFERBASEPROC)                                                             \
    X(glCopyBufferSubData, PFNGLCOPYBUFFERSUBDATAPROC)                                                       \
    X(glClearBufferSubData, PFNGLCLEARBUFFERSUBDATAPROC)                                                     \
    X(glMapBufferRange, PFNGLMAPBUFFERRANGEPROC)                                                             \
    X(glUnmapBuffer, PFNGLUNMAPBUFFERPROC)                                                                   \
    X(glGenTextures, GT_GL_PFN(glGenTextures))                                                               \
    X(glDeleteTextures, GT_GL_PFN(glDeleteTextures))                                                         \
    X(glBindTexture, GT_GL_PFN(glBindTexture))                                                               \
    X(glActiveTexture, PFNGLACTIVETEXTUREPROC)                                                               \
    X(glTexImage2D, GT_GL_PFN(glTexImage2D))                                                                 \
    X(glTexImage3D, PFNGLTEXIMAGE3DPROC)                                                                     \
    X(glTexImage2DMultisample, PFNGLTEXIMAGE2DMULTISAMPLEPROC)                                               \
    X(glTexStorage2D, PFNGLTEXSTORAGE2DPROC)                                                                 \
    X(glTexStorage3D, PFNGLTEXSTORAGE3DPROC)                                                                 \
    X(glTexSubImage2D, GT_GL_PFN(glTexSubImage2D))                                                           \
    X(glTexSubImage3D, PFNGLTEXSUBIMAGE3DPROC)                                                               \
    X(glCompressedTexSubImage2D, PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)                                           \
    X(glCompressedTexSubImage3D, PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)                                           \
    X(glTexParameteri, GT_GL_PFN(glTexParameteri))                                                           \
    X(glTextureView, PFNGLTEXTUREVIEWPROC)                                                                   \
    X(glCopyImageSubData, PFNGLCOPYIMAGESUBDATAPROC)                                                         \
    X(glPixelStorei, GT_GL_PFN(glPixelStorei))                                                               \
    X(glGenSamplers, PFNGLGENSAMPLERSPROC)                                                                   \
    X(glDeleteSamplers, PFNGLDELETESAMPLERSPROC)                                                             \
    X(glBindSampler, PFNGLBINDSAMPLERPROC)                                                                   \
    X(glSamplerParameteri, PFNGLSAMPLERPARAMETERIPROC)                                                       \
    X(glSamplerParameterf, PFNGLSAMPLERPARAMETERFPROC)                                                       \
    X(glSamplerParameterfv, PFNGLSAMPLERPARAMETERFVPROC)                                                     \
    X(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC)                                                           \
    X(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC)                                                     \
    X(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC)                                                           \
    X(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC)                                                 \
    X(glFramebufferTextureLayer, PFNGLFRAMEBUFFERTEXTURELAYERPROC)                                           \
    X(glDrawBuffers, PFNGLDRAWBUFFERSPROC)                                                                   \
    X(glReadBuffer, GT_GL_PFN(glReadBuffer))                                                                 \
    X(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC)                                             \
    X(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC)                                                           \
    X(glInvalidateFramebuffer, PFNGLINVALIDATEFRAMEBUFFERPROC)                                               \
    X(glReadPixels, GT_GL_PFN(glReadPixels))                                                                 \
    X(glCreateShader, PFNGLCREATESHADERPROC)                                                                 \
    X(glShaderSource, PFNGLSHADERSOURCEPROC)                                                                 \
    X(glCompileShader, PFNGLCOMPILESHADERPROC)                                                               \
    X(glGetShaderiv, PFNGLGETSHADERIVPROC)                                                                   \
    X(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC)                                                         \
    X(glDeleteShader, PFNGLDELETESHADERPROC)                                                                 \
    X(glCreateProgram, PFNGLCREATEPROGRAMPROC)                                                               \
    X(glAttachShader, PFNGLATTACHSHADERPROC)                                                                 \
    X(glLinkProgram, PFNGLLINKPROGRAMPROC)                                                                   \
    X(glGetProgramiv, PFNGLGETPROGRAMIVPROC)                                                                 \
    X(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC)                                                       \
    X(glDeleteProgram, PFNGLDELETEPROGRAMPROC)                                                               \
    X(glUseProgram, PFNGLUSEPROGRAMPROC)                                                                     \
    X(glGetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC)                                                 \
    X(glUniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC)                                                   \
    X(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC)                                                     \
    X(glUniform1i, PFNGLUNIFORM1IPROC)                                                                       \
    X(glGetProgramResourceIndex, PFNGLGETPROGRAMRESOURCEINDEXPROC)                                           \
    X(glShaderStorageBlockBinding, PFNGLSHADERSTORAGEBLOCKBINDINGPROC)                                       \
    X(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC)                                                           \
    X(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC)                                                     \
    X(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC)                                                           \
    X(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC)                                           \
    X(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC)                                         \
    X(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC)                                                   \
    X(glVertexAttribIPointer, PFNGLVERTEXATTRIBIPOINTERPROC)                                                 \
    X(glVertexAttribDivisor, PFNGLVERTEXATTRIBDIVISORPROC)                                                   \
    X(glDrawArraysInstanced, PFNGLDRAWARRAYSINSTANCEDPROC)                                                   \
    X(glDrawArraysInstancedBaseInstance, PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC)                           \
    X(glDrawElementsInstancedBaseVertex, PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXPROC)                           \
    X(glDrawElementsInstancedBaseVertexBaseInstance, PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC)   \
    X(glDrawArraysIndirect, PFNGLDRAWARRAYSINDIRECTPROC)                                                     \
    X(glDrawElementsIndirect, PFNGLDRAWELEMENTSINDIRECTPROC)                                                 \
    X(glMultiDrawArraysIndirect, PFNGLMULTIDRAWARRAYSINDIRECTPROC)                                           \
    X(glMultiDrawElementsIndirect, PFNGLMULTIDRAWELEMENTSINDIRECTPROC)                                       \
    X(glDispatchCompute, PFNGLDISPATCHCOMPUTEPROC)                                                           \
    X(glDispatchComputeIndirect, PFNGLDISPATCHCOMPUTEINDIRECTPROC)                                           \
    X(glMemoryBarrier, PFNGLMEMORYBARRIERPROC)                                                               \
    X(glBindImageTexture, PFNGLBINDIMAGETEXTUREPROC)                                                         \
    X(glGenQueries, PFNGLGENQUERIESPROC)                                                                     \
    X(glDeleteQueries, PFNGLDELETEQUERIESPROC)                                                               \
    X(glQueryCounter, PFNGLQUERYCOUNTERPROC)                                                                 \
    X(glBeginQuery, PFNGLBEGINQUERYPROC)                                                                     \
    X(glEndQuery, PFNGLENDQUERYPROC)                                                                         \
    X(glGetQueryObjectui64v, PFNGLGETQUERYOBJECTUI64VPROC)                                                   \
    X(glGetQueryObjectuiv, PFNGLGETQUERYOBJECTUIVPROC)                                                       \
    X(glFenceSync, PFNGLFENCESYNCPROC)                                                                       \
    X(glClientWaitSync, PFNGLCLIENTWAITSYNCPROC)                                                             \
    X(glDeleteSync, PFNGLDELETESYNCPROC)                                                                     \
    X(glFlush, GT_GL_PFN(glFlush))                                                                           \
    X(glFinish, GT_GL_PFN(glFinish))                                                                         \
    X(glDebugMessageCallback, PFNGLDEBUGMESSAGECALLBACKPROC)                                                 \
    X(glDebugMessageControl, PFNGLDEBUGMESSAGECONTROLPROC)                                                   \
    X(glPushDebugGroup, PFNGLPUSHDEBUGGROUPPROC)                                                             \
    X(glPopDebugGroup, PFNGLPOPDEBUGGROUPPROC)                                                               \
    X(glObjectLabel, PFNGLOBJECTLABELPROC)                                                                   \
    X(glClipControl, PFNGLCLIPCONTROLPROC)                                                                   \
    X(glGetInternalformativ, PFNGLGETINTERNALFORMATIVPROC)

/// Entry points of the current context (null when the context lacks them).
struct Functions {
#define GT_GL_DECLARE(name, type) type name = nullptr;
    GT_GL_FUNCTIONS(GT_GL_DECLARE)
#undef GT_GL_DECLARE
    /// Loads every entry point; returns the names of the ones missing.
    std::string load();
};

/// Functions of the current context. One context per process (the device's).
const Functions& gl();
Functions& glMutable();

struct GlFormat {
    GLenum internal = 0; ///< sized internal format
    GLenum format = 0;   ///< pixel transfer format
    GLenum type = 0;     ///< pixel transfer type
    bool compressed = false;
};
GlFormat toGl(Format f);
GLenum toGl(CompareOp op);
GLenum toGl(BlendFactor f);
GLenum toGl(BlendOp op);
GLenum toGlWrap(AddressMode m);
/// Vertex attribute: component count, type, normalized, integer.
struct GlAttrib {
    GLint components = 0;
    GLenum type = 0;
    bool normalized = false;
    bool integer = false;
};
GlAttrib toGlAttrib(Format f);

std::string glErrorName(GLenum e);

} // namespace gt::rhi::gl
