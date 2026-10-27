#pragma once
// Bit writer for video parameter sets (fixed-width fields, Exp-Golomb), the counterpart of bit_reader.h for
// encoders that leave writing the parameter sets to the application. Internal to gt_bench.

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace gt::bench::bits {

class Writer {
public:
    void u(int n, std::uint32_t v) {
        for (int i = n - 1; i >= 0; --i)
            bit((v >> i) & 1u);
    }
    void flag(bool v) { bit(v ? 1u : 0u); }
    void ue(std::uint32_t v) {
        const std::uint64_t x = std::uint64_t(v) + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        u(len, 0);
        for (int i = len; i >= 0; --i)
            bit(static_cast<std::uint32_t>((x >> i) & 1u));
    }
    void se(std::int32_t v) {
        ue(v > 0 ? static_cast<std::uint32_t>(2 * std::int64_t(v) - 1)
                 : static_cast<std::uint32_t>(-2 * std::int64_t(v)));
    }
    /// rbsp_trailing_bits(): a stop bit, then zeros to the byte boundary.
    void trailing() {
        bit(1);
        while (m_bits % 8)
            bit(0);
    }
    const std::vector<std::uint8_t>& bytes() const { return m_data; }

private:
    void bit(std::uint32_t b) {
        if (m_bits % 8 == 0) m_data.push_back(0);
        if (b) m_data.back() |= static_cast<std::uint8_t>(0x80u >> (m_bits % 8));
        ++m_bits;
    }
    std::vector<std::uint8_t> m_data;
    std::size_t m_bits = 0;
};

/// Appends a NAL unit in Annex B form: a 4-byte start code, the header bytes, then the RBSP with
/// emulation-prevention bytes (00 00 0x with x <= 3 becomes 00 00 03 0x).
inline void appendNal(std::vector<std::uint8_t>& out, std::initializer_list<std::uint8_t> header,
                      const std::vector<std::uint8_t>& rbsp) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), header);
    int zeros = 0;
    for (const std::uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

} // namespace gt::bench::bits
