// AV1 for feature.video: decode jobs from parsed OBU streams (gt/bench/av1_params.h) and a key + inter-frame
// encoder at a constant quantizer index, both on Vulkan Video.

#include "video_vk.h"

#if GT_HAS_VULKAN

#include "gt/bench/av1_params.h"

#include <algorithm>
#include <format>
#include <set>

namespace gt::bench::vkvideo {
namespace {

void fillProfile(VkVideoProfileInfoKHR& p, VkVideoCodecOperationFlagBitsKHR op, const void* codecProfile) {
    p.videoCodecOperation = op;
    p.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    p.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    p.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    p.pNext = codecProfile;
}

std::string levelName(std::uint32_t idx) {
    return std::format("{}.{}", 2 + idx / 4, idx % 4);
}

// --- decode ----------------------------------------------------------------------------------------

class Av1Decode final : public DecodeCodec {
public:
    explicit Av1Decode(av1::Stream s) : m_s(std::move(s)) {
        m_av1Profile.stdProfile = static_cast<StdVideoAV1Profile>(m_s.seq.profile);
        m_av1Profile.filmGrainSupport = VK_FALSE;
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, &m_av1Profile);
        for (const av1::Frame& f : m_s.frames) {
            std::set<std::uint32_t> refs;
            if (!f.header.frameIsIntra())
                for (const std::int32_t r : f.refFrames)
                    if (r >= 0) refs.insert(static_cast<std::uint32_t>(r));
            m_refs.emplace_back(refs.begin(), refs.end());
            m_maxRefs = std::max<std::uint32_t>(m_maxRefs, static_cast<std::uint32_t>(refs.size()));
        }
    }
    Av1Decode(const Av1Decode&) = delete;
    Av1Decode& operator=(const Av1Decode&) = delete;

    std::string name() const override {
        return std::format("AV1 {}", m_s.seq.profile == 0 ? "Main" : "other profile");
    }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities() const override {
        if (m_s.seq.levelIdx > static_cast<std::uint32_t>(m_caps.maxLevel))
            return std::format("the AV1 decoder does not support the stream's level {}",
                               levelName(m_s.seq.levelIdx));
        return {};
    }
    VkExtent2D codedExtent() const override {
        return {m_s.frames.front().header.frameWidth, m_s.frames.front().header.frameHeight};
    }
    std::uint32_t frameCount() const override { return static_cast<std::uint32_t>(m_s.frames.size()); }
    std::span<const std::uint8_t> frameData(std::uint32_t f) const override { return m_s.frames[f].data; }
    std::uint32_t maxActiveReferences() const override { return m_maxRefs; }
    std::vector<std::uint32_t> references(std::uint32_t f) const override { return m_refs[f]; }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        const av1::SequenceHeader& s = m_s.seq;
        StdVideoAV1ColorConfig color{};
        color.flags.mono_chrome = s.monoChrome;
        color.flags.color_range = s.colorRange;
        color.flags.separate_uv_delta_q = s.separateUvDeltaQ;
        color.flags.color_description_present_flag = s.colorDescriptionPresent;
        color.BitDepth = static_cast<std::uint8_t>(s.bitDepth);
        color.subsampling_x = static_cast<std::uint8_t>(s.subsamplingX);
        color.subsampling_y = static_cast<std::uint8_t>(s.subsamplingY);
        color.color_primaries = static_cast<StdVideoAV1ColorPrimaries>(s.colorPrimaries);
        color.transfer_characteristics =
            static_cast<StdVideoAV1TransferCharacteristics>(s.transferCharacteristics);
        color.matrix_coefficients = static_cast<StdVideoAV1MatrixCoefficients>(s.matrixCoefficients);
        color.chroma_sample_position = static_cast<StdVideoAV1ChromaSamplePosition>(s.chromaSamplePosition);
        StdVideoAV1TimingInfo timing{};
        timing.flags.equal_picture_interval = s.equalPictureInterval;
        timing.num_units_in_display_tick = s.numUnitsInDisplayTick;
        timing.time_scale = s.timeScale;
        timing.num_ticks_per_picture_minus_1 = s.numTicksPerPictureMinus1;
        StdVideoAV1SequenceHeader seq{};
        seq.flags.still_picture = s.stillPicture;
        seq.flags.reduced_still_picture_header = s.reducedStillPictureHeader;
        seq.flags.use_128x128_superblock = s.use128x128Superblock;
        seq.flags.enable_filter_intra = s.enableFilterIntra;
        seq.flags.enable_intra_edge_filter = s.enableIntraEdgeFilter;
        seq.flags.enable_interintra_compound = s.enableInterintraCompound;
        seq.flags.enable_masked_compound = s.enableMaskedCompound;
        seq.flags.enable_warped_motion = s.enableWarpedMotion;
        seq.flags.enable_dual_filter = s.enableDualFilter;
        seq.flags.enable_order_hint = s.enableOrderHint;
        seq.flags.enable_jnt_comp = s.enableJntComp;
        seq.flags.enable_ref_frame_mvs = s.enableRefFrameMvs;
        seq.flags.frame_id_numbers_present_flag = s.frameIdNumbersPresent;
        seq.flags.enable_superres = s.enableSuperres;
        seq.flags.enable_cdef = s.enableCdef;
        seq.flags.enable_restoration = s.enableRestoration;
        seq.flags.film_grain_params_present = s.filmGrainParamsPresent;
        seq.flags.timing_info_present_flag = s.timingInfoPresent;
        seq.flags.initial_display_delay_present_flag = s.initialDisplayDelayPresent;
        seq.seq_profile = static_cast<StdVideoAV1Profile>(s.profile);
        seq.frame_width_bits_minus_1 = static_cast<std::uint8_t>(s.frameWidthBitsMinus1);
        seq.frame_height_bits_minus_1 = static_cast<std::uint8_t>(s.frameHeightBitsMinus1);
        seq.max_frame_width_minus_1 = static_cast<std::uint16_t>(s.maxFrameWidthMinus1);
        seq.max_frame_height_minus_1 = static_cast<std::uint16_t>(s.maxFrameHeightMinus1);
        seq.delta_frame_id_length_minus_2 = static_cast<std::uint8_t>(s.deltaFrameIdLengthMinus2);
        seq.additional_frame_id_length_minus_1 = static_cast<std::uint8_t>(s.additionalFrameIdLengthMinus1);
        seq.order_hint_bits_minus_1 = static_cast<std::uint8_t>(s.orderHintBits ? s.orderHintBits - 1 : 0);
        seq.seq_force_integer_mv = static_cast<std::uint8_t>(s.seqForceIntegerMv);
        seq.seq_force_screen_content_tools = static_cast<std::uint8_t>(s.seqForceScreenContentTools);
        seq.pColorConfig = &color;
        seq.pTimingInfo = s.timingInfoPresent ? &timing : nullptr;
        VkVideoDecodeAV1SessionParametersCreateInfoKHR av1{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR};
        av1.pStdSequenceHeader = &seq;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &av1;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (AV1 decode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f) override {
        const av1::Frame& fr = m_s.frames[f];
        const av1::FrameHeader& h = fr.header;
        const av1::TileInfo& t = h.tiles;
        m_tile = {};
        m_tile.flags.uniform_tile_spacing_flag = t.uniformSpacing;
        m_tile.TileCols = static_cast<std::uint8_t>(t.cols);
        m_tile.TileRows = static_cast<std::uint8_t>(t.rows);
        m_tile.context_update_tile_id = static_cast<std::uint16_t>(t.contextUpdateTileId);
        m_tile.tile_size_bytes_minus_1 = static_cast<std::uint8_t>(t.tileSizeBytesMinus1);
        m_tile.pMiColStarts = t.miColStarts.data();
        m_tile.pMiRowStarts = t.miRowStarts.data();
        m_tile.pWidthInSbsMinus1 = t.widthInSbsMinus1.data();
        m_tile.pHeightInSbsMinus1 = t.heightInSbsMinus1.data();
        m_quant = {};
        m_quant.flags.using_qmatrix = h.usingQmatrix;
        m_quant.flags.diff_uv_delta = h.diffUvDelta;
        m_quant.base_q_idx = static_cast<std::uint8_t>(h.baseQIdx);
        m_quant.DeltaQYDc = static_cast<std::int8_t>(h.deltaQYDc);
        m_quant.DeltaQUDc = static_cast<std::int8_t>(h.deltaQUDc);
        m_quant.DeltaQUAc = static_cast<std::int8_t>(h.deltaQUAc);
        m_quant.DeltaQVDc = static_cast<std::int8_t>(h.deltaQVDc);
        m_quant.DeltaQVAc = static_cast<std::int8_t>(h.deltaQVAc);
        m_quant.qm_y = static_cast<std::uint8_t>(h.qmY);
        m_quant.qm_u = static_cast<std::uint8_t>(h.qmU);
        m_quant.qm_v = static_cast<std::uint8_t>(h.qmV);
        m_seg = {};
        for (std::size_t i = 0; i < 8; ++i) {
            m_seg.FeatureEnabled[i] = h.featureEnabled[i];
            for (std::size_t j = 0; j < 8; ++j)
                m_seg.FeatureData[i][j] = h.featureData[i][j];
        }
        m_lf = {};
        m_lf.flags.loop_filter_delta_enabled = h.loopFilterDeltaEnabled;
        m_lf.flags.loop_filter_delta_update = h.loopFilterDeltaUpdate;
        for (std::size_t i = 0; i < 4; ++i)
            m_lf.loop_filter_level[i] = static_cast<std::uint8_t>(h.loopFilterLevel[i]);
        m_lf.loop_filter_sharpness = static_cast<std::uint8_t>(h.loopFilterSharpness);
        m_lf.update_ref_delta = static_cast<std::uint8_t>(h.updateRefDelta);
        m_lf.update_mode_delta = static_cast<std::uint8_t>(h.updateModeDelta);
        for (std::size_t i = 0; i < 8; ++i)
            m_lf.loop_filter_ref_deltas[i] = h.loopFilterRefDeltas[i];
        for (std::size_t i = 0; i < 2; ++i)
            m_lf.loop_filter_mode_deltas[i] = h.loopFilterModeDeltas[i];
        m_cdef = {};
        m_cdef.cdef_damping_minus_3 = static_cast<std::uint8_t>(h.cdefDampingMinus3);
        m_cdef.cdef_bits = static_cast<std::uint8_t>(h.cdefBits);
        for (std::size_t i = 0; i < 8; ++i) {
            m_cdef.cdef_y_pri_strength[i] = h.cdefYPri[i];
            m_cdef.cdef_y_sec_strength[i] = h.cdefYSec[i];
            m_cdef.cdef_uv_pri_strength[i] = h.cdefUvPri[i];
            m_cdef.cdef_uv_sec_strength[i] = h.cdefUvSec[i];
        }
        m_lr = {};
        for (std::size_t i = 0; i < 3; ++i) {
            m_lr.FrameRestorationType[i] =
                static_cast<StdVideoAV1FrameRestorationType>(h.frameRestorationType[i]);
            std::uint16_t log2 = 0;
            while (h.loopRestorationSize[i] > (1u << log2))
                ++log2;
            m_lr.LoopRestorationSize[i] = h.loopRestorationSize[i] ? static_cast<std::uint16_t>(log2 - 5) : 0;
        }
        m_gm = {};
        for (std::size_t i = 0; i < 8; ++i) {
            m_gm.GmType[i] = static_cast<std::uint8_t>(h.gmType[i]);
            for (std::size_t j = 0; j < 6; ++j)
                m_gm.gm_params[i][j] = h.gmParams[i][j];
        }

        m_pic = {};
        auto& fl = m_pic.flags;
        fl.error_resilient_mode = h.errorResilientMode;
        fl.disable_cdf_update = h.disableCdfUpdate;
        fl.use_superres = h.useSuperres;
        fl.render_and_frame_size_different = h.renderAndFrameSizeDifferent;
        fl.allow_screen_content_tools = h.allowScreenContentTools;
        fl.is_filter_switchable = h.isFilterSwitchable;
        fl.force_integer_mv = h.forceIntegerMv;
        fl.frame_size_override_flag = h.frameSizeOverride;
        fl.buffer_removal_time_present_flag = h.bufferRemovalTimePresent;
        fl.allow_intrabc = h.allowIntrabc;
        fl.frame_refs_short_signaling = h.frameRefsShortSignaling;
        fl.allow_high_precision_mv = h.allowHighPrecisionMv;
        fl.is_motion_mode_switchable = h.isMotionModeSwitchable;
        fl.use_ref_frame_mvs = h.useRefFrameMvs;
        fl.disable_frame_end_update_cdf = h.disableFrameEndUpdateCdf;
        fl.allow_warped_motion = h.allowWarpedMotion;
        fl.reduced_tx_set = h.reducedTxSet;
        fl.reference_select = h.referenceSelect;
        fl.skip_mode_present = h.skipModePresent;
        fl.delta_q_present = h.deltaQPresent;
        fl.delta_lf_present = h.deltaLfPresent;
        fl.delta_lf_multi = h.deltaLfMulti;
        fl.segmentation_enabled = h.segmentationEnabled;
        fl.segmentation_update_map = h.segmentationUpdateMap;
        fl.segmentation_temporal_update = h.segmentationTemporalUpdate;
        fl.segmentation_update_data = h.segmentationUpdateData;
        fl.UsesLr = h.usesLr;
        fl.usesChromaLr = h.usesChromaLr;
        m_pic.frame_type = static_cast<StdVideoAV1FrameType>(h.frameType);
        m_pic.current_frame_id = h.currentFrameId;
        m_pic.OrderHint = static_cast<std::uint8_t>(h.orderHint);
        m_pic.primary_ref_frame = static_cast<std::uint8_t>(h.primaryRefFrame);
        m_pic.refresh_frame_flags = static_cast<std::uint8_t>(h.refreshFrameFlags);
        m_pic.interpolation_filter = static_cast<StdVideoAV1InterpolationFilter>(h.interpolationFilter);
        m_pic.TxMode = static_cast<StdVideoAV1TxMode>(h.txMode);
        m_pic.delta_q_res = static_cast<std::uint8_t>(h.deltaQRes);
        m_pic.delta_lf_res = static_cast<std::uint8_t>(h.deltaLfRes);
        m_pic.SkipModeFrame[0] = static_cast<std::uint8_t>(h.skipModeFrame[0]);
        m_pic.SkipModeFrame[1] = static_cast<std::uint8_t>(h.skipModeFrame[1]);
        m_pic.coded_denom = static_cast<std::uint8_t>(h.codedDenom);
        for (std::size_t i = 0; i < 8; ++i) {
            m_pic.OrderHints[i] = static_cast<std::uint8_t>(h.orderHints[i]);
            m_pic.expectedFrameId[i] = h.expectedFrameId[i];
        }
        m_pic.pTileInfo = &m_tile;
        m_pic.pQuantization = &m_quant;
        m_pic.pSegmentation = &m_seg;
        m_pic.pLoopFilter = &m_lf;
        m_pic.pCDEF = &m_cdef;
        m_pic.pLoopRestoration = &m_lr;
        m_pic.pGlobalMotion = &m_gm;

        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR};
        m_picInfo.pStdPictureInfo = &m_pic;
        for (std::size_t i = 0; i < 7; ++i)
            m_picInfo.referenceNameSlotIndices[i] = fr.refFrames[i]; // slot = frame
        m_picInfo.frameHeaderOffset = fr.frameHeaderOffset;
        m_picInfo.tileCount = static_cast<std::uint32_t>(fr.tileOffsets.size());
        m_picInfo.pTileOffsets = fr.tileOffsets.data();
        m_picInfo.pTileSizes = fr.tileSizes.data();

        const std::vector<std::uint32_t>& refs = m_refs[f];
        m_refStd.assign(refs.size() + 1, {});
        m_refInfo.assign(refs.size() + 1, {});
        auto fill = [&](std::size_t i, std::uint32_t frame) {
            const av1::FrameHeader& rh = m_s.frames[frame].header;
            StdVideoDecodeAV1ReferenceInfo& r = m_refStd[i];
            r.flags.disable_frame_end_update_cdf = rh.disableFrameEndUpdateCdf;
            r.flags.segmentation_enabled = rh.segmentationEnabled;
            r.frame_type = static_cast<std::uint8_t>(rh.frameType);
            for (std::uint32_t k = 0; k < 8; ++k)
                if (rh.refFrameSignBias[k]) r.RefFrameSignBias |= static_cast<std::uint8_t>(1u << k);
            r.OrderHint = static_cast<std::uint8_t>(rh.orderHint);
            for (std::size_t k = 0; k < 8; ++k)
                r.SavedOrderHints[k] = static_cast<std::uint8_t>(rh.orderHints[k]);
            m_refInfo[i] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR};
            m_refInfo[i].pStdReferenceInfo = &r;
        };
        FrameInfo out;
        out.picture = &m_picInfo;
        fill(0, f);
        out.setup = &m_refInfo[0];
        for (std::size_t i = 0; i < refs.size(); ++i) {
            fill(i + 1, refs[i]);
            out.refs.push_back(&m_refInfo[i + 1]);
        }
        return out;
    }

private:
    av1::Stream m_s;
    std::vector<std::vector<std::uint32_t>> m_refs;
    std::uint32_t m_maxRefs = 0;
    VkVideoDecodeAV1ProfileInfoKHR m_av1Profile{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoDecodeAV1CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};
    StdVideoAV1TileInfo m_tile{};
    StdVideoAV1Quantization m_quant{};
    StdVideoAV1Segmentation m_seg{};
    StdVideoAV1LoopFilter m_lf{};
    StdVideoAV1CDEF m_cdef{};
    StdVideoAV1LoopRestoration m_lr{};
    StdVideoAV1GlobalMotion m_gm{};
    StdVideoDecodeAV1PictureInfo m_pic{};
    VkVideoDecodeAV1PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR};
    std::vector<StdVideoDecodeAV1ReferenceInfo> m_refStd; ///< [0] = setup, then references
    std::vector<VkVideoDecodeAV1DpbSlotInfoKHR> m_refInfo;
};

std::unique_ptr<DecodeCodec> makeDecode(std::optional<av1::Stream> s, std::string* error) {
    if (!s) return nullptr;
    const av1::SequenceHeader& seq = s->seq;
    if (seq.bitDepth != 8 || seq.monoChrome || seq.subsamplingX != 1 || seq.subsamplingY != 1) {
        if (error) *error = "not 8-bit 4:2:0";
        return nullptr;
    }
    for (const av1::Frame& f : s->frames)
        if (!f.header.showFrame || f.header.frameWidth != s->frames.front().header.frameWidth ||
            f.header.frameHeight != s->frames.front().header.frameHeight || f.header.useSuperres) {
            if (error) *error = "hidden frames and frame size changes are not supported";
            return nullptr;
        }
    if (s->frames.size() > kGop) s->frames.resize(kGop);
    return std::make_unique<Av1Decode>(std::move(*s));
}

// --- encode ----------------------------------------------------------------------------------------

class Av1Encode final : public EncodeCodec {
public:
    explicit Av1Encode(VkPhysicalDevice pd) : m_pd(pd) {
        m_av1Profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, &m_av1Profile);
    }
    Av1Encode(const Av1Encode&) = delete;
    Av1Encode& operator=(const Av1Encode&) = delete;

    std::string name() const override { return "AV1 Main"; }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities(const VkVideoCapabilitiesKHR&,
                                  const VkVideoEncodeCapabilitiesKHR&) const override {
        VkPhysicalDeviceVideoEncodeAV1FeaturesKHR av1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f.pNext = &av1;
        vkGetPhysicalDeviceFeatures2(m_pd, &f);
        if (!av1.videoEncodeAV1) return "the driver does not offer the videoEncodeAV1 feature";
        if (m_caps.maxLevel < STD_VIDEO_AV1_LEVEL_4_0) return "the AV1 encoder's highest level is below 4.0";
        if (m_caps.maxSingleReferenceCount < 1 || !(m_caps.singleReferenceNameMask & 1u))
            return "the AV1 encoder cannot predict from the last frame";
        if (!m_caps.superblockSizes) return "the AV1 encoder reports no superblock size";
        return {};
    }
    VkExtent2D codedExtent() const override { return {kWidth, kHeight}; } // the sequence header's frame size
    std::string quantizer() const override { return std::format("constant q_idx {}", qIndex()); }
    const void* parametersGetInfo() override { return nullptr; }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        // A plain sequence: 1920x1080, order hints, 64x64 superblocks where offered, no optional tools (the
        // implementation may enable what it supports).
        StdVideoAV1ColorConfig color{};
        color.BitDepth = 8;
        color.subsampling_x = 1;
        color.subsampling_y = 1;
        color.color_primaries = STD_VIDEO_AV1_COLOR_PRIMARIES_BT_UNSPECIFIED;
        color.transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
        color.matrix_coefficients = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
        color.chroma_sample_position = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN;
        StdVideoAV1SequenceHeader seq{};
        seq.flags.enable_order_hint = 1;
        seq.flags.use_128x128_superblock =
            !(m_caps.superblockSizes & VK_VIDEO_ENCODE_AV1_SUPERBLOCK_SIZE_64_BIT_KHR);
        seq.seq_profile = STD_VIDEO_AV1_PROFILE_MAIN;
        seq.frame_width_bits_minus_1 = 10;
        seq.frame_height_bits_minus_1 = 10;
        seq.max_frame_width_minus_1 = kWidth - 1;
        seq.max_frame_height_minus_1 = kHeight - 1;
        seq.order_hint_bits_minus_1 = 7;
        seq.seq_force_integer_mv = 2; // SELECT_INTEGER_MV (unused without screen content tools)
        seq.seq_force_screen_content_tools = 0;
        seq.pColorConfig = &color;
        StdVideoEncodeAV1OperatingPointInfo op{};
        op.seq_level_idx = static_cast<std::uint8_t>(std::min(STD_VIDEO_AV1_LEVEL_4_1, m_caps.maxLevel));
        VkVideoEncodeAV1SessionParametersCreateInfoKHR av1{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR};
        av1.pStdSequenceHeader = &seq;
        if (m_caps.maxOperatingPoints >= 1) { // else the implementation picks the level
            av1.stdOperatingPointCount = 1;
            av1.pStdOperatingPoints = &op;
        }
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &av1;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (AV1 encode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f, bool constantQuantizer) override {
        // Frame f (< 8) refreshes reference slot (VBI) f and goes to DPB slot f % 2; every reference name
        // points at the previous frame, and only LAST_FRAME is used. No primary reference frame: every frame
        // starts from default CDFs, so any frame decodes from its references alone.
        const bool key = f == 0;
        const std::uint32_t refSlot = (f + 1) % 2;
        m_quant = {};
        m_quant.base_q_idx = static_cast<std::uint8_t>(qIndex());
        m_lf = {};
        static constexpr std::array<std::int8_t, 8> kDefaultRefDeltas = {1, 0, 0, 0, -1, 0, -1, -1};
        for (std::size_t i = 0; i < 8; ++i)
            m_lf.loop_filter_ref_deltas[i] = kDefaultRefDeltas[i];
        m_cdef = {};
        m_lr = {};
        m_gm = {};
        for (std::size_t i = 0; i < 8; ++i) {
            m_gm.gm_params[i][2] = 1 << 16;
            m_gm.gm_params[i][5] = 1 << 16;
        }
        m_pic = {};
        m_pic.flags.show_frame = 1;
        m_pic.flags.error_resilient_mode = key; // implied for shown key frames
        m_pic.frame_type = key ? STD_VIDEO_AV1_FRAME_TYPE_KEY : STD_VIDEO_AV1_FRAME_TYPE_INTER;
        m_pic.order_hint = static_cast<std::uint8_t>(f);
        m_pic.primary_ref_frame = 7; // PRIMARY_REF_NONE
        m_pic.refresh_frame_flags = key ? 0xFF : static_cast<std::uint8_t>(1u << (f % 8));
        m_pic.render_width_minus_1 = kWidth - 1;
        m_pic.render_height_minus_1 = kHeight - 1;
        m_pic.interpolation_filter = STD_VIDEO_AV1_INTERPOLATION_FILTER_EIGHTTAP;
        m_pic.TxMode = STD_VIDEO_AV1_TX_MODE_SELECT;
        for (std::uint32_t v = 0; v < 8; ++v)
            m_pic.ref_order_hint[v] = static_cast<std::uint8_t>(v < f ? v : 0);
        for (std::size_t i = 0; i < 7; ++i)
            m_pic.ref_frame_idx[i] = static_cast<std::int8_t>(key ? 0 : (f - 1) % 8);
        m_pic.pQuantization = &m_quant;
        m_pic.pLoopFilter = &m_lf;
        m_pic.pCDEF = &m_cdef;
        m_pic.pLoopRestoration = &m_lr;
        m_pic.pGlobalMotion = &m_gm;
        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR};
        m_picInfo.predictionMode = key ? VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR
                                       : VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
        m_picInfo.rateControlGroup = key ? VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR
                                         : VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR;
        m_picInfo.constantQIndex = constantQuantizer ? qIndex() : 0;
        m_picInfo.pStdPictureInfo = &m_pic;
        std::fill(std::begin(m_picInfo.referenceNameSlotIndices),
                  std::end(m_picInfo.referenceNameSlotIndices), -1);
        if (!key) m_picInfo.referenceNameSlotIndices[0] = static_cast<std::int32_t>(refSlot); // LAST_FRAME

        m_setupStd = {};
        m_setupStd.frame_type = m_pic.frame_type;
        m_setupStd.OrderHint = m_pic.order_hint;
        m_setupInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR};
        m_setupInfo.pStdReferenceInfo = &m_setupStd;
        FrameInfo out;
        out.picture = &m_picInfo;
        out.setup = &m_setupInfo;
        if (!key) {
            m_refStd = {};
            m_refStd.frame_type = f == 1 ? STD_VIDEO_AV1_FRAME_TYPE_KEY : STD_VIDEO_AV1_FRAME_TYPE_INTER;
            m_refStd.OrderHint = static_cast<std::uint8_t>(f - 1);
            m_refInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR};
            m_refInfo.pStdReferenceInfo = &m_refStd;
            out.ref = &m_refInfo;
        }
        return out;
    }

private:
    std::uint32_t qIndex() const {
        return std::clamp(110u, m_caps.minQIndex, std::max(m_caps.minQIndex, m_caps.maxQIndex));
    }

    VkPhysicalDevice m_pd;
    VkVideoEncodeAV1ProfileInfoKHR m_av1Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoEncodeAV1CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR};
    StdVideoAV1Quantization m_quant{};
    StdVideoAV1LoopFilter m_lf{};
    StdVideoAV1CDEF m_cdef{};
    StdVideoAV1LoopRestoration m_lr{};
    StdVideoAV1GlobalMotion m_gm{};
    StdVideoEncodeAV1PictureInfo m_pic{};
    VkVideoEncodeAV1PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR};
    StdVideoEncodeAV1ReferenceInfo m_setupStd{}, m_refStd{};
    VkVideoEncodeAV1DpbSlotInfoKHR m_setupInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR};
    VkVideoEncodeAV1DpbSlotInfoKHR m_refInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR};
};

} // namespace

CodecHooks av1Hooks() {
    CodecHooks h;
    h.key = "av1";
    h.label = "AV1";
    h.decodeOp = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    h.encodeOp = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    h.decodeExtension = VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME;
    h.encodeExtension = VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME;
    h.reference = [](std::span<const std::uint8_t> data, std::string* error) {
        return makeDecode(av1::parse(data, nullptr, error), error);
    };
    h.encoders = [](VkPhysicalDevice pd) {
        std::vector<std::unique_ptr<EncodeCodec>> v;
        v.push_back(std::make_unique<Av1Encode>(pd));
        return v;
    };
    h.encoded = [](const std::vector<std::uint8_t>& params,
                   const std::vector<std::vector<std::uint8_t>>& frames,
                   std::string* error) -> std::unique_ptr<DecodeCodec> {
        // The Vulkan spec does not say whether encoders write OBU size fields; joining the frames needs them.
        std::vector<std::uint8_t> header, all;
        try {
            header = av1::withSizeFields(params);
            for (const auto& f : frames) {
                const std::vector<std::uint8_t> obus = av1::withSizeFields(f);
                all.insert(all.end(), obus.begin(), obus.end());
            }
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return nullptr;
        }
        auto seq = av1::parseSequenceHeader(header, error);
        if (!seq) return nullptr;
        return makeDecode(av1::parse(all, &*seq, error), error);
    };
    return h;
}

} // namespace gt::bench::vkvideo

#endif // GT_HAS_VULKAN
