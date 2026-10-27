#pragma once
// Native Vulkan Video plumbing for feature.video: memory, pictures, queues, and a decoder and an encoder that
// are independent of the codec. Codecs (video_h264_vk.cpp, ...) supply profiles, parameter sets and per-frame
// picture information through DecodeCodec / EncodeCodec.
//
// Picture layout: every frame of the 8-frame GOP has its own image layer. Decoding uses DPB slot f for frame
// f (any frame can reference any earlier one, as AV1 does); encoding makes P-frames that reference the frame
// before, in two rotating slots. All native submissions are ordered by one timeline semaphore; images and
// buffers that cross queues use concurrent sharing.

#if GT_HAS_VULKAN

#include "gt/bench/context.h"
#include "gt/rhi/rhi.h"

#include <volk.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gt::bench::vkvideo {

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

std::string resultName(VkResult r);
void check(VkResult r, const char* what);
VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a);
bool hasExtension(const rhi::NativeDevice& n, std::string_view name);
/// Codec operations a queue family offers (0 for ~0u).
VkVideoCodecOperationFlagsKHR familyCodecs(VkPhysicalDevice pd, std::uint32_t family);
bool familyHasStatusQueries(VkPhysicalDevice pd, std::uint32_t family);
std::string codecList(VkVideoCodecOperationFlagsKHR ops);
/// Why native Vulkan video work cannot run at all on this device (empty = it can be tried).
std::string blocker(const rhi::NativeDevice& n);

/// Picks a format `profile` supports for `usage`: NV12 first, then formats with `wantUsage`, optimal tiling.
std::optional<VkVideoFormatPropertiesKHR> pickFormat(VkPhysicalDevice pd,
                                                     const VkVideoProfileInfoKHR& profile,
                                                     VkImageUsageFlags usage, VkImageUsageFlags wantUsage);

VkVideoPictureResourceInfoKHR picture(VkImageView view, std::uint32_t layer, VkExtent2D coded);

class VideoRig {
public:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t layers = 0;
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    explicit VideoRig(const rhi::NativeDevice& n);
    ~VideoRig();
    VideoRig(const VideoRig&) = delete;
    VideoRig& operator=(const VideoRig&) = delete;

    const rhi::NativeDevice& native() const { return m_n; }
    VkDevice device() const { return m_dev; }
    VkPhysicalDevice physicalDevice() const { return m_pd; }
    void onCleanup(std::function<void()> f) { m_cleanup.push_back(std::move(f)); }

    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags need,
                            VkMemoryPropertyFlags want, const void* pNext, const char* what);
    /// A picture image (kImageWidth x kImageHeight); more than one family makes it shared (concurrent).
    Image image(const VkVideoFormatPropertiesKHR& fmt, VkImageUsageFlags usage, VkImageUsageFlags viewUsage,
                std::uint32_t layers, const VkVideoProfileInfoKHR& profile,
                std::vector<std::uint32_t> families);
    /// A buffer; `profile` is required for bitstream usages. Host-visible buffers stay mapped.
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                  const VkVideoProfileInfoKHR* profile, std::vector<std::uint32_t> families);
    void flush(const Buffer& b);
    void invalidate(const Buffer& b);
    void bindSessionMemory(VkVideoSessionKHR session);

    /// Starts recording a reusable command buffer for `family`.
    VkCommandBuffer begin(std::uint32_t family);
    void end(VkCommandBuffer cb);
    /// Submits after everything submitted before (on any queue) has finished.
    void submit(VkQueue queue, VkCommandBuffer cb);
    /// Waits for the last submission; throws if the GPU takes longer than 20 s.
    void wait();
    /// Submits `cb` (which handles `framesPerSubmit` frames) until `seconds` have passed; returns frames/s.
    double timeSubmissions(TestContext& ctx, VkQueue queue, VkCommandBuffer cb, std::uint32_t framesPerSubmit,
                           double seconds);

private:
    bool waitTimeline(std::uint64_t timeoutNs);

    rhi::NativeDevice m_n;
    VkDevice m_dev;
    VkPhysicalDevice m_pd;
    VkPhysicalDeviceMemoryProperties m_memory{};
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    std::uint64_t m_value = 0;
    std::map<std::uint32_t, VkCommandPool> m_pools;
    std::vector<std::function<void()>> m_cleanup;
};

/// Records a sync2 barrier: global memory dependencies and/or image layout transitions.
struct Barrier {
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkMemoryBarrier2> memory;
    Barrier& image(VkImage img, std::uint32_t layers, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                   VkAccessFlags2 dstAccess);
    Barrier& global(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                    VkAccessFlags2 dstAccess);
    void record(VkCommandBuffer cb) const;
};

// --- decoding ----------------------------------------------------------------------------------------

/// The codec half of a decode. Frame f is decoded into DPB slot f and image layer f.
class DecodeCodec {
public:
    virtual ~DecodeCodec() = default;
    virtual std::string name() const = 0; ///< "H.264 High"
    /// The decode profile, with the codec's profile structure chained.
    virtual const VkVideoProfileInfoKHR& profile() const = 0;
    /// The codec's capability structure (sType set), chained behind VkVideoDecodeCapabilitiesKHR for the
    /// query.
    virtual void* capabilities() = 0;
    /// After the query: why the codec's capabilities do not fit the stream (empty = they do).
    virtual std::string checkCapabilities() const = 0;
    virtual VkExtent2D codedExtent() const = 0;
    virtual std::uint32_t frameCount() const = 0;
    virtual std::span<const std::uint8_t> frameData(std::uint32_t f) const = 0;
    virtual std::uint32_t maxActiveReferences() const = 0;
    virtual VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) = 0;
    /// The frames frame f references (each earlier than f).
    virtual std::vector<std::uint32_t> references(std::uint32_t f) const = 0;
    struct FrameInfo {
        const void* picture = nullptr; ///< chained into VkVideoDecodeInfoKHR
        const void* setup = nullptr;   ///< DPB slot info of frame f
        std::vector<const void*> refs; ///< DPB slot info of each reference, in references(f) order
    };
    /// Codec structures for decoding frame f; valid until the next call.
    virtual FrameInfo frameInfo(std::uint32_t f) = 0;
};

class Decoder {
public:
    Decoder(VideoRig& rig, DecodeCodec& codec) : m_rig(rig), m_codec(codec) {}
    /// Capabilities, formats, session and resources. Returns why the GPU cannot decode the stream (empty =
    /// ok).
    std::string prepare();
    /// Decodes every frame once (with result status queries where supported). Returns a failure, or empty.
    std::string decodeOnce();
    /// Decode throughput of back-to-back GOPs, in frames per second.
    double time(TestContext& ctx, double seconds);
    /// Whether decoded pictures can be copied out (NV12 + transfer source).
    bool readable() const { return m_readable; }
    /// Copies the decoded frames out on the graphics queue: NV12 at 1920x1080, frame after frame.
    std::vector<std::uint8_t> readback();
    std::string outputMode() const { return m_coincide ? "DPB and output coincide" : "distinct output"; }

private:
    void record(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query);

    VideoRig& m_rig;
    DecodeCodec& m_codec;
    VkVideoCapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR m_decodeCaps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    bool m_coincide = false, m_readable = false;
    VkVideoSessionKHR m_session = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR m_params = VK_NULL_HANDLE;
    VideoRig::Image m_dpb, m_out;
    VideoRig::Buffer m_stream;
    std::vector<VkDeviceSize> m_offsets, m_ranges; ///< each frame's bitstream range
    VkQueryPool m_status = VK_NULL_HANDLE;
};

// --- encoding ----------------------------------------------------------------------------------------

/// The codec half of an encode: P-frames reference the frame before (DPB slots and layers rotate in two).
class EncodeCodec {
public:
    virtual ~EncodeCodec() = default;
    virtual std::string name() const = 0;
    virtual const VkVideoProfileInfoKHR& profile() const = 0;
    virtual void* capabilities() = 0; ///< chained behind VkVideoEncodeCapabilitiesKHR
    virtual std::string checkCapabilities(const VkVideoCapabilitiesKHR& caps,
                                          const VkVideoEncodeCapabilitiesKHR& encode) const = 0;
    virtual VkExtent2D codedExtent() const = 0;
    virtual VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) = 0;
    /// Chained into VkVideoEncodeSessionParametersGetInfoKHR to read back the encoder's parameter sets.
    virtual const void* parametersGetInfo() = 0;
    /// The quantizer used when rate control is disabled (in the codec's range), for the details.
    virtual std::string quantizer() const = 0;
    struct FrameInfo {
        const void* picture = nullptr; ///< chained into VkVideoEncodeInfoKHR
        const void* setup = nullptr;   ///< DPB slot info of frame f
        const void* ref = nullptr;     ///< DPB slot info of frame f - 1 (P-frames)
    };
    /// Codec structures for encoding frame f (0 = IDR / key frame); valid until the next call.
    virtual FrameInfo frameInfo(std::uint32_t f, bool constantQuantizer) = 0;
};

class Encoder {
public:
    Encoder(VideoRig& rig, EncodeCodec& codec) : m_rig(rig), m_codec(codec) {}
    std::string prepare();
    /// Uploads the source frames (NV12 at the image size).
    void upload(const std::vector<std::uint8_t>& nv12);
    /// Encodes the GOP once; fills each frame's output. Returns a failure, or empty.
    std::string encodeOnce();
    double time(TestContext& ctx, double seconds);
    /// The encoder's parameter sets (Annex B / OBU bytes) and whether it overrode what it was given.
    std::vector<std::uint8_t> parameters(bool* overrides);
    const std::vector<std::vector<std::uint8_t>>& output() const { return m_output; }
    bool constantQuantizer() const { return m_rateControlDisabled; }

private:
    void record(VkCommandBuffer cb, std::uint32_t f, bool reset, bool query, VkBuffer dst);

    VideoRig& m_rig;
    EncodeCodec& m_codec;
    VkVideoCapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    VkVideoEncodeCapabilitiesKHR m_encodeCaps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR};
    VkFormat m_srcFormat = VK_FORMAT_UNDEFINED;
    VkVideoSessionKHR m_session = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR m_params = VK_NULL_HANDLE;
    VideoRig::Image m_src, m_dpb;
    VideoRig::Buffer m_stream, m_timedStream;
    VkQueryPool m_feedback = VK_NULL_HANDLE;
    VkVideoEncodeRateControlInfoKHR m_rateControl{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
    bool m_rateControlDisabled = false;
    bool m_uploaded = false;
    std::vector<std::vector<std::uint8_t>> m_output;
};

// --- one codec, end to end --------------------------------------------------------------------------

/// What a codec contributes: its operations and extensions, and how to read streams.
struct CodecHooks {
    std::string key, label; ///< "h264", "H.264"
    VkVideoCodecOperationFlagBitsKHR decodeOp{}, encodeOp{};
    const char* decodeExtension = nullptr;
    const char* encodeExtension = nullptr; ///< null: no encoder for this codec yet
    /// The reference stream as a decode job (nullptr + error if it cannot be read).
    std::function<std::unique_ptr<DecodeCodec>(std::span<const std::uint8_t> stream, std::string* error)>
        reference;
    /// Encoders to try, best profile first.
    std::function<std::vector<std::unique_ptr<EncodeCodec>>()> encoders;
    /// An encoder's output (its parameter sets and each frame's bytes) as a decode job.
    std::function<std::unique_ptr<DecodeCodec>(const std::vector<std::uint8_t>& parameters,
                                               const std::vector<std::vector<std::uint8_t>>& frames,
                                               std::string* error)>
        encoded;
};
CodecHooks h264Hooks();
CodecHooks h265Hooks();

bool canDecode(const rhi::NativeDevice& n, const CodecHooks& c);
bool canEncode(const rhi::NativeDevice& n, const CodecHooks& c);

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

/// Decodes the codec's reference stream (throughput + bit-exact check) and encodes the source frames
/// (throughput + round-trip PSNR through the GPU's decoder), as far as the GPU supports them.
CodecResult runCodec(TestContext& ctx, const rhi::NativeDevice& n, const CodecHooks& hooks,
                     const std::vector<std::uint8_t>& source, double seconds, const Progress& progress);

} // namespace gt::bench::vkvideo

#endif // GT_HAS_VULKAN
