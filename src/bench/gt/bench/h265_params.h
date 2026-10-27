#pragma once
// Minimal H.265 reader for Vulkan Video decoding: VPS/SPS/PPS fields, slice segment headers up to the
// reference picture set, POC derivation and splitting Annex B data into pictures. Main / Main 10 streams
// without scaling lists, long-term references or extensions; anything else fails with a reason.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gt::bench::h265 {

struct ProfileTierLevel {
    std::uint32_t profileSpace = 0, profileIdc = 0, levelIdc = 0; ///< general_level_idc = 30 x level
    bool tier = false, progressiveSource = false, interlacedSource = false, nonPackedConstraint = false,
         frameOnlyConstraint = false;
};

/// sps/vps_max_dec_pic_buffering_minus1 etc. per sub-layer (filled for every sub-layer up to the maximum).
struct DecPicBufMgr {
    std::array<std::uint32_t, 7> maxDecPicBufferingMinus1{}, maxNumReorderPics{}, maxLatencyIncreasePlus1{};
};

struct Vps {
    std::uint32_t id = 0, maxSubLayersMinus1 = 0;
    bool temporalIdNesting = false, subLayerOrderingInfoPresent = false;
    ProfileTierLevel ptl;
    DecPicBufMgr dpb;
    bool timingInfoPresent = false, pocProportionalToTiming = false;
    std::uint32_t numUnitsInTick = 0, timeScale = 0, numTicksPocDiffOneMinus1 = 0;
};

/// A short-term reference picture set: the syntax (for Vulkan's parameter sets) and the derived lists.
struct ShortTermRps {
    bool interPrediction = false, deltaRpsSign = false;
    std::uint32_t deltaIdxMinus1 = 0, absDeltaRpsMinus1 = 0;
    std::uint32_t usedByCurrPicFlag = 0, useDeltaFlag = 0; ///< inter prediction syntax, bit j = entry j
    std::uint32_t refRpsNumDeltaPocs = 0;                  ///< NumDeltaPocs[RefRpsIdx] (inter prediction)
    // Derived (7.4.8): negative deltas in decreasing order, positive in increasing order.
    std::vector<std::int32_t> deltaPocS0, deltaPocS1;
    std::vector<bool> usedS0, usedS1;
    std::uint32_t numDeltaPocs() const {
        return static_cast<std::uint32_t>(deltaPocS0.size() + deltaPocS1.size());
    }
};

struct Sps {
    std::uint32_t vpsId = 0, maxSubLayersMinus1 = 0, id = 0;
    bool temporalIdNesting = false;
    ProfileTierLevel ptl;
    std::uint32_t chromaFormatIdc = 1;
    bool separateColourPlane = false;
    std::uint32_t width = 0, height = 0; ///< pic_width/height_in_luma_samples
    bool conformanceWindow = false;
    std::uint32_t confLeft = 0, confRight = 0, confTop = 0, confBottom = 0;
    std::uint32_t bitDepthLumaMinus8 = 0, bitDepthChromaMinus8 = 0, log2MaxPocLsbMinus4 = 0;
    bool subLayerOrderingInfoPresent = false;
    DecPicBufMgr dpb;
    std::uint32_t log2MinCbMinus3 = 0, log2DiffMaxMinCb = 0, log2MinTbMinus2 = 0, log2DiffMaxMinTb = 0;
    std::uint32_t maxTransformHierarchyDepthInter = 0, maxTransformHierarchyDepthIntra = 0;
    bool scalingListEnabled = false, ampEnabled = false, saoEnabled = false, pcmEnabled = false;
    std::uint32_t pcmBitDepthLumaMinus1 = 0, pcmBitDepthChromaMinus1 = 0, log2MinPcmCbMinus3 = 0,
                  log2DiffMaxMinPcmCb = 0;
    bool pcmLoopFilterDisabled = false;
    std::vector<ShortTermRps> rps; ///< num_short_term_ref_pic_sets entries
    bool longTermRefPicsPresent = false, temporalMvpEnabled = false, strongIntraSmoothing = false;
    bool vuiPresent = false; ///< the VUI is not read (decoding does not need it)
    std::uint32_t ctbSize() const { return 1u << (log2MinCbMinus3 + 3 + log2DiffMaxMinCb); }
};

struct Pps {
    std::uint32_t id = 0, spsId = 0;
    bool dependentSliceSegmentsEnabled = false, outputFlagPresent = false;
    std::uint32_t numExtraSliceHeaderBits = 0;
    bool signDataHiding = false, cabacInitPresent = false;
    std::uint32_t numRefIdxL0DefaultMinus1 = 0, numRefIdxL1DefaultMinus1 = 0;
    std::int32_t initQpMinus26 = 0;
    bool constrainedIntraPred = false, transformSkipEnabled = false, cuQpDeltaEnabled = false;
    std::uint32_t diffCuQpDeltaDepth = 0;
    std::int32_t cbQpOffset = 0, crQpOffset = 0;
    bool sliceChromaQpOffsetsPresent = false, weightedPred = false, weightedBipred = false,
         transquantBypassEnabled = false, tilesEnabled = false, entropyCodingSync = false;
    std::uint32_t numTileColumnsMinus1 = 0, numTileRowsMinus1 = 0;
    bool uniformSpacing = true, loopFilterAcrossTiles = true;
    std::vector<std::uint32_t> columnWidthMinus1, rowHeightMinus1;
    bool loopFilterAcrossSlices = false, deblockingControlPresent = false, deblockingOverrideEnabled = false,
         deblockingDisabled = false;
    std::int32_t betaOffsetDiv2 = 0, tcOffsetDiv2 = 0;
    bool listsModificationPresent = false;
    std::uint32_t log2ParallelMergeLevelMinus2 = 0;
    bool sliceSegmentHeaderExtensionPresent = false;
};

struct ParameterSets {
    std::optional<Vps> vps;
    std::optional<Sps> sps;
    std::optional<Pps> pps;
};

/// Reads the first VPS, SPS and PPS in Annex B data. On failure returns nothing and fills `error`.
std::optional<ParameterSets> parse(std::span<const std::uint8_t> annexB, std::string* error);

struct NalUnit {
    std::size_t offset = 0, payload = 0, end = 0; ///< start code, NAL header, end
    std::uint32_t type = 0, temporalId = 0;
    bool vcl() const { return type < 32; }
};
std::vector<NalUnit> nalUnits(std::span<const std::uint8_t> annexB);

/// A slice segment header up to slice_temporal_mvp_enabled_flag.
struct SliceHeader {
    std::uint32_t nalType = 0, temporalId = 0;
    bool firstSliceSegmentInPic = false, dependent = false;
    std::uint32_t ppsId = 0, sliceType = 0, pocLsb = 0;
    bool shortTermRefPicSetSps = false;
    std::uint32_t shortTermRefPicSetIdx = 0;
    ShortTermRps rps;                        ///< the slice's own set, or a copy of the SPS set it selects
    std::uint32_t numBitsForStRefPicSet = 0; ///< bits of the slice's st_ref_pic_set (0 when from the SPS)
    bool temporalMvpEnabled = false;
    bool idr() const { return nalType == 19 || nalType == 20; }
    bool irap() const { return nalType >= 16 && nalType <= 23; }
    bool reference() const { return !(nalType <= 14 && nalType % 2 == 0); } ///< not a sub-layer non-reference
};
std::optional<SliceHeader> parseSliceHeader(std::span<const std::uint8_t> nal, const ParameterSets& ps,
                                            std::string* error);

/// One coded picture: its slice segment NAL units (each with a 00 00 01 start code) and decode information.
struct Picture {
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> segments; ///< offsets of the slice segments' start codes in `data`
    SliceHeader header;                  ///< of the first slice segment
    std::int32_t poc = 0;                ///< PicOrderCntVal
};
struct Stream {
    ParameterSets params;
    std::vector<Picture> pictures;
};
/// Splits Annex B data into pictures. Parameter sets come from the data unless `params` is given.
std::optional<Stream> split(std::span<const std::uint8_t> annexB, const ParameterSets* params,
                            std::string* error);

/// A picture's short-term reference picture set resolved to earlier pictures (indices into Stream::pictures):
/// RefPicSetStCurrBefore, RefPicSetStCurrAfter, and RefPicSetStFoll (kept for later pictures).
struct References {
    std::vector<std::uint32_t> before, after, follow;
};
/// Resolves every picture's set. Fails if a picture it uses is missing (a StFoll picture may be missing).
std::optional<std::vector<References>> references(const Stream& s, std::string* error);

} // namespace gt::bench::h265
