#include "gt/bench/video_streams.h"

#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <mutex>
#include <string>

namespace gt::bench::video_data {
struct EmbeddedFile {
    const char* key;
    const unsigned char* data;
    unsigned long long size;
};
extern const EmbeddedFile kStreams[];
extern const unsigned long long kStreamsCount;
} // namespace gt::bench::video_data

namespace gt::bench::video {
namespace {

std::span<const std::uint8_t> embedded(std::string_view key) {
    for (unsigned long long i = 0; i < video_data::kStreamsCount; ++i)
        if (video_data::kStreams[i].key && key == video_data::kStreams[i].key)
            return {video_data::kStreams[i].data, static_cast<std::size_t>(video_data::kStreams[i].size)};
    return {};
}

std::vector<ReferenceStream> load() {
    std::vector<ReferenceStream> out;
    const auto meta = embedded("streams.json");
    if (meta.empty()) return out;
    const auto j = nlohmann::json::parse(meta.begin(), meta.end(), nullptr, false);
    if (j.is_discarded()) return out;
    for (const auto& s : j.value("streams", nlohmann::json::array())) {
        ReferenceStream r;
        r.key = s.value("key", "");
        r.codec = s.value("codec", "");
        r.profile = s.value("profile", "");
        r.data = embedded(r.key);
        r.width = j.value("width", 0u);
        r.height = j.value("height", 0u);
        for (const auto& c : s.value("crc32", nlohmann::json::array()))
            r.crc32.push_back(static_cast<std::uint32_t>(std::stoul(c.get<std::string>(), nullptr, 16)));
        for (const auto& p : s.value("psnr", nlohmann::json::array()))
            r.psnr.push_back(p.get<double>());
        if (!r.data.empty()) out.push_back(std::move(r));
    }
    return out;
}

} // namespace

const ReferenceStream* referenceStream(std::string_view key) {
    static const std::vector<ReferenceStream> streams = load();
    for (const auto& s : streams)
        if (s.key == key) return &s;
    return nullptr;
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::uint8_t b : data)
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

} // namespace gt::bench::video
