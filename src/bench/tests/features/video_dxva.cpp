// DXVA picture parameters, inverse quantization matrices and slice / tile entries for feature.video's
// Direct3D 12 decoder, built from the parsed streams (gt/bench/h264_params.h, h265_params.h, av1_params.h).
// The structures and their meaning are those of Microsoft's DXVA specifications for H.264, HEVC and AV1;
// DXVA picture indices are the frames' slices in the decoder's texture array (frame f = index f).

#include "video_d3d12.h"

#if GT_HAS_D3D12

#include "gt/bench/av1_params.h"
#include "gt/bench/h264_params.h"
#include "gt/bench/h265_params.h"

#include <directx/d3d12video.h>
#include <dxva.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace gt::bench::d3d12video {
namespace {

constexpr std::uint8_t kInvalidEntry = 0xFF;

template <typename T>
void append(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template <typename T>
std::vector<std::uint8_t> bytes(const T& v) {
    std::vector<std::uint8_t> out;
    append(out, v);
    return out;
}
/// A value for an n-bit DXVA bit-field of type T (explicitly narrowed).
template <typename T>
T field(std::uint32_t v, unsigned bits) {
    return static_cast<T>(v & ((1u << bits) - 1u));
}
template <typename T>
T zeroed() {
    T v;
    std::memset(&v, 0, sizeof(T)); // DXVA structures are plain C data with reserved fields that must be 0
    return v;
}

/// Short-format slice entries: each slice NAL unit with its start code, in order.
template <typename Slice>
std::vector<std::uint8_t> shortSlices(const std::vector<std::uint32_t>& starts, std::size_t size) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        Slice s = zeroed<Slice>();
        s.BSNALunitDataLocation = starts[i];
        s.SliceBytesInBuffer = static_cast<UINT>((i + 1 < starts.size() ? starts[i + 1] : size) - starts[i]);
        s.wBadSliceChopping = 0; // the whole slice is in the buffer
        append(out, s);
    }
    return out;
}

/// Shifts frame data and its short-format slice entries by `bytes` inserted at the start; the first entry
/// grows over the inserted bytes instead of moving when `first` is set.
template <typename Slice>
void shiftSlices(std::vector<std::uint8_t>& slices, UINT bytes, bool first) {
    for (std::size_t at = 0; at + sizeof(Slice) <= slices.size(); at += sizeof(Slice)) {
        Slice s;
        std::memcpy(&s, slices.data() + at, sizeof(Slice));
        if (at == 0 && first)
            s.SliceBytesInBuffer += bytes;
        else
            s.BSNALunitDataLocation += bytes;
        std::memcpy(slices.data() + at, &s, sizeof(Slice));
    }
}

const char* h264ProfileName(std::uint32_t idc) {
    switch (idc) {
    case 66:
        return "Baseline";
    case 77:
        return "Main";
    case 100:
        return "High";
    default:
        return "other profile";
    }
}

} // namespace

// --- H.264 -----------------------------------------------------------------------------------------

std::optional<DxvaStream> dxvaH264(std::span<const std::uint8_t> annexB, std::string* error) {
    auto st = h264::split(annexB, nullptr, error);
    if (!st) return std::nullopt;
    const h264::Sps& sps = *st->params.sps;
    const h264::Pps& pps = *st->params.pps;
    if (!sps.frameMbsOnly || sps.chromaFormatIdc != 1 || sps.bitDepthLumaMinus8 || sps.bitDepthChromaMinus8) {
        if (error) *error = "not 8-bit 4:2:0 progressive";
        return std::nullopt;
    }
    if (st->pictures.size() > video::kGop) st->pictures.resize(video::kGop);
    const auto refs = h264::slidingWindow(*st);

    DxvaStream out;
    out.name = std::format("H.264 {}", h264ProfileName(sps.profileIdc));
    out.profile = D3D12_VIDEO_DECODE_PROFILE_H264;
    out.width = (sps.widthInMbsMinus1 + 1) * 16;
    out.height = (sps.heightInMapUnitsMinus1 + 1) * 16;

    for (const h264::NalUnit& n : h264::nalUnits(annexB)) {
        if (n.type == 1 || n.type == 5) break; // the parameter sets ahead of the first slice
        if (n.type == 7 || n.type == 8)
            out.headers.insert(out.headers.end(), annexB.begin() + static_cast<std::ptrdiff_t>(n.offset),
                               annexB.begin() + static_cast<std::ptrdiff_t>(n.end));
    }

    DXVA_Qmatrix_H264 flat = zeroed<DXVA_Qmatrix_H264>(); // no scaling matrices: Flat_4x4_16 / Flat_8x8_16
    std::memset(&flat, 16, sizeof(flat));

    for (std::uint32_t f = 0; f < st->pictures.size(); ++f) {
        const h264::Picture& pic = st->pictures[f];
        const h264::SliceHeader& h = pic.header;
        DXVA_PicParams_H264 pp = zeroed<DXVA_PicParams_H264>();
        pp.wFrameWidthInMbsMinus1 = static_cast<USHORT>(sps.widthInMbsMinus1);
        pp.wFrameHeightInMbsMinus1 = static_cast<USHORT>(sps.heightInMapUnitsMinus1); // frames only
        pp.CurrPic.Index7Bits = static_cast<UCHAR>(f);
        pp.CurrPic.AssociatedFlag = 0; // a frame
        pp.num_ref_frames = static_cast<UCHAR>(sps.maxNumRefFrames);
        pp.field_pic_flag = 0;
        pp.MbaffFrameFlag = 0;
        pp.residual_colour_transform_flag = 0;
        pp.sp_for_switch_flag = 0;
        pp.chroma_format_idc = 1;
        pp.RefPicFlag = h.nalRefIdc != 0;
        pp.constrained_intra_pred_flag = pps.constrainedIntraPred;
        pp.weighted_pred_flag = pps.weightedPred;
        pp.weighted_bipred_idc = field<USHORT>(pps.weightedBipredIdc, 2);
        pp.MbsConsecutiveFlag = 1; // no slice groups
        pp.frame_mbs_only_flag = 1;
        pp.transform_8x8_mode_flag = pps.transform8x8Mode;
        pp.MinLumaBipredSize8x8Flag = sps.levelIdc >= 31; // Table A-1
        pp.IntraPicFlag = h.intra();
        pp.bit_depth_luma_minus8 = 0;
        pp.bit_depth_chroma_minus8 = 0;
        pp.Reserved16Bits = 3; // the current DXVA H.264 specification's conformance
        pp.StatusReportFeedbackNumber = f + 1;
        for (auto& e : pp.RefFrameList)
            e.bPicEntry = kInvalidEntry;
        for (std::size_t i = 0; i < refs[f].size() && i < 16; ++i) {
            const h264::Picture& r = st->pictures[refs[f][i]];
            pp.RefFrameList[i].Index7Bits = static_cast<UCHAR>(refs[f][i]);
            pp.RefFrameList[i].AssociatedFlag = 0; // short-term
            pp.FieldOrderCntList[i][0] = r.poc[0];
            pp.FieldOrderCntList[i][1] = r.poc[1];
            pp.FrameNumList[i] = static_cast<USHORT>(r.header.frameNum);
            pp.UsedForReferenceFlags |= 3u << (2 * i); // both fields
        }
        pp.CurrFieldOrderCnt[0] = pic.poc[0];
        pp.CurrFieldOrderCnt[1] = pic.poc[1];
        pp.pic_init_qs_minus26 = static_cast<CHAR>(pps.picInitQsMinus26);
        pp.chroma_qp_index_offset = static_cast<CHAR>(pps.chromaQpIndexOffset);
        pp.second_chroma_qp_index_offset = static_cast<CHAR>(pps.secondChromaQpIndexOffset);
        pp.ContinuationFlag = 1;
        pp.pic_init_qp_minus26 = static_cast<CHAR>(pps.picInitQpMinus26);
        pp.num_ref_idx_l0_active_minus1 = static_cast<UCHAR>(pps.numRefIdxL0DefaultMinus1);
        pp.num_ref_idx_l1_active_minus1 = static_cast<UCHAR>(pps.numRefIdxL1DefaultMinus1);
        pp.NonExistingFrameFlags = 0;
        pp.frame_num = static_cast<USHORT>(h.frameNum);
        pp.log2_max_frame_num_minus4 = static_cast<UCHAR>(sps.log2MaxFrameNumMinus4);
        pp.pic_order_cnt_type = static_cast<UCHAR>(sps.pocType);
        pp.log2_max_pic_order_cnt_lsb_minus4 = static_cast<UCHAR>(sps.log2MaxPocLsbMinus4);
        pp.delta_pic_order_always_zero_flag = 0; // POC type 1 is not supported by the reader
        pp.direct_8x8_inference_flag = sps.direct8x8Inference;
        pp.entropy_coding_mode_flag = pps.entropyCodingMode;
        pp.pic_order_present_flag = pps.bottomFieldPicOrderInFramePresent;
        pp.num_slice_groups_minus1 = 0;
        pp.slice_group_map_type = 0;
        pp.deblocking_filter_control_present_flag = pps.deblockingFilterControlPresent;
        pp.redundant_pic_cnt_present_flag = pps.redundantPicCntPresent;

        DxvaFrame fr;
        fr.data = pic.data;
        fr.picParams = bytes(pp);
        fr.qmatrix = bytes(flat);
        fr.slices = shortSlices<DXVA_Slice_H264_Short>(pic.slices, pic.data.size());
        out.frames.push_back(std::move(fr));
    }
    return out;
}

// --- H.265 -----------------------------------------------------------------------------------------

std::optional<DxvaStream> dxvaH265(std::span<const std::uint8_t> annexB, std::string* error) {
    auto st = h265::split(annexB, nullptr, error);
    if (!st) return std::nullopt;
    const h265::Sps& sps = *st->params.sps;
    const h265::Pps& pps = *st->params.pps;
    if (sps.chromaFormatIdc != 1 || sps.bitDepthLumaMinus8 || sps.bitDepthChromaMinus8 ||
        sps.separateColourPlane) {
        if (error) *error = "not 8-bit 4:2:0";
        return std::nullopt;
    }
    if (sps.conformanceWindow && (sps.confLeft || sps.confTop)) {
        if (error) *error = "cropping at the top or left is not supported";
        return std::nullopt;
    }
    if (st->pictures.size() > video::kGop) st->pictures.resize(video::kGop);
    const auto refs = h265::references(*st, error);
    if (!refs) return std::nullopt;

    DxvaStream out;
    out.name = sps.ptl.profileIdc == 1 ? "H.265 Main" : "H.265 other profile";
    out.profile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
    out.width = sps.width;
    out.height = sps.height;
    for (const h265::NalUnit& n : h265::nalUnits(annexB)) {
        if (n.vcl()) break;
        if (n.type >= 32 && n.type <= 34) // VPS, SPS, PPS
            out.headers.insert(out.headers.end(), annexB.begin() + static_cast<std::ptrdiff_t>(n.offset),
                               annexB.begin() + static_cast<std::ptrdiff_t>(n.end));
    }
    const std::uint32_t log2MinCb = sps.log2MinCbMinus3 + 3;

    for (std::uint32_t f = 0; f < st->pictures.size(); ++f) {
        const h265::Picture& pic = st->pictures[f];
        const h265::SliceHeader& h = pic.header;
        DXVA_PicParams_HEVC pp = zeroed<DXVA_PicParams_HEVC>();
        pp.PicWidthInMinCbsY = static_cast<USHORT>(sps.width >> log2MinCb);
        pp.PicHeightInMinCbsY = static_cast<USHORT>(sps.height >> log2MinCb);
        pp.chroma_format_idc = 1;
        pp.separate_colour_plane_flag = 0;
        pp.bit_depth_luma_minus8 = 0;
        pp.bit_depth_chroma_minus8 = 0;
        pp.log2_max_pic_order_cnt_lsb_minus4 = field<USHORT>(sps.log2MaxPocLsbMinus4, 4);
        pp.NoPicReorderingFlag = 0;
        pp.NoBiPredFlag = 0;
        pp.CurrPic.Index7Bits = static_cast<UCHAR>(f);
        pp.CurrPic.AssociatedFlag = 0;
        pp.sps_max_dec_pic_buffering_minus1 =
            static_cast<UCHAR>(sps.dpb.maxDecPicBufferingMinus1[sps.maxSubLayersMinus1]);
        pp.log2_min_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2MinCbMinus3);
        pp.log2_diff_max_min_luma_coding_block_size = static_cast<UCHAR>(sps.log2DiffMaxMinCb);
        pp.log2_min_transform_block_size_minus2 = static_cast<UCHAR>(sps.log2MinTbMinus2);
        pp.log2_diff_max_min_transform_block_size = static_cast<UCHAR>(sps.log2DiffMaxMinTb);
        pp.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(sps.maxTransformHierarchyDepthInter);
        pp.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(sps.maxTransformHierarchyDepthIntra);
        pp.num_short_term_ref_pic_sets = static_cast<UCHAR>(sps.rps.size());
        pp.num_long_term_ref_pics_sps = 0;
        pp.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(pps.numRefIdxL0DefaultMinus1);
        pp.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(pps.numRefIdxL1DefaultMinus1);
        pp.init_qp_minus26 = static_cast<CHAR>(pps.initQpMinus26);
        pp.ucNumDeltaPocsOfRefRpsIdx =
            static_cast<UCHAR>(h.rps.interPrediction ? h.rps.refRpsNumDeltaPocs : 0);
        pp.wNumBitsForShortTermRPSInSlice = static_cast<USHORT>(h.numBitsForStRefPicSet);

        pp.scaling_list_enabled_flag = 0; // the reader refuses scaling lists
        pp.amp_enabled_flag = sps.ampEnabled;
        pp.sample_adaptive_offset_enabled_flag = sps.saoEnabled;
        pp.pcm_enabled_flag = sps.pcmEnabled;
        pp.pcm_sample_bit_depth_luma_minus1 = field<UINT32>(sps.pcmBitDepthLumaMinus1, 4);
        pp.pcm_sample_bit_depth_chroma_minus1 = field<UINT32>(sps.pcmBitDepthChromaMinus1, 4);
        pp.log2_min_pcm_luma_coding_block_size_minus3 = field<UINT32>(sps.log2MinPcmCbMinus3, 2);
        pp.log2_diff_max_min_pcm_luma_coding_block_size = field<UINT32>(sps.log2DiffMaxMinPcmCb, 2);
        pp.pcm_loop_filter_disabled_flag = sps.pcmLoopFilterDisabled;
        pp.long_term_ref_pics_present_flag = sps.longTermRefPicsPresent;
        pp.sps_temporal_mvp_enabled_flag = sps.temporalMvpEnabled;
        pp.strong_intra_smoothing_enabled_flag = sps.strongIntraSmoothing;
        pp.dependent_slice_segments_enabled_flag = pps.dependentSliceSegmentsEnabled;
        pp.output_flag_present_flag = pps.outputFlagPresent;
        pp.num_extra_slice_header_bits = field<UINT32>(pps.numExtraSliceHeaderBits, 3);
        pp.sign_data_hiding_enabled_flag = pps.signDataHiding;
        pp.cabac_init_present_flag = pps.cabacInitPresent;

        pp.constrained_intra_pred_flag = pps.constrainedIntraPred;
        pp.transform_skip_enabled_flag = pps.transformSkipEnabled;
        pp.cu_qp_delta_enabled_flag = pps.cuQpDeltaEnabled;
        pp.pps_slice_chroma_qp_offsets_present_flag = pps.sliceChromaQpOffsetsPresent;
        pp.weighted_pred_flag = pps.weightedPred;
        pp.weighted_bipred_flag = pps.weightedBipred;
        pp.transquant_bypass_enabled_flag = pps.transquantBypassEnabled;
        pp.tiles_enabled_flag = pps.tilesEnabled;
        pp.entropy_coding_sync_enabled_flag = pps.entropyCodingSync;
        pp.uniform_spacing_flag = pps.tilesEnabled && pps.uniformSpacing;
        pp.loop_filter_across_tiles_enabled_flag = pps.tilesEnabled && pps.loopFilterAcrossTiles;
        pp.pps_loop_filter_across_slices_enabled_flag = pps.loopFilterAcrossSlices;
        pp.deblocking_filter_override_enabled_flag = pps.deblockingOverrideEnabled;
        pp.pps_deblocking_filter_disabled_flag = pps.deblockingDisabled;
        pp.lists_modification_present_flag = pps.listsModificationPresent;
        pp.slice_segment_header_extension_present_flag = pps.sliceSegmentHeaderExtensionPresent;
        pp.IrapPicFlag = h.irap();
        pp.IdrPicFlag = h.idr();
        pp.IntraPicFlag = h.irap();
        pp.pps_cb_qp_offset = static_cast<CHAR>(pps.cbQpOffset);
        pp.pps_cr_qp_offset = static_cast<CHAR>(pps.crQpOffset);
        if (pps.tilesEnabled) {
            pp.num_tile_columns_minus1 = static_cast<UCHAR>(pps.numTileColumnsMinus1);
            pp.num_tile_rows_minus1 = static_cast<UCHAR>(pps.numTileRowsMinus1);
            if (!pps.uniformSpacing) {
                for (std::size_t i = 0; i < pps.columnWidthMinus1.size() && i < 19; ++i)
                    pp.column_width_minus1[i] = static_cast<USHORT>(pps.columnWidthMinus1[i]);
                for (std::size_t i = 0; i < pps.rowHeightMinus1.size() && i < 21; ++i)
                    pp.row_height_minus1[i] = static_cast<USHORT>(pps.rowHeightMinus1[i]);
            }
        }
        pp.diff_cu_qp_delta_depth = static_cast<UCHAR>(pps.diffCuQpDeltaDepth);
        pp.pps_beta_offset_div2 = static_cast<CHAR>(pps.betaOffsetDiv2);
        pp.pps_tc_offset_div2 = static_cast<CHAR>(pps.tcOffsetDiv2);
        pp.log2_parallel_merge_level_minus2 = static_cast<UCHAR>(pps.log2ParallelMergeLevelMinus2);
        pp.CurrPicOrderCntVal = pic.poc;

        // RefPicList holds every picture of the set (used now or kept for later); the RefPicSet* lists index
        // it.
        for (auto& e : pp.RefPicList)
            e.bPicEntry = kInvalidEntry;
        std::memset(pp.RefPicSetStCurrBefore, kInvalidEntry, sizeof(pp.RefPicSetStCurrBefore));
        std::memset(pp.RefPicSetStCurrAfter, kInvalidEntry, sizeof(pp.RefPicSetStCurrAfter));
        std::memset(pp.RefPicSetLtCurr, kInvalidEntry, sizeof(pp.RefPicSetLtCurr));
        std::uint32_t n = 0;
        auto add = [&](std::uint32_t frame) {
            pp.RefPicList[n].Index7Bits = static_cast<UCHAR>(frame);
            pp.RefPicList[n].AssociatedFlag = 0; // short-term
            pp.PicOrderCntValList[n] = st->pictures[frame].poc;
            return static_cast<UCHAR>(n++);
        };
        const h265::References& r = (*refs)[f];
        for (std::size_t i = 0; i < r.before.size(); ++i)
            pp.RefPicSetStCurrBefore[i] = add(r.before[i]);
        for (std::size_t i = 0; i < r.after.size(); ++i)
            pp.RefPicSetStCurrAfter[i] = add(r.after[i]);
        for (const std::uint32_t g : r.follow)
            add(g);
        pp.StatusReportFeedbackNumber = f + 1;

        DxvaFrame fr;
        fr.data = pic.data;
        fr.picParams = bytes(pp);
        fr.slices = shortSlices<DXVA_Slice_HEVC_Short>(pic.segments, pic.data.size());
        out.frames.push_back(std::move(fr));
    }
    return out;
}

// --- AV1 -------------------------------------------------------------------------------------------

std::optional<DxvaStream> dxvaAv1(std::span<const std::uint8_t> obus, std::string* error) {
#if defined(_DIRECTX_AV1_VA_) || defined(_MSC_VER) // in the Windows SDK's dxva.h since Windows 11
    auto st = av1::parse(obus, nullptr, error);
    if (!st) return std::nullopt;
    const av1::SequenceHeader& seq = st->seq;
    if (seq.bitDepth != 8 || seq.monoChrome || seq.subsamplingX != 1 || seq.subsamplingY != 1 ||
        seq.profile) {
        if (error) *error = "not 8-bit 4:2:0 (Main profile)";
        return std::nullopt;
    }
    if (st->frames.size() > video::kGop) st->frames.resize(video::kGop);
    for (const av1::Frame& f : st->frames) {
        const av1::FrameHeader& h = f.header;
        if (!h.showFrame || h.useSuperres || h.frameWidth != st->frames.front().header.frameWidth ||
            h.frameHeight != st->frames.front().header.frameHeight) {
            if (error) *error = "hidden frames, superres and frame size changes are not supported";
            return std::nullopt;
        }
        // DXVA wants each reference's global motion validity (AV1 7.11.3.6), which is not derived here.
        if (std::any_of(h.gmType.begin() + 1, h.gmType.end(), [](std::uint32_t t) { return t >= 2; })) {
            if (error) *error = "rotation / affine global motion is not supported on Direct3D 12";
            return std::nullopt;
        }
    }

    DxvaStream out;
    out.name = "AV1 Main";
    out.profile = D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
    out.width = st->frames.front().header.frameWidth;
    out.height = st->frames.front().header.frameHeight;
    const std::uint32_t sbShift = seq.use128x128Superblock ? 5 : 4; // superblock size in 4x4 mode-info units

    for (std::uint32_t f = 0; f < st->frames.size(); ++f) {
        const av1::Frame& fr = st->frames[f];
        const av1::FrameHeader& h = fr.header;
        const av1::TileInfo& t = h.tiles;
        DXVA_PicParams_AV1 pp = zeroed<DXVA_PicParams_AV1>();
        pp.width = h.frameWidth;
        pp.height = h.frameHeight;
        pp.max_width = seq.maxFrameWidthMinus1 + 1;
        pp.max_height = seq.maxFrameHeightMinus1 + 1;
        pp.CurrPicTextureIndex = static_cast<UCHAR>(f);
        pp.superres_denom = 8; // SUPERRES_NUM: no superres
        pp.bitdepth = 8;
        pp.seq_profile = static_cast<UCHAR>(seq.profile);

        pp.tiles.cols = static_cast<UCHAR>(t.cols);
        pp.tiles.rows = static_cast<UCHAR>(t.rows);
        pp.tiles.context_update_id = static_cast<USHORT>(t.contextUpdateTileId);
        for (std::uint32_t i = 0; i < t.cols && i < 64; ++i)
            pp.tiles.widths[i] = static_cast<USHORT>(
                (t.miColStarts[i + 1] - t.miColStarts[i] + (1u << sbShift) - 1) >> sbShift);
        for (std::uint32_t i = 0; i < t.rows && i < 64; ++i)
            pp.tiles.heights[i] = static_cast<USHORT>(
                (t.miRowStarts[i + 1] - t.miRowStarts[i] + (1u << sbShift) - 1) >> sbShift);

        pp.coding.use_128x128_superblock = seq.use128x128Superblock;
        pp.coding.intra_edge_filter = seq.enableIntraEdgeFilter;
        pp.coding.interintra_compound = seq.enableInterintraCompound;
        pp.coding.masked_compound = seq.enableMaskedCompound;
        pp.coding.warped_motion = h.allowWarpedMotion;
        pp.coding.dual_filter = seq.enableDualFilter;
        pp.coding.jnt_comp = seq.enableJntComp;
        pp.coding.screen_content_tools = h.allowScreenContentTools;
        pp.coding.integer_mv = h.forceIntegerMv || h.frameIsIntra();
        pp.coding.cdef = seq.enableCdef;
        pp.coding.restoration = seq.enableRestoration;
        pp.coding.film_grain = 0;
        pp.coding.intrabc = h.allowIntrabc;
        pp.coding.high_precision_mv = h.allowHighPrecisionMv;
        pp.coding.switchable_motion_mode = h.isMotionModeSwitchable;
        pp.coding.filter_intra = seq.enableFilterIntra;
        pp.coding.disable_frame_end_update_cdf = h.disableFrameEndUpdateCdf;
        pp.coding.disable_cdf_update = h.disableCdfUpdate;
        pp.coding.reference_mode = h.referenceSelect;
        pp.coding.skip_mode = h.skipModePresent;
        pp.coding.reduced_tx_set = h.reducedTxSet;
        pp.coding.superres = 0;
        pp.coding.tx_mode = field<UINT>(h.txMode, 2);
        pp.coding.use_ref_frame_mvs = h.useRefFrameMvs;
        pp.coding.enable_ref_frame_mvs = seq.enableRefFrameMvs;
        pp.coding.reference_frame_update = 1;

        pp.format.frame_type = field<UCHAR>(h.frameType, 2);
        pp.format.show_frame = h.showFrame;
        pp.format.showable_frame = h.showableFrame;
        pp.format.subsampling_x = 1;
        pp.format.subsampling_y = 1;
        pp.format.mono_chrome = 0;

        pp.primary_ref_frame = static_cast<UCHAR>(h.primaryRefFrame);
        pp.order_hint = static_cast<UCHAR>(h.orderHint);
        pp.order_hint_bits = static_cast<UCHAR>(seq.enableOrderHint ? seq.orderHintBits : 0);
        for (std::uint32_t i = 0; i < 7; ++i) {
            DXVA_PicEntry_AV1& e = pp.frame_refs[i];
            const std::int32_t g = fr.refFrames[i];
            if (h.frameIsIntra() || g < 0) {
                e.Index = kInvalidEntry;
                continue;
            }
            const av1::FrameHeader& rh = st->frames[static_cast<std::size_t>(g)].header;
            e.width = rh.frameWidth;
            e.height = rh.frameHeight;
            e.Index = static_cast<UCHAR>(h.refFrameIdx[i]);
            e.wminvalid = 0; // identity / translation only (checked above)
            e.wmtype = field<UCHAR>(h.gmType[i + 1], 2);
            for (std::size_t j = 0; j < 6; ++j)
                e.wmmat[j] = h.gmParams[i + 1][j];
        }
        for (std::size_t v = 0; v < 8; ++v)
            pp.RefFrameMapTextureIndex[v] =
                fr.refFrameMap[v] >= 0 ? static_cast<UCHAR>(fr.refFrameMap[v]) : kInvalidEntry;

        pp.loop_filter.filter_level[0] = static_cast<UCHAR>(h.loopFilterLevel[0]);
        pp.loop_filter.filter_level[1] = static_cast<UCHAR>(h.loopFilterLevel[1]);
        pp.loop_filter.filter_level_u = static_cast<UCHAR>(h.loopFilterLevel[2]);
        pp.loop_filter.filter_level_v = static_cast<UCHAR>(h.loopFilterLevel[3]);
        pp.loop_filter.sharpness_level = static_cast<UCHAR>(h.loopFilterSharpness);
        pp.loop_filter.mode_ref_delta_enabled = h.loopFilterDeltaEnabled;
        pp.loop_filter.mode_ref_delta_update = h.loopFilterDeltaUpdate;
        pp.loop_filter.delta_lf_multi = h.deltaLfMulti;
        pp.loop_filter.delta_lf_present = h.deltaLfPresent;
        for (std::size_t i = 0; i < 8; ++i)
            pp.loop_filter.ref_deltas[i] = h.loopFilterRefDeltas[i];
        pp.loop_filter.mode_deltas[0] = h.loopFilterModeDeltas[0];
        pp.loop_filter.mode_deltas[1] = h.loopFilterModeDeltas[1];
        pp.loop_filter.delta_lf_res = static_cast<UCHAR>(h.deltaLfRes);
        for (std::size_t i = 0; i < 3; ++i) {
            pp.loop_filter.frame_restoration_type[i] = static_cast<UCHAR>(h.frameRestorationType[i]);
            const std::uint32_t size = h.loopRestorationSize[i];
            pp.loop_filter.log2_restoration_unit_size[i] =
                static_cast<USHORT>(h.usesLr && size ? std::bit_width(size) - 1 : 8);
        }

        pp.quantization.delta_q_present = h.deltaQPresent;
        pp.quantization.delta_q_res = field<UCHAR>(h.deltaQRes, 2);
        pp.quantization.base_qindex = static_cast<UCHAR>(h.baseQIdx);
        pp.quantization.y_dc_delta_q = static_cast<CHAR>(h.deltaQYDc);
        pp.quantization.u_dc_delta_q = static_cast<CHAR>(h.deltaQUDc);
        pp.quantization.v_dc_delta_q = static_cast<CHAR>(h.deltaQVDc);
        pp.quantization.u_ac_delta_q = static_cast<CHAR>(h.deltaQUAc);
        pp.quantization.v_ac_delta_q = static_cast<CHAR>(h.deltaQVAc);
        pp.quantization.qm_y = h.usingQmatrix ? static_cast<UCHAR>(h.qmY) : kInvalidEntry;
        pp.quantization.qm_u = h.usingQmatrix ? static_cast<UCHAR>(h.qmU) : kInvalidEntry;
        pp.quantization.qm_v = h.usingQmatrix ? static_cast<UCHAR>(h.qmV) : kInvalidEntry;

        pp.cdef.damping = field<UCHAR>(h.cdefDampingMinus3, 2);
        pp.cdef.bits = field<UCHAR>(h.cdefBits, 2);
        for (std::size_t i = 0; i < 8; ++i) {
            pp.cdef.y_strengths[i].primary = field<UCHAR>(h.cdefYPri[i], 6);
            pp.cdef.y_strengths[i].secondary = field<UCHAR>(h.cdefYSec[i], 2); // as coded
            pp.cdef.uv_strengths[i].primary = field<UCHAR>(h.cdefUvPri[i], 6);
            pp.cdef.uv_strengths[i].secondary = field<UCHAR>(h.cdefUvSec[i], 2);
        }
        pp.interp_filter = static_cast<UCHAR>(h.interpolationFilter);

        pp.segmentation.enabled = h.segmentationEnabled;
        pp.segmentation.update_map = h.segmentationUpdateMap;
        pp.segmentation.update_data = h.segmentationUpdateData;
        pp.segmentation.temporal_update = h.segmentationTemporalUpdate;
        for (std::size_t i = 0; i < 8; ++i) {
            pp.segmentation.feature_mask[i].mask = h.featureEnabled[i];
            for (std::size_t j = 0; j < 8; ++j)
                pp.segmentation.feature_data[i][j] = h.featureData[i][j];
        }
        pp.StatusReportFeedbackNumber = f + 1;

        DxvaFrame d;
        d.data = fr.data;
        d.picParams = bytes(pp);
        for (std::size_t i = 0; i < fr.tileOffsets.size(); ++i) {
            DXVA_Tile_AV1 tile = zeroed<DXVA_Tile_AV1>();
            tile.DataOffset = fr.tileOffsets[i];
            tile.DataSize = fr.tileSizes[i];
            tile.row = static_cast<USHORT>(i / t.cols);
            tile.column = static_cast<USHORT>(i % t.cols);
            tile.anchor_frame = kInvalidEntry; // no large-scale tiles
            append(d.slices, tile);
        }
        out.frames.push_back(std::move(d));
    }
    return out;
#else
    (void)obus;
    if (error) *error = "this build's dxva.h has no AV1 structures";
    return std::nullopt;
#endif
}

void inlineHeaders(DxvaStream& s, int mode) {
    if (s.headers.empty() || s.frames.empty() || (mode != 1 && mode != 2)) return;
    DxvaFrame& f = s.frames[0];
    f.data.insert(f.data.begin(), s.headers.begin(), s.headers.end());
    const auto bytes = static_cast<UINT>(s.headers.size());
    if (std::memcmp(&s.profile, &D3D12_VIDEO_DECODE_PROFILE_H264, sizeof(GUID)) == 0)
        shiftSlices<DXVA_Slice_H264_Short>(f.slices, bytes, mode == 2);
    else
        shiftSlices<DXVA_Slice_HEVC_Short>(f.slices, bytes, mode == 2);
}

} // namespace gt::bench::d3d12video

#endif // GT_HAS_D3D12
