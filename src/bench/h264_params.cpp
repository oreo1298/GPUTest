#include "gt/bench/h264_params.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace gt::bench::h264 {
namespace {

struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Bit reader over an RBSP (emulation-prevention bytes already removed).
class Bits {
public:
    explicit Bits(std::vector<std::uint8_t> data) : m_data(std::move(data)) {
        // The RBSP ends with a stop bit (1) and zero padding: remember where, for more_rbsp_data().
        m_end = m_data.size() * 8;
        while (m_end > 0 && !bitAt(m_end - 1))
            --m_end;
        if (m_end > 0) --m_end; // the stop bit itself
    }
    std::uint32_t u(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    bool flag() { return bit() != 0; }
    std::uint32_t ue() {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31) throw Unsupported("bad exp-Golomb code");
        }
        return zeros ? ((1u << zeros) - 1u + u(zeros)) : 0u;
    }
    std::int32_t se() {
        const std::uint32_t k = ue();
        return (k & 1u) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
    }
    bool moreRbspData() const { return m_pos < m_end; }

private:
    std::uint32_t bitAt(std::size_t p) const { return (m_data[p / 8] >> (7 - p % 8)) & 1u; }
    std::uint32_t bit() {
        if (m_pos >= m_data.size() * 8) throw Unsupported("parameter set is truncated");
        return bitAt(m_pos++);
    }
    std::vector<std::uint8_t> m_data;
    std::size_t m_pos = 0, m_end = 0;
};

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
    std::size_t i = 0;
    while (i + 3 < data.size()) {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
            ++i;
            continue;
        }
        NalUnit n;
        n.offset = i;
        n.payload = i + 3;
        std::size_t end = n.payload;
        while (end + 2 < data.size() &&
               !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 || data[end + 2] == 0)))
            ++end;
        if (end + 2 >= data.size()) end = data.size();
        n.end = end;
        if (n.payload < end) {
            n.type = data[n.payload] & 0x1Fu;
            n.refIdc = (data[n.payload] >> 5) & 3u;
            out.push_back(n);
        }
        i = end;
    }
    return out;
}

namespace {

/// The RBSP of a NAL unit (header byte dropped, emulation-prevention bytes removed).
std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> nal) {
    std::vector<std::uint8_t> out;
    out.reserve(nal.size());
    for (std::size_t k = 1; k < nal.size(); ++k) {
        if (k + 2 < nal.size() && nal[k] == 0 && nal[k + 1] == 0 && nal[k + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            k += 2; // skip the emulation-prevention byte
            continue;
        }
        out.push_back(nal[k]);
    }
    return out;
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

} // namespace gt::bench::h264
