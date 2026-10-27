// H.265 for feature.video: decode jobs from parsed Annex B streams (gt/bench/h265_params.h) and an IDR +
// P-frame encoder at constant QP, both on Vulkan Video.

#include "video_vk.h"

#if GT_HAS_VULKAN

#include "gt/bench/h265_params.h"

#include <algorithm>
#include <format>

namespace gt::bench::vkvideo {
namespace {

StdVideoH265LevelIdc levelEnum(std::uint32_t levelIdc) {
    switch (levelIdc) {
    case 30:
        return STD_VIDEO_H265_LEVEL_IDC_1_0;
    case 60:
        return STD_VIDEO_H265_LEVEL_IDC_2_0;
    case 63:
        return STD_VIDEO_H265_LEVEL_IDC_2_1;
    case 90:
        return STD_VIDEO_H265_LEVEL_IDC_3_0;
    case 93:
        return STD_VIDEO_H265_LEVEL_IDC_3_1;
    case 120:
        return STD_VIDEO_H265_LEVEL_IDC_4_0;
    case 123:
        return STD_VIDEO_H265_LEVEL_IDC_4_1;
    case 150:
        return STD_VIDEO_H265_LEVEL_IDC_5_0;
    case 153:
        return STD_VIDEO_H265_LEVEL_IDC_5_1;
    case 156:
        return STD_VIDEO_H265_LEVEL_IDC_5_2;
    case 180:
        return STD_VIDEO_H265_LEVEL_IDC_6_0;
    case 183:
        return STD_VIDEO_H265_LEVEL_IDC_6_1;
    case 186:
        return STD_VIDEO_H265_LEVEL_IDC_6_2;
    default:
        return STD_VIDEO_H265_LEVEL_IDC_INVALID;
    }
}

const char* profileName(std::uint32_t idc) {
    switch (idc) {
    case STD_VIDEO_H265_PROFILE_IDC_MAIN:
        return "Main";
    case STD_VIDEO_H265_PROFILE_IDC_MAIN_10:
        return "Main 10";
    default:
        return "other profile";
    }
}

void fillProfile(VkVideoProfileInfoKHR& p, VkVideoCodecOperationFlagBitsKHR op, const void* codecProfile) {
    p.videoCodecOperation = op;
    p.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    p.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    p.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    p.pNext = codecProfile;
}

StdVideoH265ProfileTierLevel stdPtl(const h265::ProfileTierLevel& p) {
    StdVideoH265ProfileTierLevel s{};
    s.flags.general_tier_flag = p.tier;
    s.flags.general_progressive_source_flag = p.progressiveSource;
    s.flags.general_interlaced_source_flag = p.interlacedSource;
    s.flags.general_non_packed_constraint_flag = p.nonPackedConstraint;
    s.flags.general_frame_only_constraint_flag = p.frameOnlyConstraint;
    s.general_profile_idc = static_cast<StdVideoH265ProfileIdc>(p.profileIdc);
    s.general_level_idc = levelEnum(p.levelIdc);
    return s;
}

StdVideoH265DecPicBufMgr stdDpb(const h265::DecPicBufMgr& d) {
    StdVideoH265DecPicBufMgr s{};
    for (std::size_t i = 0; i < d.maxDecPicBufferingMinus1.size(); ++i) {
        s.max_dec_pic_buffering_minus1[i] = static_cast<std::uint8_t>(d.maxDecPicBufferingMinus1[i]);
        s.max_num_reorder_pics[i] = static_cast<std::uint8_t>(d.maxNumReorderPics[i]);
        s.max_latency_increase_plus1[i] = d.maxLatencyIncreasePlus1[i];
    }
    return s;
}

/// Both the syntax and the derived lists (as FFmpeg's Vulkan decoder fills them).
StdVideoH265ShortTermRefPicSet stdRps(const h265::ShortTermRps& r) {
    StdVideoH265ShortTermRefPicSet s{};
    s.flags.inter_ref_pic_set_prediction_flag = r.interPrediction;
    s.flags.delta_rps_sign = r.deltaRpsSign;
    s.delta_idx_minus1 = r.deltaIdxMinus1;
    s.use_delta_flag = static_cast<std::uint16_t>(r.useDeltaFlag);
    s.abs_delta_rps_minus1 = static_cast<std::uint16_t>(r.absDeltaRpsMinus1);
    s.used_by_curr_pic_flag = static_cast<std::uint16_t>(r.usedByCurrPicFlag);
    s.num_negative_pics = static_cast<std::uint8_t>(r.deltaPocS0.size());
    s.num_positive_pics = static_cast<std::uint8_t>(r.deltaPocS1.size());
    std::int32_t prev = 0;
    for (std::size_t i = 0; i < r.deltaPocS0.size(); ++i) {
        s.delta_poc_s0_minus1[i] = static_cast<std::uint16_t>(prev - r.deltaPocS0[i] - 1);
        prev = r.deltaPocS0[i];
        if (r.usedS0[i]) s.used_by_curr_pic_s0_flag |= static_cast<std::uint16_t>(1u << i);
    }
    prev = 0;
    for (std::size_t i = 0; i < r.deltaPocS1.size(); ++i) {
        s.delta_poc_s1_minus1[i] = static_cast<std::uint16_t>(r.deltaPocS1[i] - prev - 1);
        prev = r.deltaPocS1[i];
        if (r.usedS1[i]) s.used_by_curr_pic_s1_flag |= static_cast<std::uint16_t>(1u << i);
    }
    return s;
}

// --- decode ----------------------------------------------------------------------------------------

class H265Decode final : public DecodeCodec {
public:
    explicit H265Decode(h265::Stream s) : m_s(std::move(s)) {
        const h265::Sps& sps = *m_s.params.sps;
        m_h265Profile.stdProfileIdc = static_cast<StdVideoH265ProfileIdc>(sps.ptl.profileIdc);
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, &m_h265Profile);
    }
    H265Decode(const H265Decode&) = delete;
    H265Decode& operator=(const H265Decode&) = delete;

    /// Resolves each picture's reference picture set to earlier frames. Fails if a reference is missing.
    std::string resolve() {
        std::string error;
        const auto refs = h265::references(m_s, &error);
        if (!refs) return error;
        m_before.clear();
        m_after.clear();
        for (const h265::References& r : *refs) {
            m_before.push_back(r.before);
            m_after.push_back(r.after);
            m_maxRefs = std::max<std::uint32_t>(m_maxRefs,
                                                static_cast<std::uint32_t>(r.before.size() + r.after.size()));
        }
        return {};
    }

    std::string name() const override {
        return std::format("H.265 {}", profileName(m_s.params.sps->ptl.profileIdc));
    }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities() const override {
        const std::uint32_t level = m_s.params.sps->ptl.levelIdc;
        if (levelEnum(level) == STD_VIDEO_H265_LEVEL_IDC_INVALID || levelEnum(level) > m_caps.maxLevelIdc)
            return std::format("the H.265 decoder does not support the stream's level {}", level / 30.0);
        return {};
    }
    VkExtent2D codedExtent() const override { return {m_s.params.sps->width, m_s.params.sps->height}; }
    std::uint32_t frameCount() const override { return static_cast<std::uint32_t>(m_s.pictures.size()); }
    std::span<const std::uint8_t> frameData(std::uint32_t f) const override { return m_s.pictures[f].data; }
    std::uint32_t maxActiveReferences() const override { return m_maxRefs; }
    std::vector<std::uint32_t> references(std::uint32_t f) const override {
        std::vector<std::uint32_t> r = m_before[f];
        r.insert(r.end(), m_after[f].begin(), m_after[f].end());
        return r;
    }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        const h265::Vps& v = *m_s.params.vps;
        const h265::Sps& s = *m_s.params.sps;
        const h265::Pps& p = *m_s.params.pps;
        const StdVideoH265ProfileTierLevel vpsPtl = stdPtl(v.ptl), spsPtl = stdPtl(s.ptl);
        const StdVideoH265DecPicBufMgr vpsDpb = stdDpb(v.dpb), spsDpb = stdDpb(s.dpb);
        StdVideoH265VideoParameterSet vps{};
        vps.flags.vps_temporal_id_nesting_flag = v.temporalIdNesting;
        vps.flags.vps_sub_layer_ordering_info_present_flag = v.subLayerOrderingInfoPresent;
        vps.flags.vps_timing_info_present_flag = v.timingInfoPresent;
        vps.flags.vps_poc_proportional_to_timing_flag = v.pocProportionalToTiming;
        vps.vps_video_parameter_set_id = static_cast<std::uint8_t>(v.id);
        vps.vps_max_sub_layers_minus1 = static_cast<std::uint8_t>(v.maxSubLayersMinus1);
        vps.vps_num_units_in_tick = v.numUnitsInTick;
        vps.vps_time_scale = v.timeScale;
        vps.vps_num_ticks_poc_diff_one_minus1 = v.numTicksPocDiffOneMinus1;
        vps.pDecPicBufMgr = &vpsDpb;
        vps.pProfileTierLevel = &vpsPtl;

        std::vector<StdVideoH265ShortTermRefPicSet> sets;
        for (const auto& r : s.rps)
            sets.push_back(stdRps(r));
        StdVideoH265SequenceParameterSet sps{};
        sps.flags.sps_temporal_id_nesting_flag = s.temporalIdNesting;
        sps.flags.separate_colour_plane_flag = s.separateColourPlane;
        sps.flags.conformance_window_flag = s.conformanceWindow;
        sps.flags.sps_sub_layer_ordering_info_present_flag = s.subLayerOrderingInfoPresent;
        sps.flags.scaling_list_enabled_flag = s.scalingListEnabled;
        sps.flags.amp_enabled_flag = s.ampEnabled;
        sps.flags.sample_adaptive_offset_enabled_flag = s.saoEnabled;
        sps.flags.pcm_enabled_flag = s.pcmEnabled;
        sps.flags.pcm_loop_filter_disabled_flag = s.pcmLoopFilterDisabled;
        sps.flags.long_term_ref_pics_present_flag = s.longTermRefPicsPresent;
        sps.flags.sps_temporal_mvp_enabled_flag = s.temporalMvpEnabled;
        sps.flags.strong_intra_smoothing_enabled_flag = s.strongIntraSmoothing;
        sps.chroma_format_idc = static_cast<StdVideoH265ChromaFormatIdc>(s.chromaFormatIdc);
        sps.pic_width_in_luma_samples = s.width;
        sps.pic_height_in_luma_samples = s.height;
        sps.sps_video_parameter_set_id = static_cast<std::uint8_t>(s.vpsId);
        sps.sps_max_sub_layers_minus1 = static_cast<std::uint8_t>(s.maxSubLayersMinus1);
        sps.sps_seq_parameter_set_id = static_cast<std::uint8_t>(s.id);
        sps.bit_depth_luma_minus8 = static_cast<std::uint8_t>(s.bitDepthLumaMinus8);
        sps.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(s.bitDepthChromaMinus8);
        sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(s.log2MaxPocLsbMinus4);
        sps.log2_min_luma_coding_block_size_minus3 = static_cast<std::uint8_t>(s.log2MinCbMinus3);
        sps.log2_diff_max_min_luma_coding_block_size = static_cast<std::uint8_t>(s.log2DiffMaxMinCb);
        sps.log2_min_luma_transform_block_size_minus2 = static_cast<std::uint8_t>(s.log2MinTbMinus2);
        sps.log2_diff_max_min_luma_transform_block_size = static_cast<std::uint8_t>(s.log2DiffMaxMinTb);
        sps.max_transform_hierarchy_depth_inter =
            static_cast<std::uint8_t>(s.maxTransformHierarchyDepthInter);
        sps.max_transform_hierarchy_depth_intra =
            static_cast<std::uint8_t>(s.maxTransformHierarchyDepthIntra);
        sps.num_short_term_ref_pic_sets = static_cast<std::uint8_t>(sets.size());
        sps.pcm_sample_bit_depth_luma_minus1 = static_cast<std::uint8_t>(s.pcmBitDepthLumaMinus1);
        sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<std::uint8_t>(s.pcmBitDepthChromaMinus1);
        sps.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<std::uint8_t>(s.log2MinPcmCbMinus3);
        sps.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<std::uint8_t>(s.log2DiffMaxMinPcmCb);
        sps.conf_win_left_offset = s.confLeft;
        sps.conf_win_right_offset = s.confRight;
        sps.conf_win_top_offset = s.confTop;
        sps.conf_win_bottom_offset = s.confBottom;
        sps.pProfileTierLevel = &spsPtl;
        sps.pDecPicBufMgr = &spsDpb;
        sps.pShortTermRefPicSet = sets.empty() ? nullptr : sets.data();

        StdVideoH265PictureParameterSet pps{};
        pps.flags.dependent_slice_segments_enabled_flag = p.dependentSliceSegmentsEnabled;
        pps.flags.output_flag_present_flag = p.outputFlagPresent;
        pps.flags.sign_data_hiding_enabled_flag = p.signDataHiding;
        pps.flags.cabac_init_present_flag = p.cabacInitPresent;
        pps.flags.constrained_intra_pred_flag = p.constrainedIntraPred;
        pps.flags.transform_skip_enabled_flag = p.transformSkipEnabled;
        pps.flags.cu_qp_delta_enabled_flag = p.cuQpDeltaEnabled;
        pps.flags.pps_slice_chroma_qp_offsets_present_flag = p.sliceChromaQpOffsetsPresent;
        pps.flags.weighted_pred_flag = p.weightedPred;
        pps.flags.weighted_bipred_flag = p.weightedBipred;
        pps.flags.transquant_bypass_enabled_flag = p.transquantBypassEnabled;
        pps.flags.tiles_enabled_flag = p.tilesEnabled;
        pps.flags.entropy_coding_sync_enabled_flag = p.entropyCodingSync;
        pps.flags.uniform_spacing_flag = p.uniformSpacing;
        pps.flags.loop_filter_across_tiles_enabled_flag = p.loopFilterAcrossTiles;
        pps.flags.pps_loop_filter_across_slices_enabled_flag = p.loopFilterAcrossSlices;
        pps.flags.deblocking_filter_control_present_flag = p.deblockingControlPresent;
        pps.flags.deblocking_filter_override_enabled_flag = p.deblockingOverrideEnabled;
        pps.flags.pps_deblocking_filter_disabled_flag = p.deblockingDisabled;
        pps.flags.lists_modification_present_flag = p.listsModificationPresent;
        pps.flags.slice_segment_header_extension_present_flag = p.sliceSegmentHeaderExtensionPresent;
        pps.pps_pic_parameter_set_id = static_cast<std::uint8_t>(p.id);
        pps.pps_seq_parameter_set_id = static_cast<std::uint8_t>(p.spsId);
        pps.sps_video_parameter_set_id = static_cast<std::uint8_t>(s.vpsId);
        pps.num_extra_slice_header_bits = static_cast<std::uint8_t>(p.numExtraSliceHeaderBits);
        pps.num_ref_idx_l0_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL0DefaultMinus1);
        pps.num_ref_idx_l1_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL1DefaultMinus1);
        pps.init_qp_minus26 = static_cast<std::int8_t>(p.initQpMinus26);
        pps.diff_cu_qp_delta_depth = static_cast<std::uint8_t>(p.diffCuQpDeltaDepth);
        pps.pps_cb_qp_offset = static_cast<std::int8_t>(p.cbQpOffset);
        pps.pps_cr_qp_offset = static_cast<std::int8_t>(p.crQpOffset);
        pps.pps_beta_offset_div2 = static_cast<std::int8_t>(p.betaOffsetDiv2);
        pps.pps_tc_offset_div2 = static_cast<std::int8_t>(p.tcOffsetDiv2);
        pps.log2_parallel_merge_level_minus2 = static_cast<std::uint8_t>(p.log2ParallelMergeLevelMinus2);
        pps.num_tile_columns_minus1 = static_cast<std::uint8_t>(p.numTileColumnsMinus1);
        pps.num_tile_rows_minus1 = static_cast<std::uint8_t>(p.numTileRowsMinus1);
        for (std::size_t i = 0; i < p.columnWidthMinus1.size() && i < std::size(pps.column_width_minus1); ++i)
            pps.column_width_minus1[i] = static_cast<std::uint16_t>(p.columnWidthMinus1[i]);
        for (std::size_t i = 0; i < p.rowHeightMinus1.size() && i < std::size(pps.row_height_minus1); ++i)
            pps.row_height_minus1[i] = static_cast<std::uint16_t>(p.rowHeightMinus1[i]);

        VkVideoDecodeH265SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdVPSCount = 1;
        add.pStdVPSs = &vps;
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoDecodeH265SessionParametersCreateInfoKHR h265{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h265.maxStdVPSCount = 1;
        h265.maxStdSPSCount = 1;
        h265.maxStdPPSCount = 1;
        h265.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h265;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (H.265 decode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f) override {
        const h265::Picture& pic = m_s.pictures[f];
        const h265::SliceHeader& h = pic.header;
        m_pic = {};
        m_pic.flags.IrapPicFlag = h.irap();
        m_pic.flags.IdrPicFlag = h.idr();
        m_pic.flags.IsReference = h.reference();
        m_pic.flags.short_term_ref_pic_set_sps_flag = h.shortTermRefPicSetSps;
        m_pic.sps_video_parameter_set_id = static_cast<std::uint8_t>(m_s.params.sps->vpsId);
        m_pic.pps_seq_parameter_set_id = static_cast<std::uint8_t>(m_s.params.pps->spsId);
        m_pic.pps_pic_parameter_set_id = static_cast<std::uint8_t>(m_s.params.pps->id);
        m_pic.NumDeltaPocsOfRefRpsIdx =
            static_cast<std::uint8_t>(h.rps.interPrediction ? h.rps.refRpsNumDeltaPocs : 0);
        m_pic.PicOrderCntVal = pic.poc;
        m_pic.NumBitsForSTRefPicSetInSlice = static_cast<std::uint16_t>(h.numBitsForStRefPicSet);
        std::fill(std::begin(m_pic.RefPicSetStCurrBefore), std::end(m_pic.RefPicSetStCurrBefore),
                  STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        std::fill(std::begin(m_pic.RefPicSetStCurrAfter), std::end(m_pic.RefPicSetStCurrAfter),
                  STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        std::fill(std::begin(m_pic.RefPicSetLtCurr), std::end(m_pic.RefPicSetLtCurr),
                  STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        for (std::size_t i = 0; i < m_before[f].size(); ++i)
            m_pic.RefPicSetStCurrBefore[i] = static_cast<std::uint8_t>(m_before[f][i]); // slot = frame
        for (std::size_t i = 0; i < m_after[f].size(); ++i)
            m_pic.RefPicSetStCurrAfter[i] = static_cast<std::uint8_t>(m_after[f][i]);
        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR};
        m_picInfo.pStdPictureInfo = &m_pic;
        m_picInfo.sliceSegmentCount = static_cast<std::uint32_t>(pic.segments.size());
        m_picInfo.pSliceSegmentOffsets = pic.segments.data();

        const std::vector<std::uint32_t> refs = references(f);
        m_refStd.assign(refs.size() + 1, {});
        m_refInfo.assign(refs.size() + 1, {});
        auto fill = [&](std::size_t i, std::uint32_t frame) {
            m_refStd[i].PicOrderCntVal = m_s.pictures[frame].poc;
            m_refInfo[i] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR};
            m_refInfo[i].pStdReferenceInfo = &m_refStd[i];
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
    h265::Stream m_s;
    std::vector<std::vector<std::uint32_t>> m_before, m_after;
    std::uint32_t m_maxRefs = 0;
    VkVideoDecodeH265ProfileInfoKHR m_h265Profile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoDecodeH265CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
    StdVideoDecodeH265PictureInfo m_pic{};
    VkVideoDecodeH265PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR};
    std::vector<StdVideoDecodeH265ReferenceInfo> m_refStd; ///< [0] = setup, then references
    std::vector<VkVideoDecodeH265DpbSlotInfoKHR> m_refInfo;
};

std::unique_ptr<DecodeCodec> makeDecode(std::optional<h265::Stream> s, std::string* error) {
    if (!s) return nullptr;
    const h265::Sps& sps = *s->params.sps;
    if (sps.chromaFormatIdc != 1 || sps.bitDepthLumaMinus8 || sps.bitDepthChromaMinus8 ||
        sps.separateColourPlane) {
        if (error) *error = "not 8-bit 4:2:0";
        return nullptr;
    }
    if (sps.conformanceWindow && (sps.confLeft || sps.confTop)) {
        if (error) *error = "cropping at the top or left is not supported";
        return nullptr;
    }
    if (s->pictures.size() > kGop) s->pictures.resize(kGop);
    auto codec = std::make_unique<H265Decode>(std::move(*s));
    if (std::string why = codec->resolve(); !why.empty()) {
        if (error) *error = why;
        return nullptr;
    }
    return codec;
}

// --- encode ----------------------------------------------------------------------------------------

class H265Encode final : public EncodeCodec {
public:
    H265Encode() {
        m_h265Profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, &m_h265Profile);
        m_getInfo.writeStdVPS = VK_TRUE;
        m_getInfo.writeStdSPS = VK_TRUE;
        m_getInfo.writeStdPPS = VK_TRUE;
    }
    H265Encode(const H265Encode&) = delete;
    H265Encode& operator=(const H265Encode&) = delete;

    std::string name() const override { return "H.265 Main"; }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities(const VkVideoCapabilitiesKHR&,
                                  const VkVideoEncodeCapabilitiesKHR&) const override {
        if (m_caps.maxLevelIdc < STD_VIDEO_H265_LEVEL_IDC_4_0)
            return "the H.265 encoder's highest level is below 4.0";
        if (m_caps.maxPPictureL0ReferenceCount < 1) return "the H.265 encoder cannot make P-frames";
        if (!m_caps.ctbSizes || !m_caps.transformBlockSizes)
            return "the H.265 encoder reports no block sizes";
        return {};
    }
    VkExtent2D codedExtent() const override { return {kImageWidth, kImageHeight}; }
    std::string quantizer() const override { return std::format("constant QP {}", qp()); }
    const void* parametersGetInfo() override { return &m_getInfo; }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        // 1920x1088 coded, cropped to 1080 lines; the largest CTB and widest transform range offered; one
        // reference picture; reference picture sets in the slice headers.
        const std::uint32_t ctbLog2 = (m_caps.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR)   ? 6
                                      : (m_caps.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR) ? 5
                                                                                                     : 4;
        std::uint32_t minTb = 2, maxTb = 5;
        while (minTb < 5 && !(m_caps.transformBlockSizes & (1u << (minTb - 2))))
            ++minTb;
        while (maxTb > minTb && !(m_caps.transformBlockSizes & (1u << (maxTb - 2))))
            --maxTb;
        maxTb = std::min(maxTb, ctbLog2);
        StdVideoH265ProfileTierLevel ptl{};
        ptl.flags.general_progressive_source_flag = 1;
        ptl.flags.general_frame_only_constraint_flag = 1;
        ptl.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        ptl.general_level_idc = std::min(STD_VIDEO_H265_LEVEL_IDC_4_1, m_caps.maxLevelIdc);
        StdVideoH265DecPicBufMgr dpb{};
        dpb.max_dec_pic_buffering_minus1[0] = 1;
        StdVideoH265VideoParameterSet vps{};
        vps.flags.vps_temporal_id_nesting_flag = 1;
        vps.flags.vps_sub_layer_ordering_info_present_flag = 1;
        vps.pDecPicBufMgr = &dpb;
        vps.pProfileTierLevel = &ptl;
        const VkVideoEncodeH265StdFlagsKHR syntax = m_caps.stdSyntaxFlags;
        m_sao = (syntax & VK_VIDEO_ENCODE_H265_STD_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG_SET_BIT_KHR) != 0;
        StdVideoH265SequenceParameterSet sps{};
        sps.flags.sps_temporal_id_nesting_flag = 1;
        sps.flags.conformance_window_flag = 1;
        sps.flags.sps_sub_layer_ordering_info_present_flag = 1;
        sps.flags.sample_adaptive_offset_enabled_flag = m_sao;
        sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
        sps.pic_width_in_luma_samples = kImageWidth;
        sps.pic_height_in_luma_samples = kImageHeight;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sps.log2_min_luma_coding_block_size_minus3 = 0;
        sps.log2_diff_max_min_luma_coding_block_size = static_cast<std::uint8_t>(ctbLog2 - 3);
        sps.log2_min_luma_transform_block_size_minus2 = static_cast<std::uint8_t>(minTb - 2);
        sps.log2_diff_max_min_luma_transform_block_size = static_cast<std::uint8_t>(maxTb - minTb);
        sps.max_transform_hierarchy_depth_inter = static_cast<std::uint8_t>(ctbLog2 - minTb);
        sps.max_transform_hierarchy_depth_intra = static_cast<std::uint8_t>(ctbLog2 - minTb);
        sps.conf_win_bottom_offset = (kImageHeight - kHeight) / 2; // in chroma rows for 4:2:0
        sps.pProfileTierLevel = &ptl;
        sps.pDecPicBufMgr = &dpb;
        StdVideoH265PictureParameterSet pps{};
        pps.flags.pps_loop_filter_across_slices_enabled_flag = 1;
        VkVideoEncodeH265SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdVPSCount = 1;
        add.pStdVPSs = &vps;
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoEncodeH265SessionParametersCreateInfoKHR h265{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h265.maxStdVPSCount = 1;
        h265.maxStdSPSCount = 1;
        h265.maxStdPPSCount = 1;
        h265.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h265;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (H.265 encode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f, bool constantQuantizer) override {
        // Frame f: POC f; slot f % 2, reference slot (f + 1) % 2 (the previous frame, POC delta -1).
        const bool idr = f == 0;
        const std::uint32_t refSlot = (f + 1) % 2;
        m_rps = {};
        m_rps.num_negative_pics = 1;
        m_rps.delta_poc_s0_minus1[0] = 0;
        m_rps.used_by_curr_pic_s0_flag = 1;
        m_lists = {};
        std::fill(std::begin(m_lists.RefPicList0), std::end(m_lists.RefPicList0),
                  STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        std::fill(std::begin(m_lists.RefPicList1), std::end(m_lists.RefPicList1),
                  STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        m_lists.RefPicList0[0] = static_cast<std::uint8_t>(refSlot);
        m_pic = {};
        m_pic.flags.is_reference = 1;
        m_pic.flags.IrapPicFlag = idr;
        m_pic.flags.pic_output_flag = 1;
        m_pic.pic_type = idr ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
        m_pic.PicOrderCntVal = static_cast<std::int32_t>(f);
        m_pic.pRefLists = idr ? nullptr : &m_lists;
        m_pic.pShortTermRefPicSet = idr ? nullptr : &m_rps;
        m_slice = {};
        m_slice.flags.first_slice_segment_in_pic_flag = 1;
        m_slice.flags.slice_sao_luma_flag = m_sao;
        m_slice.flags.slice_sao_chroma_flag = m_sao;
        m_slice.flags.slice_loop_filter_across_slices_enabled_flag = 1;
        m_slice.slice_type = idr ? STD_VIDEO_H265_SLICE_TYPE_I : STD_VIDEO_H265_SLICE_TYPE_P;
        m_slice.MaxNumMergeCand = 5;
        m_nalu = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR};
        m_nalu.constantQp = constantQuantizer ? qp() : 0;
        m_nalu.pStdSliceSegmentHeader = &m_slice;
        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR};
        m_picInfo.naluSliceSegmentEntryCount = 1;
        m_picInfo.pNaluSliceSegmentEntries = &m_nalu;
        m_picInfo.pStdPictureInfo = &m_pic;

        m_setupStd = {};
        m_setupStd.pic_type = m_pic.pic_type;
        m_setupStd.PicOrderCntVal = m_pic.PicOrderCntVal;
        m_setupInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
        m_setupInfo.pStdReferenceInfo = &m_setupStd;
        FrameInfo out;
        out.picture = &m_picInfo;
        out.setup = &m_setupInfo;
        if (!idr) {
            m_refStd = {};
            m_refStd.pic_type = f == 1 ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
            m_refStd.PicOrderCntVal = static_cast<std::int32_t>(f - 1);
            m_refInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
            m_refInfo.pStdReferenceInfo = &m_refStd;
            out.ref = &m_refInfo;
        }
        return out;
    }

private:
    std::int32_t qp() const { return std::clamp(28, m_caps.minQp, m_caps.maxQp); }

    VkVideoEncodeH265ProfileInfoKHR m_h265Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoEncodeH265CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR};
    VkVideoEncodeH265SessionParametersGetInfoKHR m_getInfo{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR};
    bool m_sao = false;
    StdVideoH265ShortTermRefPicSet m_rps{};
    StdVideoEncodeH265ReferenceListsInfo m_lists{};
    StdVideoEncodeH265PictureInfo m_pic{};
    StdVideoEncodeH265SliceSegmentHeader m_slice{};
    VkVideoEncodeH265NaluSliceSegmentInfoKHR m_nalu{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR};
    VkVideoEncodeH265PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR};
    StdVideoEncodeH265ReferenceInfo m_setupStd{}, m_refStd{};
    VkVideoEncodeH265DpbSlotInfoKHR m_setupInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
    VkVideoEncodeH265DpbSlotInfoKHR m_refInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
};

} // namespace

CodecHooks h265Hooks() {
    CodecHooks h;
    h.key = "h265";
    h.label = "H.265";
    h.decodeOp = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    h.encodeOp = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    h.decodeExtension = VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME;
    h.encodeExtension = VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME;
    h.reference = [](std::span<const std::uint8_t> data, std::string* error) {
        return makeDecode(h265::split(data, nullptr, error), error);
    };
    h.encoders = [](VkPhysicalDevice) {
        std::vector<std::unique_ptr<EncodeCodec>> v;
        v.push_back(std::make_unique<H265Encode>());
        return v;
    };
    h.encoded = [](const std::vector<std::uint8_t>& params,
                   const std::vector<std::vector<std::uint8_t>>& frames,
                   std::string* error) -> std::unique_ptr<DecodeCodec> {
        auto ps = h265::parse(params, error);
        if (!ps) return nullptr;
        std::vector<std::uint8_t> all;
        for (const auto& f : frames)
            all.insert(all.end(), f.begin(), f.end());
        return makeDecode(h265::split(all, &*ps, error), error);
    };
    return h;
}

} // namespace gt::bench::vkvideo

#endif // GT_HAS_VULKAN
