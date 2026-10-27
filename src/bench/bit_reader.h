#pragma once
// Bit reader for video bitstream headers (Exp-Golomb, fixed-width fields), shared by the H.264 / H.265 / AV1
// readers. Internal to gt_bench.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace gt::bench::bits {

/// A stream feature the readers do not handle, or malformed data; the message says which.
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    /// `rbsp`: whether the data ends with an RBSP stop bit (for moreRbspData()).
    explicit Reader(std::vector<std::uint8_t> data, bool rbsp = true) : m_data(std::move(data)) {
        m_end = m_data.size() * 8;
        if (rbsp) {
            // The RBSP ends with a stop bit (1) and zero padding: remember where, for moreRbspData().
            while (m_end > 0 && !bitAt(m_end - 1))
                --m_end;
            if (m_end > 0) --m_end; // the stop bit itself
        }
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
        return zeros ? static_cast<std::uint32_t>((1ull << zeros) - 1u + u(zeros)) : 0u;
    }
    std::int32_t se() {
        const std::uint32_t k = ue();
        return (k & 1u) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
    }
    void skip(std::size_t n) {
        if (m_pos + n > m_data.size() * 8) throw Unsupported("header is truncated");
        m_pos += n;
    }
    bool moreRbspData() const { return m_pos < m_end; }
    std::size_t position() const { return m_pos; }
    void byteAlign() { m_pos = (m_pos + 7) / 8 * 8; }

private:
    std::uint32_t bitAt(std::size_t p) const { return (m_data[p / 8] >> (7 - p % 8)) & 1u; }
    std::uint32_t bit() {
        if (m_pos >= m_data.size() * 8) throw Unsupported("header is truncated");
        return bitAt(m_pos++);
    }
    std::vector<std::uint8_t> m_data;
    std::size_t m_pos = 0, m_end = 0;
};

/// The RBSP of a NAL unit: `headerBytes` dropped, emulation-prevention bytes (00 00 03) removed.
inline std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> nal, std::size_t headerBytes) {
    std::vector<std::uint8_t> out;
    out.reserve(nal.size());
    for (std::size_t k = headerBytes; k < nal.size(); ++k) {
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

/// Start-code delimited units of Annex B data: [start code offset, payload offset, end).
struct AnnexBUnit {
    std::size_t offset = 0, payload = 0, end = 0;
};
inline std::vector<AnnexBUnit> annexB(std::span<const std::uint8_t> data) {
    std::vector<AnnexBUnit> out;
    std::size_t i = 0;
    while (i + 3 < data.size()) {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
            ++i;
            continue;
        }
        AnnexBUnit u;
        u.offset = i;
        u.payload = i + 3;
        std::size_t end = u.payload;
        while (end + 2 < data.size() &&
               !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 || data[end + 2] == 0)))
            ++end;
        if (end + 2 >= data.size()) end = data.size();
        u.end = end;
        if (u.payload < end) out.push_back(u);
        i = end;
    }
    return out;
}

} // namespace gt::bench::bits
