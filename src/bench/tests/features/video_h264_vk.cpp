// H.264 for feature.video: decode jobs from parsed Annex B streams (gt/bench/h264_params.h) and an IDR +
// P-frame encoder at constant QP, both on Vulkan Video.

#include "video_vk.h"

#if GT_HAS_VULKAN

#include "gt/bench/h264_params.h"

#include <algorithm>
#include <format>

namespace gt::bench::vkvideo {
namespace {

StdVideoH264LevelIdc levelEnum(std::uint32_t levelIdc) {
    switch (levelIdc) {
    case 9:
    case 10:
        return STD_VIDEO_H264_LEVEL_IDC_1_0;
    case 11:
        return STD_VIDEO_H264_LEVEL_IDC_1_1;
    case 12:
        return STD_VIDEO_H264_LEVEL_IDC_1_2;
    case 13:
        return STD_VIDEO_H264_LEVEL_IDC_1_3;
    case 20:
        return STD_VIDEO_H264_LEVEL_IDC_2_0;
    case 21:
        return STD_VIDEO_H264_LEVEL_IDC_2_1;
    case 22:
        return STD_VIDEO_H264_LEVEL_IDC_2_2;
    case 30:
        return STD_VIDEO_H264_LEVEL_IDC_3_0;
    case 31:
        return STD_VIDEO_H264_LEVEL_IDC_3_1;
    case 32:
        return STD_VIDEO_H264_LEVEL_IDC_3_2;
    case 40:
        return STD_VIDEO_H264_LEVEL_IDC_4_0;
    case 41:
        return STD_VIDEO_H264_LEVEL_IDC_4_1;
    case 42:
        return STD_VIDEO_H264_LEVEL_IDC_4_2;
    case 50:
        return STD_VIDEO_H264_LEVEL_IDC_5_0;
    case 51:
        return STD_VIDEO_H264_LEVEL_IDC_5_1;
    case 52:
        return STD_VIDEO_H264_LEVEL_IDC_5_2;
    case 60:
        return STD_VIDEO_H264_LEVEL_IDC_6_0;
    case 61:
        return STD_VIDEO_H264_LEVEL_IDC_6_1;
    case 62:
        return STD_VIDEO_H264_LEVEL_IDC_6_2;
    default:
        return STD_VIDEO_H264_LEVEL_IDC_INVALID;
    }
}

const char* profileName(std::uint32_t idc) {
    switch (idc) {
    case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
        return "Baseline";
    case STD_VIDEO_H264_PROFILE_IDC_MAIN:
        return "Main";
    case STD_VIDEO_H264_PROFILE_IDC_HIGH:
        return "High";
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

// --- decode ----------------------------------------------------------------------------------------

class H264Decode final : public DecodeCodec {
public:
    explicit H264Decode(h264::Stream s) : m_s(std::move(s)) {
        const h264::Sps& sps = *m_s.params.sps;
        m_h264Profile.stdProfileIdc = static_cast<StdVideoH264ProfileIdc>(sps.profileIdc);
        m_h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, &m_h264Profile);
        m_refs = h264::slidingWindow(m_s);
        for (const auto& r : m_refs)
            m_maxRefs = std::max<std::uint32_t>(m_maxRefs, static_cast<std::uint32_t>(r.size()));
    }
    H264Decode(const H264Decode&) = delete;
    H264Decode& operator=(const H264Decode&) = delete;

    std::string name() const override {
        return std::format("H.264 {}", profileName(m_s.params.sps->profileIdc));
    }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities() const override {
        const std::uint32_t level = m_s.params.sps->levelIdc;
        if (levelEnum(level) == STD_VIDEO_H264_LEVEL_IDC_INVALID || levelEnum(level) > m_caps.maxLevelIdc)
            return std::format("the H.264 decoder does not support the stream's level {}.{}", level / 10,
                               level % 10);
        return {};
    }
    VkExtent2D codedExtent() const override {
        const h264::Sps& s = *m_s.params.sps;
        return {(s.widthInMbsMinus1 + 1) * 16, (s.heightInMapUnitsMinus1 + 1) * 16};
    }
    std::uint32_t frameCount() const override { return static_cast<std::uint32_t>(m_s.pictures.size()); }
    std::span<const std::uint8_t> frameData(std::uint32_t f) const override { return m_s.pictures[f].data; }
    std::uint32_t maxActiveReferences() const override { return m_maxRefs; }
    std::vector<std::uint32_t> references(std::uint32_t f) const override { return m_refs[f]; }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        // Parameter sets exactly as the stream carries them.
        const h264::Sps& s = *m_s.params.sps;
        const h264::Pps& p = *m_s.params.pps;
        StdVideoH264SequenceParameterSet sps{};
        sps.flags.constraint_set0_flag = s.constraintFlags & 1u;
        sps.flags.constraint_set1_flag = (s.constraintFlags >> 1) & 1u;
        sps.flags.constraint_set2_flag = (s.constraintFlags >> 2) & 1u;
        sps.flags.constraint_set3_flag = (s.constraintFlags >> 3) & 1u;
        sps.flags.constraint_set4_flag = (s.constraintFlags >> 4) & 1u;
        sps.flags.constraint_set5_flag = (s.constraintFlags >> 5) & 1u;
        sps.flags.direct_8x8_inference_flag = s.direct8x8Inference;
        sps.flags.mb_adaptive_frame_field_flag = s.mbAdaptiveFrameField;
        sps.flags.frame_mbs_only_flag = s.frameMbsOnly;
        sps.flags.gaps_in_frame_num_value_allowed_flag = s.gapsInFrameNumAllowed;
        sps.flags.qpprime_y_zero_transform_bypass_flag = s.qpprimeYZeroTransformBypass;
        sps.flags.frame_cropping_flag = s.frameCropping;
        sps.profile_idc = static_cast<StdVideoH264ProfileIdc>(s.profileIdc);
        sps.level_idc = levelEnum(s.levelIdc);
        sps.chroma_format_idc = static_cast<StdVideoH264ChromaFormatIdc>(s.chromaFormatIdc);
        sps.seq_parameter_set_id = static_cast<std::uint8_t>(s.spsId);
        sps.bit_depth_luma_minus8 = static_cast<std::uint8_t>(s.bitDepthLumaMinus8);
        sps.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(s.bitDepthChromaMinus8);
        sps.log2_max_frame_num_minus4 = static_cast<std::uint8_t>(s.log2MaxFrameNumMinus4);
        sps.pic_order_cnt_type = static_cast<StdVideoH264PocType>(s.pocType);
        sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(s.log2MaxPocLsbMinus4);
        sps.max_num_ref_frames = static_cast<std::uint8_t>(s.maxNumRefFrames);
        sps.pic_width_in_mbs_minus1 = s.widthInMbsMinus1;
        sps.pic_height_in_map_units_minus1 = s.heightInMapUnitsMinus1;
        sps.frame_crop_left_offset = s.cropLeft;
        sps.frame_crop_right_offset = s.cropRight;
        sps.frame_crop_top_offset = s.cropTop;
        sps.frame_crop_bottom_offset = s.cropBottom;
        StdVideoH264PictureParameterSet pps{};
        pps.flags.transform_8x8_mode_flag = p.transform8x8Mode;
        pps.flags.redundant_pic_cnt_present_flag = p.redundantPicCntPresent;
        pps.flags.constrained_intra_pred_flag = p.constrainedIntraPred;
        pps.flags.deblocking_filter_control_present_flag = p.deblockingFilterControlPresent;
        pps.flags.weighted_pred_flag = p.weightedPred;
        pps.flags.bottom_field_pic_order_in_frame_present_flag = p.bottomFieldPicOrderInFramePresent;
        pps.flags.entropy_coding_mode_flag = p.entropyCodingMode;
        pps.seq_parameter_set_id = static_cast<std::uint8_t>(p.spsId);
        pps.pic_parameter_set_id = static_cast<std::uint8_t>(p.ppsId);
        pps.num_ref_idx_l0_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL0DefaultMinus1);
        pps.num_ref_idx_l1_default_active_minus1 = static_cast<std::uint8_t>(p.numRefIdxL1DefaultMinus1);
        pps.weighted_bipred_idc = static_cast<StdVideoH264WeightedBipredIdc>(p.weightedBipredIdc);
        pps.pic_init_qp_minus26 = static_cast<std::int8_t>(p.picInitQpMinus26);
        pps.pic_init_qs_minus26 = static_cast<std::int8_t>(p.picInitQsMinus26);
        pps.chroma_qp_index_offset = static_cast<std::int8_t>(p.chromaQpIndexOffset);
        pps.second_chroma_qp_index_offset = static_cast<std::int8_t>(p.secondChromaQpIndexOffset);
        VkVideoDecodeH264SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoDecodeH264SessionParametersCreateInfoKHR h264{
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h264.maxStdSPSCount = 1;
        h264.maxStdPPSCount = 1;
        h264.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h264;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (H.264 decode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f) override {
        const h264::Picture& pic = m_s.pictures[f];
        const h264::SliceHeader& h = pic.header;
        m_pic = {};
        m_pic.flags.is_intra = h.intra();
        m_pic.flags.IdrPicFlag = h.idr();
        m_pic.flags.is_reference = h.nalRefIdc != 0;
        m_pic.seq_parameter_set_id = static_cast<std::uint8_t>(m_s.params.sps->spsId);
        m_pic.pic_parameter_set_id = static_cast<std::uint8_t>(m_s.params.pps->ppsId);
        m_pic.frame_num = static_cast<std::uint16_t>(h.frameNum);
        m_pic.idr_pic_id = static_cast<std::uint16_t>(h.idrPicId);
        m_pic.PicOrderCnt[0] = pic.poc[0];
        m_pic.PicOrderCnt[1] = pic.poc[1];
        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
        m_picInfo.pStdPictureInfo = &m_pic;
        m_picInfo.sliceCount = static_cast<std::uint32_t>(pic.slices.size());
        m_picInfo.pSliceOffsets = pic.slices.data();

        auto fill = [&](std::uint32_t frame, StdVideoDecodeH264ReferenceInfo& ref,
                        VkVideoDecodeH264DpbSlotInfoKHR& info) {
            const h264::Picture& p = m_s.pictures[frame];
            ref = {}; // a frame: neither field flag set
            ref.FrameNum = static_cast<std::uint16_t>(p.header.frameNum);
            ref.PicOrderCnt[0] = p.poc[0];
            ref.PicOrderCnt[1] = p.poc[1];
            info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
            info.pStdReferenceInfo = &ref;
        };
        fill(f, m_setupStd, m_setupInfo);
        const std::vector<std::uint32_t>& refs = m_refs[f];
        m_refStd.assign(refs.size(), {});
        m_refInfo.assign(refs.size(), {});
        FrameInfo out;
        out.picture = &m_picInfo;
        out.setup = &m_setupInfo;
        for (std::size_t i = 0; i < refs.size(); ++i) {
            fill(refs[i], m_refStd[i], m_refInfo[i]);
            out.refs.push_back(&m_refInfo[i]);
        }
        return out;
    }

private:
    h264::Stream m_s;
    std::vector<std::vector<std::uint32_t>> m_refs;
    std::uint32_t m_maxRefs = 0;
    VkVideoDecodeH264ProfileInfoKHR m_h264Profile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoDecodeH264CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    StdVideoDecodeH264PictureInfo m_pic{};
    VkVideoDecodeH264PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    StdVideoDecodeH264ReferenceInfo m_setupStd{};
    VkVideoDecodeH264DpbSlotInfoKHR m_setupInfo{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
    std::vector<StdVideoDecodeH264ReferenceInfo> m_refStd;
    std::vector<VkVideoDecodeH264DpbSlotInfoKHR> m_refInfo;
};

std::unique_ptr<DecodeCodec> makeDecode(std::optional<h264::Stream> s, std::string* error) {
    if (!s) return nullptr;
    const h264::Sps& sps = *s->params.sps;
    if (!sps.frameMbsOnly || sps.chromaFormatIdc != 1 || sps.bitDepthLumaMinus8 || sps.bitDepthChromaMinus8) {
        if (error) *error = "not 8-bit 4:2:0 progressive";
        return nullptr;
    }
    if (s->pictures.size() > kGop) s->pictures.resize(kGop);
    return std::make_unique<H264Decode>(std::move(*s));
}

// --- encode ----------------------------------------------------------------------------------------

class H264Encode final : public EncodeCodec {
public:
    explicit H264Encode(StdVideoH264ProfileIdc idc) {
        m_h264Profile.stdProfileIdc = idc;
        fillProfile(m_profile, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &m_h264Profile);
        m_getInfo.writeStdSPS = VK_TRUE;
        m_getInfo.writeStdPPS = VK_TRUE;
    }
    H264Encode(const H264Encode&) = delete;
    H264Encode& operator=(const H264Encode&) = delete;

    std::string name() const override {
        return std::format("H.264 {}", profileName(m_h264Profile.stdProfileIdc));
    }
    const VkVideoProfileInfoKHR& profile() const override { return m_profile; }
    void* capabilities() override { return &m_caps; }
    std::string checkCapabilities(const VkVideoCapabilitiesKHR&,
                                  const VkVideoEncodeCapabilitiesKHR&) const override {
        if (m_caps.maxLevelIdc < STD_VIDEO_H264_LEVEL_IDC_4_0)
            return "the H.264 encoder's highest level is below 4.0";
        if (m_caps.maxPPictureL0ReferenceCount < 1) return "the H.264 encoder cannot make P-frames";
        return {};
    }
    VkExtent2D codedExtent() const override { return {kImageWidth, kImageHeight}; }
    std::string quantizer() const override { return std::format("constant QP {}", qp()); }
    const void* parametersGetInfo() override { return &m_getInfo; }

    VkVideoSessionParametersKHR createParameters(VkDevice dev, VkVideoSessionKHR session) override {
        // 1920x1088 coded, cropped to 1080 lines, one reference frame, POC type 0.
        const VkVideoEncodeH264StdFlagsKHR syntax = m_caps.stdSyntaxFlags;
        const StdVideoH264ProfileIdc idc = m_h264Profile.stdProfileIdc;
        StdVideoH264SequenceParameterSet sps{};
        sps.flags.direct_8x8_inference_flag = 1;
        sps.flags.frame_mbs_only_flag = 1;
        sps.flags.frame_cropping_flag = 1;
        sps.profile_idc = idc;
        sps.level_idc = std::min(STD_VIDEO_H264_LEVEL_IDC_4_1, m_caps.maxLevelIdc);
        sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        sps.log2_max_frame_num_minus4 = 0;
        sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 2;
        sps.max_num_ref_frames = 1;
        sps.pic_width_in_mbs_minus1 = kImageWidth / 16 - 1;
        sps.pic_height_in_map_units_minus1 = kImageHeight / 16 - 1;
        sps.frame_crop_bottom_offset = (kImageHeight - kHeight) / 2; // in 2-line units for 4:2:0 frames
        StdVideoH264PictureParameterSet pps{};
        pps.flags.entropy_coding_mode_flag =
            idc != STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
            (syntax & VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR);
        pps.flags.transform_8x8_mode_flag =
            idc == STD_VIDEO_H264_PROFILE_IDC_HIGH &&
            (syntax & VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR);
        pps.weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;
        VkVideoEncodeH264SessionParametersAddInfoKHR add{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoEncodeH264SessionParametersCreateInfoKHR h264{
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
        h264.maxStdSPSCount = 1;
        h264.maxStdPPSCount = 1;
        h264.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pi{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pi.pNext = &h264;
        pi.videoSession = session;
        VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
        check(vkCreateVideoSessionParametersKHR(dev, &pi, nullptr, &params),
              "vkCreateVideoSessionParametersKHR (H.264 encode)");
        return params;
    }

    FrameInfo frameInfo(std::uint32_t f, bool constantQuantizer) override {
        // Frame f: frame_num f, POC 2f (also what POC type 2 would give); slot f % 2, reference slot (f + 1)
        // % 2.
        const bool idr = f == 0;
        const std::uint32_t refSlot = (f + 1) % 2;
        m_lists = {};
        std::fill(std::begin(m_lists.RefPicList0), std::end(m_lists.RefPicList0),
                  STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        std::fill(std::begin(m_lists.RefPicList1), std::end(m_lists.RefPicList1),
                  STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        m_lists.RefPicList0[0] = static_cast<std::uint8_t>(refSlot);
        m_pic = {};
        m_pic.flags.IdrPicFlag = idr;
        m_pic.flags.is_reference = 1;
        m_pic.primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        m_pic.frame_num = f;
        m_pic.PicOrderCnt = static_cast<std::int32_t>(2 * f);
        m_pic.pRefLists = idr ? nullptr : &m_lists;
        m_slice = {};
        m_slice.slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
        m_slice.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
        m_slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;
        m_nalu = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
        m_nalu.constantQp = constantQuantizer ? qp() : 0;
        m_nalu.pStdSliceHeader = &m_slice;
        m_picInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
        m_picInfo.naluSliceEntryCount = 1;
        m_picInfo.pNaluSliceEntries = &m_nalu;
        m_picInfo.pStdPictureInfo = &m_pic;

        m_setupStd = {};
        m_setupStd.primary_pic_type = m_pic.primary_pic_type;
        m_setupStd.FrameNum = f;
        m_setupStd.PicOrderCnt = m_pic.PicOrderCnt;
        m_setupInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
        m_setupInfo.pStdReferenceInfo = &m_setupStd;
        FrameInfo out;
        out.picture = &m_picInfo;
        out.setup = &m_setupInfo;
        if (!idr) {
            m_refStd = {};
            m_refStd.primary_pic_type =
                f == 1 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
            m_refStd.FrameNum = f - 1;
            m_refStd.PicOrderCnt = static_cast<std::int32_t>(2 * (f - 1));
            m_refInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
            m_refInfo.pStdReferenceInfo = &m_refStd;
            out.ref = &m_refInfo;
        }
        return out;
    }

private:
    std::int32_t qp() const { return std::clamp(26, m_caps.minQp, m_caps.maxQp); }

    VkVideoEncodeH264ProfileInfoKHR m_h264Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR m_profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoEncodeH264CapabilitiesKHR m_caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
    VkVideoEncodeH264SessionParametersGetInfoKHR m_getInfo{
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
    StdVideoEncodeH264ReferenceListsInfo m_lists{};
    StdVideoEncodeH264PictureInfo m_pic{};
    StdVideoEncodeH264SliceHeader m_slice{};
    VkVideoEncodeH264NaluSliceInfoKHR m_nalu{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
    VkVideoEncodeH264PictureInfoKHR m_picInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
    StdVideoEncodeH264ReferenceInfo m_setupStd{}, m_refStd{};
    VkVideoEncodeH264DpbSlotInfoKHR m_setupInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
    VkVideoEncodeH264DpbSlotInfoKHR m_refInfo{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
};

} // namespace

CodecHooks h264Hooks() {
    CodecHooks h;
    h.key = "h264";
    h.label = "H.264";
    h.decodeOp = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    h.encodeOp = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    h.decodeExtension = VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME;
    h.encodeExtension = VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
    h.reference = [](std::span<const std::uint8_t> data, std::string* error) {
        return makeDecode(h264::split(data, nullptr, error), error);
    };
    h.encoders = [](VkPhysicalDevice) {
        std::vector<std::unique_ptr<EncodeCodec>> v;
        for (const StdVideoH264ProfileIdc idc :
             {STD_VIDEO_H264_PROFILE_IDC_HIGH, STD_VIDEO_H264_PROFILE_IDC_MAIN,
              STD_VIDEO_H264_PROFILE_IDC_BASELINE})
            v.push_back(std::make_unique<H264Encode>(idc));
        return v;
    };
    h.encoded = [](const std::vector<std::uint8_t>& params,
                   const std::vector<std::vector<std::uint8_t>>& frames,
                   std::string* error) -> std::unique_ptr<DecodeCodec> {
        auto ps = h264::parse(params, error);
        if (!ps) return nullptr;
        std::vector<std::uint8_t> all;
        for (const auto& f : frames)
            all.insert(all.end(), f.begin(), f.end());
        return makeDecode(h264::split(all, &*ps, error), error);
    };
    return h;
}

} // namespace gt::bench::vkvideo

#endif // GT_HAS_VULKAN
