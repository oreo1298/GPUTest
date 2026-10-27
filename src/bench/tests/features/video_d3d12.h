#pragma once
// Native Direct3D 12 Video plumbing for feature.video: a decoder that is independent of the codec and runs on
// the GPU's video decode queue, fed with DXVA buffers that video_dxva.cpp builds from the parsed streams.
//
// Picture layout: one NV12 texture array with a slice per frame of the 8-frame GOP; frame f decodes into
// slice f, which is also its DXVA picture index, so any frame can reference any earlier one. Decoders that
// require reference-only allocations get a second array, written through the output conversion.

#if GT_HAS_D3D12

#include "gt/bench/context.h"

#include "d3d12/d3d12_common.h"
#include "video_common.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gt::bench::d3d12video {

/// One coded frame as DXVA buffers.
struct DxvaFrame {
    std::vector<std::uint8_t> data;      ///< the bitstream the slice / tile entries point into
    std::vector<std::uint8_t> picParams; ///< DXVA_PicParams_*
    std::vector<std::uint8_t> qmatrix;   ///< DXVA_Qmatrix_* (empty: not sent)
    std::vector<std::uint8_t> slices;    ///< DXVA_Slice_*_Short / DXVA_Tile_AV1 entries
};

/// A stream to decode. Frame f is DXVA picture index f.
struct DxvaStream {
    std::string name;                    ///< "H.264 High"
    GUID profile{};                      ///< D3D12_VIDEO_DECODE_PROFILE_*
    std::uint32_t width = 0, height = 0; ///< coded size
    std::vector<DxvaFrame> frames;
};

/// The parsed reference streams as DXVA buffers (video_dxva.cpp). Empty + error if a stream cannot be used.
std::optional<DxvaStream> dxvaH264(std::span<const std::uint8_t> annexB, std::string* error);
std::optional<DxvaStream> dxvaH265(std::span<const std::uint8_t> annexB, std::string* error);
std::optional<DxvaStream> dxvaAv1(std::span<const std::uint8_t> obus, std::string* error);

struct CodecHooks {
    std::string key, label; ///< "h264", "H.264"
    GUID decodeProfile{};
    std::function<std::optional<DxvaStream>(std::span<const std::uint8_t>, std::string*)> reference;
};
std::vector<CodecHooks> codecs();

/// Why Direct3D 12 video work cannot run at all on this device (empty = it can be tried).
std::string blocker(ID3D12Device* dev);
/// Whether the video decoder takes the codec's profile at the reference streams' size.
bool canDecode(ID3D12Device* dev, const CodecHooks& c);
/// Names of the decode profiles the driver lists.
std::vector<std::string> decodeProfiles(ID3D12Device* dev);

/// Decodes the codec's reference stream (throughput + bit-exact check), as far as the GPU supports it.
video::CodecResult runCodec(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks, double seconds,
                            const video::Progress& progress);

} // namespace gt::bench::d3d12video

#endif // GT_HAS_D3D12
