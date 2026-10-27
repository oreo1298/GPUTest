#include "video_d3d12.h"

#if GT_HAS_D3D12

#include <directx/d3d12video.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace gt::bench::d3d12video {

using rhi::d3d12::alignUp;
using rhi::d3d12::check;
using rhi::d3d12::Com;
using rhi::d3d12::hresultName;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

HRESULT committed(ID3D12Device* dev, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc,
                  D3D12_RESOURCE_STATES state, Com<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    return dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        __uuidof(ID3D12Resource), out.putVoid());
}

HRESULT makeQueue(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type, Queue& q) {
    D3D12_COMMAND_QUEUE_DESC d{};
    d.Type = type;
    HRESULT hr = dev->CreateCommandQueue(&d, __uuidof(ID3D12CommandQueue), q.queue.putVoid());
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), q.allocator.putVoid());
    return hr;
}

void execute(ID3D12CommandQueue* q, ID3D12CommandList* list) {
    ID3D12CommandList* lists[] = {list};
    q->ExecuteCommandLists(1, lists);
}

namespace {

constexpr std::uint32_t kRepeats = 8;   ///< GOPs per timed submission
constexpr UINT64 kBitstreamAlign = 256; ///< frame offsets in the bitstream buffer (DXVA wants 128)
constexpr UINT64 kStatsStride = 32;     ///< resolved D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS per frame
constexpr std::uint32_t kPlanes = 2;    ///< NV12: luma, interleaved chroma

D3D12_VIDEO_DECODE_CONFIGURATION configuration(const GUID& profile) {
    D3D12_VIDEO_DECODE_CONFIGURATION c{};
    c.DecodeProfile = profile;
    c.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    c.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    return c;
}

Com<ID3D12VideoDevice> videoDevice(ID3D12Device* dev) {
    Com<ID3D12VideoDevice> v;
    if (dev) dev->QueryInterface(__uuidof(ID3D12VideoDevice), v.putVoid());
    return v;
}

std::optional<D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT>
decodeSupport(ID3D12VideoDevice* video, const GUID& profile, std::uint32_t width, std::uint32_t height) {
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT ds{};
    ds.NodeIndex = 0;
    ds.Configuration = configuration(profile);
    ds.Width = width;
    ds.Height = height;
    ds.DecodeFormat = DXGI_FORMAT_NV12;
    ds.FrameRate = {60, 1};
    ds.BitRate = 0;
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof(ds))) ||
        !(ds.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) ||
        ds.DecodeTier == D3D12_VIDEO_DECODE_TIER_NOT_SUPPORTED)
        return std::nullopt;
    return ds;
}

std::string profileName(const GUID& g) {
    struct Known {
        const GUID* guid;
        const char* name;
    };
    static const Known kKnown[] = {
        {&D3D12_VIDEO_DECODE_PROFILE_MPEG2, "MPEG-2"},
        {&D3D12_VIDEO_DECODE_PROFILE_MPEG1_AND_MPEG2, "MPEG-1/2"},
        {&D3D12_VIDEO_DECODE_PROFILE_H264, "H.264"},
        {&D3D12_VIDEO_DECODE_PROFILE_H264_STEREO_PROGRESSIVE, "H.264 stereo progressive"},
        {&D3D12_VIDEO_DECODE_PROFILE_H264_STEREO, "H.264 stereo"},
        {&D3D12_VIDEO_DECODE_PROFILE_H264_MULTIVIEW, "H.264 multiview"},
        {&D3D12_VIDEO_DECODE_PROFILE_VC1, "VC-1"},
        {&D3D12_VIDEO_DECODE_PROFILE_VC1_D2010, "VC-1 (D2010)"},
        {&D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_SIMPLE, "MPEG-4 Part 2 Simple"},
        {&D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_ADVSIMPLE_NOGMC, "MPEG-4 Part 2 Advanced Simple"},
        {&D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, "H.265 Main"},
        {&D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10, "H.265 Main 10"},
        {&D3D12_VIDEO_DECODE_PROFILE_VP9, "VP9"},
        {&D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2, "VP9 10-bit"},
        {&D3D12_VIDEO_DECODE_PROFILE_VP8, "VP8"},
        {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0, "AV1 Main"},
        {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1, "AV1 High"},
        {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2, "AV1 Professional"},
        {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2, "AV1 Professional 12-bit"},
        {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2_420, "AV1 Professional 12-bit 4:2:0"},
    };
    for (const Known& k : kKnown)
        if (std::memcmp(k.guid, &g, sizeof(GUID)) == 0) return k.name;
    return std::format("{{{:08x}-{:04x}-{:04x}-...}}", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3);
}

// --- decoder -------------------------------------------------------------------------------------------

class Decoder {
public:
    Decoder(ID3D12Device* dev, const DxvaStream& s) : m_dev(dev), m_s(s), m_sync(dev) {}
    ~Decoder() { m_sync.drain(m_dev); }
    /// Waits for the GPU work; false if the GPU never finished it.
    bool settle() noexcept {
        m_sync.drain(m_dev);
        return !m_sync.timedOut();
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Creates the decoder and its resources; returns why the stream cannot be decoded (empty: it can).
    std::string prepare() {
        m_video = videoDevice(m_dev);
        if (!m_video) return "no Direct3D 12 video device";
        const auto ds = decodeSupport(m_video.get(), m_s.profile, m_s.width, m_s.height);
        if (!ds)
            return std::format("the video decoder does not take {} at {}x{}", m_s.name, m_s.width,
                               m_s.height);
        m_tier = ds->DecodeTier;
        // Tier 1 decoders take one texture array; tier 2 and up also an array of separate textures, which is
        // how they are usually driven (as Mesa's d3d12 video driver does).
        m_separate = m_tier >= D3D12_VIDEO_DECODE_TIER_2;
        m_referenceOnly = (ds->ConfigurationFlags &
                           D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_REFERENCE_ONLY_ALLOCATIONS_REQUIRED) != 0;
        std::uint32_t texHeight = std::max(m_s.height, video::kImageHeight);
        if (ds->ConfigurationFlags &
            D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED)
            texHeight = static_cast<std::uint32_t>(alignUp(texHeight, 32));

        D3D12_VIDEO_DECODER_DESC dd{};
        dd.Configuration = configuration(m_s.profile);
        if (HRESULT hr = m_video->CreateVideoDecoder(&dd, __uuidof(ID3D12VideoDecoder), m_decoder.putVoid());
            FAILED(hr))
            return "CreateVideoDecoder failed: " + hresultName(hr);
        D3D12_VIDEO_DECODER_HEAP_DESC hd{};
        hd.Configuration = dd.Configuration;
        hd.DecodeWidth = m_s.width;
        hd.DecodeHeight = m_s.height;
        hd.Format = DXGI_FORMAT_NV12;
        hd.FrameRate = {60, 1};
        hd.MaxDecodePictureBufferCount = video::kGop;
        if (HRESULT hr =
                m_video->CreateVideoDecoderHeap(&hd, __uuidof(ID3D12VideoDecoderHeap), m_heap.putVoid());
            FAILED(hr))
            return "CreateVideoDecoderHeap failed: " + hresultName(hr);

        // Pictures: one per frame (a texture or an array slice); a reference-only DPB is written through the
        // output conversion.
        m_texDesc = {};
        m_texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        m_texDesc.Width = std::max(m_s.width, video::kImageWidth);
        m_texDesc.Height = texHeight;
        m_texDesc.DepthOrArraySize = static_cast<UINT16>(m_separate ? 1 : video::kGop);
        m_texDesc.MipLevels = 1;
        m_texDesc.Format = DXGI_FORMAT_NV12;
        m_texDesc.SampleDesc.Count = 1;
        m_texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_RESOURCE_DESC dpbDesc = m_texDesc;
        if (m_referenceOnly)
            dpbDesc.Flags =
                D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        const std::uint32_t textures = m_separate ? video::kGop : 1;
        m_dpb.resize(textures);
        m_output.resize(m_referenceOnly ? textures : 0);
        for (std::uint32_t i = 0; i < textures; ++i) {
            if (HRESULT hr =
                    committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, dpbDesc, D3D12_RESOURCE_STATE_COMMON, m_dpb[i]);
                FAILED(hr))
                return "the picture textures could not be created: " + hresultName(hr);
            if (m_referenceOnly)
                if (HRESULT hr = committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, m_texDesc,
                                           D3D12_RESOURCE_STATE_COMMON, m_output[i]);
                    FAILED(hr))
                    return "the output textures could not be created: " + hresultName(hr);
        }

        if (HRESULT hr = makeQueue(m_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, m_decode); FAILED(hr))
            return "no video decode queue: " + hresultName(hr);
        // Per-frame decode status where the driver offers it (optional: nothing depends on it).
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS;
        qd.Count = video::kGop;
        if (SUCCEEDED(m_dev->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), m_statsHeap.putVoid())) &&
            FAILED(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, bufferDesc(kStatsStride * video::kGop),
                             D3D12_RESOURCE_STATE_COMMON, m_stats)))
            m_statsHeap.reset();
        check(makeQueue(m_dev, D3D12_COMMAND_LIST_TYPE_COPY, m_copy), "copy queue");
        uploadBitstream();

        // The DXVA reference list: every picture of the DPB, picture index = frame.
        for (std::uint32_t i = 0; i < video::kGop; ++i) {
            m_refTextures.push_back(picture(i));
            m_refSubresources.push_back(subresource(i, 0));
            m_refHeaps.push_back(m_heap.get());
        }
        // One command list per frame, as D3D12 video decoders are usually driven; the timed submissions pass
        // kRepeats GOPs of them to one ExecuteCommandLists call.
        for (std::uint32_t r = 0; r < kRepeats; ++r)
            for (std::uint32_t f = 0; f < m_s.frames.size(); ++f)
                m_lists.push_back(recordFrame(f));
        return {};
    }

    /// Decodes the stream once, a frame at a time; returns a failure message (empty on success).
    std::string decodeOnce() {
        for (std::uint32_t f = 0; f < m_s.frames.size(); ++f) {
            execute(m_decode.queue.get(), m_lists[f].get());
            m_sync.signal(m_decode.queue.get());
            try {
                m_sync.wait(m_dev);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::format("decoding frame {}: {}", f, e.what()));
            }
        }
        return {};
    }

    double time(TestContext& ctx, double seconds) {
        const auto frames = static_cast<std::uint64_t>(m_lists.size());
        std::vector<ID3D12CommandList*> lists;
        for (const auto& l : m_lists)
            lists.push_back(l.get());
        auto submit = [&] {
            m_decode.queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
            m_sync.signal(m_decode.queue.get());
            m_sync.wait(m_dev);
        };
        submit(); // warm-up
        std::uint64_t done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        double elapsed = 0.0;
        do {
            ctx.checkStop();
            submit();
            done += frames;
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        } while (elapsed < seconds);
        return double(done) / elapsed;
    }

    /// The decoded frames, NV12 at 1920x1080 each (call after decodeOnce or time).
    std::vector<std::uint8_t> readback() {
        const std::uint32_t count = static_cast<std::uint32_t>(m_s.frames.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(count * kPlanes);
        UINT64 total = 0;
        for (std::uint32_t f = 0; f < count; ++f)
            for (std::uint32_t p = 0; p < kPlanes; ++p) {
                UINT64 size = 0;
                m_dev->GetCopyableFootprints(&m_texDesc, subresource(f, p), 1, total, &fp[f * kPlanes + p],
                                             nullptr, nullptr, &size);
                total = alignUp(total + size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            }
        const UINT64 statsAt = total, statsBytes = m_statsHeap ? kStatsStride * count : 0;
        Com<ID3D12Resource> rb;
        check(committed(m_dev, D3D12_HEAP_TYPE_READBACK, bufferDesc(total + statsBytes),
                        D3D12_RESOURCE_STATE_COPY_DEST, rb),
              "readback buffer");
        Com<ID3D12GraphicsCommandList> cl;
        check(m_copy.allocator->Reset(), "ID3D12CommandAllocator::Reset");
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copy.allocator.get(), nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), cl.putVoid()),
              "CreateCommandList (copy)");
        for (std::uint32_t f = 0; f < count; ++f)
            for (std::uint32_t p = 0; p < kPlanes; ++p) {
                // The pictures are in COMMON after decoding; copies promote them to COPY_SOURCE.
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = shown(f);
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = subresource(f, p);
                dst.pResource = rb.get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = fp[f * kPlanes + p];
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        if (statsBytes) cl->CopyBufferRegion(rb.get(), statsAt, m_stats.get(), 0, statsBytes);
        check(cl->Close(), "Close (copy)");
        m_sync.gpuWait(m_copy.queue.get());
        execute(m_copy.queue.get(), cl.get());
        m_sync.signal(m_copy.queue.get());
        m_sync.wait(m_dev);

        std::vector<std::uint8_t> out(video::kFrameBytes * count);
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(total + statsBytes)};
        check(rb->Map(0, &range, &mapped), "Map (readback)");
        const auto* base = static_cast<const std::uint8_t*>(mapped);
        for (std::uint32_t f = 0; f < count; ++f) {
            std::uint8_t* dst = out.data() + f * video::kFrameBytes;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& y = fp[f * kPlanes];
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& uv = fp[f * kPlanes + 1];
            for (std::uint32_t r = 0; r < video::kHeight; ++r)
                std::memcpy(dst + std::size_t(r) * video::kWidth,
                            base + y.Offset + std::size_t(r) * y.Footprint.RowPitch, video::kWidth);
            dst += std::size_t(video::kWidth) * video::kHeight;
            for (std::uint32_t r = 0; r < video::kHeight / 2; ++r)
                std::memcpy(dst + std::size_t(r) * video::kWidth,
                            base + uv.Offset + std::size_t(r) * uv.Footprint.RowPitch, video::kWidth);
        }
        m_status = DecodeStatus::array();
        for (std::uint32_t f = 0; f < count && statsBytes; ++f) {
            static constexpr const char* kNames[] = {"ok", "continue", "continue, skip display", "restart",
                                                     "rate exceeded"};
            D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS st;
            std::memcpy(&st, base + statsAt + f * kStatsStride, sizeof(st));
            const std::string name =
                st.Status < std::size(kNames) ? kNames[st.Status] : std::format("status {}", st.Status);
            m_status.push_back(st.NumMacroblocksAffected ? std::format("{} ({} macroblocks affected)", name,
                                                                       st.NumMacroblocksAffected)
                                                         : name);
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        return out;
    }
    /// The decoder's status for each frame of the last readback (empty without decode statistics).
    const DecodeStatus& status() const { return m_status; }

    std::string outputMode() const {
        return std::format("NV12 {}, decode tier {}{}", m_separate ? "textures" : "texture array",
                           static_cast<int>(m_tier),
                           m_referenceOnly ? ", reference-only pictures + output conversion" : "");
    }

private:
    /// Frame f's picture in the DPB, and the picture that is read back (the output conversion's target when
    /// the DPB is reference-only).
    ID3D12Resource* picture(std::uint32_t f) const { return m_dpb[m_separate ? f : 0].get(); }
    ID3D12Resource* shown(std::uint32_t f) const {
        return m_referenceOnly ? m_output[m_separate ? f : 0].get() : picture(f);
    }
    /// The subresource of frame f's plane p (luma 0, chroma 1) in picture(f) / shown(f).
    UINT subresource(std::uint32_t f, std::uint32_t p) const { return m_separate ? p : f + p * video::kGop; }

    /// Copies every frame's bitstream into one default-heap buffer, each frame at an aligned offset.
    void uploadBitstream() {
        UINT64 total = 0;
        for (const DxvaFrame& f : m_s.frames) {
            m_offsets.push_back(total);
            m_sizes.push_back(alignUp(f.data.size(), 128)); // zero padding to the DXVA alignment
            total = alignUp(total + m_sizes.back(), kBitstreamAlign);
        }
        Com<ID3D12Resource> upload;
        check(committed(m_dev, D3D12_HEAP_TYPE_UPLOAD, bufferDesc(total), D3D12_RESOURCE_STATE_GENERIC_READ,
                        upload),
              "bitstream upload buffer");
        check(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, bufferDesc(total), D3D12_RESOURCE_STATE_COMMON,
                        m_bitstream),
              "bitstream buffer");
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        check(upload->Map(0, &none, &mapped), "Map (bitstream)");
        std::memset(mapped, 0, static_cast<std::size_t>(total));
        for (std::size_t i = 0; i < m_s.frames.size(); ++i)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + m_offsets[i], m_s.frames[i].data.data(),
                        m_s.frames[i].data.size());
        upload->Unmap(0, nullptr);

        Com<ID3D12GraphicsCommandList> cl;
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copy.allocator.get(), nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), cl.putVoid()),
              "CreateCommandList (copy)");
        cl->CopyBufferRegion(m_bitstream.get(), 0, upload.get(), 0, total);
        check(cl->Close(), "Close (copy)");
        execute(m_copy.queue.get(), cl.get());
        m_sync.signal(m_copy.queue.get());
        m_sync.wait(m_dev); // the buffer decays to COMMON
    }

    /// Records frame f's decode: its slice (both planes) is written and every other slice is a possible
    /// reference; everything returns to COMMON afterwards.
    Com<ID3D12VideoDecodeCommandList> recordFrame(std::uint32_t f) {
        Com<ID3D12VideoDecodeCommandList> cl;
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, m_decode.allocator.get(),
                                       nullptr, __uuidof(ID3D12VideoDecodeCommandList), cl.putVoid()),
              "CreateCommandList (video decode)");
        const D3D12_RESOURCE_BARRIER toRead =
            transition(m_bitstream.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_READ);
        cl->ResourceBarrier(1, &toRead);
        std::vector<D3D12_RESOURCE_BARRIER> before, after;
        const DxvaFrame& fr = m_s.frames[f];
        for (std::uint32_t slot = 0; slot < video::kGop; ++slot)
            for (std::uint32_t p = 0; p < kPlanes; ++p)
                before.push_back(transition(picture(slot), subresource(slot, p), D3D12_RESOURCE_STATE_COMMON,
                                            slot == f ? D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE
                                                      : D3D12_RESOURCE_STATE_VIDEO_DECODE_READ));
        if (m_referenceOnly)
            for (std::uint32_t p = 0; p < kPlanes; ++p)
                before.push_back(transition(shown(f), subresource(f, p), D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE));
        cl->ResourceBarrier(static_cast<UINT>(before.size()), before.data());

        D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out{};
        if (m_referenceOnly) {
            out.pOutputTexture2D = shown(f);
            out.OutputSubresource = subresource(f, 0);
            out.ConversionArguments.Enable = TRUE;
            out.ConversionArguments.pReferenceTexture2D = picture(f);
            out.ConversionArguments.ReferenceSubresource = subresource(f, 0);
            out.ConversionArguments.OutputColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            out.ConversionArguments.DecodeColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        } else {
            out.pOutputTexture2D = picture(f);
            out.OutputSubresource = subresource(f, 0);
        }
        D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in{};
        auto arg = [&](D3D12_VIDEO_DECODE_ARGUMENT_TYPE type, const std::vector<std::uint8_t>& data) {
            if (data.empty()) return;
            in.FrameArguments[in.NumFrameArguments++] = {type, static_cast<UINT>(data.size()),
                                                         const_cast<std::uint8_t*>(data.data())};
        };
        arg(D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, fr.picParams);
        arg(D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, fr.qmatrix);
        arg(D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, fr.slices);
        in.ReferenceFrames.NumTexture2Ds = video::kGop;
        in.ReferenceFrames.ppTexture2Ds = m_refTextures.data();
        in.ReferenceFrames.pSubresources = m_refSubresources.data();
        in.ReferenceFrames.ppHeaps = m_refHeaps.data();
        in.CompressedBitstream.pBuffer = m_bitstream.get();
        in.CompressedBitstream.Offset = m_offsets[f];
        in.CompressedBitstream.Size = m_sizes[f];
        in.pHeap = m_heap.get();
        cl->DecodeFrame(m_decoder.get(), &out, &in);
        if (m_statsHeap) {
            cl->EndQuery(m_statsHeap.get(), D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, f);
            const D3D12_RESOURCE_BARRIER toWrite =
                transition(m_stats.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
            cl->ResourceBarrier(1, &toWrite);
            cl->ResolveQueryData(m_statsHeap.get(), D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, f, 1,
                                 m_stats.get(), f * kStatsStride);
            const D3D12_RESOURCE_BARRIER back =
                transition(m_stats.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
            cl->ResourceBarrier(1, &back);
        }

        after = before;
        for (D3D12_RESOURCE_BARRIER& b : after)
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        const D3D12_RESOURCE_BARRIER toCommon =
            transition(m_bitstream.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_VIDEO_DECODE_READ, D3D12_RESOURCE_STATE_COMMON);
        cl->ResourceBarrier(1, &toCommon);
        check(cl->Close(), "Close (video decode)");
        return cl;
    }

    ID3D12Device* m_dev;
    const DxvaStream& m_s;
    Sync m_sync;
    Com<ID3D12VideoDevice> m_video;
    Com<ID3D12VideoDecoder> m_decoder;
    Com<ID3D12VideoDecoderHeap> m_heap;
    D3D12_VIDEO_DECODE_TIER m_tier = D3D12_VIDEO_DECODE_TIER_NOT_SUPPORTED;
    bool m_referenceOnly = false;
    D3D12_RESOURCE_DESC m_texDesc{};
    bool m_separate = false;                          ///< one texture per picture instead of a texture array
    std::vector<Com<ID3D12Resource>> m_dpb, m_output; ///< the DPB pictures; the outputs if reference-only
    Com<ID3D12Resource> m_bitstream, m_stats;
    Com<ID3D12QueryHeap> m_statsHeap;
    DecodeStatus m_status = DecodeStatus::array();
    std::vector<UINT64> m_offsets, m_sizes;
    Queue m_decode, m_copy;
    std::vector<Com<ID3D12VideoDecodeCommandList>> m_lists; ///< kRepeats GOPs, one list per frame
    std::vector<ID3D12Resource*> m_refTextures;
    std::vector<UINT> m_refSubresources;
    std::vector<ID3D12VideoDecoderHeap*> m_refHeaps;
};

} // namespace

std::vector<CodecHooks> codecs() {
    return {
        {"h264", "H.264", D3D12_VIDEO_DECODE_PROFILE_H264, dxvaH264, h264Encoder},
        {"h265", "H.265", D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, dxvaH265, h265Encoder},
        {"av1", "AV1", D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0, dxvaAv1, av1Encoder},
    };
}

std::string decodeFrames(ID3D12Device* dev, const DxvaStream& s, std::vector<std::uint8_t>& frames,
                         DecodeStatus* status) {
    const std::unique_ptr<Decoder, KeepIfHung> dec(new Decoder(dev, s));
    if (std::string why = dec->prepare(); !why.empty()) return why;
    if (std::string failure = dec->decodeOnce(); !failure.empty()) return failure;
    frames = dec->readback();
    if (status) *status = dec->status();
    return {};
}

std::string blocker(ID3D12Device* dev) {
    const Com<ID3D12VideoDevice> video = videoDevice(dev);
    if (!video) return "the device has no Direct3D 12 video interface (ID3D12VideoDevice)";
    D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count{};
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof(count))) ||
        count.ProfileCount == 0)
        return "the GPU lists no Direct3D 12 video decode profiles";
    return {};
}

bool canDecode(ID3D12Device* dev, const CodecHooks& c) {
    const Com<ID3D12VideoDevice> video = videoDevice(dev);
    return video && decodeSupport(video.get(), c.decodeProfile, video::kWidth, video::kHeight).has_value();
}

std::vector<std::string> decodeProfiles(ID3D12Device* dev) {
    std::vector<std::string> out;
    const Com<ID3D12VideoDevice> video = videoDevice(dev);
    if (!video) return out;
    D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count{};
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof(count))))
        return out;
    std::vector<GUID> guids(count.ProfileCount);
    D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES list{};
    list.ProfileCount = count.ProfileCount;
    list.pProfiles = guids.data();
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILES, &list, sizeof(list))))
        return out;
    for (const GUID& g : guids)
        out.push_back(profileName(g));
    return out;
}

namespace {

void decodeReference(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks, double seconds,
                     const video::Progress& progress, video::CodecResult& res) {
    if (!canDecode(dev, hooks)) {
        res.decodeNote = std::format("no Direct3D 12 {} decoder", hooks.label);
        return;
    }
    const video::ReferenceStream* ref = video::referenceStream(hooks.key);
    if (!ref) {
        res.decodeNote = "the reference stream is missing from this build";
        return;
    }
    std::string error;
    std::optional<DxvaStream> stream = hooks.reference(ref->data, &error);
    if (!stream) {
        res.failure = std::format("the {} reference stream could not be read: {}", hooks.label, error);
        return;
    }
    // --param feature.video.frames=N decodes only the first N frames: a diagnostic for decoders that stall
    // part-way through (the frames up to there are still checked bit for bit).
    const std::string limit = ctx.settings().param("feature.video.frames");
    std::uint32_t n = 0;
    if (std::from_chars(limit.data(), limit.data() + limit.size(), n).ec == std::errc{} && n >= 1 &&
        n < stream->frames.size())
        stream->frames.resize(n);
    const std::unique_ptr<Decoder, KeepIfHung> dec(new Decoder(dev, *stream));
    if (std::string why = dec->prepare(); !why.empty()) {
        res.decodeNote = why;
        return;
    }
    res.details["decode_stream"] = std::format("{} ({} frames, {} KiB, from {})", stream->name,
                                               stream->frames.size(), ref->data.size() / 1024, "data/video");
    res.details["decode_output"] = dec->outputMode();
    ctx.log(std::format("{}: {} ready ({})", hooks.label, stream->name, dec->outputMode()));
    progress(0.0, std::format("{}: decoding the reference stream", hooks.label));
    if (std::string failure = dec->decodeOnce(); !failure.empty()) {
        res.failure = failure;
        return;
    }
    ctx.log(std::format("{}: decoded the reference stream once", hooks.label));
    // The pictures are checked before the throughput is measured, so a decoder that fails under load still
    // gets its correctness verdict.
    progress(0.3, std::format("{}: checking the decoded frames", hooks.label));
    video::checkReference(*ref, dec->readback(), static_cast<std::uint32_t>(stream->frames.size()), res);
    if (!dec->status().empty()) res.details["decode_status"] = dec->status();
    progress(0.35, std::format("{}: measuring decode throughput", hooks.label));
    res.decodeFps = dec->time(ctx, seconds);
    ctx.log(std::format("{}: {:.1f} fps decode", hooks.label, *res.decodeFps));
}

} // namespace

video::CodecResult runCodec(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks,
                            const std::vector<std::uint8_t>& source, double seconds,
                            const video::Progress& progress) {
    video::CodecResult res;
    res.key = hooks.key;
    res.label = hooks.label;
    // A codec whose GPU work fails or never finishes is that codec's failure; the others still run.
    try {
        decodeReference(ctx, dev, hooks, seconds, progress, res);
    } catch (const std::exception& e) { // a stop request is not a std::exception and passes
        const std::string why = std::format("{} decode: {}", hooks.label, e.what());
        res.failure = res.failure.empty() ? why : res.failure + "; " + why; // keep a CRC failure found first
    }
    ctx.checkStop();
    // Decoding and encoding run on separate engines: a decode failure does not hide the encode measurement.
    const std::string decodeFailure = std::exchange(res.failure, {});
    if (!hooks.encoder) {
        res.encodeNote = std::format("{} encoding is measured through Vulkan Video only", hooks.label);
    } else {
        try {
            encodeAndCheck(ctx, dev, hooks, source, seconds, progress, res);
        } catch (const std::exception& e) {
            const std::string why = std::format("{} encode: {}", hooks.label, e.what());
            res.failure = res.failure.empty() ? why : res.failure + "; " + why;
        }
    }
    if (!decodeFailure.empty())
        res.failure = res.failure.empty() ? decodeFailure : decodeFailure + "; " + res.failure;
    return res;
}

} // namespace gt::bench::d3d12video

#endif // GT_HAS_D3D12
