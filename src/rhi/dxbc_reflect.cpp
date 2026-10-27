#include "dxbc_reflect.h"

#include "gt/rhi/types.h"

#include <cstring>
#include <format>

namespace gt::rhi::dxbc {
namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24;
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}
    std::uint32_t u32(std::size_t offset) const {
        if (offset + 4 > m_data.size()) throw Error("DXBC: truncated shader blob");
        std::uint32_t v = 0;
        std::memcpy(&v, m_data.data() + offset, 4);
        return v;
    }
    std::string str(std::size_t offset) const {
        std::string s;
        for (std::size_t i = offset; i < m_data.size() && m_data[i]; ++i) s.push_back(static_cast<char>(m_data[i]));
        return s;
    }
    std::span<const std::uint8_t> sub(std::size_t offset, std::size_t size) const {
        if (offset + size > m_data.size()) throw Error("DXBC: truncated shader blob");
        return m_data.subspan(offset, size);
    }

private:
    std::span<const std::uint8_t> m_data;
};

// D3D_SHADER_INPUT_TYPE and D3D_SRV_DIMENSION values used below.
enum : std::uint32_t {
    kCBuffer = 0,
    kTBuffer = 1,
    kTexture = 2,
    kSampler = 3,
    kUavTyped = 4,
    kStructured = 5,
    kUavStructured = 6,
    kByteAddress = 7,
    kUavByteAddress = 8,
    kUavAppend = 9,
    kUavConsume = 10,
    kUavStructuredCounter = 11,
};
constexpr std::uint32_t kDimensionBuffer = 1;

} // namespace

std::vector<ResourceBinding> reflect(std::span<const std::uint8_t> blob) {
    const Reader file(blob);
    if (blob.size() < 32 || file.u32(0) != fourcc("DXBC")) throw Error("not a DXBC shader");
    const std::uint32_t chunks = file.u32(28);
    for (std::uint32_t c = 0; c < chunks; ++c) {
        const std::uint32_t at = file.u32(32 + std::size_t{c} * 4);
        if (file.u32(at) != fourcc("RDEF")) continue;
        const Reader rdef(file.sub(at + 8, file.u32(at + 4)));
        const std::uint32_t count = rdef.u32(8), offset = rdef.u32(12);
        const std::uint32_t major = (rdef.u32(16) >> 8) & 0xFF;
        // Shader model 5 headers ("RD11") give the binding record size: 32 bytes, 40 in 5.1.
        std::uint32_t recordSize = 32;
        if (major >= 5) {
            const std::uint32_t magic = rdef.u32(28);
            if (magic == fourcc("RD11") || magic == fourcc("11DR")) recordSize = rdef.u32(40);
        }
        std::vector<ResourceBinding> out;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t r = offset + std::size_t{i} * recordSize;
            ResourceBinding b;
            b.name = rdef.str(rdef.u32(r));
            const std::uint32_t type = rdef.u32(r + 4), dimension = rdef.u32(r + 12);
            const std::uint32_t samples = rdef.u32(r + 16);
            b.slot = rdef.u32(r + 20);
            b.count = rdef.u32(r + 24);
            switch (type) {
            case kCBuffer:
            case kTBuffer: b.kind = ResourceKind::ConstantBuffer; break;
            case kSampler: b.kind = ResourceKind::Sampler; break;
            case kTexture:
            case kUavTyped:
                b.kind = dimension == kDimensionBuffer ? ResourceKind::TypedBuffer : ResourceKind::Texture;
                b.uav = type == kUavTyped;
                break;
            case kStructured:
            case kUavStructured:
            case kUavAppend:
            case kUavConsume:
            case kUavStructuredCounter:
                b.kind = ResourceKind::StructuredBuffer;
                b.uav = type != kStructured;
                b.stride = samples; // RDEF stores the structure stride in the sample-count field
                break;
            case kByteAddress:
            case kUavByteAddress:
                b.kind = ResourceKind::RawBuffer;
                b.uav = type == kUavByteAddress;
                break;
            default: b.kind = ResourceKind::Other; break;
            }
            out.push_back(std::move(b));
        }
        return out;
    }
    throw Error("DXBC shader has no reflection data (RDEF chunk)");
}

} // namespace gt::rhi::dxbc
