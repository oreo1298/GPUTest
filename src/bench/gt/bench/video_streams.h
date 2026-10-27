#pragma once
// The reference video streams feature.video decodes (data/video, made by scripts/make-video-streams.py with
// FFmpeg's software encoders) and the CRC-32 of every frame FFmpeg decoded from them. Decoding is bit-exact
// in all three codecs, so a GPU decoder must reproduce each CRC.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gt::bench::video {

struct ReferenceStream {
    std::string key, codec, profile; ///< "h264" / "H.264" / "High", ...
    std::span<const std::uint8_t> data;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint32_t> crc32; ///< per frame: NV12 at width x height, luma plane then interleaved CbCr
    std::vector<double> psnr;         ///< per frame: luma PSNR of FFmpeg's decode against the source pattern
};

/// The embedded stream "h264", "h265" or "av1"; nullptr if it is missing.
const ReferenceStream* referenceStream(std::string_view key);

/// CRC-32 (IEEE 802.3, as zlib's crc32): pass the previous result to continue over more data.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

} // namespace gt::bench::video
