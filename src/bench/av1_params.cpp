#include "gt/bench/av1_params.h"

#include "bit_reader.h"
#include "bit_writer.h"

#include <algorithm>
#include <format>

namespace gt::bench::av1 {
namespace {

using bits::Unsupported;
using Bits = bits::Reader;

enum : std::uint32_t {
    kObuSequenceHeader = 1,
    kObuTemporalDelimiter = 2,
    kObuFrameHeader = 3,
    kObuTileGroup = 4,
    kObuFrame = 6,
    kObuRedundantFrameHeader = 7,
    kObuTileList = 8,
};
constexpr std::uint32_t kRefsPerFrame = 7, kNumRefFrames = 8, kLastFrame = 1;
constexpr std::uint32_t kAltref = 7;
constexpr int kWarpedModelPrecBits = 16;
constexpr std::array<std::int8_t, 8> kDefaultRefDeltas = {1, 0, 0, 0, -1, 0, -1, -1};

// --- syntax helpers (AV1 section 4.10) ------------------------------------------------------------------

std::int32_t su(Bits& b, int n) {
    std::int32_t v = static_cast<std::int32_t>(b.u(n));
    const std::int32_t signMask = 1 << (n - 1);
    if (v & signMask) v -= 2 * signMask;
    return v;
}

std::uint32_t ns(Bits& b, std::uint32_t n) {
    int w = 0;
    for (std::uint32_t x = n; x; x >>= 1)
        ++w;
    const std::uint32_t m = (1u << w) - n;
    const std::uint32_t v = b.u(w - 1);
    if (v < m) return v;
    return (v << 1) - m + b.u(1);
}

std::uint32_t uvlc(Bits& b) {
    int zeros = 0;
    while (!b.flag()) {
        if (++zeros >= 32) return 0xFFFFFFFFu;
    }
    return zeros ? b.u(zeros) + ((1u << zeros) - 1u) : 0u;
}

std::uint32_t tileLog2(std::uint32_t blk, std::uint32_t target) {
    std::uint32_t k = 0;
    while ((blk << k) < target)
        ++k;
    return k;
}

std::int32_t inverseRecenter(std::int32_t r, std::int32_t v) {
    if (v > 2 * r) return v;
    if (v & 1) return r - ((v + 1) >> 1);
    return r + (v >> 1);
}

std::int32_t decodeSubexp(Bits& b, std::int32_t numSyms) {
    std::int32_t i = 0, mk = 0;
    const std::int32_t k = 3;
    while (true) {
        const std::int32_t b2 = i ? k + i - 1 : k;
        const std::int32_t a = 1 << b2;
        if (numSyms <= mk + 3 * a)
            return static_cast<std::int32_t>(ns(b, static_cast<std::uint32_t>(numSyms - mk))) + mk;
        if (b.flag()) {
            ++i;
            mk += a;
        } else {
            return static_cast<std::int32_t>(b.u(b2)) + mk;
        }
    }
}

std::int32_t decodeUnsignedSubexpWithRef(Bits& b, std::int32_t mx, std::int32_t r) {
    const std::int32_t v = decodeSubexp(b, mx);
    if ((r << 1) <= mx) return inverseRecenter(r, v);
    return mx - 1 - inverseRecenter(mx - 1 - r, v);
}

std::int32_t decodeSignedSubexpWithRef(Bits& b, std::int32_t low, std::int32_t high, std::int32_t r) {
    return decodeUnsignedSubexpWithRef(b, high - low, r - low) + low;
}

std::array<std::array<std::int32_t, 6>, 8> defaultGm() {
    std::array<std::array<std::int32_t, 6>, 8> g{};
    for (auto& p : g)
        for (int i = 0; i < 6; ++i)
            p[i] = (i % 3 == 2) ? (1 << kWarpedModelPrecBits) : 0;
    return g;
}

// --- sequence header (5.5) ----------------------------------------------------------------------------

SequenceHeader readSequenceHeader(Bits& b) {
    SequenceHeader s;
    s.profile = b.u(3);
    s.stillPicture = b.flag();
    s.reducedStillPictureHeader = b.flag();
    if (s.reducedStillPictureHeader) {
        s.operatingPointIdc = {0};
        s.decoderModelPresentForThisOp = {false};
        s.levelIdx = b.u(5);
        s.opLevelIdx = {s.levelIdx};
        s.opTier = {0};
    } else {
        s.timingInfoPresent = b.flag();
        std::uint32_t bufferDelayLength = 0;
        if (s.timingInfoPresent) {
            s.numUnitsInDisplayTick = b.u(32);
            s.timeScale = b.u(32);
            s.equalPictureInterval = b.flag();
            if (s.equalPictureInterval) s.numTicksPerPictureMinus1 = uvlc(b);
            s.decoderModelInfoPresent = b.flag();
            if (s.decoderModelInfoPresent) {
                bufferDelayLength = b.u(5) + 1;
                b.skip(32); // num_units_in_decoding_tick
                s.bufferRemovalTimeLengthMinus1 = b.u(5);
                s.framePresentationTimeLengthMinus1 = b.u(5);
            }
        }
        s.initialDisplayDelayPresent = b.flag();
        const std::uint32_t cnt = b.u(5) + 1;
        for (std::uint32_t i = 0; i < cnt; ++i) {
            s.operatingPointIdc.push_back(b.u(12));
            const std::uint32_t level = b.u(5);
            const std::uint32_t tier = level > 7 ? b.u(1) : 0;
            if (i == 0) {
                s.levelIdx = level;
                s.tier = tier;
            }
            s.opLevelIdx.push_back(level);
            s.opTier.push_back(tier);
            bool modelPresent = false;
            if (s.decoderModelInfoPresent) {
                modelPresent = b.flag();
                if (modelPresent) {
                    b.skip(bufferDelayLength); // decoder_buffer_delay
                    b.skip(bufferDelayLength); // encoder_buffer_delay
                    b.skip(1);                 // low_delay_mode_flag
                }
            }
            s.decoderModelPresentForThisOp.push_back(modelPresent);
            if (s.initialDisplayDelayPresent && b.flag()) b.skip(4);
        }
    }
    s.frameWidthBitsMinus1 = b.u(4);
    s.frameHeightBitsMinus1 = b.u(4);
    s.maxFrameWidthMinus1 = b.u(static_cast<int>(s.frameWidthBitsMinus1 + 1));
    s.maxFrameHeightMinus1 = b.u(static_cast<int>(s.frameHeightBitsMinus1 + 1));
    if (!s.reducedStillPictureHeader) s.frameIdNumbersPresent = b.flag();
    if (s.frameIdNumbersPresent) {
        s.deltaFrameIdLengthMinus2 = b.u(4);
        s.additionalFrameIdLengthMinus1 = b.u(3);
    }
    s.use128x128Superblock = b.flag();
    s.enableFilterIntra = b.flag();
    s.enableIntraEdgeFilter = b.flag();
    if (!s.reducedStillPictureHeader) {
        s.enableInterintraCompound = b.flag();
        s.enableMaskedCompound = b.flag();
        s.enableWarpedMotion = b.flag();
        s.enableDualFilter = b.flag();
        s.enableOrderHint = b.flag();
        if (s.enableOrderHint) {
            s.enableJntComp = b.flag();
            s.enableRefFrameMvs = b.flag();
        }
        s.seqForceScreenContentTools = b.flag() ? 2u : b.u(1); // seq_choose_screen_content_tools
        if (s.seqForceScreenContentTools > 0)
            s.seqForceIntegerMv = b.flag() ? 2u : b.u(1);
        else
            s.seqForceIntegerMv = 2;
        if (s.enableOrderHint) s.orderHintBits = b.u(3) + 1;
    }
    s.enableSuperres = b.flag();
    s.enableCdef = b.flag();
    s.enableRestoration = b.flag();
    // color_config()
    const bool highBitdepth = b.flag();
    if (s.profile == 2 && highBitdepth)
        s.bitDepth = b.flag() ? 12 : 10;
    else
        s.bitDepth = highBitdepth ? 10 : 8;
    s.monoChrome = s.profile == 1 ? false : b.flag();
    s.colorDescriptionPresent = b.flag();
    if (s.colorDescriptionPresent) {
        s.colorPrimaries = b.u(8);
        s.transferCharacteristics = b.u(8);
        s.matrixCoefficients = b.u(8);
    }
    if (s.monoChrome) {
        s.colorRange = b.flag();
        s.subsamplingX = s.subsamplingY = 1;
        s.chromaSamplePosition = 0;
        s.separateUvDeltaQ = false;
    } else {
        if (s.colorPrimaries == 1 && s.transferCharacteristics == 13 && s.matrixCoefficients == 0) {
            s.colorRange = true;
            s.subsamplingX = s.subsamplingY = 0;
        } else {
            s.colorRange = b.flag();
            if (s.profile == 0) {
                s.subsamplingX = s.subsamplingY = 1;
            } else if (s.profile == 1) {
                s.subsamplingX = s.subsamplingY = 0;
            } else if (s.bitDepth == 12) {
                s.subsamplingX = b.u(1);
                s.subsamplingY = s.subsamplingX ? b.u(1) : 0;
            } else {
                s.subsamplingX = 1;
                s.subsamplingY = 0;
            }
            if (s.subsamplingX && s.subsamplingY) s.chromaSamplePosition = b.u(2);
        }
        s.separateUvDeltaQ = b.flag();
    }
    s.filmGrainParamsPresent = b.flag();
    return s;
}

// --- frame header (5.9) with the reference state (7.20) ----------------------------------------------

/// What the decoding process saves per reference frame slot (VBI index).
struct RefState {
    bool valid = false;
    std::int32_t frame = -1; ///< index of the decoded frame in the slot
    std::uint32_t frameId = 0, frameType = 0, orderHint = 0;
    std::uint32_t upscaledWidth = 0, frameWidth = 0, frameHeight = 0, renderWidth = 0, renderHeight = 0;
    std::array<std::uint32_t, 8> savedOrderHints{};
    std::array<std::int8_t, 8> loopFilterRefDeltas = kDefaultRefDeltas;
    std::array<std::int8_t, 2> loopFilterModeDeltas{};
    std::array<std::uint8_t, 8> featureEnabled{};
    std::array<std::array<std::int16_t, 8>, 8> featureData{};
    std::array<std::array<std::int32_t, 6>, 8> gmParams = defaultGm();
};

class Parser {
public:
    explicit Parser(const SequenceHeader& seq) : m_seq(seq) {}

    /// uncompressed_header(); `temporalId` / `spatialId` come from the OBU extension header.
    FrameHeader frameHeader(Bits& b, std::uint32_t temporalId, std::uint32_t spatialId) {
        const SequenceHeader& s = m_seq;
        FrameHeader h;
        const std::uint32_t idLen =
            s.frameIdNumbersPresent ? s.additionalFrameIdLengthMinus1 + s.deltaFrameIdLengthMinus2 + 3 : 0;
        const std::uint32_t allFrames = (1u << kNumRefFrames) - 1;
        if (s.reducedStillPictureHeader) {
            h.frameType = kKeyFrame;
            h.showFrame = true;
        } else {
            h.showExistingFrame = b.flag();
            if (h.showExistingFrame) throw Unsupported("show_existing_frame is not supported");
            h.frameType = b.u(2);
            h.showFrame = b.flag();
            if (h.showFrame && s.decoderModelInfoPresent && !s.equalPictureInterval)
                b.skip(s.framePresentationTimeLengthMinus1 + 1); // temporal_point_info()
            h.showableFrame = h.showFrame ? h.frameType != kKeyFrame : b.flag();
            if (h.frameType == kSwitchFrame || (h.frameType == kKeyFrame && h.showFrame))
                h.errorResilientMode = true;
            else
                h.errorResilientMode = b.flag();
        }
        if (h.frameType == kKeyFrame && h.showFrame) {
            for (auto& r : m_refs) {
                r.valid = false;
                r.orderHint = 0;
            }
            m_orderHints = {};
        }
        h.disableCdfUpdate = b.flag();
        h.allowScreenContentTools =
            s.seqForceScreenContentTools == 2 ? b.flag() : s.seqForceScreenContentTools != 0;
        if (h.allowScreenContentTools)
            h.forceIntegerMv = s.seqForceIntegerMv == 2 ? b.flag() : s.seqForceIntegerMv != 0;
        if (h.frameIsIntra()) h.forceIntegerMv = true;
        if (s.frameIdNumbersPresent) {
            h.currentFrameId = b.u(static_cast<int>(idLen));
            // mark_ref_frames(): frames too far from the current id are no longer valid.
            const std::uint32_t diffLen = s.deltaFrameIdLengthMinus2 + 2;
            for (auto& r : m_refs) {
                if (h.currentFrameId > (1u << diffLen)) {
                    if (r.frameId > h.currentFrameId || r.frameId < h.currentFrameId - (1u << diffLen))
                        r.valid = false;
                } else if (r.frameId > h.currentFrameId &&
                           r.frameId < (1u << idLen) + h.currentFrameId - (1u << diffLen)) {
                    r.valid = false;
                }
            }
        }
        if (h.frameType == kSwitchFrame)
            h.frameSizeOverride = true;
        else if (!s.reducedStillPictureHeader)
            h.frameSizeOverride = b.flag();
        h.orderHint = b.u(static_cast<int>(s.orderHintBits));
        h.primaryRefFrame = (h.frameIsIntra() || h.errorResilientMode) ? kPrimaryRefNone : b.u(3);
        if (s.decoderModelInfoPresent) {
            h.bufferRemovalTimePresent = b.flag();
            if (h.bufferRemovalTimePresent)
                for (std::size_t op = 0; op < s.operatingPointIdc.size(); ++op) {
                    if (!s.decoderModelPresentForThisOp[op]) continue;
                    const std::uint32_t idc = s.operatingPointIdc[op];
                    const bool inTemporal = (idc >> temporalId) & 1u,
                               inSpatial = (idc >> (spatialId + 8)) & 1u;
                    if (idc == 0 || (inTemporal && inSpatial)) b.skip(s.bufferRemovalTimeLengthMinus1 + 1);
                }
        }
        h.refreshFrameFlags =
            (h.frameType == kSwitchFrame || (h.frameType == kKeyFrame && h.showFrame)) ? allFrames : b.u(8);
        if ((!h.frameIsIntra() || h.refreshFrameFlags != allFrames) && h.errorResilientMode &&
            s.enableOrderHint)
            for (std::uint32_t i = 0; i < kNumRefFrames; ++i) {
                RefState& r = m_refs[i];
                const std::uint32_t hint = b.u(static_cast<int>(s.orderHintBits));
                h.refOrderHint[i] = hint;
                if (hint != r.orderHint) {
                    r.valid = false;
                    r.orderHint = hint;
                }
            }
        if (h.frameIsIntra()) {
            frameSize(b, h);
            renderSize(b, h);
            if (h.allowScreenContentTools && h.upscaledWidth == h.frameWidth) h.allowIntrabc = b.flag();
        } else {
            if (s.enableOrderHint) h.frameRefsShortSignaling = b.flag();
            if (h.frameRefsShortSignaling) throw Unsupported("frame_refs_short_signaling is not supported");
            for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
                h.refFrameIdx[i] = b.u(3);
                if (s.frameIdNumbersPresent) {
                    const std::uint32_t delta = b.u(static_cast<int>(s.deltaFrameIdLengthMinus2 + 2)) + 1;
                    h.expectedFrameId[i + kLastFrame] =
                        (h.currentFrameId + (1u << idLen) - delta) % (1u << idLen);
                }
                if (!m_refs[h.refFrameIdx[i]].valid)
                    throw Unsupported("a frame references an empty reference slot");
            }
            if (h.frameSizeOverride && !h.errorResilientMode) {
                bool found = false;
                for (std::uint32_t i = 0; i < kRefsPerFrame && !found; ++i) {
                    if (!b.flag()) continue;
                    found = true;
                    h.foundRef = static_cast<std::int32_t>(i);
                    const RefState& r = m_refs[h.refFrameIdx[i]];
                    h.upscaledWidth = r.upscaledWidth;
                    h.frameWidth = h.upscaledWidth;
                    h.frameHeight = r.frameHeight;
                    h.renderWidth = r.renderWidth;
                    h.renderHeight = r.renderHeight;
                }
                if (!found) {
                    frameSize(b, h);
                    renderSize(b, h);
                } else {
                    superresParams(b, h);
                    computeImageSize(h);
                }
            } else {
                frameSize(b, h);
                renderSize(b, h);
            }
            h.allowHighPrecisionMv = h.forceIntegerMv ? false : b.flag();
            h.isFilterSwitchable = b.flag();
            h.interpolationFilter = h.isFilterSwitchable ? 4 : b.u(2);
            h.isMotionModeSwitchable = b.flag();
            h.useRefFrameMvs = (h.errorResilientMode || !s.enableRefFrameMvs) ? false : b.flag();
            for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
                const std::uint32_t hint = m_refs[h.refFrameIdx[i]].orderHint;
                m_orderHints[kLastFrame + i] = hint;
                h.refFrameSignBias[kLastFrame + i] = s.enableOrderHint && relativeDist(hint, h.orderHint) > 0;
            }
        }
        // OrderHints is decoder state: intra-only frames keep the previous frame's.
        h.orderHints = m_orderHints;
        h.orderHints[0] = h.orderHint;
        h.disableFrameEndUpdateCdf = (s.reducedStillPictureHeader || h.disableCdfUpdate) ? true : b.flag();

        // setup_past_independence() / load_previous()
        RefState prev;
        if (h.primaryRefFrame != kPrimaryRefNone) prev = m_refs[h.refFrameIdx[h.primaryRefFrame]];
        h.loopFilterRefDeltas = prev.loopFilterRefDeltas;
        h.loopFilterModeDeltas = prev.loopFilterModeDeltas;
        const auto prevGm = prev.gmParams;

        tileInfo(b, h);
        quantizationParams(b, h);
        segmentationParams(b, h, prev);
        // delta_q_params(), delta_lf_params()
        if (h.baseQIdx > 0) h.deltaQPresent = b.flag();
        if (h.deltaQPresent) {
            h.deltaQRes = b.u(2);
            if (!h.allowIntrabc) h.deltaLfPresent = b.flag();
            if (h.deltaLfPresent) {
                h.deltaLfRes = b.u(2);
                h.deltaLfMulti = b.flag();
            }
        }
        h.codedLossless = true;
        for (std::uint32_t seg = 0; seg < 8; ++seg) {
            std::int32_t q = static_cast<std::int32_t>(h.baseQIdx);
            if (h.segmentationEnabled && (h.featureEnabled[seg] & 1u))
                q = std::clamp(q + h.featureData[seg][0], 0, 255);
            const bool lossless = q == 0 && h.deltaQYDc == 0 && h.deltaQUAc == 0 && h.deltaQUDc == 0 &&
                                  h.deltaQVAc == 0 && h.deltaQVDc == 0;
            if (!lossless) h.codedLossless = false;
        }
        h.allLossless = h.codedLossless && h.frameWidth == h.upscaledWidth;
        loopFilterParams(b, h);
        cdefParams(b, h);
        lrParams(b, h);
        h.txMode = h.codedLossless ? 0 : (b.flag() ? 2 : 1);
        h.referenceSelect = h.frameIsIntra() ? false : b.flag();
        skipModeParams(b, h);
        h.allowWarpedMotion =
            (h.frameIsIntra() || h.errorResilientMode || !s.enableWarpedMotion) ? false : b.flag();
        h.reducedTxSet = b.flag();
        globalMotionParams(b, h, prevGm);
        if (s.filmGrainParamsPresent && (h.showFrame || h.showableFrame) && b.flag())
            throw Unsupported("film grain is not supported");
        return h;
    }

    /// decode_frame_wrapup(): the reference frame update process.
    void update(const FrameHeader& h, std::int32_t frame) {
        for (std::uint32_t i = 0; i < kNumRefFrames; ++i) {
            if (!((h.refreshFrameFlags >> i) & 1u)) continue;
            RefState& r = m_refs[i];
            r.valid = true;
            r.frame = frame;
            r.frameId = h.currentFrameId;
            r.frameType = h.frameType;
            r.orderHint = h.orderHint;
            r.upscaledWidth = h.upscaledWidth;
            r.frameWidth = h.frameWidth;
            r.frameHeight = h.frameHeight;
            r.renderWidth = h.renderWidth;
            r.renderHeight = h.renderHeight;
            r.savedOrderHints = h.orderHints;
            r.loopFilterRefDeltas = h.loopFilterRefDeltas;
            r.loopFilterModeDeltas = h.loopFilterModeDeltas;
            r.featureEnabled = h.featureEnabled;
            r.featureData = h.featureData;
            r.gmParams = h.gmParams;
        }
    }
    std::int32_t frameIn(std::uint32_t slot) const { return m_refs[slot].frame; }

private:
    std::int32_t relativeDist(std::uint32_t a, std::uint32_t b) const {
        if (!m_seq.enableOrderHint) return 0;
        const std::int32_t diff = static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b);
        const std::int32_t m = 1 << (m_seq.orderHintBits - 1);
        return (diff & (m - 1)) - (diff & m);
    }

    void superresParams(Bits& b, FrameHeader& h) {
        h.useSuperres = m_seq.enableSuperres ? b.flag() : false;
        std::uint32_t denom = 8;
        if (h.useSuperres) {
            h.codedDenom = b.u(3);
            denom = h.codedDenom + 9;
        }
        h.upscaledWidth = h.frameWidth;
        h.frameWidth = (h.upscaledWidth * 8 + denom / 2) / denom;
    }
    static void computeImageSize(FrameHeader& h) {
        h.miCols = 2 * ((h.frameWidth + 7) >> 3);
        h.miRows = 2 * ((h.frameHeight + 7) >> 3);
    }
    void frameSize(Bits& b, FrameHeader& h) {
        if (h.frameSizeOverride) {
            h.frameWidth = b.u(static_cast<int>(m_seq.frameWidthBitsMinus1 + 1)) + 1;
            h.frameHeight = b.u(static_cast<int>(m_seq.frameHeightBitsMinus1 + 1)) + 1;
        } else {
            h.frameWidth = m_seq.maxFrameWidthMinus1 + 1;
            h.frameHeight = m_seq.maxFrameHeightMinus1 + 1;
        }
        superresParams(b, h);
        computeImageSize(h);
    }
    static void renderSize(Bits& b, FrameHeader& h) {
        h.renderAndFrameSizeDifferent = b.flag();
        if (h.renderAndFrameSizeDifferent) {
            h.renderWidth = b.u(16) + 1;
            h.renderHeight = b.u(16) + 1;
        } else {
            h.renderWidth = h.upscaledWidth;
            h.renderHeight = h.frameHeight;
        }
    }

    void tileInfo(Bits& b, FrameHeader& h) {
        TileInfo& t = h.tiles;
        const bool sb128 = m_seq.use128x128Superblock;
        const std::uint32_t sbCols = sb128 ? (h.miCols + 31) >> 5 : (h.miCols + 15) >> 4;
        const std::uint32_t sbRows = sb128 ? (h.miRows + 31) >> 5 : (h.miRows + 15) >> 4;
        const std::uint32_t sbShift = sb128 ? 5 : 4, sbSize = sbShift + 2;
        const std::uint32_t maxTileWidthSb = 4096 >> sbSize;
        std::uint32_t maxTileAreaSb = (4096 * 2304) >> (2 * sbSize);
        const std::uint32_t minLog2TileCols = tileLog2(maxTileWidthSb, sbCols);
        const std::uint32_t maxLog2TileCols = tileLog2(1, std::min(sbCols, 64u));
        const std::uint32_t maxLog2TileRows = tileLog2(1, std::min(sbRows, 64u));
        const std::uint32_t minLog2Tiles =
            std::max(minLog2TileCols, tileLog2(maxTileAreaSb, sbRows * sbCols));
        t.uniformSpacing = b.flag();
        t.miColStarts.clear();
        t.miRowStarts.clear();
        t.widthInSbsMinus1.clear();
        t.heightInSbsMinus1.clear();
        if (t.uniformSpacing) {
            t.colsLog2 = minLog2TileCols;
            while (t.colsLog2 < maxLog2TileCols && b.flag())
                ++t.colsLog2;
            const std::uint32_t tileWidthSb = (sbCols + (1u << t.colsLog2) - 1) >> t.colsLog2;
            for (std::uint32_t start = 0; start < sbCols; start += tileWidthSb) {
                t.miColStarts.push_back(static_cast<std::uint16_t>(start << sbShift));
                t.widthInSbsMinus1.push_back(
                    static_cast<std::uint16_t>(std::min(tileWidthSb, sbCols - start) - 1));
            }
            const std::uint32_t minLog2TileRows = minLog2Tiles > t.colsLog2 ? minLog2Tiles - t.colsLog2 : 0;
            t.rowsLog2 = minLog2TileRows;
            while (t.rowsLog2 < maxLog2TileRows && b.flag())
                ++t.rowsLog2;
            const std::uint32_t tileHeightSb = (sbRows + (1u << t.rowsLog2) - 1) >> t.rowsLog2;
            for (std::uint32_t start = 0; start < sbRows; start += tileHeightSb) {
                t.miRowStarts.push_back(static_cast<std::uint16_t>(start << sbShift));
                t.heightInSbsMinus1.push_back(
                    static_cast<std::uint16_t>(std::min(tileHeightSb, sbRows - start) - 1));
            }
        } else {
            std::uint32_t widest = 0;
            for (std::uint32_t start = 0; start < sbCols;) {
                t.miColStarts.push_back(static_cast<std::uint16_t>(start << sbShift));
                const std::uint32_t size = ns(b, std::min(sbCols - start, maxTileWidthSb)) + 1;
                t.widthInSbsMinus1.push_back(static_cast<std::uint16_t>(size - 1));
                widest = std::max(widest, size);
                start += size;
            }
            t.colsLog2 = tileLog2(1, static_cast<std::uint32_t>(t.widthInSbsMinus1.size()));
            maxTileAreaSb = minLog2Tiles > 0 ? (sbRows * sbCols) >> (minLog2Tiles + 1) : sbRows * sbCols;
            const std::uint32_t maxTileHeightSb = std::max(maxTileAreaSb / widest, 1u);
            for (std::uint32_t start = 0; start < sbRows;) {
                t.miRowStarts.push_back(static_cast<std::uint16_t>(start << sbShift));
                const std::uint32_t size = ns(b, std::min(sbRows - start, maxTileHeightSb)) + 1;
                t.heightInSbsMinus1.push_back(static_cast<std::uint16_t>(size - 1));
                start += size;
            }
            t.rowsLog2 = tileLog2(1, static_cast<std::uint32_t>(t.heightInSbsMinus1.size()));
        }
        t.cols = static_cast<std::uint32_t>(t.widthInSbsMinus1.size());
        t.rows = static_cast<std::uint32_t>(t.heightInSbsMinus1.size());
        t.miColStarts.push_back(static_cast<std::uint16_t>(h.miCols));
        t.miRowStarts.push_back(static_cast<std::uint16_t>(h.miRows));
        if (t.colsLog2 > 0 || t.rowsLog2 > 0) {
            t.contextUpdateTileId = b.u(static_cast<int>(t.rowsLog2 + t.colsLog2));
            t.tileSizeBytesMinus1 = b.u(2);
        } else {
            t.contextUpdateTileId = 0;
        }
    }

    void quantizationParams(Bits& b, FrameHeader& h) {
        auto deltaQ = [&] { return b.flag() ? su(b, 7) : 0; };
        h.baseQIdx = b.u(8);
        h.deltaQYDc = deltaQ();
        if (m_seq.numPlanes() > 1) {
            h.diffUvDelta = m_seq.separateUvDeltaQ ? b.flag() : false;
            h.deltaQUDc = deltaQ();
            h.deltaQUAc = deltaQ();
            if (h.diffUvDelta) {
                h.deltaQVDc = deltaQ();
                h.deltaQVAc = deltaQ();
            } else {
                h.deltaQVDc = h.deltaQUDc;
                h.deltaQVAc = h.deltaQUAc;
            }
        }
        h.usingQmatrix = b.flag();
        if (h.usingQmatrix) {
            h.qmY = b.u(4);
            h.qmU = b.u(4);
            h.qmV = m_seq.separateUvDeltaQ ? b.u(4) : h.qmU;
        }
    }

    static void segmentationParams(Bits& b, FrameHeader& h, const RefState& prev) {
        static constexpr std::array<int, 8> kBits = {8, 6, 6, 6, 6, 3, 0, 0};
        static constexpr std::array<bool, 8> kSigned = {true, true, true, true, true, false, false, false};
        static constexpr std::array<int, 8> kMax = {255, 63, 63, 63, 63, 7, 0, 0};
        h.segmentationEnabled = b.flag();
        if (!h.segmentationEnabled) return; // features stay cleared
        if (h.primaryRefFrame == kPrimaryRefNone) {
            h.segmentationUpdateMap = true;
            h.segmentationTemporalUpdate = false;
            h.segmentationUpdateData = true;
        } else {
            h.segmentationUpdateMap = b.flag();
            if (h.segmentationUpdateMap) h.segmentationTemporalUpdate = b.flag();
            h.segmentationUpdateData = b.flag();
        }
        if (!h.segmentationUpdateData) {
            h.featureEnabled = prev.featureEnabled;
            h.featureData = prev.featureData;
            return;
        }
        for (std::uint32_t i = 0; i < 8; ++i)
            for (std::uint32_t j = 0; j < 8; ++j) {
                std::int32_t value = 0;
                if (b.flag()) {
                    h.featureEnabled[i] |= static_cast<std::uint8_t>(1u << j);
                    if (kSigned[j])
                        value = std::clamp(su(b, 1 + kBits[j]), -kMax[j], kMax[j]);
                    else
                        value = std::clamp(static_cast<std::int32_t>(b.u(kBits[j])), 0, kMax[j]);
                }
                h.featureData[i][j] = static_cast<std::int16_t>(value);
            }
    }

    void loopFilterParams(Bits& b, FrameHeader& h) {
        if (h.codedLossless || h.allowIntrabc) {
            h.loopFilterRefDeltas = kDefaultRefDeltas;
            h.loopFilterModeDeltas = {};
            return;
        }
        h.loopFilterLevel[0] = b.u(6);
        h.loopFilterLevel[1] = b.u(6);
        if (m_seq.numPlanes() > 1 && (h.loopFilterLevel[0] || h.loopFilterLevel[1])) {
            h.loopFilterLevel[2] = b.u(6);
            h.loopFilterLevel[3] = b.u(6);
        }
        h.loopFilterSharpness = b.u(3);
        h.loopFilterDeltaEnabled = b.flag();
        if (!h.loopFilterDeltaEnabled) return;
        h.loopFilterDeltaUpdate = b.flag();
        if (!h.loopFilterDeltaUpdate) return;
        for (std::uint32_t i = 0; i < 8; ++i)
            if (b.flag()) {
                h.updateRefDelta |= 1u << i;
                h.loopFilterRefDeltas[i] = static_cast<std::int8_t>(su(b, 7));
            }
        for (std::uint32_t i = 0; i < 2; ++i)
            if (b.flag()) {
                h.updateModeDelta |= 1u << i;
                h.loopFilterModeDeltas[i] = static_cast<std::int8_t>(su(b, 7));
            }
    }

    void cdefParams(Bits& b, FrameHeader& h) const {
        if (h.codedLossless || h.allowIntrabc || !m_seq.enableCdef) return; // all zero, damping 3
        h.cdefDampingMinus3 = b.u(2);
        h.cdefBits = b.u(2);
        for (std::uint32_t i = 0; i < (1u << h.cdefBits); ++i) {
            h.cdefYPri[i] = static_cast<std::uint8_t>(b.u(4));
            h.cdefYSec[i] = static_cast<std::uint8_t>(b.u(2));
            if (m_seq.numPlanes() > 1) {
                h.cdefUvPri[i] = static_cast<std::uint8_t>(b.u(4));
                h.cdefUvSec[i] = static_cast<std::uint8_t>(b.u(2));
            }
        }
    }

    void lrParams(Bits& b, FrameHeader& h) const {
        if (h.allLossless || h.allowIntrabc || !m_seq.enableRestoration) return;
        static constexpr std::array<std::uint32_t, 4> kRemap = {0, 3, 1,
                                                                2}; // NONE, SWITCHABLE, WIENER, SGRPROJ
        for (std::uint32_t i = 0; i < m_seq.numPlanes(); ++i) {
            h.frameRestorationType[i] = kRemap[b.u(2)];
            if (h.frameRestorationType[i] != 0) {
                h.usesLr = true;
                if (i > 0) h.usesChromaLr = true;
            }
        }
        if (!h.usesLr) return;
        std::uint32_t shift = b.u(1);
        if (m_seq.use128x128Superblock)
            ++shift;
        else if (shift)
            shift += b.u(1);
        h.loopRestorationSize[0] = 256u >> (2 - shift);
        const std::uint32_t uvShift =
            (m_seq.subsamplingX && m_seq.subsamplingY && h.usesChromaLr) ? b.u(1) : 0;
        h.loopRestorationSize[1] = h.loopRestorationSize[0] >> uvShift;
        h.loopRestorationSize[2] = h.loopRestorationSize[0] >> uvShift;
    }

    void skipModeParams(Bits& b, FrameHeader& h) const {
        bool allowed = false;
        if (!h.frameIsIntra() && h.referenceSelect && m_seq.enableOrderHint) {
            std::int32_t forwardIdx = -1, backwardIdx = -1;
            std::uint32_t forwardHint = 0, backwardHint = 0;
            for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
                const std::uint32_t refHint = m_refs[h.refFrameIdx[i]].orderHint;
                if (relativeDist(refHint, h.orderHint) < 0) {
                    if (forwardIdx < 0 || relativeDist(refHint, forwardHint) > 0) {
                        forwardIdx = static_cast<std::int32_t>(i);
                        forwardHint = refHint;
                    }
                } else if (relativeDist(refHint, h.orderHint) > 0) {
                    if (backwardIdx < 0 || relativeDist(refHint, backwardHint) < 0) {
                        backwardIdx = static_cast<std::int32_t>(i);
                        backwardHint = refHint;
                    }
                }
            }
            if (forwardIdx >= 0 && backwardIdx >= 0) {
                allowed = true;
                h.skipModeFrame = {kLastFrame + static_cast<std::uint32_t>(std::min(forwardIdx, backwardIdx)),
                                   kLastFrame +
                                       static_cast<std::uint32_t>(std::max(forwardIdx, backwardIdx))};
            } else if (forwardIdx >= 0) {
                std::int32_t secondIdx = -1;
                std::uint32_t secondHint = 0;
                for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
                    const std::uint32_t refHint = m_refs[h.refFrameIdx[i]].orderHint;
                    if (relativeDist(refHint, forwardHint) < 0 &&
                        (secondIdx < 0 || relativeDist(refHint, secondHint) > 0)) {
                        secondIdx = static_cast<std::int32_t>(i);
                        secondHint = refHint;
                    }
                }
                if (secondIdx >= 0) {
                    allowed = true;
                    h.skipModeFrame = {
                        kLastFrame + static_cast<std::uint32_t>(std::min(forwardIdx, secondIdx)),
                        kLastFrame + static_cast<std::uint32_t>(std::max(forwardIdx, secondIdx))};
                }
            }
        }
        h.skipModePresent = allowed ? b.flag() : false;
    }

    static void globalMotionParams(Bits& b, FrameHeader& h,
                                   const std::array<std::array<std::int32_t, 6>, 8>& prev) {
        h.gmParams = defaultGm();
        h.gmType = {};
        h.prevGmParams = prev;
        if (h.frameIsIntra()) return;
        enum { kIdentity = 0, kTranslation = 1, kRotZoom = 2, kAffine = 3 };
        for (std::uint32_t ref = kLastFrame; ref <= kAltref; ++ref) {
            std::uint32_t type = kIdentity;
            if (b.flag()) {
                if (b.flag())
                    type = kRotZoom;
                else
                    type = b.flag() ? kTranslation : kAffine;
            }
            h.gmType[ref] = type;
            auto param = [&](int idx) {
                int absBits = 12, precBits = 15;
                if (idx < 2) {
                    if (type == kTranslation) {
                        absBits = 9 - !h.allowHighPrecisionMv;
                        precBits = 3 - !h.allowHighPrecisionMv;
                    } else {
                        absBits = 12;
                        precBits = 6;
                    }
                }
                const int precDiff = kWarpedModelPrecBits - precBits;
                const std::int32_t round = (idx % 3) == 2 ? (1 << kWarpedModelPrecBits) : 0;
                const std::int32_t sub = (idx % 3) == 2 ? (1 << precBits) : 0;
                const std::int32_t mx = 1 << absBits;
                const std::int32_t r = (prev[ref][idx] >> precDiff) - sub;
                h.gmParams[ref][idx] = (decodeSignedSubexpWithRef(b, -mx, mx + 1, r) << precDiff) + round;
            };
            if (type >= kRotZoom) {
                param(2);
                param(3);
                if (type == kAffine) {
                    param(4);
                    param(5);
                } else {
                    h.gmParams[ref][4] = -h.gmParams[ref][3];
                    h.gmParams[ref][5] = h.gmParams[ref][2];
                }
            }
            if (type >= kTranslation) {
                param(0);
                param(1);
            }
        }
    }

    const SequenceHeader& m_seq;
    std::array<RefState, 8> m_refs{};
    std::array<std::uint32_t, 8> m_orderHints{};
};

struct Obu {
    std::uint32_t type = 0, temporalId = 0, spatialId = 0;
    std::size_t start = 0, payload = 0, end = 0; ///< OBU header, payload, end (offsets in the data)
};

std::vector<Obu> obus(std::span<const std::uint8_t> data) {
    std::vector<Obu> out;
    std::size_t i = 0;
    while (i < data.size()) {
        Obu o;
        o.start = i;
        const std::uint8_t h = data[i++];
        if (h & 0x80) throw Unsupported("forbidden bit set in an OBU header");
        o.type = (h >> 3) & 0xFu;
        const bool extension = h & 4u, hasSize = h & 2u;
        if (extension) {
            if (i >= data.size()) throw Unsupported("truncated OBU");
            o.temporalId = data[i] >> 5;
            o.spatialId = (data[i] >> 3) & 3u;
            ++i;
        }
        if (!hasSize) throw Unsupported("OBUs without size fields are not supported");
        std::uint64_t size = 0;
        for (int k = 0; k < 8; ++k) {
            if (i >= data.size()) throw Unsupported("truncated OBU size");
            const std::uint8_t byte = data[i++];
            size |= std::uint64_t(byte & 0x7F) << (7 * k);
            if (!(byte & 0x80)) break;
        }
        o.payload = i;
        if (size > data.size() - i) throw Unsupported("truncated OBU");
        o.end = i + static_cast<std::size_t>(size);
        i = o.end;
        out.push_back(o);
    }
    return out;
}

void appendLeb128(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (true) {
        const auto byte = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;
        out.push_back(static_cast<std::uint8_t>(v ? byte | 0x80u : byte));
        if (!v) break;
    }
}

} // namespace

std::vector<std::uint8_t> withSizeFields(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    out.reserve(data.size() + 16);
    std::size_t i = 0;
    while (i < data.size()) {
        const std::uint8_t h = data[i++];
        out.push_back(h | 2u); // obu_has_size_field
        if (h & 4u) {
            if (i >= data.size()) throw Unsupported("truncated OBU");
            out.push_back(data[i++]);
        }
        std::uint64_t size = data.size() - i;
        if (h & 2u) {
            size = 0;
            for (int k = 0; k < 8; ++k) {
                if (i >= data.size()) throw Unsupported("truncated OBU size");
                const std::uint8_t byte = data[i++];
                size |= std::uint64_t(byte & 0x7F) << (7 * k);
                if (!(byte & 0x80)) break;
            }
            if (size > data.size() - i) throw Unsupported("truncated OBU");
        }
        appendLeb128(out, size);
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
                   data.begin() + static_cast<std::ptrdiff_t>(i + size));
        i += static_cast<std::size_t>(size);
    }
    return out;
}

namespace {

/// tile_group_obu(): appends the tile positions; `payloadOffset` is where the OBU payload starts in the frame
/// data, `headerBits` how much of the payload precedes the tile group (the frame header of a frame OBU).
void tileGroup(std::span<const std::uint8_t> payload, std::size_t payloadOffset, const FrameHeader& h,
               Frame& f, std::size_t headerBits) {
    const TileInfo& t = h.tiles;
    const std::uint32_t numTiles = t.cols * t.rows;
    Bits b(std::vector<std::uint8_t>(payload.begin(), payload.end()), false);
    b.skip(headerBits);
    std::uint32_t tgStart = 0, tgEnd = numTiles - 1;
    if (numTiles > 1 && b.flag()) {
        const int tileBits = static_cast<int>(t.colsLog2 + t.rowsLog2);
        tgStart = b.u(tileBits);
        tgEnd = b.u(tileBits);
    }
    b.byteAlign();
    std::size_t pos = b.position() / 8;
    const std::uint32_t sizeBytes = t.tileSizeBytesMinus1 + 1;
    for (std::uint32_t tile = tgStart; tile <= tgEnd; ++tile) {
        std::size_t size = 0;
        if (tile == tgEnd) {
            size = payload.size() - pos;
        } else {
            if (pos + sizeBytes > payload.size()) throw Unsupported("truncated tile group");
            for (std::uint32_t k = 0; k < sizeBytes; ++k)
                size |= std::size_t(payload[pos + k]) << (8 * k);
            size += 1;
            pos += sizeBytes;
        }
        if (pos + size > payload.size()) throw Unsupported("truncated tile");
        f.tileOffsets.push_back(static_cast<std::uint32_t>(payloadOffset + pos));
        f.tileSizes.push_back(static_cast<std::uint32_t>(size));
        pos += size;
    }
}

} // namespace

std::optional<SequenceHeader> parseSequenceHeader(std::span<const std::uint8_t> data, std::string* error) {
    try {
        for (const Obu& o : obus(data))
            if (o.type == kObuSequenceHeader) {
                Bits b(std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(o.payload),
                                                 data.begin() + static_cast<std::ptrdiff_t>(o.end)),
                       false);
                return readSequenceHeader(b);
            }
        if (error) *error = "no sequence header";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return std::nullopt;
}

std::optional<Stream> parse(std::span<const std::uint8_t> data, const SequenceHeader* seq,
                            std::string* error) {
    Stream out;
    try {
        const std::vector<Obu> list = obus(data);
        if (seq) {
            out.seq = *seq;
        } else {
            auto s = parseSequenceHeader(data, error);
            if (!s) return std::nullopt;
            out.seq = *s;
        }
        if (out.seq.reducedStillPictureHeader || out.seq.stillPicture)
            throw Unsupported("still pictures are not supported");
        Parser parser(out.seq);
        Frame* current = nullptr; // the frame whose tile groups are still coming
        std::uint32_t tilesSeen = 0;
        auto finish = [&] {
            parser.update(current->header, static_cast<std::int32_t>(out.frames.size() - 1));
            current = nullptr;
        };
        for (const Obu& o : list) {
            if (o.type == kObuTileList) throw Unsupported("large scale tile lists are not supported");
            if (o.type != kObuFrameHeader && o.type != kObuFrame && o.type != kObuTileGroup) continue;
            const std::span<const std::uint8_t> payload = data.subspan(o.payload, o.end - o.payload);
            if (o.type == kObuFrameHeader || o.type == kObuFrame) {
                if (current) {
                    if (o.type == kObuFrameHeader) continue; // a copy of the current frame's header
                    throw Unsupported("a frame OBU arrived before the previous frame's tiles");
                }
                Frame f;
                // The reference names resolve to the frames in the slots before this frame updates them.
                Bits b(std::vector<std::uint8_t>(payload.begin(), payload.end()), false);
                f.header = parser.frameHeader(b, o.temporalId, o.spatialId);
                for (std::uint32_t v = 0; v < kNumRefFrames; ++v)
                    f.refFrameMap[v] = parser.frameIn(v);
                if (!f.header.frameIsIntra())
                    for (std::uint32_t i = 0; i < kRefsPerFrame; ++i)
                        f.refFrames[i] = parser.frameIn(f.header.refFrameIdx[i]);
                f.frameHeaderOffset = 0;
                f.data.assign(data.begin() + static_cast<std::ptrdiff_t>(o.start),
                              data.begin() + static_cast<std::ptrdiff_t>(o.end));
                out.frames.push_back(std::move(f));
                current = &out.frames.back();
                tilesSeen = 0;
                if (o.type == kObuFrame) {
                    b.byteAlign();
                    tileGroup(payload, o.payload - o.start, current->header, *current, b.position());
                    tilesSeen = static_cast<std::uint32_t>(current->tileSizes.size());
                }
            } else { // tile group OBU of the current frame
                if (!current) throw Unsupported("a tile group without a frame header");
                const std::size_t base = current->data.size();
                current->data.insert(current->data.end(), data.begin() + static_cast<std::ptrdiff_t>(o.start),
                                     data.begin() + static_cast<std::ptrdiff_t>(o.end));
                tileGroup(payload, base + (o.payload - o.start), current->header, *current, 0);
                tilesSeen = static_cast<std::uint32_t>(current->tileSizes.size());
            }
            if (current && tilesSeen >= current->header.tiles.cols * current->header.tiles.rows) finish();
        }
        if (current) throw Unsupported("the last frame is missing tiles");
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
    if (out.frames.empty()) {
        if (error) *error = "no frames";
        return std::nullopt;
    }
    return out;
}

// --- writing ---------------------------------------------------------------------------------------------

namespace {

using Out = bits::Writer;

void putSu(Out& w, int n, std::int32_t v) {
    w.u(n, static_cast<std::uint32_t>(v) & ((1u << n) - 1u));
}

void putNs(Out& w, std::uint32_t n, std::uint32_t v) {
    int width = 0;
    for (std::uint32_t x = n; x; x >>= 1)
        ++width;
    const std::uint32_t m = (1u << width) - n;
    if (v < m) {
        w.u(width - 1, v);
        return;
    }
    w.u(width - 1, (v + m) >> 1);
    w.u(1, (v + m) & 1u);
}

void putUvlc(Out& w, std::uint32_t v) {
    const std::uint64_t x = std::uint64_t(v) + 1;
    int zeros = 0;
    while ((x >> (zeros + 1)) != 0)
        ++zeros;
    w.u(zeros, 0);
    w.flag(true);
    w.u(zeros, static_cast<std::uint32_t>(x - (std::uint64_t(1) << zeros)));
}

/// The inverse of inverseRecenter().
std::int32_t recenter(std::int32_t r, std::int32_t v) {
    if (v > 2 * r) return v;
    if (v >= r) return (v - r) << 1;
    return ((r - v) << 1) - 1;
}

void putSubexp(Out& w, std::int32_t numSyms, std::int32_t v) {
    std::int32_t i = 0, mk = 0;
    const std::int32_t k = 3;
    while (true) {
        const std::int32_t b2 = i ? k + i - 1 : k;
        const std::int32_t a = 1 << b2;
        if (numSyms <= mk + 3 * a) {
            putNs(w, static_cast<std::uint32_t>(numSyms - mk), static_cast<std::uint32_t>(v - mk));
            return;
        }
        const bool more = v >= mk + a;
        w.flag(more);
        if (!more) {
            w.u(b2, static_cast<std::uint32_t>(v - mk));
            return;
        }
        ++i;
        mk += a;
    }
}

void putSignedSubexpWithRef(Out& w, std::int32_t low, std::int32_t high, std::int32_t r, std::int32_t v) {
    const std::int32_t mx = high - low;
    r -= low;
    v -= low;
    if ((r << 1) <= mx)
        putSubexp(w, mx, recenter(r, v));
    else
        putSubexp(w, mx, recenter(mx - 1 - r, mx - 1 - v));
}

std::vector<std::uint8_t> obu(std::uint32_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>((type << 3) | 2u)}; // obu_has_size_field
    appendLeb128(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// The superblock grid and tile limits of tile_info() (5.9.15).
struct TileGrid {
    std::uint32_t sbCols = 0, sbRows = 0, maxTileWidthSb = 0, maxTileAreaSb = 0;
    std::uint32_t minLog2TileCols = 0, maxLog2TileCols = 0, maxLog2TileRows = 0, minLog2Tiles = 0;
};

TileGrid tileGrid(const SequenceHeader& s, const FrameHeader& h) {
    TileGrid g;
    const bool sb128 = s.use128x128Superblock;
    const std::uint32_t miCols = 2 * ((h.frameWidth + 7) >> 3), miRows = 2 * ((h.frameHeight + 7) >> 3);
    g.sbCols = sb128 ? (miCols + 31) >> 5 : (miCols + 15) >> 4;
    g.sbRows = sb128 ? (miRows + 31) >> 5 : (miRows + 15) >> 4;
    const std::uint32_t sbSize = (sb128 ? 5 : 4) + 2;
    g.maxTileWidthSb = 4096 >> sbSize;
    g.maxTileAreaSb = (4096 * 2304) >> (2 * sbSize);
    g.minLog2TileCols = tileLog2(g.maxTileWidthSb, g.sbCols);
    g.maxLog2TileCols = tileLog2(1, std::min(g.sbCols, 64u));
    g.maxLog2TileRows = tileLog2(1, std::min(g.sbRows, 64u));
    g.minLog2Tiles = std::max(g.minLog2TileCols, tileLog2(g.maxTileAreaSb, g.sbRows * g.sbCols));
    return g;
}

/// Number of tiles the frame header's tile_info() describes.
std::uint32_t tileCount(const SequenceHeader& s, const FrameHeader& h) {
    const TileInfo& t = h.tiles;
    if (!t.uniformSpacing)
        return static_cast<std::uint32_t>(t.widthInSbsMinus1.size() * t.heightInSbsMinus1.size());
    const TileGrid g = tileGrid(s, h);
    const std::uint32_t tileWidthSb = (g.sbCols + (1u << t.colsLog2) - 1) >> t.colsLog2;
    const std::uint32_t tileHeightSb = (g.sbRows + (1u << t.rowsLog2) - 1) >> t.rowsLog2;
    return ((g.sbCols + tileWidthSb - 1) / tileWidthSb) * ((g.sbRows + tileHeightSb - 1) / tileHeightSb);
}

class HeaderWriter {
public:
    HeaderWriter(const SequenceHeader& s, const FrameHeader& h) : m_s(s), m_h(h) {}

    /// uncompressed_header() (5.9.2).
    void write(Out& w) {
        const SequenceHeader& s = m_s;
        const FrameHeader& h = m_h;
        if (s.reducedStillPictureHeader) throw Unsupported("reduced still-picture headers are not written");
        if (s.decoderModelInfoPresent) throw Unsupported("decoder model information is not written");
        if (h.showExistingFrame) throw Unsupported("show_existing_frame is not written");
        if (h.frameRefsShortSignaling) throw Unsupported("frame_refs_short_signaling is not written");
        const std::uint32_t idLen =
            s.frameIdNumbersPresent ? s.additionalFrameIdLengthMinus1 + s.deltaFrameIdLengthMinus2 + 3 : 0;
        const bool intra = h.frameIsIntra();
        const bool shownKey = h.frameType == kKeyFrame && h.showFrame;
        w.flag(false); // show_existing_frame
        w.u(2, h.frameType);
        w.flag(h.showFrame);
        if (!h.showFrame) w.flag(h.showableFrame);
        if (h.frameType != kSwitchFrame && !shownKey) w.flag(h.errorResilientMode);
        m_errorResilient = h.frameType == kSwitchFrame || shownKey || h.errorResilientMode;
        w.flag(h.disableCdfUpdate);
        if (s.seqForceScreenContentTools == 2) w.flag(h.allowScreenContentTools);
        m_screenContent =
            s.seqForceScreenContentTools == 2 ? h.allowScreenContentTools : s.seqForceScreenContentTools != 0;
        bool forceIntegerMv = false;
        if (m_screenContent) {
            if (s.seqForceIntegerMv == 2) w.flag(h.forceIntegerMv);
            forceIntegerMv = s.seqForceIntegerMv == 2 ? h.forceIntegerMv : s.seqForceIntegerMv != 0;
        }
        if (intra) forceIntegerMv = true;
        if (s.frameIdNumbersPresent) w.u(static_cast<int>(idLen), h.currentFrameId);
        if (h.frameType != kSwitchFrame) w.flag(h.frameSizeOverride);
        const bool sizeOverride = h.frameType == kSwitchFrame || h.frameSizeOverride;
        w.u(static_cast<int>(s.orderHintBits), h.orderHint);
        m_primaryRef = (intra || m_errorResilient) ? kPrimaryRefNone : h.primaryRefFrame;
        if (!intra && !m_errorResilient) w.u(3, h.primaryRefFrame);
        const std::uint32_t refresh =
            (h.frameType == kSwitchFrame || shownKey) ? (1u << kNumRefFrames) - 1 : h.refreshFrameFlags;
        if (h.frameType != kSwitchFrame && !shownKey) w.u(8, h.refreshFrameFlags);
        if ((!intra || refresh != (1u << kNumRefFrames) - 1) && m_errorResilient && s.enableOrderHint)
            for (std::uint32_t i = 0; i < kNumRefFrames; ++i)
                w.u(static_cast<int>(s.orderHintBits), h.refOrderHint[i]);
        if (intra) {
            frameSize(w, sizeOverride);
            renderSize(w);
            m_intrabc = m_screenContent && h.upscaledWidth == h.frameWidth && h.allowIntrabc;
            if (m_screenContent && h.upscaledWidth == h.frameWidth) w.flag(h.allowIntrabc);
        } else {
            if (s.enableOrderHint) w.flag(false); // frame_refs_short_signaling
            for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
                w.u(3, h.refFrameIdx[i]);
                if (s.frameIdNumbersPresent) {
                    const std::uint32_t delta =
                        (h.currentFrameId + (1u << idLen) - h.expectedFrameId[i + kLastFrame]) %
                        (1u << idLen);
                    w.u(static_cast<int>(s.deltaFrameIdLengthMinus2 + 2), delta - 1);
                }
            }
            if (sizeOverride && !m_errorResilient) {
                for (std::int32_t i = 0; i < static_cast<std::int32_t>(kRefsPerFrame); ++i) {
                    w.flag(i == h.foundRef);
                    if (i == h.foundRef) break;
                }
                if (h.foundRef < 0) {
                    frameSize(w, sizeOverride);
                    renderSize(w);
                } else {
                    superres(w);
                }
            } else {
                frameSize(w, sizeOverride);
                renderSize(w);
            }
            if (!forceIntegerMv) w.flag(h.allowHighPrecisionMv);
            m_highPrecisionMv = !forceIntegerMv && h.allowHighPrecisionMv;
            w.flag(h.isFilterSwitchable);
            if (!h.isFilterSwitchable) w.u(2, h.interpolationFilter);
            w.flag(h.isMotionModeSwitchable);
            if (!m_errorResilient && s.enableRefFrameMvs) w.flag(h.useRefFrameMvs);
        }
        if (!h.disableCdfUpdate) w.flag(h.disableFrameEndUpdateCdf);
        tileInfo(w);
        quantization(w);
        segmentation(w);
        const bool deltaQ = h.baseQIdx > 0 && h.deltaQPresent;
        if (h.baseQIdx > 0) w.flag(h.deltaQPresent);
        if (deltaQ) {
            w.u(2, h.deltaQRes);
            if (!m_intrabc) {
                w.flag(h.deltaLfPresent);
                if (h.deltaLfPresent) {
                    w.u(2, h.deltaLfRes);
                    w.flag(h.deltaLfMulti);
                }
            }
        }
        bool lossless = true;
        for (std::uint32_t seg = 0; seg < 8; ++seg) {
            std::int32_t q = static_cast<std::int32_t>(h.baseQIdx);
            if (h.segmentationEnabled && (h.featureEnabled[seg] & 1u))
                q = std::clamp(q + h.featureData[seg][0], 0, 255);
            if (q != 0 || h.deltaQYDc || h.deltaQUAc || h.deltaQUDc || h.deltaQVAc || h.deltaQVDc)
                lossless = false;
        }
        const bool allLossless = lossless && h.frameWidth == h.upscaledWidth;
        if (!lossless && !m_intrabc) loopFilter(w);
        if (!lossless && !m_intrabc && s.enableCdef) cdef(w);
        if (!allLossless && !m_intrabc && s.enableRestoration) loopRestoration(w);
        if (!lossless) w.flag(h.txMode == 2); // tx_mode_select
        if (!intra) w.flag(h.referenceSelect);
        if (skipModeAllowed()) w.flag(h.skipModePresent);
        if (!intra && !m_errorResilient && s.enableWarpedMotion) w.flag(h.allowWarpedMotion);
        w.flag(h.reducedTxSet);
        if (!intra) globalMotion(w);
        if (s.filmGrainParamsPresent && (h.showFrame || h.showableFrame)) w.flag(false); // apply_grain
    }

private:
    std::int32_t relativeDist(std::uint32_t a, std::uint32_t b) const {
        if (!m_s.enableOrderHint) return 0;
        const std::int32_t diff = static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b);
        const std::int32_t m = 1 << (m_s.orderHintBits - 1);
        return (diff & (m - 1)) - (diff & m);
    }

    void superres(Out& w) const {
        if (!m_s.enableSuperres) return;
        w.flag(m_h.useSuperres);
        if (m_h.useSuperres) w.u(3, m_h.codedDenom);
    }
    void frameSize(Out& w, bool sizeOverride) const {
        if (sizeOverride) {
            w.u(static_cast<int>(m_s.frameWidthBitsMinus1 + 1), m_h.upscaledWidth - 1);
            w.u(static_cast<int>(m_s.frameHeightBitsMinus1 + 1), m_h.frameHeight - 1);
        }
        superres(w);
    }
    void renderSize(Out& w) const {
        w.flag(m_h.renderAndFrameSizeDifferent);
        if (m_h.renderAndFrameSizeDifferent) {
            w.u(16, m_h.renderWidth - 1);
            w.u(16, m_h.renderHeight - 1);
        }
    }

    void tileInfo(Out& w) const {
        const TileInfo& t = m_h.tiles;
        const TileGrid g = tileGrid(m_s, m_h);
        std::uint32_t colsLog2 = t.colsLog2, rowsLog2 = t.rowsLog2;
        w.flag(t.uniformSpacing);
        if (t.uniformSpacing) {
            if (colsLog2 < g.minLog2TileCols || colsLog2 > g.maxLog2TileCols)
                throw Unsupported("the tile columns do not fit the frame");
            for (std::uint32_t i = g.minLog2TileCols; i < colsLog2; ++i)
                w.flag(true);
            if (colsLog2 < g.maxLog2TileCols) w.flag(false);
            const std::uint32_t minLog2TileRows = g.minLog2Tiles > colsLog2 ? g.minLog2Tiles - colsLog2 : 0;
            if (rowsLog2 < minLog2TileRows || rowsLog2 > g.maxLog2TileRows)
                throw Unsupported("the tile rows do not fit the frame");
            for (std::uint32_t i = minLog2TileRows; i < rowsLog2; ++i)
                w.flag(true);
            if (rowsLog2 < g.maxLog2TileRows) w.flag(false);
        } else {
            std::uint32_t start = 0, widest = 0;
            for (const std::uint16_t sizeMinus1 : t.widthInSbsMinus1) {
                if (start >= g.sbCols) throw Unsupported("the tile columns do not fit the frame");
                putNs(w, std::min(g.sbCols - start, g.maxTileWidthSb), sizeMinus1);
                widest = std::max<std::uint32_t>(widest, sizeMinus1 + 1u);
                start += sizeMinus1 + 1u;
            }
            if (start != g.sbCols) throw Unsupported("the tile columns do not cover the frame");
            const std::uint32_t maxTileAreaSb =
                g.minLog2Tiles > 0 ? (g.sbRows * g.sbCols) >> (g.minLog2Tiles + 1) : g.sbRows * g.sbCols;
            const std::uint32_t maxTileHeightSb = std::max(maxTileAreaSb / widest, 1u);
            start = 0;
            for (const std::uint16_t sizeMinus1 : t.heightInSbsMinus1) {
                if (start >= g.sbRows) throw Unsupported("the tile rows do not fit the frame");
                putNs(w, std::min(g.sbRows - start, maxTileHeightSb), sizeMinus1);
                start += sizeMinus1 + 1u;
            }
            if (start != g.sbRows) throw Unsupported("the tile rows do not cover the frame");
            colsLog2 = tileLog2(1, static_cast<std::uint32_t>(t.widthInSbsMinus1.size()));
            rowsLog2 = tileLog2(1, static_cast<std::uint32_t>(t.heightInSbsMinus1.size()));
        }
        if (colsLog2 > 0 || rowsLog2 > 0) {
            w.u(static_cast<int>(rowsLog2 + colsLog2), t.contextUpdateTileId);
            w.u(2, t.tileSizeBytesMinus1);
        }
    }

    void quantization(Out& w) const {
        const FrameHeader& h = m_h;
        auto deltaQ = [&](std::int32_t v) {
            w.flag(v != 0);
            if (v) putSu(w, 7, v);
        };
        w.u(8, h.baseQIdx);
        deltaQ(h.deltaQYDc);
        if (m_s.numPlanes() > 1) {
            const bool diff = m_s.separateUvDeltaQ && h.diffUvDelta;
            if (m_s.separateUvDeltaQ) w.flag(h.diffUvDelta);
            deltaQ(h.deltaQUDc);
            deltaQ(h.deltaQUAc);
            if (diff) {
                deltaQ(h.deltaQVDc);
                deltaQ(h.deltaQVAc);
            }
        }
        w.flag(h.usingQmatrix);
        if (h.usingQmatrix) {
            w.u(4, h.qmY);
            w.u(4, h.qmU);
            if (m_s.separateUvDeltaQ) w.u(4, h.qmV);
        }
    }

    void segmentation(Out& w) const {
        static constexpr std::array<int, 8> kBits = {8, 6, 6, 6, 6, 3, 0, 0};
        static constexpr std::array<bool, 8> kSigned = {true, true, true, true, true, false, false, false};
        const FrameHeader& h = m_h;
        w.flag(h.segmentationEnabled);
        if (!h.segmentationEnabled) return;
        bool updateData = true;
        if (m_primaryRef != kPrimaryRefNone) {
            w.flag(h.segmentationUpdateMap);
            if (h.segmentationUpdateMap) w.flag(h.segmentationTemporalUpdate);
            w.flag(h.segmentationUpdateData);
            updateData = h.segmentationUpdateData;
        }
        if (!updateData) return;
        for (std::uint32_t i = 0; i < 8; ++i)
            for (std::uint32_t j = 0; j < 8; ++j) {
                const bool enabled = (h.featureEnabled[i] >> j) & 1u;
                w.flag(enabled);
                if (!enabled) continue;
                if (kSigned[j])
                    putSu(w, 1 + kBits[j], h.featureData[i][j]);
                else
                    w.u(kBits[j], static_cast<std::uint32_t>(h.featureData[i][j]));
            }
    }

    void loopFilter(Out& w) const {
        const FrameHeader& h = m_h;
        w.u(6, h.loopFilterLevel[0]);
        w.u(6, h.loopFilterLevel[1]);
        if (m_s.numPlanes() > 1 && (h.loopFilterLevel[0] || h.loopFilterLevel[1])) {
            w.u(6, h.loopFilterLevel[2]);
            w.u(6, h.loopFilterLevel[3]);
        }
        w.u(3, h.loopFilterSharpness);
        w.flag(h.loopFilterDeltaEnabled);
        if (!h.loopFilterDeltaEnabled) return;
        w.flag(h.loopFilterDeltaUpdate);
        if (!h.loopFilterDeltaUpdate) return;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const bool update = (h.updateRefDelta >> i) & 1u;
            w.flag(update);
            if (update) putSu(w, 7, h.loopFilterRefDeltas[i]);
        }
        for (std::uint32_t i = 0; i < 2; ++i) {
            const bool update = (h.updateModeDelta >> i) & 1u;
            w.flag(update);
            if (update) putSu(w, 7, h.loopFilterModeDeltas[i]);
        }
    }

    void cdef(Out& w) const {
        const FrameHeader& h = m_h;
        w.u(2, h.cdefDampingMinus3);
        w.u(2, h.cdefBits);
        for (std::uint32_t i = 0; i < (1u << h.cdefBits); ++i) {
            w.u(4, h.cdefYPri[i]);
            w.u(2, h.cdefYSec[i]);
            if (m_s.numPlanes() > 1) {
                w.u(4, h.cdefUvPri[i]);
                w.u(2, h.cdefUvSec[i]);
            }
        }
    }

    void loopRestoration(Out& w) const {
        static constexpr std::array<std::uint32_t, 4> kCoded = {0, 2, 3,
                                                                1}; // NONE, WIENER, SGRPROJ, SWITCHABLE
        const FrameHeader& h = m_h;
        bool usesLr = false, usesChromaLr = false;
        for (std::uint32_t i = 0; i < m_s.numPlanes(); ++i) {
            w.u(2, kCoded[h.frameRestorationType[i] & 3u]);
            if (h.frameRestorationType[i] != 0) {
                usesLr = true;
                if (i > 0) usesChromaLr = true;
            }
        }
        if (!usesLr) return;
        // LoopRestorationSize[0] = 64 << shift.
        std::uint32_t shift = 0;
        while ((64u << shift) < h.loopRestorationSize[0] && shift < 2)
            ++shift;
        if (m_s.use128x128Superblock) {
            if (shift == 0) throw Unsupported("64-sample restoration units need 64x64 superblocks");
            w.u(1, shift - 1);
        } else {
            w.u(1, shift ? 1 : 0);
            if (shift) w.u(1, shift - 1);
        }
        if (m_s.subsamplingX && m_s.subsamplingY && usesChromaLr)
            w.u(1, h.loopRestorationSize[1] < h.loopRestorationSize[0] ? 1 : 0);
    }

    bool skipModeAllowed() const {
        const FrameHeader& h = m_h;
        if (h.frameIsIntra() || !h.referenceSelect || !m_s.enableOrderHint) return false;
        std::int32_t forwardIdx = -1, backwardIdx = -1;
        std::uint32_t forwardHint = 0, backwardHint = 0;
        for (std::uint32_t i = 0; i < kRefsPerFrame; ++i) {
            const std::uint32_t refHint = h.orderHints[kLastFrame + i];
            if (relativeDist(refHint, h.orderHint) < 0) {
                if (forwardIdx < 0 || relativeDist(refHint, forwardHint) > 0) {
                    forwardIdx = static_cast<std::int32_t>(i);
                    forwardHint = refHint;
                }
            } else if (relativeDist(refHint, h.orderHint) > 0) {
                if (backwardIdx < 0 || relativeDist(refHint, backwardHint) < 0) {
                    backwardIdx = static_cast<std::int32_t>(i);
                    backwardHint = refHint;
                }
            }
        }
        if (forwardIdx < 0) return false;
        if (backwardIdx >= 0) return true;
        for (std::uint32_t i = 0; i < kRefsPerFrame; ++i)
            if (relativeDist(h.orderHints[kLastFrame + i], forwardHint) < 0) return true;
        return false;
    }

    void globalMotion(Out& w) const {
        enum { kIdentity = 0, kTranslation = 1, kRotZoom = 2, kAffine = 3 };
        const FrameHeader& h = m_h;
        for (std::uint32_t ref = kLastFrame; ref <= kAltref; ++ref) {
            const std::uint32_t type = h.gmType[ref];
            w.flag(type != kIdentity);
            if (type != kIdentity) {
                w.flag(type == kRotZoom);
                if (type != kRotZoom) w.flag(type == kTranslation);
            }
            auto param = [&](int idx) {
                int absBits = 12, precBits = 15;
                if (idx < 2) {
                    if (type == kTranslation) {
                        absBits = 9 - !m_highPrecisionMv;
                        precBits = 3 - !m_highPrecisionMv;
                    } else {
                        absBits = 12;
                        precBits = 6;
                    }
                }
                const int precDiff = kWarpedModelPrecBits - precBits;
                const std::int32_t round = (idx % 3) == 2 ? (1 << kWarpedModelPrecBits) : 0;
                const std::int32_t sub = (idx % 3) == 2 ? (1 << precBits) : 0;
                const std::int32_t mx = 1 << absBits;
                const std::int32_t r = (h.prevGmParams[ref][idx] >> precDiff) - sub;
                putSignedSubexpWithRef(w, -mx, mx + 1, r, (h.gmParams[ref][idx] - round) >> precDiff);
            };
            if (type >= kRotZoom) {
                param(2);
                param(3);
                if (type == kAffine) {
                    param(4);
                    param(5);
                }
            }
            if (type >= kTranslation) {
                param(0);
                param(1);
            }
        }
    }

    const SequenceHeader& m_s;
    const FrameHeader& m_h;
    bool m_errorResilient = false, m_screenContent = false, m_intrabc = false, m_highPrecisionMv = false;
    std::uint32_t m_primaryRef = kPrimaryRefNone;
};

} // namespace

std::vector<std::uint8_t> temporalDelimiter() {
    return obu(kObuTemporalDelimiter, {});
}

std::vector<std::uint8_t> writeSequenceHeader(const SequenceHeader& s) {
    if (s.reducedStillPictureHeader) throw Unsupported("reduced still-picture headers are not written");
    if (s.decoderModelInfoPresent) throw Unsupported("decoder model information is not written");
    if (s.initialDisplayDelayPresent) throw Unsupported("initial display delays are not written");
    Out w;
    w.u(3, s.profile);
    w.flag(s.stillPicture);
    w.flag(false); // reduced_still_picture_header
    w.flag(s.timingInfoPresent);
    if (s.timingInfoPresent) {
        w.u(32, s.numUnitsInDisplayTick);
        w.u(32, s.timeScale);
        w.flag(s.equalPictureInterval);
        if (s.equalPictureInterval) putUvlc(w, s.numTicksPerPictureMinus1);
        w.flag(false); // decoder_model_info_present_flag
    }
    w.flag(false); // initial_display_delay_present_flag
    const std::size_t ops = std::max<std::size_t>(s.operatingPointIdc.size(), 1);
    const bool perOp = s.opLevelIdx.size() == ops && s.opTier.size() == ops;
    w.u(5, static_cast<std::uint32_t>(ops - 1));
    for (std::size_t i = 0; i < ops; ++i) {
        w.u(12, i < s.operatingPointIdc.size() ? s.operatingPointIdc[i] : 0);
        const std::uint32_t level = perOp ? s.opLevelIdx[i] : s.levelIdx;
        w.u(5, level);
        if (level > 7) w.u(1, perOp ? s.opTier[i] : s.tier);
    }
    w.u(4, s.frameWidthBitsMinus1);
    w.u(4, s.frameHeightBitsMinus1);
    w.u(static_cast<int>(s.frameWidthBitsMinus1 + 1), s.maxFrameWidthMinus1);
    w.u(static_cast<int>(s.frameHeightBitsMinus1 + 1), s.maxFrameHeightMinus1);
    w.flag(s.frameIdNumbersPresent);
    if (s.frameIdNumbersPresent) {
        w.u(4, s.deltaFrameIdLengthMinus2);
        w.u(3, s.additionalFrameIdLengthMinus1);
    }
    w.flag(s.use128x128Superblock);
    w.flag(s.enableFilterIntra);
    w.flag(s.enableIntraEdgeFilter);
    w.flag(s.enableInterintraCompound);
    w.flag(s.enableMaskedCompound);
    w.flag(s.enableWarpedMotion);
    w.flag(s.enableDualFilter);
    w.flag(s.enableOrderHint);
    if (s.enableOrderHint) {
        w.flag(s.enableJntComp);
        w.flag(s.enableRefFrameMvs);
    }
    w.flag(s.seqForceScreenContentTools == 2); // seq_choose_screen_content_tools
    if (s.seqForceScreenContentTools != 2) w.u(1, s.seqForceScreenContentTools);
    if (s.seqForceScreenContentTools > 0) {
        w.flag(s.seqForceIntegerMv == 2); // seq_choose_integer_mv
        if (s.seqForceIntegerMv != 2) w.u(1, s.seqForceIntegerMv);
    }
    if (s.enableOrderHint) w.u(3, s.orderHintBits - 1);
    w.flag(s.enableSuperres);
    w.flag(s.enableCdef);
    w.flag(s.enableRestoration);
    // color_config()
    const bool highBitdepth = s.bitDepth > 8;
    w.flag(highBitdepth);
    if (s.profile == 2 && highBitdepth) w.flag(s.bitDepth == 12);
    if (s.profile != 1) w.flag(s.monoChrome);
    w.flag(s.colorDescriptionPresent);
    if (s.colorDescriptionPresent) {
        w.u(8, s.colorPrimaries);
        w.u(8, s.transferCharacteristics);
        w.u(8, s.matrixCoefficients);
    }
    if (s.monoChrome) {
        w.flag(s.colorRange);
    } else {
        if (!(s.colorPrimaries == 1 && s.transferCharacteristics == 13 && s.matrixCoefficients == 0)) {
            w.flag(s.colorRange);
            if (s.profile == 2 && s.bitDepth == 12) {
                w.u(1, s.subsamplingX);
                if (s.subsamplingX) w.u(1, s.subsamplingY);
            }
            if (s.subsamplingX && s.subsamplingY) w.u(2, s.chromaSamplePosition);
        }
        w.flag(s.separateUvDeltaQ);
    }
    w.flag(s.filmGrainParamsPresent);
    w.trailing();
    return obu(kObuSequenceHeader, w.bytes());
}

std::vector<std::uint8_t> writeFrameHeader(const SequenceHeader& s, const FrameHeader& h) {
    Out w;
    HeaderWriter(s, h).write(w);
    w.trailing();
    return obu(kObuFrameHeader, w.bytes());
}

std::vector<std::uint8_t> writeFrame(const SequenceHeader& s, const FrameHeader& h,
                                     std::span<const std::span<const std::uint8_t>> tiles) {
    const std::uint32_t numTiles = tileCount(s, h);
    if (tiles.size() != numTiles)
        throw Unsupported(std::format("the frame has {} tiles, not {}", numTiles, tiles.size()));
    Out w;
    HeaderWriter(s, h).write(w);
    w.alignZero();
    if (numTiles > 1) w.flag(false); // tile_start_and_end_present_flag: one tile group
    w.alignZero();
    std::vector<std::uint8_t> payload = w.bytes();
    const std::uint32_t sizeBytes = h.tiles.tileSizeBytesMinus1 + 1;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (tiles[i].empty()) throw Unsupported("an empty tile");
        if (i + 1 < tiles.size()) { // tile_size_minus_1, le(TileSizeBytes)
            const std::uint64_t sizeMinus1 = tiles[i].size() - 1;
            if (sizeBytes < 8 && (sizeMinus1 >> (8 * sizeBytes)) != 0)
                throw Unsupported("a tile is too large for its size field");
            for (std::uint32_t k = 0; k < sizeBytes; ++k)
                payload.push_back(static_cast<std::uint8_t>(sizeMinus1 >> (8 * k)));
        }
        payload.insert(payload.end(), tiles[i].begin(), tiles[i].end());
    }
    return obu(kObuFrame, payload);
}

} // namespace gt::bench::av1
