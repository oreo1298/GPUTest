#include "gt/bench/h264_params.h"

#include "bit_reader.h"
#include "bit_writer.h"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <vector>

namespace gt::bench::h264 {
namespace {

using bits::Unsupported;
using Bits = bits::Reader;

Sps readSps(Bits& b) {
    Sps s;
    s.profileIdc = b.u(8);
    s.constraintFlags = 0;
    for (int i = 0; i < 6; ++i)
        s.constraintFlags |= b.u(1) << i;
    b.u(2); // reserved_zero_2bits
    s.levelIdc = b.u(8);
    s.spsId = b.ue();
    const std::uint32_t p = s.profileIdc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 ||
        p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        s.chromaFormatIdc = b.ue();
        if (s.chromaFormatIdc == 3) b.flag(); // separate_colour_plane_flag
        s.bitDepthLumaMinus8 = b.ue();
        s.bitDepthChromaMinus8 = b.ue();
        s.qpprimeYZeroTransformBypass = b.flag();
        if (b.flag()) throw Unsupported("SPS scaling matrices are not supported");
    }
    s.log2MaxFrameNumMinus4 = b.ue();
    s.pocType = b.ue();
    if (s.pocType == 0)
        s.log2MaxPocLsbMinus4 = b.ue();
    else if (s.pocType == 1)
        throw Unsupported("picture order count type 1 is not supported");
    s.maxNumRefFrames = b.ue();
    s.gapsInFrameNumAllowed = b.flag();
    s.widthInMbsMinus1 = b.ue();
    s.heightInMapUnitsMinus1 = b.ue();
    s.frameMbsOnly = b.flag();
    if (!s.frameMbsOnly) s.mbAdaptiveFrameField = b.flag();
    s.direct8x8Inference = b.flag();
    s.frameCropping = b.flag();
    if (s.frameCropping) {
        s.cropLeft = b.ue();
        s.cropRight = b.ue();
        s.cropTop = b.ue();
        s.cropBottom = b.ue();
    }
    s.vuiPresent = b.flag(); // VUI is not needed to decode
    return s;
}

Pps readPps(Bits& b) {
    Pps p;
    p.ppsId = b.ue();
    p.spsId = b.ue();
    p.entropyCodingMode = b.flag();
    p.bottomFieldPicOrderInFramePresent = b.flag();
    if (b.ue() != 0) throw Unsupported("slice groups are not supported");
    p.numRefIdxL0DefaultMinus1 = b.ue();
    p.numRefIdxL1DefaultMinus1 = b.ue();
    p.weightedPred = b.flag();
    p.weightedBipredIdc = b.u(2);
    p.picInitQpMinus26 = b.se();
    p.picInitQsMinus26 = b.se();
    p.chromaQpIndexOffset = b.se();
    p.deblockingFilterControlPresent = b.flag();
    p.constrainedIntraPred = b.flag();
    p.redundantPicCntPresent = b.flag();
    p.secondChromaQpIndexOffset = p.chromaQpIndexOffset;
    if (b.moreRbspData()) {
        p.transform8x8Mode = b.flag();
        if (b.flag()) throw Unsupported("PPS scaling matrices are not supported");
        p.secondChromaQpIndexOffset = b.se();
    }
    return p;
}

} // namespace

std::vector<NalUnit> nalUnits(std::span<const std::uint8_t> data) {
    std::vector<NalUnit> out;
    for (const bits::AnnexBUnit& u : bits::annexB(data)) {
        NalUnit n;
        n.offset = u.offset;
        n.payload = u.payload;
        n.end = u.end;
        n.type = data[u.payload] & 0x1Fu;
        n.refIdc = (data[u.payload] >> 5) & 3u;
        out.push_back(n);
    }
    return out;
}

namespace {

std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> nal) {
    return bits::rbsp(nal, 1);
}

} // namespace

std::optional<ParameterSets> parse(std::span<const std::uint8_t> data, std::string* error) {
    ParameterSets out;
    try {
        for (const NalUnit& n : nalUnits(data)) {
            if (n.type != 7 && n.type != 8) continue;
            Bits b(rbsp(data.subspan(n.payload, n.end - n.payload)));
            if (n.type == 7 && !out.sps) out.sps = readSps(b);
            if (n.type == 8 && !out.pps) out.pps = readPps(b);
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
    if (!out.sps || !out.pps) {
        if (error) *error = "no SPS/PPS in the encoder's parameter data";
        return std::nullopt;
    }
    return out;
}

std::optional<SliceHeader> parseSliceHeader(std::span<const std::uint8_t> nal, const ParameterSets& ps,
                                            std::string* error) {
    try {
        if (nal.empty() || !ps.sps || !ps.pps) throw Unsupported("no slice or parameter sets");
        const Sps& sps = *ps.sps;
        const Pps& pps = *ps.pps;
        SliceHeader h;
        h.nalType = nal[0] & 0x1Fu;
        h.nalRefIdc = (nal[0] >> 5) & 3u;
        if (h.nalType != 1 && h.nalType != 5) throw Unsupported("not a slice NAL unit");
        // Only the first bytes are needed; the bit reader's stop-bit search must not cut the header short.
        std::vector<std::uint8_t> bytes = rbsp(nal.first(std::min<std::size_t>(nal.size(), 64)));
        bytes.push_back(0x80);
        Bits b(std::move(bytes));
        h.firstMb = b.ue();
        h.sliceType = b.ue();
        h.ppsId = b.ue();
        if (h.ppsId != pps.ppsId) throw Unsupported("slice refers to another PPS");
        if (!sps.frameMbsOnly) throw Unsupported("field coding is not supported");
        h.frameNum = b.u(static_cast<int>(sps.log2MaxFrameNumMinus4 + 4));
        if (h.idr()) h.idrPicId = b.ue();
        if (sps.pocType == 0) {
            h.pocLsb = b.u(static_cast<int>(sps.log2MaxPocLsbMinus4 + 4));
            if (pps.bottomFieldPicOrderInFramePresent) h.deltaPocBottom = b.se();
        }
        return h;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::array<std::int32_t, 2> PocCounter::next(const SliceHeader& h, const Sps& sps) {
    std::int32_t top = 0;
    if (sps.pocType == 0) {
        if (h.idr()) m_prevMsb = m_prevLsb = 0;
        const std::int32_t maxLsb = 1 << (sps.log2MaxPocLsbMinus4 + 4);
        const std::int32_t lsb = static_cast<std::int32_t>(h.pocLsb);
        std::int32_t msb = m_prevMsb;
        if (lsb < m_prevLsb && m_prevLsb - lsb >= maxLsb / 2)
            msb += maxLsb;
        else if (lsb > m_prevLsb && lsb - m_prevLsb > maxLsb / 2)
            msb -= maxLsb;
        top = msb + lsb;
        if (h.nalRefIdc) {
            m_prevMsb = msb;
            m_prevLsb = lsb;
        }
        return {top, top + h.deltaPocBottom};
    }
    // Type 2: output order follows decoding order.
    const std::uint32_t maxFrameNum = 1u << (sps.log2MaxFrameNumMinus4 + 4);
    if (h.idr())
        m_frameNumOffset = 0;
    else if (m_prevFrameNum > h.frameNum)
        m_frameNumOffset += maxFrameNum;
    m_prevFrameNum = h.frameNum;
    if (!h.idr()) {
        const std::int32_t absFrameNum = static_cast<std::int32_t>(m_frameNumOffset + h.frameNum);
        top = h.nalRefIdc ? 2 * absFrameNum : 2 * absFrameNum - 1;
    }
    return {top, top};
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
    PocCounter poc;
    for (const NalUnit& n : nalUnits(data)) {
        if (n.type != 1 && n.type != 5) continue;
        auto h = parseSliceHeader(data.subspan(n.payload, n.end - n.payload), out.params, error);
        if (!h) return std::nullopt;
        if (h->firstMb == 0 || out.pictures.empty()) {
            Picture p;
            p.header = *h;
            p.poc = poc.next(*h, *out.params.sps);
            out.pictures.push_back(std::move(p));
        }
        Picture& p = out.pictures.back();
        p.slices.push_back(static_cast<std::uint32_t>(p.data.size()));
        p.data.insert(p.data.end(), data.begin() + static_cast<std::ptrdiff_t>(n.offset),
                      data.begin() + static_cast<std::ptrdiff_t>(n.end));
    }
    if (out.pictures.empty()) {
        if (error) *error = "no slices";
        return std::nullopt;
    }
    return out;
}

std::vector<std::uint8_t> write(const ParameterSets& ps) {
    const Sps& s = *ps.sps;
    const Pps& p = *ps.pps;
    const bool high = s.profileIdc == 100 || s.profileIdc == 110 || s.profileIdc == 122 ||
                      s.profileIdc == 244 || s.profileIdc == 44 || s.profileIdc == 83 || s.profileIdc == 86 ||
                      s.profileIdc == 118 || s.profileIdc == 128 || s.profileIdc == 138 ||
                      s.profileIdc == 139 || s.profileIdc == 134 || s.profileIdc == 135;
    bits::Writer w; // 7.3.2.1.1
    w.u(8, s.profileIdc);
    for (int i = 0; i < 6; ++i)
        w.flag((s.constraintFlags >> i) & 1u); // constraint_set0_flag first
    w.u(2, 0);
    w.u(8, s.levelIdc);
    w.ue(s.spsId);
    if (high) {
        w.ue(s.chromaFormatIdc);
        if (s.chromaFormatIdc == 3) w.flag(false); // separate_colour_plane_flag
        w.ue(s.bitDepthLumaMinus8);
        w.ue(s.bitDepthChromaMinus8);
        w.flag(s.qpprimeYZeroTransformBypass);
        w.flag(false); // seq_scaling_matrix_present_flag
    }
    w.ue(s.log2MaxFrameNumMinus4);
    w.ue(s.pocType);
    if (s.pocType == 0) w.ue(s.log2MaxPocLsbMinus4);
    w.ue(s.maxNumRefFrames);
    w.flag(s.gapsInFrameNumAllowed);
    w.ue(s.widthInMbsMinus1);
    w.ue(s.heightInMapUnitsMinus1);
    w.flag(s.frameMbsOnly);
    if (!s.frameMbsOnly) w.flag(s.mbAdaptiveFrameField);
    w.flag(s.direct8x8Inference);
    w.flag(s.frameCropping);
    if (s.frameCropping) {
        w.ue(s.cropLeft);
        w.ue(s.cropRight);
        w.ue(s.cropTop);
        w.ue(s.cropBottom);
    }
    w.flag(false); // vui_parameters_present_flag
    w.trailing();
    std::vector<std::uint8_t> out;
    bits::appendNal(out, {0x67}, w.bytes()); // nal_ref_idc 3, SPS

    bits::Writer q; // 7.3.2.2
    q.ue(p.ppsId);
    q.ue(p.spsId);
    q.flag(p.entropyCodingMode);
    q.flag(p.bottomFieldPicOrderInFramePresent);
    q.ue(0); // num_slice_groups_minus1
    q.ue(p.numRefIdxL0DefaultMinus1);
    q.ue(p.numRefIdxL1DefaultMinus1);
    q.flag(p.weightedPred);
    q.u(2, p.weightedBipredIdc);
    q.se(p.picInitQpMinus26);
    q.se(p.picInitQsMinus26);
    q.se(p.chromaQpIndexOffset);
    q.flag(p.deblockingFilterControlPresent);
    q.flag(p.constrainedIntraPred);
    q.flag(p.redundantPicCntPresent);
    if (high) {
        q.flag(p.transform8x8Mode);
        q.flag(false); // pic_scaling_matrix_present_flag
        q.se(p.secondChromaQpIndexOffset);
    }
    q.trailing();
    bits::appendNal(out, {0x68}, q.bytes()); // nal_ref_idc 3, PPS
    return out;
}

std::vector<std::vector<std::uint32_t>> slidingWindow(const Stream& s) {
    std::vector<std::vector<std::uint32_t>> out;
    std::deque<std::uint32_t> window;
    const std::uint32_t size = std::max(s.params.sps ? s.params.sps->maxNumRefFrames : 1u, 1u);
    for (std::uint32_t f = 0; f < s.pictures.size(); ++f) {
        const SliceHeader& h = s.pictures[f].header;
        if (h.idr()) window.clear();
        out.emplace_back(window.begin(), window.end());
        if (h.nalRefIdc) {
            window.push_back(f);
            while (window.size() > size)
                window.pop_front();
        }
    }
    return out;
}

} // namespace gt::bench::h264
