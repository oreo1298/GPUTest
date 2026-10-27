#pragma once
// AV1 reader for Vulkan Video decoding (AV1 spec sections 5 and 7.20): OBU framing, the sequence header, the
// uncompressed frame header with the reference state carried between frames (order hints, loop filter
// deltas, segmentation, global motion), and where each frame's tiles are. Streams with show_existing_frame,
// short reference signalling, film grain or large scale tiles fail with a reason.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gt::bench::av1 {

enum : std::uint32_t { kKeyFrame = 0, kInterFrame = 1, kIntraOnlyFrame = 2, kSwitchFrame = 3 };
enum : std::uint32_t { kPrimaryRefNone = 7 };

struct SequenceHeader {
    std::uint32_t profile = 0, levelIdx = 0, tier = 0; ///< level / tier of operating point 0
    bool stillPicture = false, reducedStillPictureHeader = false;
    bool timingInfoPresent = false, decoderModelInfoPresent = false, equalPictureInterval = false;
    bool initialDisplayDelayPresent = false;
    std::uint32_t numUnitsInDisplayTick = 0, timeScale = 0, numTicksPerPictureMinus1 = 0;
    std::uint32_t bufferRemovalTimeLengthMinus1 = 0, framePresentationTimeLengthMinus1 = 0;
    std::vector<std::uint32_t> operatingPointIdc;
    std::vector<bool> decoderModelPresentForThisOp;
    std::uint32_t frameWidthBitsMinus1 = 0, frameHeightBitsMinus1 = 0, maxFrameWidthMinus1 = 0,
                  maxFrameHeightMinus1 = 0;
    bool frameIdNumbersPresent = false;
    std::uint32_t deltaFrameIdLengthMinus2 = 0, additionalFrameIdLengthMinus1 = 0;
    bool use128x128Superblock = false, enableFilterIntra = false, enableIntraEdgeFilter = false,
         enableInterintraCompound = false, enableMaskedCompound = false, enableWarpedMotion = false,
         enableDualFilter = false, enableOrderHint = false, enableJntComp = false, enableRefFrameMvs = false;
    std::uint32_t seqForceScreenContentTools = 2, seqForceIntegerMv = 2, orderHintBits = 0;
    bool enableSuperres = false, enableCdef = false, enableRestoration = false;
    // color_config()
    std::uint32_t bitDepth = 8;
    bool monoChrome = false, colorDescriptionPresent = false, colorRange = false;
    std::uint32_t colorPrimaries = 2, transferCharacteristics = 2, matrixCoefficients = 2;
    std::uint32_t subsamplingX = 1, subsamplingY = 1, chromaSamplePosition = 0;
    bool separateUvDeltaQ = false;
    bool filmGrainParamsPresent = false;
    std::uint32_t numPlanes() const { return monoChrome ? 1 : 3; }
};

struct TileInfo {
    bool uniformSpacing = true;
    std::uint32_t cols = 1, rows = 1, colsLog2 = 0, rowsLog2 = 0;
    std::vector<std::uint16_t> miColStarts, miRowStarts; ///< cols + 1 / rows + 1 entries
    std::vector<std::uint16_t> widthInSbsMinus1, heightInSbsMinus1;
    std::uint32_t contextUpdateTileId = 0, tileSizeBytesMinus1 = 3;
};

struct FrameHeader {
    bool showExistingFrame = false;
    std::uint32_t frameType = kKeyFrame;
    bool showFrame = true, showableFrame = false, errorResilientMode = false, disableCdfUpdate = false;
    bool allowScreenContentTools = false, forceIntegerMv = false;
    std::uint32_t currentFrameId = 0;
    bool frameSizeOverride = false;
    std::uint32_t orderHint = 0, primaryRefFrame = kPrimaryRefNone, refreshFrameFlags = 0;
    bool bufferRemovalTimePresent = false;
    std::uint32_t frameWidth = 0, frameHeight = 0, upscaledWidth = 0, renderWidth = 0, renderHeight = 0;
    std::uint32_t miCols = 0, miRows = 0;
    bool renderAndFrameSizeDifferent = false, useSuperres = false;
    std::uint32_t codedDenom = 0;
    bool allowIntrabc = false, frameRefsShortSignaling = false;
    std::array<std::uint32_t, 7> refFrameIdx{};
    std::array<std::uint32_t, 8> expectedFrameId{};
    bool allowHighPrecisionMv = false, isFilterSwitchable = false;
    std::uint32_t interpolationFilter = 0;
    bool isMotionModeSwitchable = false, useRefFrameMvs = false;
    std::array<std::uint32_t, 8> orderHints{}; ///< by reference frame (INTRA_FRAME = this frame)
    std::array<bool, 8> refFrameSignBias{};
    bool disableFrameEndUpdateCdf = false;
    TileInfo tiles;
    // quantization_params()
    std::uint32_t baseQIdx = 0;
    std::int32_t deltaQYDc = 0, deltaQUDc = 0, deltaQUAc = 0, deltaQVDc = 0, deltaQVAc = 0;
    bool diffUvDelta = false, usingQmatrix = false;
    std::uint32_t qmY = 0, qmU = 0, qmV = 0;
    // segmentation_params()
    bool segmentationEnabled = false, segmentationUpdateMap = false, segmentationTemporalUpdate = false,
         segmentationUpdateData = false;
    std::array<std::uint8_t, 8> featureEnabled{}; ///< bit j of entry i = FeatureEnabled[i][j]
    std::array<std::array<std::int16_t, 8>, 8> featureData{};
    bool deltaQPresent = false, deltaLfPresent = false, deltaLfMulti = false;
    std::uint32_t deltaQRes = 0, deltaLfRes = 0;
    bool codedLossless = false, allLossless = false;
    // loop_filter_params()
    std::array<std::uint32_t, 4> loopFilterLevel{};
    std::uint32_t loopFilterSharpness = 0;
    bool loopFilterDeltaEnabled = false, loopFilterDeltaUpdate = false;
    std::uint32_t updateRefDelta = 0, updateModeDelta = 0; ///< bitmasks
    std::array<std::int8_t, 8> loopFilterRefDeltas{};
    std::array<std::int8_t, 2> loopFilterModeDeltas{};
    // cdef_params(): secondary strengths as coded (3 means 4)
    std::uint32_t cdefDampingMinus3 = 0, cdefBits = 0;
    std::array<std::uint8_t, 8> cdefYPri{}, cdefYSec{}, cdefUvPri{}, cdefUvSec{};
    // lr_params()
    std::array<std::uint32_t, 3> frameRestorationType{}; ///< 0 none, 1 Wiener, 2 self-guided, 3 switchable
    std::array<std::uint32_t, 3> loopRestorationSize{};  ///< in samples (0 without restoration)
    bool usesLr = false, usesChromaLr = false;
    std::uint32_t txMode = 0; ///< 0 only 4x4, 1 largest, 2 select
    bool referenceSelect = false, skipModePresent = false;
    std::array<std::uint32_t, 2> skipModeFrame{};
    bool allowWarpedMotion = false, reducedTxSet = false;
    std::array<std::uint32_t, 8> gmType{};
    std::array<std::array<std::int32_t, 6>, 8> gmParams{};
    bool frameIsIntra() const { return frameType == kKeyFrame || frameType == kIntraOnlyFrame; }
};

/// One decodable frame: its frame header and tile group OBUs, and where the header and tiles are in them.
struct Frame {
    std::vector<std::uint8_t> data;
    std::uint32_t frameHeaderOffset = 0;
    std::vector<std::uint32_t> tileOffsets, tileSizes;
    FrameHeader header;
    /// For each reference name LAST..ALTREF: the earlier frame (index in Stream::frames) it refers to.
    std::array<std::int32_t, 7> refFrames{-1, -1, -1, -1, -1, -1, -1};
};

struct Stream {
    SequenceHeader seq;
    std::vector<Frame> frames;
};

/// Reads the first sequence header in low-overhead OBU data.
std::optional<SequenceHeader> parseSequenceHeader(std::span<const std::uint8_t> obus, std::string* error);

/// `obus` with a size field in every OBU header (an OBU without one runs to the end of `obus`), so that
/// pieces written separately, such as each encode operation's output, can be joined. Throws
/// std::runtime_error on malformed data.
std::vector<std::uint8_t> withSizeFields(std::span<const std::uint8_t> obus);

/// Reads every frame of low-overhead OBU data (a temporal-unit stream). The sequence header is read from the
/// data unless `seq` is given (an encoder's own, for output without one).
std::optional<Stream> parse(std::span<const std::uint8_t> obus, const SequenceHeader* seq,
                            std::string* error);

} // namespace gt::bench::av1
