#include "gt/shaders.h"

#include <string>

namespace gt::shaders {
namespace detail {
struct EmbeddedFile {
    const char* key;
    const unsigned char* data;
    unsigned long long size;
};
extern const EmbeddedFile kBlobs[];
extern const unsigned long long kBlobsCount;
} // namespace detail

std::string_view formatName(Format f) {
    switch (f) {
    case Format::Spirv: return "spirv";
    case Format::Dxil: return "dxil";
    case Format::Dxbc: return "dxbc";
    case Format::Glsl: return "glsl";
    }
    return "?";
}

Blob find(std::string_view module, std::string_view entry, Format format) {
    std::string key;
    key.reserve(module.size() + entry.size() + 8);
    key.append(module).append(":").append(entry).append(":").append(formatName(format));
    for (unsigned long long i = 0; i < detail::kBlobsCount; ++i) {
        const auto& f = detail::kBlobs[i];
        if (f.key && key == f.key) return Blob{f.data, static_cast<std::size_t>(f.size)};
    }
    return {};
}

std::vector<std::string> keys() {
    std::vector<std::string> out;
    for (unsigned long long i = 0; i < detail::kBlobsCount; ++i)
        if (detail::kBlobs[i].key) out.emplace_back(detail::kBlobs[i].key);
    return out;
}

} // namespace gt::shaders
