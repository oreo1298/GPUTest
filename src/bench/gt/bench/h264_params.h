#pragma once
// Minimal H.264 parameter-set reader: finds the SPS and PPS NAL units in Annex B bytes and reads the fields a
// Vulkan Video decoder needs. Encoders may override what the application asked for, so feature.video decodes
// with what the encoder actually wrote. Scaling matrices and POC type 1 are not supported (the reader then
// fails and says why).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gt::bench::h264 {

struct Sps {
    std::uint32_t profileIdc = 0, levelIdc = 0, constraintFlags = 0; ///< constraint_set0..5 in bits 0..5
    std::uint32_t spsId = 0, chromaFormatIdc = 1, bitDepthLumaMinus8 = 0, bitDepthChromaMinus8 = 0;
    bool qpprimeYZeroTransformBypass = false;
    std::uint32_t log2MaxFrameNumMinus4 = 0, pocType = 0, log2MaxPocLsbMinus4 = 0;
    std::uint32_t maxNumRefFrames = 0;
    bool gapsInFrameNumAllowed = false;
    std::uint32_t widthInMbsMinus1 = 0, heightInMapUnitsMinus1 = 0;
    bool frameMbsOnly = true, mbAdaptiveFrameField = false, direct8x8Inference = false;
    bool frameCropping = false;
    std::uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    bool vuiPresent = false;
};

struct Pps {
    std::uint32_t ppsId = 0, spsId = 0;
    bool entropyCodingMode = false, bottomFieldPicOrderInFramePresent = false;
    std::uint32_t numRefIdxL0DefaultMinus1 = 0, numRefIdxL1DefaultMinus1 = 0;
    bool weightedPred = false;
    std::uint32_t weightedBipredIdc = 0;
    std::int32_t picInitQpMinus26 = 0, picInitQsMinus26 = 0, chromaQpIndexOffset = 0,
                 secondChromaQpIndexOffset = 0;
    bool deblockingFilterControlPresent = false, constrainedIntraPred = false, redundantPicCntPresent = false;
    bool transform8x8Mode = false;
};

struct ParameterSets {
    std::optional<Sps> sps;
    std::optional<Pps> pps;
};

/// Reads the first SPS and PPS in Annex B data. On failure returns nothing and fills `error`.
std::optional<ParameterSets> parse(std::span<const std::uint8_t> annexB, std::string* error);

/// One NAL unit in Annex B data: `offset` is where its start code (00 00 01) begins, `payload` the byte
/// after.
struct NalUnit {
    std::size_t offset = 0, payload = 0, end = 0;
    std::uint32_t type = 0, refIdc = 0;
};
std::vector<NalUnit> nalUnits(std::span<const std::uint8_t> annexB);

/// The start of a slice header (up to the picture order count fields), for picture-level decode parameters.
struct SliceHeader {
    std::uint32_t nalType = 0, nalRefIdc = 0, firstMb = 0, sliceType = 0, ppsId = 0;
    std::uint32_t frameNum = 0, idrPicId = 0, pocLsb = 0;
    std::int32_t deltaPocBottom = 0;
    bool idr() const { return nalType == 5; }
    bool intra() const { return sliceType % 5 == 2 || sliceType % 5 == 4; } ///< I or SI
};
/// Reads a slice NAL unit's header (`nal` starts at the NAL header byte). Frames only (no field coding).
std::optional<SliceHeader> parseSliceHeader(std::span<const std::uint8_t> nal, const ParameterSets& ps,
                                            std::string* error);

/// Derives TopFieldOrderCnt / BottomFieldOrderCnt for consecutive frames (POC types 0 and 2).
class PocCounter {
public:
    std::array<std::int32_t, 2> next(const SliceHeader& h, const Sps& sps);

private:
    std::int32_t m_prevMsb = 0, m_prevLsb = 0;
    std::uint32_t m_prevFrameNum = 0, m_frameNumOffset = 0;
};

/// One coded frame: its slice NAL units (each with a 00 00 01 start code) and what decoding it needs.
struct Picture {
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> slices; ///< offsets of the slices' start codes in `data`
    SliceHeader header;                ///< of the first slice
    std::array<std::int32_t, 2> poc{};
};
struct Stream {
    ParameterSets params;
    std::vector<Picture> pictures;
};
/// Splits Annex B data into frames (a frame starts at a slice with first_mb_in_slice 0). The parameter sets
/// are read from the data unless `params` is given (an encoder's own, for its slice-only output).
std::optional<Stream> split(std::span<const std::uint8_t> annexB, const ParameterSets* params,
                            std::string* error);

} // namespace gt::bench::h264
