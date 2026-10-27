#pragma once
// What feature.video's Vulkan Video (video_vk.h) and Direct3D 12 Video (video_d3d12.h) paths share: the
// picture size, the synthetic source frames, per-codec results and the checks against the reference streams.

#include "gt/bench/video_streams.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gt::bench::video {

constexpr std::uint32_t kWidth = 1920, kHeight = 1080;           ///< shown picture
constexpr std::uint32_t kImageWidth = 1920, kImageHeight = 1088; ///< picture images (16, 32 and 64 aligned)
constexpr std::uint32_t kGop = 8;                                ///< frames per group of pictures
constexpr std::size_t kFrameBytes = std::size_t(kWidth) * kHeight * 3 / 2; ///< NV12 at the shown size

/// The source frames the encoders take: NV12 at the image size (rows below 1080 repeat the last one), the
/// same pattern scripts/make-video-streams.py encodes into the reference streams.
std::vector<std::uint8_t> makeSource();
constexpr std::size_t kSourceLuma = std::size_t(kImageWidth) * kImageHeight;
constexpr std::size_t kSourceFrame = kSourceLuma * 3 / 2;
/// Luma PSNR of a decoded frame (NV12, 1920x1080) against source frame f.
double lumaPsnr(const std::uint8_t* decoded, const std::vector<std::uint8_t>& source, std::uint32_t f);

struct CodecResult {
    std::string key, label;
    std::optional<double> decodeFps, encodeFps;
    std::optional<double> psnr, psnrMin; ///< GPU encoder -> GPU decoder round trip, luma
    bool decodeExact = false;            ///< every reference frame matched FFmpeg's decode
    std::string decodeNote, encodeNote;  ///< why something was not measured or checked
    std::string failure;                 ///< wrong results
    nlohmann::json details = nlohmann::json::object();
};
using Progress = std::function<void(double fraction, const std::string& status)>;

/// Compares decoded frames (NV12, 1920x1080 each) with the reference stream's CRCs: records them in the
/// details, sets decodeExact, or a failure naming the frames that differ.
void checkReference(const ReferenceStream& ref, const std::vector<std::uint8_t>& frames, std::uint32_t count,
                    CodecResult& res);
/// Records the luma PSNR of each decoded frame of an encoder's output against the source.
void checkRoundTrip(const std::vector<std::uint8_t>& frames, std::uint32_t count,
                    const std::vector<std::uint8_t>& source, CodecResult& res);

} // namespace gt::bench::video
