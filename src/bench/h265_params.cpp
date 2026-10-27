#include "gt/bench/h265_params.h"

#include "bit_reader.h"
#include "bit_writer.h"

#include <algorithm>
#include <format>

namespace gt::bench::h265 {
namespace {

using bits::Unsupported;
using Bits = bits::Reader;

ProfileTierLevel readPtl(Bits& b, std::uint32_t maxSubLayersMinus1) {
    ProfileTierLevel p;
    p.profileSpace = b.u(2);
    p.tier = b.flag();
    p.profileIdc = b.u(5);
    b.skip(32); // general_profile_compatibility_flag[32]
    p.progressiveSource = b.flag();
    p.interlacedSource = b.flag();
    p.nonPackedConstraint = b.flag();
    p.frameOnlyConstraint = b.flag();
    b.skip(43 + 1); // constraint / reserved flags, general_inbld_flag
    p.levelIdc = b.u(8);
    std::array<bool, 8> profilePresent{}, levelPresent{};
    for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        profilePresent[i] = b.flag();
        levelPresent[i] = b.flag();
    }
    if (maxSubLayersMinus1 > 0)
        for (std::uint32_t i = maxSubLayersMinus1; i < 8; ++i)
            b.skip(2);
    for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        if (profilePresent[i]) b.skip(88);
        if (levelPresent[i]) b.skip(8);
    }
    return p;
}

void readOrdering(Bits& b, bool present, std::uint32_t maxSubLayersMinus1, DecPicBufMgr& d) {
    for (std::uint32_t i = present ? 0 : maxSubLayersMinus1; i <= maxSubLayersMinus1; ++i) {
        d.maxDecPicBufferingMinus1[i] = b.ue();
        d.maxNumReorderPics[i] = b.ue();
        d.maxLatencyIncreasePlus1[i] = b.ue();
    }
    if (!present) // inferred equal to the highest sub-layer's
        for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
            d.maxDecPicBufferingMinus1[i] = d.maxDecPicBufferingMinus1[maxSubLayersMinus1];
            d.maxNumReorderPics[i] = d.maxNumReorderPics[maxSubLayersMinus1];
            d.maxLatencyIncreasePlus1[i] = d.maxLatencyIncreasePlus1[maxSubLayersMinus1];
        }
}

/// st_ref_pic_set(stRpsIdx) with the derivation of 7.4.8; `sets` are the SPS's sets before this one.
ShortTermRps readRps(Bits& b, std::uint32_t idx, std::uint32_t numSpsSets,
                     const std::vector<ShortTermRps>& sets) {
    ShortTermRps r;
    if (idx != 0) r.interPrediction = b.flag();
    if (r.interPrediction) {
        if (idx == numSpsSets) r.deltaIdxMinus1 = b.ue();
        if (r.deltaIdxMinus1 + 1 > idx) throw Unsupported("bad reference picture set prediction");
        const ShortTermRps& ref = sets.at(idx - (r.deltaIdxMinus1 + 1));
        r.deltaRpsSign = b.flag();
        r.absDeltaRpsMinus1 = b.ue();
        const std::int32_t deltaRps =
            (r.deltaRpsSign ? -1 : 1) * static_cast<std::int32_t>(r.absDeltaRpsMinus1 + 1);
        const std::uint32_t n = ref.numDeltaPocs();
        r.refRpsNumDeltaPocs = n;
        std::vector<bool> used(n + 1), useDelta(n + 1, true);
        for (std::uint32_t j = 0; j <= n; ++j) {
            used[j] = b.flag();
            if (!used[j]) useDelta[j] = b.flag();
            if (used[j]) r.usedByCurrPicFlag |= 1u << j;
            if (useDelta[j]) r.useDeltaFlag |= 1u << j;
        }
        const auto negatives = static_cast<std::uint32_t>(ref.deltaPocS0.size());
        const auto positives = static_cast<std::uint32_t>(ref.deltaPocS1.size());
        // (7-61)
        for (std::uint32_t k = positives; k-- > 0;) {
            const std::int32_t d = ref.deltaPocS1[k] + deltaRps;
            if (d < 0 && useDelta[negatives + k]) {
                r.deltaPocS0.push_back(d);
                r.usedS0.push_back(used[negatives + k]);
            }
        }
        if (deltaRps < 0 && useDelta[n]) {
            r.deltaPocS0.push_back(deltaRps);
            r.usedS0.push_back(used[n]);
        }
        for (std::uint32_t k = 0; k < negatives; ++k) {
            const std::int32_t d = ref.deltaPocS0[k] + deltaRps;
            if (d < 0 && useDelta[k]) {
                r.deltaPocS0.push_back(d);
                r.usedS0.push_back(used[k]);
            }
        }
        // (7-62)
        for (std::uint32_t k = negatives; k-- > 0;) {
            const std::int32_t d = ref.deltaPocS0[k] + deltaRps;
            if (d > 0 && useDelta[k]) {
                r.deltaPocS1.push_back(d);
                r.usedS1.push_back(used[k]);
            }
        }
        if (deltaRps > 0 && useDelta[n]) {
            r.deltaPocS1.push_back(deltaRps);
            r.usedS1.push_back(used[n]);
        }
        for (std::uint32_t k = 0; k < positives; ++k) {
            const std::int32_t d = ref.deltaPocS1[k] + deltaRps;
            if (d > 0 && useDelta[negatives + k]) {
                r.deltaPocS1.push_back(d);
                r.usedS1.push_back(used[negatives + k]);
            }
        }
    } else {
        const std::uint32_t negatives = b.ue(), positives = b.ue();
        if (negatives > 16 || positives > 16) throw Unsupported("reference picture set too large");
        std::int32_t poc = 0;
        for (std::uint32_t i = 0; i < negatives; ++i) {
            poc -= static_cast<std::int32_t>(b.ue() + 1);
            r.deltaPocS0.push_back(poc);
            r.usedS0.push_back(b.flag());
        }
        poc = 0;
        for (std::uint32_t i = 0; i < positives; ++i) {
            poc += static_cast<std::int32_t>(b.ue() + 1);
            r.deltaPocS1.push_back(poc);
            r.usedS1.push_back(b.flag());
        }
    }
    if (r.numDeltaPocs() > 16) throw Unsupported("reference picture set too large");
    return r;
}

Vps readVps(Bits& b) {
    Vps v;
    v.id = b.u(4);
    b.skip(2); // vps_base_layer_internal_flag, vps_base_layer_available_flag
    if (b.u(6) != 0) throw Unsupported("multi-layer streams are not supported");
    v.maxSubLayersMinus1 = b.u(3);
    if (v.maxSubLayersMinus1 > 6) throw Unsupported("bad VPS");
    v.temporalIdNesting = b.flag();
    b.skip(16);
    v.ptl = readPtl(b, v.maxSubLayersMinus1);
    v.subLayerOrderingInfoPresent = b.flag();
    readOrdering(b, v.subLayerOrderingInfoPresent, v.maxSubLayersMinus1, v.dpb);
    const std::uint32_t maxLayerId = b.u(6);
    const std::uint32_t numLayerSetsMinus1 = b.ue();
    for (std::uint32_t i = 1; i <= numLayerSetsMinus1; ++i)
        b.skip(maxLayerId + 1);
    v.timingInfoPresent = b.flag();
    if (v.timingInfoPresent) {
        v.numUnitsInTick = b.u(32);
        v.timeScale = b.u(32);
        v.pocProportionalToTiming = b.flag();
        if (v.pocProportionalToTiming) v.numTicksPocDiffOneMinus1 = b.ue();
        // vps_num_hrd_parameters and the HRD parameters follow; decoding does not need them.
    }
    return v;
}

Sps readSps(Bits& b) {
    Sps s;
    s.vpsId = b.u(4);
    s.maxSubLayersMinus1 = b.u(3);
    if (s.maxSubLayersMinus1 > 6) throw Unsupported("bad SPS");
    s.temporalIdNesting = b.flag();
    s.ptl = readPtl(b, s.maxSubLayersMinus1);
    s.id = b.ue();
    s.chromaFormatIdc = b.ue();
    if (s.chromaFormatIdc == 3) s.separateColourPlane = b.flag();
    s.width = b.ue();
    s.height = b.ue();
    s.conformanceWindow = b.flag();
    if (s.conformanceWindow) {
        s.confLeft = b.ue();
        s.confRight = b.ue();
        s.confTop = b.ue();
        s.confBottom = b.ue();
    }
    s.bitDepthLumaMinus8 = b.ue();
    s.bitDepthChromaMinus8 = b.ue();
    s.log2MaxPocLsbMinus4 = b.ue();
    if (s.log2MaxPocLsbMinus4 > 12) throw Unsupported("bad SPS");
    s.subLayerOrderingInfoPresent = b.flag();
    readOrdering(b, s.subLayerOrderingInfoPresent, s.maxSubLayersMinus1, s.dpb);
    s.log2MinCbMinus3 = b.ue();
    s.log2DiffMaxMinCb = b.ue();
    s.log2MinTbMinus2 = b.ue();
    s.log2DiffMaxMinTb = b.ue();
    s.maxTransformHierarchyDepthInter = b.ue();
    s.maxTransformHierarchyDepthIntra = b.ue();
    s.scalingListEnabled = b.flag();
    if (s.scalingListEnabled && b.flag()) throw Unsupported("SPS scaling lists are not supported");
    s.ampEnabled = b.flag();
    s.saoEnabled = b.flag();
    s.pcmEnabled = b.flag();
    if (s.pcmEnabled) {
        s.pcmBitDepthLumaMinus1 = b.u(4);
        s.pcmBitDepthChromaMinus1 = b.u(4);
        s.log2MinPcmCbMinus3 = b.ue();
        s.log2DiffMaxMinPcmCb = b.ue();
        s.pcmLoopFilterDisabled = b.flag();
    }
    const std::uint32_t numSets = b.ue();
    if (numSets > 64) throw Unsupported("bad SPS");
    for (std::uint32_t i = 0; i < numSets; ++i)
        s.rps.push_back(readRps(b, i, numSets, s.rps));
    s.longTermRefPicsPresent = b.flag();
    if (s.longTermRefPicsPresent) throw Unsupported("long-term reference pictures are not supported");
    s.temporalMvpEnabled = b.flag();
    s.strongIntraSmoothing = b.flag();
    s.vuiPresent = b.flag();
    return s;
}

Pps readPps(Bits& b) {
    Pps p;
    p.id = b.ue();
    p.spsId = b.ue();
    p.dependentSliceSegmentsEnabled = b.flag();
    p.outputFlagPresent = b.flag();
    p.numExtraSliceHeaderBits = b.u(3);
    p.signDataHiding = b.flag();
    p.cabacInitPresent = b.flag();
    p.numRefIdxL0DefaultMinus1 = b.ue();
    p.numRefIdxL1DefaultMinus1 = b.ue();
    p.initQpMinus26 = b.se();
    p.constrainedIntraPred = b.flag();
    p.transformSkipEnabled = b.flag();
    p.cuQpDeltaEnabled = b.flag();
    if (p.cuQpDeltaEnabled) p.diffCuQpDeltaDepth = b.ue();
    p.cbQpOffset = b.se();
    p.crQpOffset = b.se();
    p.sliceChromaQpOffsetsPresent = b.flag();
    p.weightedPred = b.flag();
    p.weightedBipred = b.flag();
    p.transquantBypassEnabled = b.flag();
    p.tilesEnabled = b.flag();
    p.entropyCodingSync = b.flag();
    if (p.tilesEnabled) {
        p.numTileColumnsMinus1 = b.ue();
        p.numTileRowsMinus1 = b.ue();
        if (p.numTileColumnsMinus1 > 19 || p.numTileRowsMinus1 > 21) throw Unsupported("too many tiles");
        p.uniformSpacing = b.flag();
        if (!p.uniformSpacing) {
            for (std::uint32_t i = 0; i < p.numTileColumnsMinus1; ++i)
                p.columnWidthMinus1.push_back(b.ue());
            for (std::uint32_t i = 0; i < p.numTileRowsMinus1; ++i)
                p.rowHeightMinus1.push_back(b.ue());
        }
        p.loopFilterAcrossTiles = b.flag();
    }
    p.loopFilterAcrossSlices = b.flag();
    p.deblockingControlPresent = b.flag();
    if (p.deblockingControlPresent) {
        p.deblockingOverrideEnabled = b.flag();
        p.deblockingDisabled = b.flag();
        if (!p.deblockingDisabled) {
            p.betaOffsetDiv2 = b.se();
            p.tcOffsetDiv2 = b.se();
        }
    }
    if (b.flag()) throw Unsupported("PPS scaling lists are not supported");
    p.listsModificationPresent = b.flag();
    p.log2ParallelMergeLevelMinus2 = b.ue();
    p.sliceSegmentHeaderExtensionPresent = b.flag();
    // pps_extension_present_flag and the range / SCC extensions follow (not used by Main profiles).
    return p;
}

void writePtl(bits::Writer& w, const ProfileTierLevel& p, std::uint32_t maxSubLayersMinus1) {
    w.u(2, p.profileSpace);
    w.flag(p.tier);
    w.u(5, p.profileIdc);
    for (std::uint32_t j = 0; j < 32; ++j) // a Main stream also conforms to Main 10
        w.flag(j == p.profileIdc || (p.profileIdc == 1 && j == 2));
    w.flag(p.progressiveSource);
    w.flag(p.interlacedSource);
    w.flag(p.nonPackedConstraint);
    w.flag(p.frameOnlyConstraint);
    w.u(22, 0); // 43 reserved bits and general_inbld_flag
    w.u(22, 0);
    w.u(8, p.levelIdc);
    for (std::uint32_t i = 0; i < maxSubLayersMinus1; ++i)
        w.u(2, 0); // sub_layer_profile_present_flag, sub_layer_level_present_flag
    if (maxSubLayersMinus1 > 0)
        for (std::uint32_t i = maxSubLayersMinus1; i < 8; ++i)
            w.u(2, 0);
}

void writeOrdering(bits::Writer& w, bool present, std::uint32_t maxSubLayersMinus1, const DecPicBufMgr& d) {
    for (std::uint32_t i = present ? 0 : maxSubLayersMinus1; i <= maxSubLayersMinus1; ++i) {
        w.ue(d.maxDecPicBufferingMinus1[i]);
        w.ue(d.maxNumReorderPics[i]);
        w.ue(d.maxLatencyIncreasePlus1[i]);
    }
}

/// st_ref_pic_set(idx) in an SPS: the syntax the set carries (predicted sets keep their prediction syntax).
void writeRps(bits::Writer& w, const ShortTermRps& r, std::uint32_t idx) {
    if (idx != 0) w.flag(r.interPrediction);
    if (r.interPrediction) {
        w.flag(r.deltaRpsSign);
        w.ue(r.absDeltaRpsMinus1);
        for (std::uint32_t j = 0; j <= r.refRpsNumDeltaPocs; ++j) {
            const bool used = (r.usedByCurrPicFlag >> j) & 1u;
            w.flag(used);
            if (!used) w.flag((r.useDeltaFlag >> j) & 1u);
        }
        return;
    }
    w.ue(static_cast<std::uint32_t>(r.deltaPocS0.size()));
    w.ue(static_cast<std::uint32_t>(r.deltaPocS1.size()));
    std::int32_t prev = 0;
    for (std::size_t i = 0; i < r.deltaPocS0.size(); ++i) {
        w.ue(static_cast<std::uint32_t>(prev - r.deltaPocS0[i] - 1));
        w.flag(r.usedS0[i]);
        prev = r.deltaPocS0[i];
    }
    prev = 0;
    for (std::size_t i = 0; i < r.deltaPocS1.size(); ++i) {
        w.ue(static_cast<std::uint32_t>(r.deltaPocS1[i] - prev - 1));
        w.flag(r.usedS1[i]);
        prev = r.deltaPocS1[i];
    }
}

std::uint32_t ceilLog2(std::uint32_t v) {
    std::uint32_t n = 0;
    while ((1u << n) < v)
        ++n;
    return n;
}

} // namespace

std::vector<NalUnit> nalUnits(std::span<const std::uint8_t> data) {
    std::vector<NalUnit> out;
    for (const bits::AnnexBUnit& u : bits::annexB(data)) {
        if (u.end - u.payload < 2) continue;
        NalUnit n;
        n.offset = u.offset;
        n.payload = u.payload;
        n.end = u.end;
        n.type = (data[u.payload] >> 1) & 0x3Fu;
        n.temporalId = (data[u.payload + 1] & 7u) - 1u;
        out.push_back(n);
    }
    return out;
}

std::optional<ParameterSets> parse(std::span<const std::uint8_t> data, std::string* error) {
    ParameterSets out;
    try {
        for (const NalUnit& n : nalUnits(data)) {
            if (n.type < 32 || n.type > 34) continue;
            Bits b(bits::rbsp(data.subspan(n.payload, n.end - n.payload), 2));
            if (n.type == 32 && !out.vps) out.vps = readVps(b);
            if (n.type == 33 && !out.sps) out.sps = readSps(b);
            if (n.type == 34 && !out.pps) out.pps = readPps(b);
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
    if (!out.vps || !out.sps || !out.pps) {
        if (error) *error = "no VPS/SPS/PPS in the parameter data";
        return std::nullopt;
    }
    return out;
}

std::vector<std::uint8_t> write(const ParameterSets& ps) {
    const Vps& v = *ps.vps;
    const Sps& s = *ps.sps;
    const Pps& p = *ps.pps;
    std::vector<std::uint8_t> out;

    bits::Writer w; // 7.3.2.1
    w.u(4, v.id);
    w.flag(true); // vps_base_layer_internal_flag
    w.flag(true); // vps_base_layer_available_flag
    w.u(6, 0);    // vps_max_layers_minus1
    w.u(3, v.maxSubLayersMinus1);
    w.flag(v.temporalIdNesting);
    w.u(16, 0xFFFF);
    writePtl(w, v.ptl, v.maxSubLayersMinus1);
    w.flag(v.subLayerOrderingInfoPresent);
    writeOrdering(w, v.subLayerOrderingInfoPresent, v.maxSubLayersMinus1, v.dpb);
    w.u(6, 0); // vps_max_layer_id
    w.ue(0);   // vps_num_layer_sets_minus1
    w.flag(v.timingInfoPresent);
    if (v.timingInfoPresent) {
        w.u(32, v.numUnitsInTick);
        w.u(32, v.timeScale);
        w.flag(v.pocProportionalToTiming);
        if (v.pocProportionalToTiming) w.ue(v.numTicksPocDiffOneMinus1);
        w.ue(0); // vps_num_hrd_parameters
    }
    w.flag(false); // vps_extension_flag
    w.trailing();
    bits::appendNal(out, {32 << 1, 1}, w.bytes());

    bits::Writer q; // 7.3.2.2
    q.u(4, s.vpsId);
    q.u(3, s.maxSubLayersMinus1);
    q.flag(s.temporalIdNesting);
    writePtl(q, s.ptl, s.maxSubLayersMinus1);
    q.ue(s.id);
    q.ue(s.chromaFormatIdc);
    if (s.chromaFormatIdc == 3) q.flag(s.separateColourPlane);
    q.ue(s.width);
    q.ue(s.height);
    q.flag(s.conformanceWindow);
    if (s.conformanceWindow) {
        q.ue(s.confLeft);
        q.ue(s.confRight);
        q.ue(s.confTop);
        q.ue(s.confBottom);
    }
    q.ue(s.bitDepthLumaMinus8);
    q.ue(s.bitDepthChromaMinus8);
    q.ue(s.log2MaxPocLsbMinus4);
    q.flag(s.subLayerOrderingInfoPresent);
    writeOrdering(q, s.subLayerOrderingInfoPresent, s.maxSubLayersMinus1, s.dpb);
    q.ue(s.log2MinCbMinus3);
    q.ue(s.log2DiffMaxMinCb);
    q.ue(s.log2MinTbMinus2);
    q.ue(s.log2DiffMaxMinTb);
    q.ue(s.maxTransformHierarchyDepthInter);
    q.ue(s.maxTransformHierarchyDepthIntra);
    q.flag(s.scalingListEnabled);
    if (s.scalingListEnabled) q.flag(false); // sps_scaling_list_data_present_flag: default lists
    q.flag(s.ampEnabled);
    q.flag(s.saoEnabled);
    q.flag(s.pcmEnabled);
    if (s.pcmEnabled) {
        q.u(4, s.pcmBitDepthLumaMinus1);
        q.u(4, s.pcmBitDepthChromaMinus1);
        q.ue(s.log2MinPcmCbMinus3);
        q.ue(s.log2DiffMaxMinPcmCb);
        q.flag(s.pcmLoopFilterDisabled);
    }
    q.ue(static_cast<std::uint32_t>(s.rps.size()));
    for (std::uint32_t i = 0; i < s.rps.size(); ++i)
        writeRps(q, s.rps[i], i);
    q.flag(false); // long_term_ref_pics_present_flag
    q.flag(s.temporalMvpEnabled);
    q.flag(s.strongIntraSmoothing);
    q.flag(false); // vui_parameters_present_flag
    q.flag(false); // sps_extension_present_flag
    q.trailing();
    bits::appendNal(out, {33 << 1, 1}, q.bytes());

    bits::Writer r; // 7.3.2.3
    r.ue(p.id);
    r.ue(p.spsId);
    r.flag(p.dependentSliceSegmentsEnabled);
    r.flag(p.outputFlagPresent);
    r.u(3, p.numExtraSliceHeaderBits);
    r.flag(p.signDataHiding);
    r.flag(p.cabacInitPresent);
    r.ue(p.numRefIdxL0DefaultMinus1);
    r.ue(p.numRefIdxL1DefaultMinus1);
    r.se(p.initQpMinus26);
    r.flag(p.constrainedIntraPred);
    r.flag(p.transformSkipEnabled);
    r.flag(p.cuQpDeltaEnabled);
    if (p.cuQpDeltaEnabled) r.ue(p.diffCuQpDeltaDepth);
    r.se(p.cbQpOffset);
    r.se(p.crQpOffset);
    r.flag(p.sliceChromaQpOffsetsPresent);
    r.flag(p.weightedPred);
    r.flag(p.weightedBipred);
    r.flag(p.transquantBypassEnabled);
    r.flag(p.tilesEnabled);
    r.flag(p.entropyCodingSync);
    if (p.tilesEnabled) {
        r.ue(p.numTileColumnsMinus1);
        r.ue(p.numTileRowsMinus1);
        r.flag(p.uniformSpacing);
        if (!p.uniformSpacing) {
            for (const std::uint32_t c : p.columnWidthMinus1)
                r.ue(c);
            for (const std::uint32_t h : p.rowHeightMinus1)
                r.ue(h);
        }
        r.flag(p.loopFilterAcrossTiles);
    }
    r.flag(p.loopFilterAcrossSlices);
    r.flag(p.deblockingControlPresent);
    if (p.deblockingControlPresent) {
        r.flag(p.deblockingOverrideEnabled);
        r.flag(p.deblockingDisabled);
        if (!p.deblockingDisabled) {
            r.se(p.betaOffsetDiv2);
            r.se(p.tcOffsetDiv2);
        }
    }
    r.flag(false); // pps_scaling_list_data_present_flag
    r.flag(p.listsModificationPresent);
    r.ue(p.log2ParallelMergeLevelMinus2);
    r.flag(p.sliceSegmentHeaderExtensionPresent);
    r.flag(false); // pps_extension_present_flag
    r.trailing();
    bits::appendNal(out, {34 << 1, 1}, r.bytes());
    return out;
}

std::optional<SliceHeader> parseSliceHeader(std::span<const std::uint8_t> nal, const ParameterSets& ps,
                                            std::string* error) {
    try {
        if (nal.size() < 3 || !ps.sps || !ps.pps) throw Unsupported("no slice or parameter sets");
        const Sps& sps = *ps.sps;
        const Pps& pps = *ps.pps;
        SliceHeader h;
        h.nalType = (nal[0] >> 1) & 0x3Fu;
        h.temporalId = (nal[1] & 7u) - 1u;
        if (h.nalType >= 32) throw Unsupported("not a slice segment");
        // The header needs at most a few hundred bytes; a stop bit makes the end of the copy safe.
        std::vector<std::uint8_t> bytes = bits::rbsp(nal.first(std::min<std::size_t>(nal.size(), 512)), 2);
        bytes.push_back(0x80);
        Bits b(std::move(bytes));
        h.firstSliceSegmentInPic = b.flag();
        if (h.irap()) b.flag(); // no_output_of_prior_pics_flag
        h.ppsId = b.ue();
        if (h.ppsId != pps.id) throw Unsupported("slice refers to another PPS");
        if (!h.firstSliceSegmentInPic) {
            if (pps.dependentSliceSegmentsEnabled) h.dependent = b.flag();
            const std::uint32_t ctb = sps.ctbSize();
            const std::uint32_t ctbs = ((sps.width + ctb - 1) / ctb) * ((sps.height + ctb - 1) / ctb);
            b.skip(ceilLog2(ctbs)); // slice_segment_address
        }
        if (h.dependent) return h; // the picture-level fields are in the independent segment
        b.skip(pps.numExtraSliceHeaderBits);
        h.sliceType = b.ue();
        if (pps.outputFlagPresent) b.flag();
        if (sps.separateColourPlane) b.skip(2);
        if (!h.idr()) {
            h.pocLsb = b.u(static_cast<int>(sps.log2MaxPocLsbMinus4 + 4));
            h.shortTermRefPicSetSps = b.flag();
            if (!h.shortTermRefPicSetSps) {
                const std::size_t start = b.position();
                const auto n = static_cast<std::uint32_t>(sps.rps.size());
                h.rps = readRps(b, n, n, sps.rps);
                h.numBitsForStRefPicSet = static_cast<std::uint32_t>(b.position() - start);
            } else {
                if (sps.rps.empty()) throw Unsupported("slice selects a missing reference picture set");
                if (sps.rps.size() > 1)
                    h.shortTermRefPicSetIdx =
                        b.u(static_cast<int>(ceilLog2(static_cast<std::uint32_t>(sps.rps.size()))));
                h.rps = sps.rps.at(h.shortTermRefPicSetIdx);
            }
            if (sps.longTermRefPicsPresent)
                throw Unsupported("long-term reference pictures are not supported");
            if (sps.temporalMvpEnabled) h.temporalMvpEnabled = b.flag();
        }
        return h;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::optional<Stream> split(std::span<const std::uint8_t> data, const ParameterSets* params,
                            std::string* error) {
    Stream out;
    if (params) {
        out.params = *params;
    } else {
        auto ps = parse(data, error);
        if (!ps) return std::nullopt;
        out.params = *ps;
    }
    if (!out.params.sps || !out.params.pps) {
        if (error) *error = "no SPS/PPS";
        return std::nullopt;
    }
    const Sps& sps = *out.params.sps;
    const std::int32_t maxLsb = 1 << (sps.log2MaxPocLsbMinus4 + 4);
    std::int32_t prevLsb = 0, prevMsb = 0;
    for (const NalUnit& n : nalUnits(data)) {
        if (!n.vcl()) continue;
        if (n.type > 21) {
            if (error) *error = std::format("reserved NAL unit type {}", n.type);
            return std::nullopt;
        }
        auto h = parseSliceHeader(data.subspan(n.payload, n.end - n.payload), out.params, error);
        if (!h) return std::nullopt;
        if (h->firstSliceSegmentInPic || out.pictures.empty()) {
            if (!h->firstSliceSegmentInPic) {
                if (error) *error = "the stream does not start with a picture's first slice segment";
                return std::nullopt;
            }
            Picture p;
            p.header = *h;
            // PicOrderCntVal (8.3.1): IRAP pictures here all start a sequence (no RASL handling).
            std::int32_t msb = 0;
            const auto lsb = static_cast<std::int32_t>(h->pocLsb);
            if (!h->irap()) {
                msb = prevMsb;
                if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2)
                    msb += maxLsb;
                else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2)
                    msb -= maxLsb;
            }
            p.poc = msb + lsb;
            // prevTid0Pic: TemporalId 0 and not RASL / RADL / sub-layer non-reference.
            const bool radlRasl = h->nalType >= 6 && h->nalType <= 9;
            if (h->temporalId == 0 && !radlRasl && h->reference()) {
                prevLsb = lsb;
                prevMsb = msb;
            }
            out.pictures.push_back(std::move(p));
        }
        Picture& p = out.pictures.back();
        p.segments.push_back(static_cast<std::uint32_t>(p.data.size()));
        p.data.insert(p.data.end(), data.begin() + static_cast<std::ptrdiff_t>(n.offset),
                      data.begin() + static_cast<std::ptrdiff_t>(n.end));
    }
    if (out.pictures.empty()) {
        if (error) *error = "no slices";
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<References>> references(const Stream& s, std::string* error) {
    std::vector<References> out(s.pictures.size());
    for (std::uint32_t f = 0; f < s.pictures.size(); ++f) {
        const Picture& p = s.pictures[f];
        const ShortTermRps& r = p.header.rps;
        auto find = [&](std::int32_t poc) -> std::optional<std::uint32_t> {
            for (std::uint32_t g = f; g-- > 0;)
                if (s.pictures[g].poc == poc) return g;
            return std::nullopt;
        };
        auto add = [&](std::int32_t delta, bool used, std::vector<std::uint32_t>& list) {
            const auto g = find(p.poc + delta);
            if (g) {
                (used ? list : out[f].follow).push_back(*g);
                return true;
            }
            return !used;
        };
        for (std::size_t i = 0; i < r.deltaPocS0.size(); ++i)
            if (!add(r.deltaPocS0[i], r.usedS0[i], out[f].before)) {
                if (error)
                    *error = std::format("picture {} references a picture before the stream starts", f);
                return std::nullopt;
            }
        for (std::size_t i = 0; i < r.deltaPocS1.size(); ++i)
            if (!add(r.deltaPocS1[i], r.usedS1[i], out[f].after)) {
                if (error)
                    *error = std::format("picture {} references a later picture that was not decoded", f);
                return std::nullopt;
            }
        if (out[f].before.size() > 8 || out[f].after.size() > 8 ||
            out[f].before.size() + out[f].after.size() + out[f].follow.size() > 15) {
            if (error) *error = "too many references";
            return std::nullopt;
        }
    }
    return out;
}

} // namespace gt::bench::h265
