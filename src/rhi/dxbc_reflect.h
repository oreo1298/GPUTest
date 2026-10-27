#pragma once
// Resource bindings declared by a DXBC shader (its RDEF chunk). Direct3D 11 views must match what a
// register is declared as (a structured buffer of a given stride, a raw buffer or a texture), so the
// D3D11 backend creates its views from this instead of from the buffer alone. Portable so it can be
// unit-tested off Windows.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gt::rhi::dxbc {

enum class ResourceKind : std::uint8_t { ConstantBuffer, Texture, Sampler, StructuredBuffer, RawBuffer, TypedBuffer, Other };

struct ResourceBinding {
    std::string name;
    ResourceKind kind = ResourceKind::Other;
    bool uav = false;          ///< u# register (read-write); t# otherwise for buffers and textures
    std::uint32_t slot = 0;    ///< first register
    std::uint32_t count = 1;   ///< registers used (arrays)
    std::uint32_t stride = 0;  ///< element size of structured buffers
};

/// Parses the bindings of a DXBC container. Throws rhi::Error if the blob is not DXBC or has no
/// reflection data.
std::vector<ResourceBinding> reflect(std::span<const std::uint8_t> blob);

} // namespace gt::rhi::dxbc
