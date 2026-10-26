#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gt::shaders {

enum class Format { Spirv, Dxil, Dxbc, Glsl };

std::string_view formatName(Format f);

struct Blob {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    explicit operator bool() const { return data != nullptr && size > 0; }
    std::span<const std::uint8_t> bytes() const { return {data, size}; }
    std::string_view text() const { return {reinterpret_cast<const char*>(data), size}; }
};

/// Looks up a compiled entry point. `module` is the path below shaders/src without
/// ".slang" (e.g. "common/blit"), `entry` the entry-point name, optionally suffixed
/// with "@variant" for manifest entries that set name=.
Blob find(std::string_view module, std::string_view entry, Format format);

/// All embedded keys ("module:entry:format"), for diagnostics.
std::vector<std::string> keys();

} // namespace gt::shaders
