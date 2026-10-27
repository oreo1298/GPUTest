#include "gl_common.h"

#include <SDL3/SDL_video.h>

#include <format>

namespace gt::rhi::gl {
namespace {
Functions g_functions;
} // namespace

std::string Functions::load() {
    std::string missing;
#define GT_GL_LOAD(name, type)                                                                               \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                                             \
    if (!name) missing += std::string(missing.empty() ? "" : ", ") + #name;
    GT_GL_FUNCTIONS(GT_GL_LOAD)
#undef GT_GL_LOAD
    return missing;
}

const Functions& gl() {
    return g_functions;
}
Functions& glMutable() {
    return g_functions;
}

GlFormat toGl(Format f) {
    switch (f) {
    case Format::Unknown:
        break;
    case Format::R8_UNORM:
        return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case Format::RG8_UNORM:
        return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case Format::RGBA8_UNORM:
        return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::RGBA8_SRGB:
        return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::BGRA8_UNORM:
        return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE};
    case Format::BGRA8_SRGB:
        return {GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE};
    case Format::R16_FLOAT:
        return {GL_R16F, GL_RED, GL_HALF_FLOAT};
    case Format::RG16_FLOAT:
        return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
    case Format::RGBA16_FLOAT:
        return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    case Format::RGBA16_UNORM:
        return {GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT};
    case Format::R32_FLOAT:
        return {GL_R32F, GL_RED, GL_FLOAT};
    case Format::RG32_FLOAT:
        return {GL_RG32F, GL_RG, GL_FLOAT};
    case Format::RGB32_FLOAT:
        return {GL_RGB32F, GL_RGB, GL_FLOAT};
    case Format::RGBA32_FLOAT:
        return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
    case Format::R32_UINT:
        return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT};
    case Format::RG32_UINT:
        return {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT};
    case Format::RGBA32_UINT:
        return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT};
    case Format::R32_SINT:
        return {GL_R32I, GL_RED_INTEGER, GL_INT};
    case Format::R16_UINT:
        return {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT};
    case Format::R8_UINT:
        return {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE};
    case Format::R11G11B10_FLOAT:
        return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV};
    case Format::RGB10A2_UNORM:
        return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV};
    case Format::D16_UNORM:
        return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
    case Format::D24_UNORM_S8_UINT:
        return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
    case Format::D32_FLOAT:
        return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
    case Format::D32_FLOAT_S8_UINT:
        return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV};
    case Format::BC1_UNORM:
        return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_RGBA, GL_UNSIGNED_BYTE, true};
    case Format::BC3_UNORM:
        return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_RGBA, GL_UNSIGNED_BYTE, true};
    case Format::BC5_UNORM:
        return {GL_COMPRESSED_RG_RGTC2, GL_RG, GL_UNSIGNED_BYTE, true};
    case Format::BC7_UNORM:
        return {GL_COMPRESSED_RGBA_BPTC_UNORM, GL_RGBA, GL_UNSIGNED_BYTE, true};
    case Format::Count:
        break;
    }
    return {};
}

GLenum toGl(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return GL_NEVER;
    case CompareOp::Less:
        return GL_LESS;
    case CompareOp::Equal:
        return GL_EQUAL;
    case CompareOp::LessEqual:
        return GL_LEQUAL;
    case CompareOp::Greater:
        return GL_GREATER;
    case CompareOp::NotEqual:
        return GL_NOTEQUAL;
    case CompareOp::GreaterEqual:
        return GL_GEQUAL;
    case CompareOp::Always:
        return GL_ALWAYS;
    }
    return GL_ALWAYS;
}

GLenum toGl(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SrcColor:
        return GL_SRC_COLOR;
    case BlendFactor::InvSrcColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstColor:
        return GL_DST_COLOR;
    case BlendFactor::InvDstColor:
        return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::DstAlpha:
        return GL_DST_ALPHA;
    case BlendFactor::InvDstAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

GLenum toGl(BlendOp op) {
    switch (op) {
    case BlendOp::Add:
        return GL_FUNC_ADD;
    case BlendOp::Subtract:
        return GL_FUNC_SUBTRACT;
    case BlendOp::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Min:
        return GL_MIN;
    case BlendOp::Max:
        return GL_MAX;
    }
    return GL_FUNC_ADD;
}

GLenum toGlWrap(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap:
        return GL_REPEAT;
    case AddressMode::Clamp:
        return GL_CLAMP_TO_EDGE;
    case AddressMode::Mirror:
        return GL_MIRRORED_REPEAT;
    case AddressMode::Border:
        return GL_CLAMP_TO_BORDER;
    }
    return GL_REPEAT;
}

GlAttrib toGlAttrib(Format f) {
    switch (f) {
    case Format::R32_FLOAT:
        return {1, GL_FLOAT, false, false};
    case Format::RG32_FLOAT:
        return {2, GL_FLOAT, false, false};
    case Format::RGB32_FLOAT:
        return {3, GL_FLOAT, false, false};
    case Format::RGBA32_FLOAT:
        return {4, GL_FLOAT, false, false};
    case Format::R16_FLOAT:
        return {1, GL_HALF_FLOAT, false, false};
    case Format::RG16_FLOAT:
        return {2, GL_HALF_FLOAT, false, false};
    case Format::RGBA16_FLOAT:
        return {4, GL_HALF_FLOAT, false, false};
    case Format::R8_UNORM:
        return {1, GL_UNSIGNED_BYTE, true, false};
    case Format::RG8_UNORM:
        return {2, GL_UNSIGNED_BYTE, true, false};
    case Format::RGBA8_UNORM:
        return {4, GL_UNSIGNED_BYTE, true, false};
    case Format::RGBA16_UNORM:
        return {4, GL_UNSIGNED_SHORT, true, false};
    case Format::R32_UINT:
        return {1, GL_UNSIGNED_INT, false, true};
    case Format::RG32_UINT:
        return {2, GL_UNSIGNED_INT, false, true};
    case Format::RGBA32_UINT:
        return {4, GL_UNSIGNED_INT, false, true};
    case Format::R32_SINT:
        return {1, GL_INT, false, true};
    case Format::R16_UINT:
        return {1, GL_UNSIGNED_SHORT, false, true};
    case Format::R8_UINT:
        return {1, GL_UNSIGNED_BYTE, false, true};
    case Format::RGB10A2_UNORM:
        return {4, GL_UNSIGNED_INT_2_10_10_10_REV, true, false};
    default:
        break;
    }
    throw Error(std::format("format {} cannot be a vertex attribute", formatInfo(f).name));
}

std::string glErrorName(GLenum e) {
    switch (e) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return std::format("GL error {:#x}", e);
    }
}

} // namespace gt::rhi::gl
