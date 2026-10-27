#include "dxbc_reflect.h"

#include "gt/rhi/types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace gt::rhi;

namespace {

void put(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void putTag(std::vector<std::uint8_t>& b, const char* tag) {
    b.insert(b.end(), tag, tag + 4);
}

struct Record {
    const char* name;
    std::uint32_t type, dimension, samples, slot, count;
};

/// A DXBC container with one RDEF chunk laid out the way fxc writes shader model 5.0 reflection.
std::vector<std::uint8_t> makeBlob(const std::vector<Record>& records, bool sm5 = true) {
    std::vector<std::uint8_t> rdef;
    const std::uint32_t header = sm5 ? 60 : 28;
    put(rdef, 0);                                                    // constant buffers
    put(rdef, 0);                                                    // constant buffer offset
    put(rdef, static_cast<std::uint32_t>(records.size()));           // bindings
    put(rdef, header);                                               // binding offset
    put(rdef, (sm5 ? 5u : 4u) << 8 | 0xFFFE0000u);                   // version 5.0, compute
    put(rdef, 0);                                                    // flags
    put(rdef, 0);                                                    // creator
    if (sm5) {
        putTag(rdef, "RD11");
        for (std::uint32_t v : {60u, 24u, 32u, 40u, 36u, 12u, 0u}) put(rdef, v);
    }
    std::uint32_t names = header + static_cast<std::uint32_t>(records.size()) * 32;
    std::string strings;
    for (const Record& r : records) {
        put(rdef, names + static_cast<std::uint32_t>(strings.size()));
        strings += r.name;
        strings.push_back('\0');
        for (std::uint32_t v : {r.type, 0u, r.dimension, r.samples, r.slot, r.count, 0u}) put(rdef, v);
    }
    rdef.insert(rdef.end(), strings.begin(), strings.end());

    std::vector<std::uint8_t> blob;
    putTag(blob, "DXBC");
    blob.resize(20, 0); // checksum
    put(blob, 1);
    put(blob, 0); // total size (patched below)
    put(blob, 2); // chunk count
    put(blob, 40);
    put(blob, 48);
    putTag(blob, "SFI0"); // an unrelated chunk first
    put(blob, 0);
    putTag(blob, "RDEF");
    put(blob, static_cast<std::uint32_t>(rdef.size()));
    blob.insert(blob.end(), rdef.begin(), rdef.end());
    const auto total = static_cast<std::uint32_t>(blob.size());
    std::memcpy(blob.data() + 24, &total, 4);
    return blob;
}

} // namespace

TEST_CASE("DXBC reflection reads register classes and structure strides", "[dxbc]") {
    const auto blob = makeBlob({
        {"params", 0, 0, 0, 0, 1},      // cbuffer b0
        {"pc", 0, 0, 0, 13, 1},         // push constants b13
        {"albedo", 2, 4, 0xFFFFFFFF, 1, 1}, // Texture2D t1
        {"lights", 5, 1, 32, 2, 1},     // StructuredBuffer<Light> t2, stride 32
        {"data", 7, 1, 0, 3, 1},        // ByteAddressBuffer t3
        {"out", 6, 1, 4, 0, 1},         // RWStructuredBuffer<uint> u0
        {"img", 4, 4, 0, 1, 1},         // RWTexture2D u1
        {"raw", 8, 1, 0, 2, 1},         // RWByteAddressBuffer u2
        {"linear", 3, 0, 0, 0, 1},      // sampler s0
        {"typed", 2, 1, 0, 4, 1},       // Buffer<float4> t4
    });
    const auto b = dxbc::reflect(blob);
    REQUIRE(b.size() == 10);
    CHECK(b[0].kind == dxbc::ResourceKind::ConstantBuffer);
    CHECK(b[1].slot == 13);
    CHECK(b[2].kind == dxbc::ResourceKind::Texture);
    CHECK_FALSE(b[2].uav);
    CHECK(b[3].kind == dxbc::ResourceKind::StructuredBuffer);
    CHECK(b[3].stride == 32);
    CHECK(b[3].name == "lights");
    CHECK(b[4].kind == dxbc::ResourceKind::RawBuffer);
    CHECK(b[5].kind == dxbc::ResourceKind::StructuredBuffer);
    CHECK(b[5].uav);
    CHECK(b[5].stride == 4);
    CHECK(b[6].kind == dxbc::ResourceKind::Texture);
    CHECK(b[6].uav);
    CHECK(b[7].kind == dxbc::ResourceKind::RawBuffer);
    CHECK(b[7].uav);
    CHECK(b[7].slot == 2);
    CHECK(b[8].kind == dxbc::ResourceKind::Sampler);
    CHECK(b[9].kind == dxbc::ResourceKind::TypedBuffer);
}

TEST_CASE("DXBC reflection handles shader model 4 headers and rejects other blobs", "[dxbc]") {
    const auto b = dxbc::reflect(makeBlob({{"tex", 2, 4, 0xFFFFFFFF, 7, 2}}, false));
    REQUIRE(b.size() == 1);
    CHECK(b[0].slot == 7);
    CHECK(b[0].count == 2);
    const std::vector<std::uint8_t> spirv = {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0};
    CHECK_THROWS_AS(dxbc::reflect(spirv), Error);
    auto truncated = makeBlob({{"x", 0, 0, 0, 0, 1}});
    truncated.resize(60);
    CHECK_THROWS_AS(dxbc::reflect(truncated), Error);
}
