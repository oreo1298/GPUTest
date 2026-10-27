#pragma once
// Native Direct3D 12 Video plumbing for feature.video: a decoder that is independent of the codec and runs on
// the GPU's video decode queue, fed with DXVA buffers that video_dxva.cpp builds from the parsed streams.
//
// Picture layout: one NV12 picture per frame of the 8-frame GOP (separate textures on decode tier 2 and up,
// the slices of one texture array on tier 1); frame f decodes into picture f, which is also its DXVA picture
// index, so any frame can reference any earlier one. Decoders that require reference-only allocations get a
// second set of pictures, written through the output conversion.

#if GT_HAS_D3D12

#include "gt/bench/context.h"

#include "d3d12/d3d12_common.h"
#include "video_common.h"

#include <directx/d3d12video.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt::bench::d3d12video {

// --- plumbing shared by the decoder and the encoder (video_d3d12.cpp) ---------------------------------

constexpr DWORD kTimeoutMs = 20000; ///< longest wait for the GPU

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to);
D3D12_RESOURCE_DESC bufferDesc(UINT64 size);
HRESULT committed(ID3D12Device* dev, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc,
                  D3D12_RESOURCE_STATES state, rhi::d3d12::Com<ID3D12Resource>& out);

/// One fence for all queues, and an event to wait on it.
class Sync {
public:
    explicit Sync(ID3D12Device* dev) {
        rhi::d3d12::check(
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), m_fence.putVoid()),
            "CreateFence");
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) throw std::runtime_error("CreateEvent failed");
    }
    ~Sync() {
        if (!m_timedOut && m_value && m_fence->GetCompletedValue() < m_value) {
            m_fence->SetEventOnCompletion(m_value, m_event);
            WaitForSingleObject(m_event, kTimeoutMs);
        }
        CloseHandle(m_event);
    }
    Sync(const Sync&) = delete;
    Sync& operator=(const Sync&) = delete;

    /// Signals the fence on `q` after its work so far; returns the value.
    UINT64 signal(ID3D12CommandQueue* q) {
        rhi::d3d12::check(q->Signal(m_fence.get(), ++m_value), "ID3D12CommandQueue::Signal");
        return m_value;
    }
    /// Makes `q` wait for everything signalled so far.
    void gpuWait(ID3D12CommandQueue* q) {
        if (m_value) rhi::d3d12::check(q->Wait(m_fence.get(), m_value), "ID3D12CommandQueue::Wait");
    }
    /// Waits for the last signal without throwing (before releasing what the GPU may still use); returns at
    /// once after a timeout.
    void drain(ID3D12Device* dev) noexcept {
        if (m_timedOut) return;
        try {
            wait(dev);
        } catch (...) {
        }
    }
    /// Waits for the last signal; throws if the GPU takes longer than 20 s.
    void wait(ID3D12Device* dev) {
        if (m_fence->GetCompletedValue() >= m_value) return;
        rhi::d3d12::check(m_fence->SetEventOnCompletion(m_value, m_event), "SetEventOnCompletion");
        if (WaitForSingleObject(m_event, kTimeoutMs) != WAIT_OBJECT_0) {
            m_timedOut = true;
            const HRESULT removed = dev->GetDeviceRemovedReason();
            throw std::runtime_error(FAILED(removed)
                                         ? "the device was removed during video work (" +
                                               rhi::d3d12::hresultName(removed) + ")"
                                         : std::string("the GPU did not finish the video work within 20 s"));
        }
    }

    /// Whether a wait ran out of time: the GPU may still be using everything submitted.
    bool timedOut() const { return m_timedOut; }

private:
    rhi::d3d12::Com<ID3D12Fence> m_fence;
    HANDLE m_event = nullptr;
    UINT64 m_value = 0;
    bool m_timedOut = false;
};

/// unique_ptr deleter for the decoder and encoder: waits for their GPU work (settle()) and, if the GPU never
/// finishes it, keeps the object and everything the GPU may still use alive instead of releasing it, which
/// hangs some drivers (WARP). A deliberate leak, for a device whose video engine is stuck anyway.
struct KeepIfHung {
    template <typename T>
    void operator()(T* p) const noexcept {
        if (p->settle()) delete p;
    }
};

struct Queue {
    rhi::d3d12::Com<ID3D12CommandQueue> queue;
    rhi::d3d12::Com<ID3D12CommandAllocator> allocator;
};

HRESULT makeQueue(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type, Queue& q);
void execute(ID3D12CommandQueue* q, ID3D12CommandList* list);

// --- decoding ----------------------------------------------------------------------------------------

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
    std::vector<std::uint8_t> headers; ///< H.264 / H.265: the parameter-set NAL units, which DXVA never sends
};

/// The parsed reference streams as DXVA buffers (video_dxva.cpp). Empty + error if a stream cannot be used.
std::optional<DxvaStream> dxvaH264(std::span<const std::uint8_t> annexB, std::string* error);
std::optional<DxvaStream> dxvaH265(std::span<const std::uint8_t> annexB, std::string* error);
std::optional<DxvaStream> dxvaAv1(std::span<const std::uint8_t> obus, std::string* error);
/// The feature.video.inline_headers diagnostic: puts the stream's parameter sets in frame 0's bitstream ahead
/// of its first slice, outside the slice entries (mode 1) or inside the first one (mode 2). DXVA decoders
/// take the parameters from the picture parameters; this shows whether one wants the NAL units as well.
void inlineHeaders(DxvaStream& s, int mode);

/// What the decoder reported for each frame (D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS): "ok", or the status
/// and the macroblocks affected. Empty when the driver has no decode statistics.
using DecodeStatus = nlohmann::json;

/// Decodes `s` once and reads the frames back (NV12, 1920x1080 each); returns why it could not (empty: done).
std::string decodeFrames(ID3D12Device* dev, const DxvaStream& s, std::vector<std::uint8_t>& frames,
                         DecodeStatus* status = nullptr);

// --- encoding ----------------------------------------------------------------------------------------

/// The codec half of an encode (video_d3d12_encode.cpp): what to ask the encoder for, the headers the
/// application writes, and how to read the result back. Frame f of the GOP is a key frame (IDR, f = 0) or is
/// predicted from frame f - 1.
class EncodeCodec {
public:
    virtual ~EncodeCodec() = default;
    virtual std::string name() const = 0; ///< "H.264 High"
    virtual D3D12_VIDEO_ENCODER_CODEC codec() const = 0;
    /// Settles the profile, level, coding tools and GOP with the encoder's support queries; returns why it
    /// cannot encode (empty: it can).
    virtual std::string configure(ID3D12VideoDevice3* video) = 0;
    virtual D3D12_VIDEO_ENCODER_PROFILE_DESC profile() = 0;
    virtual D3D12_VIDEO_ENCODER_LEVEL_SETTING level() = 0;
    virtual D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration() = 0;
    virtual D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop() = 0;
    virtual std::uint32_t quantizer() const = 0;
    /// How a frame is split: one slice (H.264, H.265) or a grid of tiles (AV1).
    virtual D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE layoutMode() const {
        return D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    }
    virtual D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA layoutData() { return {}; }
    /// Bytes of resolved metadata per frame (after configure()).
    virtual UINT64 resolvedSize() const { return 4096; }
    /// Picture control for frame f in GOP repeat r (valid for the encoder's lifetime).
    virtual D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picture(std::uint32_t f, std::uint32_t r) = 0;
    /// What precedes the frames: the parameter sets as Annex B NAL units (AV1: nothing, its sequence header
    /// travels with the key frame).
    virtual std::vector<std::uint8_t> headers() const = 0;
    /// Frame f's part of the stream from the encoder's output: `bitstream` from the frame's start, `resolved`
    /// its resolved metadata. Returns why it cannot (empty: done). By default the slices as Annex B NAL
    /// units.
    virtual std::string frameData(std::uint32_t f, std::span<const std::uint8_t> bitstream,
                                  std::span<const std::uint8_t> resolved,
                                  std::vector<std::uint8_t>& out) const;
    /// Parameter sets + encoded frames as a decode job.
    virtual std::optional<DxvaStream> readBack(std::span<const std::uint8_t> stream,
                                               std::string* error) const = 0;
    /// What the configuration chose, for the details.
    virtual nlohmann::json describe() const = 0;
};
std::unique_ptr<EncodeCodec> h264Encoder();
std::unique_ptr<EncodeCodec> h265Encoder();
std::unique_ptr<EncodeCodec> av1Encoder();

struct CodecHooks {
    std::string key, label; ///< "h264", "H.264"
    GUID decodeProfile{};
    std::function<std::optional<DxvaStream>(std::span<const std::uint8_t>, std::string*)> reference;
    std::function<std::unique_ptr<EncodeCodec>()> encoder; ///< empty: not encoded on Direct3D 12
};
std::vector<CodecHooks> codecs();

/// Why Direct3D 12 video work cannot run at all on this device (empty = it can be tried).
std::string blocker(ID3D12Device* dev);
/// Whether the video decoder takes the codec's profile at the reference streams' size.
bool canDecode(ID3D12Device* dev, const CodecHooks& c);
/// Names of the decode profiles the driver lists.
std::vector<std::string> decodeProfiles(ID3D12Device* dev);

/// Encodes the source frames (throughput) and decodes the output again (round-trip PSNR) into `res`.
void encodeAndCheck(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks,
                    const std::vector<std::uint8_t>& source, double seconds, const video::Progress& progress,
                    video::CodecResult& res);

/// Decodes the codec's reference stream (throughput + bit-exact check) and encodes the source frames
/// (throughput + round trip), as far as the GPU supports them.
video::CodecResult runCodec(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks,
                            const std::vector<std::uint8_t>& source, double seconds,
                            const video::Progress& progress);

} // namespace gt::bench::d3d12video

#endif // GT_HAS_D3D12
