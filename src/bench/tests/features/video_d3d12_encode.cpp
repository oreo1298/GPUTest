// Direct3D 12 Video encoding for feature.video: a key frame and predicted frames at a constant QP on the
// GPU's video encoder, H.264, H.265 and AV1. The encoder writes slice / tile data only; the parameter sets
// come from the configuration the support queries settle on (h264::write / h265::write), and AV1's sequence
// and frame headers from the values the encoder reports after each frame (av1::writeFrame), as Microsoft's
// D3D12 video encoding specifications lay out. The joined stream is decoded again on the Direct3D 12 decoder
// to check the pictures.

#include "video_d3d12.h"

#if GT_HAS_D3D12

#include "gt/bench/av1_params.h"
#include "gt/bench/h264_params.h"
#include "gt/bench/h265_params.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

namespace gt::bench::d3d12video {

using rhi::d3d12::alignUp;
using rhi::d3d12::check;
using rhi::d3d12::Com;
using rhi::d3d12::hresultName;

namespace {

constexpr std::uint32_t kRepeats = 8;       ///< GOPs per timed submission
constexpr UINT64 kFrameStride = 4ull << 20; ///< bitstream bytes reserved per frame
constexpr std::uint32_t kPlanes = 2;
const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC kResolution{video::kWidth, video::kHeight};
UINT kZero = 0; ///< L0 lists: the one reference descriptor

// The codec-specific unions, set by member (no designated initializers for anonymous unions).
D3D12_VIDEO_ENCODER_PROFILE_DESC desc(D3D12_VIDEO_ENCODER_PROFILE_H264* p) {
    D3D12_VIDEO_ENCODER_PROFILE_DESC d{};
    d.DataSize = sizeof(*p);
    d.pH264Profile = p;
    return d;
}
D3D12_VIDEO_ENCODER_PROFILE_DESC desc(D3D12_VIDEO_ENCODER_PROFILE_HEVC* p) {
    D3D12_VIDEO_ENCODER_PROFILE_DESC d{};
    d.DataSize = sizeof(*p);
    d.pHEVCProfile = p;
    return d;
}
D3D12_VIDEO_ENCODER_LEVEL_SETTING desc(D3D12_VIDEO_ENCODER_LEVELS_H264* p) {
    D3D12_VIDEO_ENCODER_LEVEL_SETTING d{};
    d.DataSize = sizeof(*p);
    d.pH264LevelSetting = p;
    return d;
}
D3D12_VIDEO_ENCODER_LEVEL_SETTING desc(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC* p) {
    D3D12_VIDEO_ENCODER_LEVEL_SETTING d{};
    d.DataSize = sizeof(*p);
    d.pHEVCLevelSetting = p;
    return d;
}
D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION desc(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264* p) {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION d{};
    d.DataSize = sizeof(*p);
    d.pH264Config = p;
    return d;
}
D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION desc(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC* p) {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION d{};
    d.DataSize = sizeof(*p);
    d.pHEVCConfig = p;
    return d;
}
D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE desc(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264* p) {
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE d{};
    d.DataSize = sizeof(*p);
    d.pH264GroupOfPictures = p;
    return d;
}
D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE desc(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC* p) {
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE d{};
    d.DataSize = sizeof(*p);
    d.pHEVCGroupOfPictures = p;
    return d;
}
D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA desc(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264* p) {
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA d{};
    d.DataSize = sizeof(*p);
    d.pH264PicData = p;
    return d;
}
D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA desc(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC* p) {
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA d{};
    d.DataSize = sizeof(*p);
    d.pHEVCPicData = p;
    return d;
}
D3D12_VIDEO_ENCODER_PROFILE_DESC desc(D3D12_VIDEO_ENCODER_AV1_PROFILE* p) {
    D3D12_VIDEO_ENCODER_PROFILE_DESC d{};
    d.DataSize = sizeof(*p);
    d.pAV1Profile = p;
    return d;
}
D3D12_VIDEO_ENCODER_LEVEL_SETTING desc(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS* p) {
    D3D12_VIDEO_ENCODER_LEVEL_SETTING d{};
    d.DataSize = sizeof(*p);
    d.pAV1LevelSetting = p;
    return d;
}
D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION desc(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION* p) {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION d{};
    d.DataSize = sizeof(*p);
    d.pAV1Config = p;
    return d;
}
D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE desc(D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE* p) {
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE d{};
    d.DataSize = sizeof(*p);
    d.pAV1SequenceStructure = p;
    return d;
}
D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA desc(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA* p) {
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA d{};
    d.DataSize = sizeof(*p);
    d.pAV1PicData = p;
    return d;
}

std::string validationText(D3D12_VIDEO_ENCODER_VALIDATION_FLAGS f) {
    struct Name {
        D3D12_VIDEO_ENCODER_VALIDATION_FLAGS flag;
        const char* text;
    };
    static const Name kNames[] = {
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_NOT_SUPPORTED, "codec"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED, "input format"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED, "codec configuration"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED, "rate control mode"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED,
         "rate control settings"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED, "intra refresh"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED, "slice layout"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST, "resolution"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED, "GOP structure"},
        {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_DATA_NOT_SUPPORTED, "tile layout"},
    };
    std::string s;
    for (const Name& n : kNames)
        if (f & n.flag) s += (s.empty() ? "" : ", ") + std::string(n.text);
    return s.empty() ? std::format("validation flags {:#x}", static_cast<unsigned>(f)) : s;
}

D3D12_VIDEO_ENCODER_RATE_CONTROL cqp(const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP& q) {
    D3D12_VIDEO_ENCODER_RATE_CONTROL rc{};
    rc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    rc.Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
    rc.ConfigParams.DataSize = sizeof(q);
    rc.ConfigParams.pConfiguration_CQP = &q;
    rc.TargetFrameRate = {60, 1};
    return rc;
}

/// The checks every codec makes: codec, input format, constant QP and how a frame is split.
std::string basicSupport(ID3D12VideoDevice3* video, D3D12_VIDEO_ENCODER_CODEC codec,
                         D3D12_VIDEO_ENCODER_PROFILE_DESC profile, D3D12_VIDEO_ENCODER_LEVEL_SETTING level,
                         D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE layoutMode =
                             D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT fmt{};
    fmt.Codec = codec;
    fmt.Profile = profile;
    fmt.Format = DXGI_FORMAT_NV12;
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &fmt, sizeof(fmt))) ||
        !fmt.IsSupported)
        return "the encoder does not take NV12 input";
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE rc{};
    rc.Codec = codec;
    rc.RateControlMode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE, &rc, sizeof(rc))) ||
        !rc.IsSupported)
        return "the encoder has no constant-QP mode";
    D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE layout{};
    layout.Codec = codec;
    layout.Profile = profile;
    layout.Level = level;
    layout.SubregionMode = layoutMode;
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE, &layout,
                                          sizeof(layout))) ||
        !layout.IsSupported)
        return layoutMode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME
                   ? "the encoder cannot write a frame as one slice"
                   : "the encoder cannot split a frame into a uniform tile grid";
    return {};
}

/// D3D12_FEATURE_VIDEO_ENCODER_SUPPORT for the whole configuration: the only query that guarantees the parts
/// work together. Fills the suggested level and the per-resolution limits.
std::string fullSupport(ID3D12VideoDevice3* video, D3D12_VIDEO_ENCODER_CODEC codec,
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config,
                        D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop,
                        const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP& q,
                        D3D12_VIDEO_ENCODER_PROFILE_DESC suggestedProfile,
                        D3D12_VIDEO_ENCODER_LEVEL_SETTING suggestedLevel,
                        D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS& limits,
                        D3D12_VIDEO_ENCODER_SUPPORT_FLAGS& flags) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT s{};
    s.Codec = codec;
    s.InputFormat = DXGI_FORMAT_NV12;
    s.CodecConfiguration = config;
    s.CodecGopSequence = gop;
    s.RateControl = cqp(q);
    s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    s.ResolutionsListCount = 1;
    s.pResolutionList = &kResolution;
    s.MaxReferenceFramesInDPB = 1;
    s.SuggestedProfile = suggestedProfile;
    s.SuggestedLevel = suggestedLevel;
    s.pResolutionDependentSupport = &limits;
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s, sizeof(s))))
        return "the encoder support query failed";
    flags = s.SupportFlags;
    if (!(s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK))
        return "the encoder does not support this " + validationText(s.ValidationFlags);
    return {};
}

std::string referenceSupport(ID3D12VideoDevice3* video, D3D12_VIDEO_ENCODER_CODEC codec,
                             D3D12_VIDEO_ENCODER_PROFILE_DESC profile) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pc{};
    pc.Codec = codec;
    pc.Profile = profile;
    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264 h264{};
    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC hevc{};
    UINT maxP = 0, dpb = 0;
    if (codec == D3D12_VIDEO_ENCODER_CODEC_H264) {
        pc.PictureSupport.DataSize = sizeof(h264);
        pc.PictureSupport.pH264Support = &h264;
    } else {
        pc.PictureSupport.DataSize = sizeof(hevc);
        pc.PictureSupport.pHEVCSupport = &hevc;
    }
    if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pc,
                                          sizeof(pc))) ||
        !pc.IsSupported)
        return "the encoder reports no picture control support";
    maxP = codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? h264.MaxL0ReferencesForP : hevc.MaxL0ReferencesForP;
    dpb = codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? h264.MaxDPBCapacity : hevc.MaxDPBCapacity;
    if (maxP < 1 || dpb < 1) return "the encoder cannot predict P-frames from a reference";
    return {};
}

// --- H.264 -----------------------------------------------------------------------------------------

std::uint32_t h264LevelIdc(D3D12_VIDEO_ENCODER_LEVELS_H264 l) {
    static constexpr std::uint32_t kIdc[] = {10, 11, 11, 12, 13, 20, 21, 22, 30, 31,
                                             32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
    const auto i = static_cast<std::size_t>(l);
    return i < std::size(kIdc) ? kIdc[i] : 0;
}

class H264Encode final : public EncodeCodec {
public:
    std::string name() const override {
        return m_profile == D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH ? "H.264 High" : "H.264 Main";
    }
    D3D12_VIDEO_ENCODER_CODEC codec() const override { return D3D12_VIDEO_ENCODER_CODEC_H264; }
    std::uint32_t quantizer() const override { return 26; }

    std::string configure(ID3D12VideoDevice3* video) override {
        std::string why = "the encoder supports neither H.264 High nor Main";
        for (const auto p : {D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH, D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN}) {
            m_profile = p;
            why = tryProfile(video);
            if (why.empty()) return {};
        }
        return why;
    }
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile() override { return desc(&m_profile); }
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level() override { return desc(&m_level); }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration() override { return desc(&m_config); }
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop() override { return desc(&m_gop); }

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picture(std::uint32_t f, std::uint32_t r) override {
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264& p = m_pictures[(r & 1) * video::kGop + f];
        p = {};
        p.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_NONE;
        p.FrameType = f == 0 ? D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME
                             : D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
        p.pic_parameter_set_id = 0;
        p.idr_pic_id = r & 1;              // consecutive IDRs differ
        p.PictureOrderCountNumber = 2 * f; // TopFieldOrderCnt of POC type 2 (and the lsb for type 0)
        p.FrameDecodingOrderNumber = f;    // frame_num: every frame is a reference
        p.TemporalLayerIndex = 0;
        if (f > 0) {
            D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264& d = m_refs[f];
            d = {};
            d.ReconstructedPictureResourceIndex = 0;
            d.IsLongTermReference = FALSE;
            d.PictureOrderCountNumber = 2 * (f - 1);
            d.FrameDecodingOrderNumber = f - 1;
            p.List0ReferenceFramesCount = 1;
            p.pList0ReferenceFrames = &kZero;
            p.ReferenceFramesReconPictureDescriptorsCount = 1;
            p.pReferenceFramesReconPictureDescriptors = &d;
        }
        p.adaptive_ref_pic_marking_mode_flag = 0; // sliding window
        return desc(&p);
    }

    std::vector<std::uint8_t> headers() const override {
        // The SPS / PPS the D3D12 specification gives for this configuration.
        h264::ParameterSets ps;
        h264::Sps& s = ps.sps.emplace();
        s.profileIdc = m_profile == D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH ? 100 : 77;
        s.levelIdc = h264LevelIdc(m_level);
        s.chromaFormatIdc = 1;
        s.log2MaxFrameNumMinus4 = m_gop.log2_max_frame_num_minus4;
        s.pocType = m_gop.pic_order_cnt_type;
        s.log2MaxPocLsbMinus4 = m_gop.log2_max_pic_order_cnt_lsb_minus4;
        s.maxNumRefFrames = 1;
        s.widthInMbsMinus1 = (video::kWidth + 15) / 16 - 1;
        s.heightInMapUnitsMinus1 = (video::kHeight + 15) / 16 - 1;
        s.frameMbsOnly = true;
        s.direct8x8Inference = true;
        s.frameCropping = true; // 1088 coded rows, the last 8 cropped
        s.cropBottom = ((s.heightInMapUnitsMinus1 + 1) * 16 - video::kHeight) / 2;
        h264::Pps& p = ps.pps.emplace();
        p.entropyCodingMode = (m_config.ConfigurationFlags &
                               D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING) != 0;
        p.deblockingFilterControlPresent = true;
        p.constrainedIntraPred = false;
        p.transform8x8Mode =
            (m_config.ConfigurationFlags &
             D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM) != 0;
        return h264::write(ps);
    }
    std::optional<DxvaStream> readBack(std::span<const std::uint8_t> stream,
                                       std::string* error) const override {
        return dxvaH264(stream, error);
    }
    nlohmann::json describe() const override {
        return {
            {"profile", name()},
            {"level", h264LevelIdc(m_level) / 10.0},
            {"entropy_coding", m_config.ConfigurationFlags &
                                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING
                                   ? "CABAC"
                                   : "CAVLC"},
            {"transform_8x8",
             (m_config.ConfigurationFlags &
              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM) != 0},
            {"pic_order_cnt_type", m_gop.pic_order_cnt_type},
            {"deblocking_mode", static_cast<int>(m_config.DisableDeblockingFilterConfig)}};
    }

private:
    std::string tryProfile(ID3D12VideoDevice3* video) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl{};
        D3D12_VIDEO_ENCODER_LEVELS_H264 minLevel{}, maxLevel{};
        pl.Codec = codec();
        pl.Profile = profile();
        pl.MinSupportedLevel = desc(&minLevel);
        pl.MaxSupportedLevel = desc(&maxLevel);
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof(pl))) ||
            !pl.IsSupported)
            return std::format("the encoder does not support {}", name());
        if (maxLevel < D3D12_VIDEO_ENCODER_LEVELS_H264_4)
            return "the H.264 encoder's highest level is below 4.0";
        m_level = std::min(maxLevel, D3D12_VIDEO_ENCODER_LEVELS_H264_41);
        if (std::string why = basicSupport(video, codec(), profile(), level()); !why.empty()) return why;
        if (std::string why = referenceSupport(video, codec(), profile()); !why.empty()) return why;

        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT cs{};
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 limits{};
        cs.Codec = codec();
        cs.Profile = profile();
        cs.CodecSupportLimits.DataSize = sizeof(limits);
        cs.CodecSupportLimits.pH264Support = &limits;
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &cs,
                                              sizeof(cs))) ||
            !cs.IsSupported)
            return "the H.264 encoder reports no coding tools";
        m_config = {};
        if (limits.SupportFlags &
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT)
            m_config.ConfigurationFlags |=
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
        if (m_profile == D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH &&
            (limits.SupportFlags &
             D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_ADAPTIVE_8x8_TRANSFORM_ENCODING_SUPPORT))
            m_config.ConfigurationFlags |=
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
        m_config.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
        // Full deblocking where offered, else the first mode the encoder has.
        const UINT modes = static_cast<UINT>(limits.DisableDeblockingFilterSupportedModes);
        int mode = 0;
        while (mode < 7 && !(modes & (1u << mode)))
            ++mode;
        m_config.DisableDeblockingFilterConfig =
            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODES>(mode < 7 ? mode
                                                                                                       : 0);

        const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP q{quantizer(), quantizer(), quantizer()};
        std::string why;
        for (const UCHAR pocType : {UCHAR(2), UCHAR(0)}) { // POC type 2 suits P-only streams; 0 otherwise
            m_gop = {};
            m_gop.GOPLength = video::kGop;
            m_gop.PPicturePeriod = 1;
            m_gop.pic_order_cnt_type = pocType;
            m_gop.log2_max_frame_num_minus4 = 0;         // MaxFrameNum 16 > 8 frames
            m_gop.log2_max_pic_order_cnt_lsb_minus4 = 4; // MaxPicOrderCntLsb 256 > 2 x 8
            D3D12_VIDEO_ENCODER_PROFILE_H264 suggestedProfile{};
            D3D12_VIDEO_ENCODER_LEVELS_H264 suggestedLevel{};
            D3D12_VIDEO_ENCODER_SUPPORT_FLAGS flags{};
            why = fullSupport(video, codec(), configuration(), gop(), q, desc(&suggestedProfile),
                              desc(&suggestedLevel), m_limits, flags);
            if (why.empty()) {
                if (suggestedLevel >= D3D12_VIDEO_ENCODER_LEVELS_H264_4 && suggestedLevel <= maxLevel)
                    m_level = suggestedLevel;
                return {};
            }
        }
        return why;
    }

    D3D12_VIDEO_ENCODER_PROFILE_H264 m_profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_LEVELS_H264 m_level = D3D12_VIDEO_ENCODER_LEVELS_H264_41;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 m_config{};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 m_gop{};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS m_limits{};
    std::array<D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264, 2 * video::kGop> m_pictures{};
    std::array<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264, video::kGop> m_refs{};
};

// --- H.265 -----------------------------------------------------------------------------------------

std::uint32_t h265LevelIdc(D3D12_VIDEO_ENCODER_LEVELS_HEVC l) {
    static constexpr std::uint32_t kIdc[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    const auto i = static_cast<std::size_t>(l);
    return i < std::size(kIdc) ? kIdc[i] : 0;
}

std::uint32_t log2Of(std::uint32_t v) {
    std::uint32_t n = 0;
    while ((2u << n) <= v)
        ++n;
    return n;
}

class H265Encode final : public EncodeCodec {
public:
    std::string name() const override { return "H.265 Main"; }
    D3D12_VIDEO_ENCODER_CODEC codec() const override { return D3D12_VIDEO_ENCODER_CODEC_HEVC; }
    std::uint32_t quantizer() const override { return 28; }

    std::string configure(ID3D12VideoDevice3* video) override {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl{};
        D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC minLevel{}, maxLevel{};
        pl.Codec = codec();
        pl.Profile = profile();
        pl.MinSupportedLevel = desc(&minLevel);
        pl.MaxSupportedLevel = desc(&maxLevel);
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof(pl))) ||
            !pl.IsSupported)
            return "the encoder does not support H.265 Main";
        if (maxLevel.Level < D3D12_VIDEO_ENCODER_LEVELS_HEVC_4)
            return "the H.265 encoder's highest level is below 4";
        m_level = {std::min(maxLevel.Level, D3D12_VIDEO_ENCODER_LEVELS_HEVC_41),
                   D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
        if (std::string why = basicSupport(video, codec(), profile(), level()); !why.empty()) return why;
        if (std::string why = referenceSupport(video, codec(), profile()); !why.empty()) return why;

        // Coding block sizes: the largest CTU the encoder takes, 8x8 CUs, 4x4 to 32x32 TUs.
        if (std::string why = pickBlockSizes(video); !why.empty()) return why;
        const auto sup = static_cast<UINT>(m_support.SupportFlags);
        m_config.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE;
        if (sup & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_SAO_FILTER_SUPPORT)
            m_config.ConfigurationFlags |=
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER;
        if (sup &
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED)
            m_config.ConfigurationFlags |=
                D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
        m_lowDelayB =
            (sup &
             D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_P_FRAMES_IMPLEMENTED_AS_LOW_DELAY_B_FRAMES) !=
            0;

        m_gop = {};
        m_gop.GOPLength = video::kGop;
        m_gop.PPicturePeriod = 1;
        m_gop.log2_max_pic_order_cnt_lsb_minus4 = 4;
        const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP q{quantizer(), quantizer(), quantizer()};
        D3D12_VIDEO_ENCODER_PROFILE_HEVC suggestedProfile{};
        D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel{};
        D3D12_VIDEO_ENCODER_SUPPORT_FLAGS flags{};
        if (std::string why = fullSupport(video, codec(), configuration(), gop(), q, desc(&suggestedProfile),
                                          desc(&suggestedLevel), m_limits, flags);
            !why.empty())
            return why;
        if (suggestedLevel.Level >= D3D12_VIDEO_ENCODER_LEVELS_HEVC_4 &&
            suggestedLevel.Level <= maxLevel.Level)
            m_level = suggestedLevel;
        return {};
    }
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile() override { return desc(&m_profile); }
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level() override { return desc(&m_level); }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration() override { return desc(&m_config); }
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop() override { return desc(&m_gop); }

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picture(std::uint32_t f, std::uint32_t r) override {
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC& p = m_pictures[(r & 1) * video::kGop + f];
        p = {};
        p.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_NONE;
        p.FrameType = f == 0 ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME
                             : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME;
        p.slice_pic_parameter_set_id = 0;
        p.PictureOrderCountNumber = f;
        p.TemporalLayerIndex = 0;
        if (f > 0) {
            D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC& d = m_refs[f];
            d = {};
            d.ReconstructedPictureResourceIndex = 0;
            d.IsRefUsedByCurrentPic = TRUE;
            d.IsLongTermReference = FALSE;
            d.PictureOrderCountNumber = f - 1;
            p.List0ReferenceFramesCount = 1;
            p.pList0ReferenceFrames = &kZero;
            p.ReferenceFramesReconPictureDescriptorsCount = 1;
            p.pReferenceFramesReconPictureDescriptors = &d;
        }
        return desc(&p);
    }

    std::vector<std::uint8_t> headers() const override {
        // The VPS / SPS / PPS the D3D12 specification gives for this configuration.
        h265::ProfileTierLevel ptl;
        ptl.profileIdc = 1;
        ptl.tier = m_level.Tier == D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH;
        ptl.levelIdc = h265LevelIdc(m_level.Level);
        ptl.progressiveSource = true;
        ptl.nonPackedConstraint = true;
        ptl.frameOnlyConstraint = true;
        h265::DecPicBufMgr dpb;
        dpb.maxDecPicBufferingMinus1[0] = 1; // the reference + the current picture
        dpb.maxLatencyIncreasePlus1[0] = 1;
        h265::ParameterSets ps;
        h265::Vps& v = ps.vps.emplace();
        v.temporalIdNesting = true;
        v.ptl = ptl;
        v.dpb = dpb;
        h265::Sps& s = ps.sps.emplace();
        s.temporalIdNesting = true;
        s.ptl = ptl;
        s.chromaFormatIdc = 1;
        const std::uint32_t block = std::max<std::uint32_t>(m_limits.SubregionBlockPixelsSize,
                                                            cuSize(m_config.MinLumaCodingUnitSize));
        s.width = static_cast<std::uint32_t>(alignUp(video::kWidth, block));
        s.height = static_cast<std::uint32_t>(alignUp(video::kHeight, block));
        s.conformanceWindow = s.width != video::kWidth || s.height != video::kHeight;
        s.confRight = (s.width - video::kWidth) / 2;
        s.confBottom = (s.height - video::kHeight) / 2;
        s.log2MaxPocLsbMinus4 = m_gop.log2_max_pic_order_cnt_lsb_minus4;
        s.dpb = dpb;
        const std::uint32_t minCb = log2Of(cuSize(m_config.MinLumaCodingUnitSize));
        const std::uint32_t maxCb = log2Of(cuSize(m_config.MaxLumaCodingUnitSize));
        const std::uint32_t minTb = log2Of(tuSize(m_config.MinLumaTransformUnitSize));
        const std::uint32_t maxTb = log2Of(tuSize(m_config.MaxLumaTransformUnitSize));
        s.log2MinCbMinus3 = minCb - 3;
        s.log2DiffMaxMinCb = maxCb - minCb;
        s.log2MinTbMinus2 = minTb - 2;
        s.log2DiffMaxMinTb = maxTb - minTb;
        s.maxTransformHierarchyDepthInter = m_config.max_transform_hierarchy_depth_inter;
        s.maxTransformHierarchyDepthIntra = m_config.max_transform_hierarchy_depth_intra;
        s.ampEnabled =
            (m_config.ConfigurationFlags &
             D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION) != 0;
        s.saoEnabled = (m_config.ConfigurationFlags &
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER) != 0;
        s.temporalMvpEnabled = false;
        s.strongIntraSmoothing = false;
        h265::Pps& p = ps.pps.emplace();
        p.cabacInitPresent = true;
        p.cuQpDeltaEnabled = true;
        p.sliceChromaQpOffsetsPresent = true;
        p.loopFilterAcrossSlices =
            !(m_config.ConfigurationFlags &
              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_DISABLE_LOOP_FILTER_ACROSS_SLICES);
        p.deblockingControlPresent = true;
        return h265::write(ps);
    }
    std::optional<DxvaStream> readBack(std::span<const std::uint8_t> stream,
                                       std::string* error) const override {
        return dxvaH265(stream, error);
    }
    nlohmann::json describe() const override {
        return {{"profile", name()},
                {"level", h265LevelIdc(m_level.Level) / 30.0},
                {"ctb_size", cuSize(m_config.MaxLumaCodingUnitSize)},
                {"min_cu_size", cuSize(m_config.MinLumaCodingUnitSize)},
                {"tu_sizes", std::format("{}-{}", tuSize(m_config.MinLumaTransformUnitSize),
                                         tuSize(m_config.MaxLumaTransformUnitSize))},
                {"sao", (m_config.ConfigurationFlags &
                         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER) != 0},
                {"p_frames_as_low_delay_b", m_lowDelayB}};
    }

private:
    static std::uint32_t cuSize(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE s) {
        return 8u << static_cast<unsigned>(s);
    }
    static std::uint32_t tuSize(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE s) {
        return 4u << static_cast<unsigned>(s);
    }

    std::string pickBlockSizes(ID3D12VideoDevice3* video) {
        using Cu = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE;
        using Tu = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE;
        for (const Cu maxCu : {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64,
                               D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32,
                               D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_16x16})
            for (const Tu maxTu : {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32,
                                   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_16x16}) {
                if (tuSize(maxTu) > cuSize(maxCu)) continue;
                const std::uint32_t maxDepth = log2Of(cuSize(maxCu)) - 2; // CtbLog2SizeY - MinTbLog2SizeY
                for (const std::uint32_t depth : {maxDepth, 2u, 1u, 0u}) {
                    if (depth > maxDepth) continue;
                    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT cs{};
                    m_support = {};
                    m_support.MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8;
                    m_support.MaxLumaCodingUnitSize = maxCu;
                    m_support.MinLumaTransformUnitSize =
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4;
                    m_support.MaxLumaTransformUnitSize = maxTu;
                    m_support.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(depth);
                    m_support.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(depth);
                    cs.Codec = codec();
                    cs.Profile = profile();
                    cs.CodecSupportLimits.DataSize = sizeof(m_support);
                    cs.CodecSupportLimits.pHEVCSupport = &m_support;
                    if (FAILED(video->CheckFeatureSupport(
                            D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &cs, sizeof(cs))) ||
                        !cs.IsSupported)
                        continue;
                    m_config = {};
                    m_config.MinLumaCodingUnitSize = m_support.MinLumaCodingUnitSize;
                    m_config.MaxLumaCodingUnitSize = m_support.MaxLumaCodingUnitSize;
                    m_config.MinLumaTransformUnitSize = m_support.MinLumaTransformUnitSize;
                    m_config.MaxLumaTransformUnitSize = m_support.MaxLumaTransformUnitSize;
                    m_config.max_transform_hierarchy_depth_inter =
                        m_support.max_transform_hierarchy_depth_inter;
                    m_config.max_transform_hierarchy_depth_intra =
                        m_support.max_transform_hierarchy_depth_intra;
                    return {};
                }
            }
        return "the H.265 encoder takes none of the tried coding block sizes";
    }

    D3D12_VIDEO_ENCODER_PROFILE_HEVC m_profile = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC m_level{D3D12_VIDEO_ENCODER_LEVELS_HEVC_41,
                                                            D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC m_support{};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC m_config{};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC m_gop{};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS m_limits{};
    bool m_lowDelayB = false;
    std::array<D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC, 2 * video::kGop> m_pictures{};
    std::array<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC, video::kGop> m_refs{};
};

// --- AV1 --------------------------------------------------------------------------------------------

constexpr std::uint32_t kAv1QIndex = 110; ///< base_q_idx (the constant QP is a q index for AV1), as on Vulkan
constexpr UINT kInvalidDpbIndex = 0xFF;   ///< D3D12_VIDEO_ENCODER_AV1_INVALID_DPB_RESOURCE_INDEX
constexpr std::array<std::uint32_t, 4> kAv1LoopFilter = {10, 10, 5, 5}; ///< Y vertical / horizontal, U, V

std::uint32_t bitLength(std::uint64_t v) {
    std::uint32_t n = 0;
    while (v >> n)
        ++n;
    return n;
}

std::uint32_t log2Ceil(std::uint64_t v) {
    std::uint32_t n = 0;
    while ((std::uint64_t(1) << n) < v)
        ++n;
    return n;
}

struct Av1FeatureName {
    D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS flag;
    const char* name;
};
constexpr Av1FeatureName kAv1Features[] = {
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_128x128_SUPERBLOCK, "128x128 superblocks"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FILTER_INTRA, "filter intra"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_EDGE_FILTER, "intra edge filter"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTERINTRA_COMPOUND, "inter-intra compound"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MASKED_COMPOUND, "masked compound"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_WARPED_MOTION, "warped motion"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_DUAL_FILTER, "dual filter"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_JNT_COMP, "distance-weighted compound"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FORCED_INTEGER_MOTION_VECTORS, "integer motion vectors"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_SUPER_RESOLUTION, "super-resolution"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_RESTORATION_FILTER, "loop restoration"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING, "palette"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING, "CDEF"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_BLOCK_COPY, "intra block copy"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FRAME_REFERENCE_MOTION_VECTORS, "reference motion vectors"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS, "order hints"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_AUTO_SEGMENTATION, "automatic segmentation"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CUSTOM_SEGMENTATION, "custom segmentation"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_FILTER_DELTAS, "loop filter deltas"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_QUANTIZATION_DELTAS, "quantizer deltas"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_QUANTIZATION_MATRIX, "quantizer matrices"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_REDUCED_TX_SET, "reduced transform set"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MOTION_MODE_SWITCHABLE, "switchable motion mode"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV, "high-precision motion vectors"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_SKIP_MODE_PRESENT, "skip mode"},
    {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_DELTA_LF_PARAMS, "loop filter delta parameters"},
};

class Av1Encode final : public EncodeCodec {
public:
    using Tiles = D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES;
    using Post = D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES;
    using Subregion = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA;

    std::string name() const override { return "AV1 Main"; }
    D3D12_VIDEO_ENCODER_CODEC codec() const override { return D3D12_VIDEO_ENCODER_CODEC_AV1; }
    std::uint32_t quantizer() const override { return kAv1QIndex; }
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE layoutMode() const override {
        return D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION;
    }
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA layoutData() override {
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA d{};
        d.DataSize = sizeof(m_tiles);
        d.pTilesPartition_AV1 = &m_tiles;
        return d;
    }
    UINT64 resolvedSize() const override {
        // The AV1 layout: the metadata, one entry per tile, the tile grid, then the post-encode values.
        const UINT64 tiles =
            std::clamp<UINT64>(m_limits.MaxSubregionsNumber, m_tiles.RowCount * m_tiles.ColCount, 4096);
        return sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA) + tiles * sizeof(Subregion) + sizeof(Tiles) +
               sizeof(Post);
    }

    std::string configure(ID3D12VideoDevice3* video) override {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl{};
        D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS minLevel{}, maxLevel{};
        pl.Codec = codec();
        pl.Profile = profile();
        pl.MinSupportedLevel = desc(&minLevel);
        pl.MaxSupportedLevel = desc(&maxLevel);
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof(pl))) ||
            !pl.IsSupported)
            return "the encoder does not support AV1 Main";
        if (maxLevel.Level < D3D12_VIDEO_ENCODER_AV1_LEVELS_4_0)
            return "the AV1 encoder's highest level is below 4.0";
        m_level = {std::min(maxLevel.Level, D3D12_VIDEO_ENCODER_AV1_LEVELS_4_1),
                   D3D12_VIDEO_ENCODER_AV1_TIER_MAIN};
        if (std::string why = basicSupport(video, codec(), profile(), level(), layoutMode()); !why.empty())
            return why;

        // Coding tools: the ones the encoder requires (and order hints when a required tool needs them).
        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT cs{};
        cs.Codec = codec();
        cs.Profile = profile();
        cs.CodecSupportLimits.DataSize = sizeof(m_support);
        cs.CodecSupportLimits.pAV1Support = &m_support;
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &cs,
                                              sizeof(cs))) ||
            !cs.IsSupported)
            return "the AV1 encoder reports no coding tools";
        m_config = {};
        m_config.FeatureFlags = m_support.RequiredFeatureFlags;
        if ((m_config.FeatureFlags & (D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_JNT_COMP |
                                      D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FRAME_REFERENCE_MOTION_VECTORS)) &&
            (m_support.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS))
            m_config.FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS;
        m_config.OrderHintBitsMinus1 = 7;
        // A fixed eight-tap filter where offered; transform size selection where offered.
        bool filter = false;
        for (const auto f : {D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP,
                             D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_SWITCHABLE,
                             D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP_SMOOTH,
                             D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP_SHARP,
                             D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_BILINEAR})
            if (!filter && (static_cast<UINT>(m_support.SupportedInterpolationFilters) & (1u << f))) {
                m_filter = f;
                filter = true;
            }
        if (!filter) return "the AV1 encoder reports no interpolation filter";
        for (std::size_t t = 0; t < m_txMode.size(); ++t) { // key frames, inter frames
            bool found = false;
            for (const auto mode :
                 {D3D12_VIDEO_ENCODER_AV1_TX_MODE_SELECT, D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST,
                  D3D12_VIDEO_ENCODER_AV1_TX_MODE_ONLY4x4})
                if (!found && (static_cast<UINT>(m_support.SupportedTxModes[t]) & (1u << mode))) {
                    m_txMode[t] = mode;
                    found = true;
                }
            if (!found) return "the AV1 encoder reports no transform mode";
        }

        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pc{};
        D3D12_VIDEO_ENCODER_CODEC_AV1_PICTURE_CONTROL_SUPPORT pictures{};
        pictures.PredictionMode = D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE;
        pc.Codec = codec();
        pc.Profile = profile();
        pc.PictureSupport.DataSize = sizeof(pictures);
        pc.PictureSupport.pAV1Support = &pictures;
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pc,
                                              sizeof(pc))) ||
            !pc.IsSupported)
            return "the encoder reports no picture control support";
        if (!(pictures.SupportedFrameTypes & D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_KEY_FRAME) ||
            !(pictures.SupportedFrameTypes & D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_INTER_FRAME) ||
            pictures.MaxUniqueReferencesPerFrame < 1)
            return "the AV1 encoder cannot predict inter frames from a reference";

        if (std::string why = pickTiles(video); !why.empty()) return why;
        m_gop = {video::kGop, 1};

        // The whole configuration (AV1 needs the SUPPORT1 query, which also checks the tile grid).
        const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP q{quantizer(), quantizer(), quantizer()};
        D3D12_VIDEO_ENCODER_AV1_PROFILE suggestedProfile{};
        D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS suggestedLevel{};
        D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 s{};
        s.Codec = codec();
        s.InputFormat = DXGI_FORMAT_NV12;
        s.CodecConfiguration = configuration();
        s.CodecGopSequence = gop();
        s.RateControl = cqp(q);
        s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
        s.SubregionFrameEncoding = layoutMode();
        s.ResolutionsListCount = 1;
        s.pResolutionList = &kResolution;
        s.MaxReferenceFramesInDPB = 1;
        s.SuggestedProfile = desc(&suggestedProfile);
        s.SuggestedLevel = desc(&suggestedLevel);
        s.pResolutionDependentSupport = &m_limits;
        s.SubregionFrameEncodingData = layoutData();
        if (FAILED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof(s))))
            return "the encoder support query failed";
        if (!(s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK))
            return "the encoder does not support this " + validationText(s.ValidationFlags);
        if (suggestedLevel.Level >= D3D12_VIDEO_ENCODER_AV1_LEVELS_4_0 &&
            suggestedLevel.Level <= maxLevel.Level)
            m_level = suggestedLevel;
        return {};
    }
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile() override { return desc(&m_profile); }
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level() override { return desc(&m_level); }
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration() override { return desc(&m_config); }
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop() override { return desc(&m_gop); }

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picture(std::uint32_t f, std::uint32_t r) override {
        D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA& p = m_pictures[(r & 1) * video::kGop + f];
        p = pictureFor(f);
        return desc(&p);
    }

    std::vector<std::uint8_t> headers() const override { return {}; }

    std::string frameData(std::uint32_t f, std::span<const std::uint8_t> bitstream,
                          std::span<const std::uint8_t> resolved,
                          std::vector<std::uint8_t>& out) const override {
        D3D12_VIDEO_ENCODER_OUTPUT_METADATA m;
        std::memcpy(&m, resolved.data(), sizeof(m));
        const UINT64 n = m.WrittenSubregionsCount;
        const UINT64 tilesAt = sizeof(m) + n * sizeof(Subregion), postAt = tilesAt + sizeof(Tiles);
        if (n == 0 || postAt + sizeof(Post) > resolved.size())
            return std::format("the encoder reported {} tiles for frame {}", n, f);
        Tiles layout;
        Post post;
        std::memcpy(&layout, resolved.data() + tilesAt, sizeof(layout));
        std::memcpy(&post, resolved.data() + postAt, sizeof(post));
        if (layout.RowCount != m_tiles.RowCount || layout.ColCount != m_tiles.ColCount ||
            n != layout.RowCount * layout.ColCount)
            return std::format("frame {} came back as {} tiles in {}x{}, not the {}x{} grid asked for", f, n,
                               layout.ColCount, layout.RowCount, m_tiles.ColCount, m_tiles.RowCount);
        // Tile i starts after the earlier tiles' bSize, plus its own start offset.
        std::vector<std::span<const std::uint8_t>> tiles;
        UINT64 offset = 0;
        for (UINT64 i = 0; i < n; ++i) {
            Subregion t;
            std::memcpy(&t, resolved.data() + sizeof(m) + i * sizeof(t), sizeof(t));
            if (t.bStartOffset >= t.bSize || offset + t.bSize > bitstream.size())
                return std::format("frame {}'s tile sizes do not fit the bitstream buffer", f);
            tiles.emplace_back(bitstream.data() + offset + t.bStartOffset,
                               static_cast<std::size_t>(t.bSize - t.bStartOffset));
            offset += t.bSize;
        }
        try {
            const av1::SequenceHeader seq = sequence();
            const std::vector<std::uint8_t> td = av1::temporalDelimiter();
            out.insert(out.end(), td.begin(), td.end());
            if (f == 0) {
                const std::vector<std::uint8_t> sh = av1::writeSequenceHeader(seq);
                out.insert(out.end(), sh.begin(), sh.end());
            }
            const std::vector<std::uint8_t> frame = av1::writeFrame(seq, frameHeader(f, layout, post), tiles);
            out.insert(out.end(), frame.begin(), frame.end());
        } catch (const std::exception& e) {
            return std::format("frame {}'s headers could not be written: {}", f, e.what());
        }
        return {};
    }

    std::optional<DxvaStream> readBack(std::span<const std::uint8_t> stream,
                                       std::string* error) const override {
        return dxvaAv1(stream, error);
    }
    nlohmann::json describe() const override {
        nlohmann::json features = nlohmann::json::array(), postEncode = nlohmann::json::array();
        for (const Av1FeatureName& f : kAv1Features)
            if (m_config.FeatureFlags & f.flag) features.push_back(f.name);
        struct PostName {
            D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAGS flag;
            const char* name;
        };
        static constexpr PostName kPost[] = {
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION, "quantization"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION_DELTA, "quantizer deltas"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER, "loop filter"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER_DELTA, "loop filter deltas"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_CDEF_DATA, "CDEF"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_CONTEXT_UPDATE_TILE_ID, "context update tile"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_COMPOUND_PREDICTION_MODE, "compound prediction"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_PRIMARY_REF_FRAME, "primary reference"},
            {D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_REFERENCE_INDICES, "reference indices"},
        };
        for (const PostName& p : kPost)
            if (m_support.PostEncodeValuesFlags & p.flag) postEncode.push_back(p.name);
        static constexpr const char* kFilters[] = {"eight-tap", "eight-tap smooth", "eight-tap sharp",
                                                   "bilinear", "switchable"};
        static constexpr const char* kTx[] = {"4x4 only", "largest", "select"};
        const auto level = static_cast<std::uint32_t>(m_level.Level);
        return {
            {"profile", name()},
            {"level", std::format("{}.{}", 2 + level / 4, level % 4)},
            {"tier", m_level.Tier == D3D12_VIDEO_ENCODER_AV1_TIER_HIGH ? "high" : "main"},
            {"tiles", std::format("{}x{}", m_tiles.ColCount, m_tiles.RowCount)},
            {"coding_tools", features},
            {"interpolation_filter", kFilters[static_cast<std::size_t>(m_filter) % std::size(kFilters)]},
            {"tx_mode", std::format("{} / {}", kTx[static_cast<std::size_t>(m_txMode[0]) % std::size(kTx)],
                                    kTx[static_cast<std::size_t>(m_txMode[1]) % std::size(kTx)])},
            {"base_q_idx", kAv1QIndex},
            {"post_encode_values", postEncode}};
    }

private:
    bool has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS f) const { return (m_config.FeatureFlags & f) != 0; }

    /// One tile where the encoder takes it, else its smallest uniform grid.
    std::string pickTiles(ID3D12VideoDevice3* video) {
        D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_SUPPORT t{};
        auto ask = [&](UINT64 rows, UINT64 cols) {
            t = {};
            t.Use128SuperBlocks = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_128x128_SUPERBLOCK);
            t.TilesConfiguration.RowCount = rows;
            t.TilesConfiguration.ColCount = cols;
            D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG q{};
            q.Codec = codec();
            q.Profile = profile();
            q.Level = level();
            q.SubregionMode = layoutMode();
            q.FrameResolution = kResolution;
            q.CodecSupport.DataSize = sizeof(t);
            q.CodecSupport.pAV1Support = &t;
            return SUCCEEDED(video->CheckFeatureSupport(
                       D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG, &q, sizeof(q))) &&
                   q.IsSupported;
        };
        bool ok = ask(1, 1);
        if (!ok && std::max(t.MinTileRows, t.MinTileCols) > 1) {
            const UINT64 rows = UINT64(1) << log2Ceil(std::max(t.MinTileRows, 1u));
            const UINT64 cols = UINT64(1) << log2Ceil(std::max(t.MinTileCols, 1u));
            ok = rows <= 64 && cols <= 64 && ask(rows, cols);
        }
        if (!ok) return "the AV1 encoder takes neither one tile nor its smallest uniform tile grid";
        m_tiles = t.TilesConfiguration;
        m_tileSizeBytesMinus1 = t.TileSizeBytesMinus1;
        return {};
    }

    /// Frame f: a key frame, or an inter frame whose every reference is frame f - 1 (the one reference
    /// texture). Every frame replaces all eight reference slots, so the slots never hold a picture that is
    /// gone.
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA pictureFor(std::uint32_t f) const {
        D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA p{};
        p.Flags = D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_NONE;
        p.FrameType = f == 0 ? D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME
                             : D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME;
        p.CompoundPredictionType = D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE;
        p.InterpolationFilter = m_filter;
        p.TxMode = m_txMode[f == 0 ? 0 : 1];
        p.SuperResDenominator = 8; // no super-resolution
        p.OrderHint = f;
        p.PictureIndex = f;
        for (D3D12_VIDEO_ENCODER_AV1_REFERENCE_PICTURE_DESCRIPTOR& d :
             p.ReferenceFramesReconPictureDescriptors) {
            d.ReconstructedPictureResourceIndex = f == 0 ? kInvalidDpbIndex : 0;
            if (f == 0) continue;
            d.FrameType = f == 1 ? D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME
                                 : D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME;
            d.WarpedMotionInfo.TransformationType =
                D3D12_VIDEO_ENCODER_AV1_REFERENCE_WARPED_MOTION_TRANSFORMATION_IDENTITY;
            d.OrderHint = f - 1;
            d.PictureIndex = f - 1;
        }
        p.PrimaryRefFrame = av1::kPrimaryRefNone;
        p.RefreshFrameFlags = 0xFF;
        p.LoopFilter.LoopFilterLevel[0] = kAv1LoopFilter[0];
        p.LoopFilter.LoopFilterLevel[1] = kAv1LoopFilter[1];
        p.LoopFilter.LoopFilterLevelU = kAv1LoopFilter[2];
        p.LoopFilter.LoopFilterLevelV = kAv1LoopFilter[3];
        static constexpr std::array<INT64, 8> kDefaultRefDeltas = {1, 0, 0, 0, -1, 0, -1, -1};
        for (std::size_t i = 0; i < kDefaultRefDeltas.size(); ++i)
            p.LoopFilter.RefDeltas[i] = kDefaultRefDeltas[i];
        p.Quantization.BaseQIndex = kAv1QIndex;
        return p;
    }

    av1::SequenceHeader sequence() const {
        av1::SequenceHeader s;
        s.profile = 0;
        s.levelIdx = static_cast<std::uint32_t>(m_level.Level);
        s.tier = m_level.Tier == D3D12_VIDEO_ENCODER_AV1_TIER_HIGH ? 1 : 0;
        s.operatingPointIdc = {0};
        s.opLevelIdx = {s.levelIdx};
        s.opTier = {s.tier};
        s.frameWidthBitsMinus1 = bitLength(video::kWidth - 1) - 1;
        s.frameHeightBitsMinus1 = bitLength(video::kHeight - 1) - 1;
        s.maxFrameWidthMinus1 = video::kWidth - 1;
        s.maxFrameHeightMinus1 = video::kHeight - 1;
        s.use128x128Superblock = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_128x128_SUPERBLOCK);
        s.enableFilterIntra = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FILTER_INTRA);
        s.enableIntraEdgeFilter = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_EDGE_FILTER);
        s.enableInterintraCompound = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTERINTRA_COMPOUND);
        s.enableMaskedCompound = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_MASKED_COMPOUND);
        s.enableWarpedMotion = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_WARPED_MOTION);
        s.enableDualFilter = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_DUAL_FILTER);
        s.enableOrderHint = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS);
        s.enableJntComp = s.enableOrderHint && has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_JNT_COMP);
        s.enableRefFrameMvs =
            s.enableOrderHint && has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FRAME_REFERENCE_MOTION_VECTORS);
        const bool screenContent = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING) ||
                                   has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_BLOCK_COPY);
        s.seqForceScreenContentTools = screenContent ? 2 : 0; // per frame, or never
        s.seqForceIntegerMv =
            !screenContent || has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_FORCED_INTEGER_MOTION_VECTORS) ? 2 : 0;
        s.orderHintBits = s.enableOrderHint ? m_config.OrderHintBitsMinus1 + 1 : 0;
        s.enableSuperres = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_SUPER_RESOLUTION);
        s.enableCdef = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING);
        s.enableRestoration = has(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_RESTORATION_FILTER);
        s.bitDepth = 8;
        s.separateUvDeltaQ = true; // the specification codes it as 1
        return s;
    }

    /// Frame f's header: what was asked for, with the values the encoder reports after encoding where it may
    /// choose them itself (PostEncodeValuesFlags).
    av1::FrameHeader frameHeader(std::uint32_t f, const Tiles& layout, const Post& post) const {
        const D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA p = pictureFor(f);
        const av1::SequenceHeader s = sequence();
        const auto chosen = [&](D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAGS flag) {
            return (m_support.PostEncodeValuesFlags & flag) != 0;
        };
        const auto flag = [&](D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAGS fl) {
            return (p.Flags & fl) != 0;
        };
        av1::FrameHeader h;
        h.frameType = f == 0 ? av1::kKeyFrame : av1::kInterFrame;
        h.showFrame = true;
        h.showableFrame = f != 0;
        h.errorResilientMode = f == 0;
        h.disableCdfUpdate = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_DISABLE_CDF_UPDATE);
        h.forceIntegerMv = f == 0;
        const std::uint32_t hintMask = s.enableOrderHint ? (1u << s.orderHintBits) - 1 : 0;
        h.orderHint = p.OrderHint & hintMask;
        h.primaryRefFrame = static_cast<std::uint32_t>(
            chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_PRIMARY_REF_FRAME) ? post.PrimaryRefFrame
                                                                                      : p.PrimaryRefFrame);
        h.refreshFrameFlags = p.RefreshFrameFlags;
        h.upscaledWidth = h.frameWidth = h.renderWidth = video::kWidth;
        h.frameHeight = h.renderHeight = video::kHeight;
        h.miCols = 2 * ((h.frameWidth + 7) >> 3);
        h.miRows = 2 * ((h.frameHeight + 7) >> 3);
        for (std::uint32_t i = 0; i < 7; ++i) {
            h.refFrameIdx[i] = static_cast<std::uint32_t>(
                chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_REFERENCE_INDICES)
                    ? post.ReferenceIndices[i]
                    : p.ReferenceIndices[i]);
            h.orderHints[1 + i] = f > 0 ? (f - 1) & hintMask : 0;
        }
        h.orderHints[0] = h.orderHint;
        h.allowHighPrecisionMv = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV);
        h.isFilterSwitchable =
            p.InterpolationFilter == D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_SWITCHABLE;
        h.interpolationFilter = static_cast<std::uint32_t>(p.InterpolationFilter);
        h.isMotionModeSwitchable = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_MOTION_MODE_SWITCHABLE);
        h.useRefFrameMvs = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_FRAME_REFERENCE_MOTION_VECTORS);
        h.disableFrameEndUpdateCdf =
            flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_DISABLE_FRAME_END_UPDATE_CDF);
        h.tiles.uniformSpacing = true;
        h.tiles.colsLog2 = log2Ceil(layout.ColCount);
        h.tiles.rowsLog2 = log2Ceil(layout.RowCount);
        h.tiles.cols = static_cast<std::uint32_t>(layout.ColCount);
        h.tiles.rows = static_cast<std::uint32_t>(layout.RowCount);
        h.tiles.contextUpdateTileId = static_cast<std::uint32_t>(layout.ContextUpdateTileId);
        h.tiles.tileSizeBytesMinus1 = m_tileSizeBytesMinus1;

        const auto& q = chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION)
                            ? post.Quantization
                            : p.Quantization;
        h.baseQIdx = static_cast<std::uint32_t>(q.BaseQIndex);
        h.deltaQYDc = static_cast<std::int32_t>(q.YDCDeltaQ);
        h.deltaQUDc = static_cast<std::int32_t>(q.UDCDeltaQ);
        h.deltaQUAc = static_cast<std::int32_t>(q.UACDeltaQ);
        h.deltaQVDc = static_cast<std::int32_t>(q.VDCDeltaQ);
        h.deltaQVAc = static_cast<std::int32_t>(q.VACDeltaQ);
        h.diffUvDelta = h.deltaQUDc != h.deltaQVDc || h.deltaQUAc != h.deltaQVAc;
        h.usingQmatrix = q.UsingQMatrix != 0;
        h.qmY = static_cast<std::uint32_t>(q.QMY);
        h.qmU = static_cast<std::uint32_t>(q.QMU);
        h.qmV = static_cast<std::uint32_t>(q.QMV);
        const auto& qd = chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION_DELTA)
                             ? post.QuantizationDelta
                             : p.QuantizationDelta;
        h.deltaQPresent = qd.DeltaQPresent != 0;
        h.deltaQRes = static_cast<std::uint32_t>(qd.DeltaQRes);
        const auto& lfd = chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER_DELTA)
                              ? post.LoopFilterDelta
                              : p.LoopFilterDelta;
        h.deltaLfPresent = lfd.DeltaLFPresent != 0;
        h.deltaLfMulti = lfd.DeltaLFMulti != 0;
        h.deltaLfRes = static_cast<std::uint32_t>(lfd.DeltaLFRes);

        // Segmentation stays off (neither automatic nor custom segmentation is asked for).
        const auto& lf = chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER) ? post.LoopFilter
                                                                                             : p.LoopFilter;
        h.loopFilterLevel = {static_cast<std::uint32_t>(lf.LoopFilterLevel[0]),
                             static_cast<std::uint32_t>(lf.LoopFilterLevel[1]),
                             static_cast<std::uint32_t>(lf.LoopFilterLevelU),
                             static_cast<std::uint32_t>(lf.LoopFilterLevelV)};
        h.loopFilterSharpness = static_cast<std::uint32_t>(lf.LoopFilterSharpnessLevel);
        h.loopFilterDeltaEnabled = lf.LoopFilterDeltaEnabled != 0;
        h.updateRefDelta = static_cast<std::uint32_t>(lf.UpdateRefDelta & 0xFF);
        h.updateModeDelta = static_cast<std::uint32_t>(lf.UpdateModeDelta & 0x3);
        h.loopFilterDeltaUpdate = h.updateRefDelta || h.updateModeDelta;
        for (std::size_t i = 0; i < 8; ++i)
            h.loopFilterRefDeltas[i] = static_cast<std::int8_t>(lf.RefDeltas[i]);
        for (std::size_t i = 0; i < 2; ++i)
            h.loopFilterModeDeltas[i] = static_cast<std::int8_t>(lf.ModeDeltas[i]);
        const auto& cdef =
            chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_CDEF_DATA) ? post.CDEF : p.CDEF;
        h.cdefDampingMinus3 = static_cast<std::uint32_t>(cdef.CdefDampingMinus3);
        h.cdefBits = static_cast<std::uint32_t>(cdef.CdefBits);
        for (std::size_t i = 0; i < 8; ++i) {
            h.cdefYPri[i] = static_cast<std::uint8_t>(cdef.CdefYPriStrength[i]);
            h.cdefYSec[i] = static_cast<std::uint8_t>(cdef.CdefYSecStrength[i]);
            h.cdefUvPri[i] = static_cast<std::uint8_t>(cdef.CdefUVPriStrength[i]);
            h.cdefUvSec[i] = static_cast<std::uint8_t>(cdef.CdefUVSecStrength[i]);
        }
        // No loop restoration is asked for: every plane's type stays RESTORE_NONE.
        h.txMode = static_cast<std::uint32_t>(p.TxMode);
        h.referenceSelect =
            (chosen(D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_COMPOUND_PREDICTION_MODE)
                 ? post.CompoundPredictionType
                 : static_cast<UINT64>(p.CompoundPredictionType)) ==
            static_cast<UINT64>(D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_COMPOUND_REFERENCE);
        h.skipModePresent = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_SKIP_MODE);
        h.allowWarpedMotion = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_WARPED_MOTION);
        h.reducedTxSet = flag(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_REDUCED_TX_SET);
        return h;
    }

    D3D12_VIDEO_ENCODER_AV1_PROFILE m_profile = D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN;
    D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS m_level{D3D12_VIDEO_ENCODER_AV1_LEVELS_4_1,
                                                           D3D12_VIDEO_ENCODER_AV1_TIER_MAIN};
    D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT m_support{};
    D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION m_config{};
    D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE m_gop{};
    D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS m_filter =
        D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP;
    std::array<D3D12_VIDEO_ENCODER_AV1_TX_MODE, 2> m_txMode{D3D12_VIDEO_ENCODER_AV1_TX_MODE_SELECT,
                                                            D3D12_VIDEO_ENCODER_AV1_TX_MODE_SELECT};
    Tiles m_tiles{};
    std::uint32_t m_tileSizeBytesMinus1 = 3;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS m_limits{};
    std::array<D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA, 2 * video::kGop> m_pictures{};
};

// --- encoder -------------------------------------------------------------------------------------------

class Encoder {
public:
    Encoder(ID3D12Device* dev, EncodeCodec& c) : m_dev(dev), m_c(c), m_sync(dev) {}
    ~Encoder() { m_sync.drain(m_dev); }
    /// Waits for the GPU work; false if the GPU never finished it.
    bool settle() noexcept {
        m_sync.drain(m_dev);
        return !m_sync.timedOut();
    }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    /// Settles the configuration and creates the encoder and its resources; returns why the codec cannot be
    /// encoded (empty: it can).
    std::string prepare() {
        m_dev->QueryInterface(__uuidof(ID3D12VideoDevice3), m_video.putVoid());
        if (!m_video) return "the driver has no Direct3D 12 video encoding (ID3D12VideoDevice3)";
        D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT area{};
        if (FAILED(m_video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT, &area,
                                                sizeof(area))) ||
            !area.VideoEncodeSupport)
            return "the GPU has no Direct3D 12 video encoder";
        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC cs{};
        cs.Codec = m_c.codec();
        if (FAILED(m_video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &cs, sizeof(cs))) ||
            !cs.IsSupported)
            return "the video encoder does not support the codec";
        if (std::string why = m_c.configure(m_video.get()); !why.empty()) return why;
        m_resolvedStride = alignUp(std::max<UINT64>(m_c.resolvedSize(), 4096), 256);

        D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS req{};
        req.Codec = m_c.codec();
        req.Profile = m_c.profile();
        req.InputFormat = DXGI_FORMAT_NV12;
        req.PictureTargetResolution = kResolution;
        if (FAILED(m_video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &req,
                                                sizeof(req))) ||
            !req.IsSupported)
            return "the encoder reports no resource requirements for 1080p";
        m_frameStride =
            alignUp(kFrameStride, std::max<UINT>(req.CompressedBitstreamBufferAccessAlignment, 1));
        m_metaStride = alignUp(std::max<UINT>(req.MaxEncoderOutputMetadataBufferSize, 1),
                               std::max<UINT>(req.EncoderMetadataBufferAccessAlignment, 256));

        D3D12_VIDEO_ENCODER_DESC ed{};
        ed.Flags = D3D12_VIDEO_ENCODER_FLAG_NONE;
        ed.EncodeCodec = m_c.codec();
        ed.EncodeProfile = m_c.profile();
        ed.InputFormat = DXGI_FORMAT_NV12;
        ed.CodecConfiguration = m_c.configuration();
        ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
        if (HRESULT hr = m_video->CreateVideoEncoder(&ed, __uuidof(ID3D12VideoEncoder), m_encoder.putVoid());
            FAILED(hr))
            return "CreateVideoEncoder failed: " + hresultName(hr);
        D3D12_VIDEO_ENCODER_HEAP_DESC hd{};
        hd.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
        hd.EncodeCodec = m_c.codec();
        hd.EncodeProfile = m_c.profile();
        hd.EncodeLevel = m_c.level();
        hd.ResolutionsListCount = 1;
        hd.pResolutionList = &kResolution;
        if (HRESULT hr =
                m_video->CreateVideoEncoderHeap(&hd, __uuidof(ID3D12VideoEncoderHeap), m_heap.putVoid());
            FAILED(hr))
            return "CreateVideoEncoderHeap failed: " + hresultName(hr);

        // Source frames (one slice each), two reconstructed pictures used in turn, and the output buffers.
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = video::kWidth;
        tex.Height = video::kHeight;
        tex.DepthOrArraySize = static_cast<UINT16>(video::kGop);
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_NV12;
        tex.SampleDesc.Count = 1;
        m_inputDesc = tex;
        check(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, tex, D3D12_RESOURCE_STATE_COMMON, m_input),
              "encoder input");
        tex.DepthOrArraySize = 2;
        tex.Flags =
            D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        if (HRESULT hr = committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, tex, D3D12_RESOURCE_STATE_COMMON, m_recon);
            FAILED(hr))
            return "the reconstructed pictures could not be created: " + hresultName(hr);
        check(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, bufferDesc(m_frameStride * video::kGop),
                        D3D12_RESOURCE_STATE_COMMON, m_bitstream),
              "bitstream buffer");
        check(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, bufferDesc(m_metaStride * video::kGop),
                        D3D12_RESOURCE_STATE_COMMON, m_metadata),
              "encoder metadata buffer");
        check(committed(m_dev, D3D12_HEAP_TYPE_DEFAULT, bufferDesc(m_resolvedStride * video::kGop),
                        D3D12_RESOURCE_STATE_COMMON, m_resolved),
              "resolved metadata buffer");

        if (HRESULT hr = makeQueue(m_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, m_encode); FAILED(hr))
            return "no video encode queue: " + hresultName(hr);
        check(makeQueue(m_dev, D3D12_COMMAND_LIST_TYPE_COPY, m_copy), "copy queue");
        // One command list per frame (the timed submissions pass kRepeats GOPs to one ExecuteCommandLists).
        m_cqp = {m_c.quantizer(), m_c.quantizer(), m_c.quantizer()};
        for (std::uint32_t r = 0; r < kRepeats; ++r)
            for (std::uint32_t f = 0; f < video::kGop; ++f)
                m_lists.push_back(recordFrame(f, r));
        m_resolve = recordResolve();
        return {};
    }

    /// Copies the source frames (NV12 at 1920x1088) into the input texture array.
    void upload(const std::vector<std::uint8_t>& source) {
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(video::kGop * kPlanes);
        UINT64 total = 0;
        for (std::uint32_t i = 0; i < fp.size(); ++i) {
            UINT64 size = 0;
            m_dev->GetCopyableFootprints(&m_inputDesc, i, 1, total, &fp[i], nullptr, nullptr, &size);
            total = alignUp(total + size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        }
        Com<ID3D12Resource> up;
        check(committed(m_dev, D3D12_HEAP_TYPE_UPLOAD, bufferDesc(total), D3D12_RESOURCE_STATE_GENERIC_READ,
                        up),
              "upload buffer");
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        check(up->Map(0, &none, &mapped), "Map (upload)");
        auto* base = static_cast<std::uint8_t*>(mapped);
        for (std::uint32_t f = 0; f < video::kGop; ++f) {
            const std::uint8_t* src = source.data() + f * video::kSourceFrame;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& y = fp[f];
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& uv = fp[video::kGop + f];
            for (std::uint32_t r = 0; r < video::kHeight; ++r)
                std::memcpy(base + y.Offset + std::size_t(r) * y.Footprint.RowPitch,
                            src + std::size_t(r) * video::kImageWidth, video::kWidth);
            for (std::uint32_t r = 0; r < video::kHeight / 2; ++r)
                std::memcpy(base + uv.Offset + std::size_t(r) * uv.Footprint.RowPitch,
                            src + video::kSourceLuma + std::size_t(r) * video::kImageWidth, video::kWidth);
        }
        up->Unmap(0, nullptr);
        Com<ID3D12GraphicsCommandList> cl;
        check(m_copy.allocator->Reset(), "ID3D12CommandAllocator::Reset");
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copy.allocator.get(), nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), cl.putVoid()),
              "CreateCommandList (copy)");
        for (std::uint32_t i = 0; i < fp.size(); ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = up.get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[i];
            dst.pResource = m_input.get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        check(cl->Close(), "Close (copy)");
        execute(m_copy.queue.get(), cl.get());
        m_sync.signal(m_copy.queue.get());
        m_sync.wait(m_dev); // the texture decays to COMMON
    }

    /// Encodes the GOP once and reads the frames back; returns a failure message (empty on success).
    std::string encodeOnce() {
        m_sync.gpuWait(m_encode.queue.get());
        for (std::uint32_t f = 0; f <= video::kGop; ++f) { // the frames, then the metadata resolves
            execute(m_encode.queue.get(), f < video::kGop ? m_lists[f].get() : m_resolve.get());
            m_sync.signal(m_encode.queue.get());
            try {
                m_sync.wait(m_dev);
            } catch (const std::exception& e) {
                throw std::runtime_error(f < video::kGop
                                             ? std::format("encoding frame {}: {}", f, e.what())
                                             : std::format("resolving the metadata: {}", e.what()));
            }
        }
        return readOutput();
    }

    double time(TestContext& ctx, double seconds) {
        std::vector<ID3D12CommandList*> lists;
        for (const auto& l : m_lists)
            lists.push_back(l.get());
        auto submit = [&] {
            m_encode.queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
            m_sync.signal(m_encode.queue.get());
            m_sync.wait(m_dev);
        };
        submit(); // warm-up
        std::uint64_t frames = 0;
        const auto t0 = std::chrono::steady_clock::now();
        double elapsed = 0.0;
        do {
            ctx.checkStop();
            submit();
            frames += std::uint64_t(video::kGop) * kRepeats;
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        } while (elapsed < seconds);
        return double(frames) / elapsed;
    }

    const std::vector<std::vector<std::uint8_t>>& output() const { return m_output; }

private:
    Com<ID3D12VideoEncodeCommandList2> newList() {
        Com<ID3D12VideoEncodeCommandList2> cl;
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, m_encode.allocator.get(),
                                       nullptr, __uuidof(ID3D12VideoEncodeCommandList2), cl.putVoid()),
              "CreateCommandList (video encode)");
        return cl;
    }

    /// Records frame f of GOP repeat r: its input slice is read, its reconstructed picture written, the
    /// previous frame's read; everything returns to COMMON afterwards.
    Com<ID3D12VideoEncodeCommandList2> recordFrame(std::uint32_t f, std::uint32_t r) {
        Com<ID3D12VideoEncodeCommandList2> cl = newList();
        constexpr UINT kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        const UINT slot = f & 1, refSlot = (f + 1) & 1;
        std::vector<D3D12_RESOURCE_BARRIER> before = {
            transition(m_input.get(), kAll, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
            transition(m_bitstream.get(), kAll, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
            transition(m_metadata.get(), kAll, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        };
        for (std::uint32_t p = 0; p < kPlanes; ++p) {
            before.push_back(transition(m_recon.get(), slot + p * 2, D3D12_RESOURCE_STATE_COMMON,
                                        D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE));
            if (f > 0)
                before.push_back(transition(m_recon.get(), refSlot + p * 2, D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ));
        }
        cl->ResourceBarrier(static_cast<UINT>(before.size()), before.data());

        D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in{};
        D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC& seq = in.SequenceControlDesc;
        seq.Flags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
        seq.IntraRefreshConfig = {D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE, 0};
        seq.RateControl = cqp(m_cqp);
        seq.PictureTargetResolution = kResolution;
        seq.SelectedLayoutMode = m_c.layoutMode();
        seq.FrameSubregionsLayoutData = m_c.layoutData();
        seq.CodecGopSequence = m_c.gop();
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC& pic = in.PictureControlDesc;
        pic.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
        pic.PictureControlCodecData = m_c.picture(f, r);
        if (f > 0) {
            pic.ReferenceFrames.NumTexture2Ds = 1;
            m_refTexture = m_recon.get();
            pic.ReferenceFrames.ppTexture2Ds = &m_refTexture;
            pic.ReferenceFrames.pSubresources = &m_refSubresources[refSlot];
        }
        in.pInputFrame = m_input.get();
        in.InputFrameSubresource = f;
        in.CurrentFrameBitstreamMetadataSize = 0;
        D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out{};
        out.Bitstream = {m_bitstream.get(), f * m_frameStride};
        out.ReconstructedPicture = {m_recon.get(), slot};
        out.EncoderOutputMetadata = {m_metadata.get(), f * m_metaStride};
        cl->EncodeFrame(m_encoder.get(), m_heap.get(), &in, &out);

        std::vector<D3D12_RESOURCE_BARRIER> after = before;
        for (D3D12_RESOURCE_BARRIER& b : after)
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        check(cl->Close(), "Close (video encode)");
        return cl;
    }

    /// Records the resolves of every frame's metadata.
    Com<ID3D12VideoEncodeCommandList2> recordResolve() {
        Com<ID3D12VideoEncodeCommandList2> cl = newList();
        constexpr UINT kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        const D3D12_RESOURCE_BARRIER toResolve[] = {
            transition(m_metadata.get(), kAll, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
            transition(m_resolved.get(), kAll, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        };
        cl->ResourceBarrier(static_cast<UINT>(std::size(toResolve)), toResolve);
        for (std::uint32_t f = 0; f < video::kGop; ++f) {
            D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS in{};
            in.EncoderCodec = m_c.codec();
            in.EncoderProfile = m_c.profile();
            in.EncoderInputFormat = DXGI_FORMAT_NV12;
            in.EncodedPictureEffectiveResolution = kResolution;
            in.HWLayoutMetadata = {m_metadata.get(), f * m_metaStride};
            D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS out{};
            out.ResolvedLayoutMetadata = {m_resolved.get(), f * m_resolvedStride};
            cl->ResolveEncoderOutputMetadata(&in, &out);
        }
        const D3D12_RESOURCE_BARRIER done[] = {
            transition(m_metadata.get(), kAll, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ,
                       D3D12_RESOURCE_STATE_COMMON),
            transition(m_resolved.get(), kAll, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                       D3D12_RESOURCE_STATE_COMMON),
        };
        cl->ResourceBarrier(static_cast<UINT>(std::size(done)), done);
        check(cl->Close(), "Close (video encode)");
        return cl;
    }

    /// Copies the bitstream and the resolved metadata out and has the codec turn each frame into its part of
    /// the stream.
    std::string readOutput() {
        const UINT64 bitstreamBytes = m_frameStride * video::kGop, metaBytes = m_resolvedStride * video::kGop;
        Com<ID3D12Resource> rb;
        check(committed(m_dev, D3D12_HEAP_TYPE_READBACK, bufferDesc(bitstreamBytes + metaBytes),
                        D3D12_RESOURCE_STATE_COPY_DEST, rb),
              "readback buffer");
        Com<ID3D12GraphicsCommandList> cl;
        check(m_copy.allocator->Reset(), "ID3D12CommandAllocator::Reset");
        check(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copy.allocator.get(), nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), cl.putVoid()),
              "CreateCommandList (copy)");
        cl->CopyBufferRegion(rb.get(), 0, m_bitstream.get(), 0, bitstreamBytes);
        cl->CopyBufferRegion(rb.get(), bitstreamBytes, m_resolved.get(), 0, metaBytes);
        check(cl->Close(), "Close (copy)");
        m_sync.gpuWait(m_copy.queue.get());
        execute(m_copy.queue.get(), cl.get());
        m_sync.signal(m_copy.queue.get());
        m_sync.wait(m_dev);

        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(bitstreamBytes + metaBytes)};
        check(rb->Map(0, &range, &mapped), "Map (readback)");
        const auto* base = static_cast<const std::uint8_t*>(mapped);
        std::string failure;
        m_output.assign(video::kGop, {});
        for (std::uint32_t f = 0; f < video::kGop && failure.empty(); ++f) {
            const std::span<const std::uint8_t> meta(base + bitstreamBytes + f * m_resolvedStride,
                                                     static_cast<std::size_t>(m_resolvedStride));
            D3D12_VIDEO_ENCODER_OUTPUT_METADATA m;
            std::memcpy(&m, meta.data(), sizeof(m));
            if (m.EncodeErrorFlags) {
                failure =
                    std::format("the encoder reported error flags {:#x} for frame {}", m.EncodeErrorFlags, f);
                break;
            }
            const std::span<const std::uint8_t> frame(base + f * m_frameStride,
                                                      static_cast<std::size_t>(m_frameStride));
            failure = m_c.frameData(f, frame, meta, m_output[f]);
            if (failure.empty() && m_output[f].empty())
                failure = std::format("the encoder wrote nothing for frame {}", f);
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        return failure;
    }

    ID3D12Device* m_dev;
    EncodeCodec& m_c;
    Sync m_sync;
    Com<ID3D12VideoDevice3> m_video;
    Com<ID3D12VideoEncoder> m_encoder;
    Com<ID3D12VideoEncoderHeap> m_heap;
    D3D12_RESOURCE_DESC m_inputDesc{};
    Com<ID3D12Resource> m_input, m_recon, m_bitstream, m_metadata, m_resolved;
    UINT64 m_frameStride = 0, m_metaStride = 0, m_resolvedStride = 4096;
    Queue m_encode, m_copy;
    std::vector<Com<ID3D12VideoEncodeCommandList2>> m_lists; ///< kRepeats GOPs, one list per frame
    Com<ID3D12VideoEncodeCommandList2> m_resolve;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP m_cqp{};
    ID3D12Resource* m_refTexture = nullptr;
    std::array<UINT, 2> m_refSubresources{0, 1};
    std::vector<std::vector<std::uint8_t>> m_output;
};

} // namespace

std::string EncodeCodec::frameData(std::uint32_t f, std::span<const std::uint8_t> bitstream,
                                   std::span<const std::uint8_t> resolved,
                                   std::vector<std::uint8_t>& out) const {
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA m;
    std::memcpy(&m, resolved.data(), sizeof(m));
    if (m.WrittenSubregionsCount == 0) {
        out.assign(bitstream.begin(),
                   bitstream.begin() + static_cast<std::ptrdiff_t>(std::min<UINT64>(
                                           m.EncodedBitstreamWrittenBytesCount, bitstream.size())));
        return {};
    }
    // Slices follow each other; each may start with padding to skip.
    UINT64 offset = 0;
    for (UINT64 i = 0; i < m.WrittenSubregionsCount; ++i) {
        D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA s;
        if (sizeof(m) + (i + 1) * sizeof(s) > resolved.size())
            return std::format("frame {} has more slices than its metadata holds", f);
        std::memcpy(&s, resolved.data() + sizeof(m) + i * sizeof(s), sizeof(s));
        if (offset + s.bSize > bitstream.size() || s.bStartOffset > s.bSize)
            return std::format("frame {}'s slice sizes do not fit the bitstream buffer", f);
        const std::uint8_t* slice = bitstream.data() + offset + s.bStartOffset;
        const bool startCode = s.bSize - s.bStartOffset >= 4 && slice[0] == 0 && slice[1] == 0 &&
                               (slice[2] == 1 || (slice[2] == 0 && slice[3] == 1));
        if (!startCode) out.insert(out.end(), {0, 0, 0, 1}); // the stream needs Annex B framing
        out.insert(out.end(), slice, slice + (s.bSize - s.bStartOffset));
        offset += s.bSize;
    }
    return {};
}

std::unique_ptr<EncodeCodec> h264Encoder() {
    return std::make_unique<H264Encode>();
}
std::unique_ptr<EncodeCodec> h265Encoder() {
    return std::make_unique<H265Encode>();
}
std::unique_ptr<EncodeCodec> av1Encoder() {
    return std::make_unique<Av1Encode>();
}

void encodeAndCheck(TestContext& ctx, ID3D12Device* dev, const CodecHooks& hooks,
                    const std::vector<std::uint8_t>& source, double seconds, const video::Progress& progress,
                    video::CodecResult& res) {
    std::unique_ptr<EncodeCodec> codec = hooks.encoder();
    const std::unique_ptr<Encoder, KeepIfHung> enc(new Encoder(dev, *codec));
    // If the GPU never finishes, the encoder is kept (KeepIfHung) and so is the codec its commands came from.
    struct KeepCodecIfHung {
        std::unique_ptr<EncodeCodec>& codec;
        Encoder& enc;
        ~KeepCodecIfHung() {
            if (!enc.settle()) (void)codec.release();
        }
    } keepCodec{codec, *enc};
    if (std::string why = enc->prepare(); !why.empty()) {
        res.encodeNote = why;
        return;
    }
    res.details["encode"] = codec->describe();
    ctx.log(std::format("{}: encoder ready ({})", hooks.label, codec->describe().dump()));
    progress(0.45, std::format("{}: encoding the source frames", hooks.label));
    enc->upload(source);
    if (std::string failure = enc->encodeOnce(); !failure.empty()) {
        res.failure = failure;
        return;
    }
    std::vector<std::uint8_t> stream = codec->headers();
    std::size_t total = 0;
    for (const auto& f : enc->output()) {
        stream.insert(stream.end(), f.begin(), f.end());
        total += f.size();
    }
    // --param feature.video.dump=DIR keeps the encoded stream, to examine with other decoders.
    if (const std::string dir = ctx.settings().param("feature.video.dump"); !dir.empty()) {
        static constexpr const char* kExt[] = {"h264", "h265", "obu"};
        const std::string ext = codec->codec() == D3D12_VIDEO_ENCODER_CODEC_H264   ? kExt[0]
                                : codec->codec() == D3D12_VIDEO_ENCODER_CODEC_HEVC ? kExt[1]
                                                                                   : kExt[2];
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path path =
            std::filesystem::path(dir) / std::format("d3d12_{}.{}", hooks.key, ext);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(stream.data()), static_cast<std::streamsize>(stream.size()));
        ctx.log(std::format("{}: wrote the encoded stream to {}", hooks.label, path.string()));
    }
    nlohmann::json sizes = nlohmann::json::array();
    for (const auto& f : enc->output())
        sizes.push_back(f.size());
    res.details["encode_frame_bytes"] = sizes;
    if (codec->codec() != D3D12_VIDEO_ENCODER_CODEC_AV1 && !enc->output().empty()) {
        // What the encoder itself wrote for the key frame (Annex B NAL unit types), to tell its own parameter
        // sets or SEI from the slices when a round trip goes wrong.
        const std::vector<std::uint8_t>& f = enc->output().front();
        nlohmann::json types = nlohmann::json::array();
        for (std::size_t i = 0; i + 3 < f.size() && types.size() < 16; ++i)
            if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1) {
                const std::uint8_t h = f[i + 3];
                types.push_back(codec->codec() == D3D12_VIDEO_ENCODER_CODEC_H264 ? h & 0x1F
                                                                                 : (h >> 1) & 0x3F);
                i += 3;
            }
        res.details["encode_key_frame_nal_types"] = types;
    }
    ctx.log(std::format("{}: encoded the GOP once ({} bytes)", hooks.label, total));
    res.details["encode_profile"] = codec->name();
    res.details["encode_rate_control"] =
        std::format("constant {} {}", codec->codec() == D3D12_VIDEO_ENCODER_CODEC_AV1 ? "base_q_idx" : "QP",
                    codec->quantizer());
    res.details["encode_gop_bytes"] = total;
    res.details["encode_mbit_per_s_at_60fps"] = double(total) * 8.0 / video::kGop * 60.0 / 1e6;
    progress(0.55, std::format("{}: measuring encode throughput", hooks.label));
    res.encodeFps = enc->time(ctx, seconds);
    ctx.log(std::format("{}: {:.1f} fps encode", hooks.label, *res.encodeFps));

    if (!canDecode(dev, hooks)) {
        res.encodeNote = "no decoder to check the encoded stream with";
        return;
    }
    std::string error;
    const std::optional<DxvaStream> rt = codec->readBack(stream, &error);
    if (!rt) {
        res.encodeNote = "the encoded stream could not be read back: " + error;
        return;
    }
    progress(0.8, std::format("{}: decoding the encoded stream", hooks.label));
    std::vector<std::uint8_t> frames;
    std::string why;
    try {
        why = decodeFrames(dev, *rt, frames);
    } catch (const std::exception& e) { // the decoder's failure is not the encoder's
        why = e.what();
    }
    if (!why.empty()) {
        res.encodeNote = "the encoded stream was not decoded: " + why;
        return;
    }
    video::checkRoundTrip(frames, static_cast<std::uint32_t>(rt->frames.size()), source, res);
}

} // namespace gt::bench::d3d12video

#endif // GT_HAS_D3D12
