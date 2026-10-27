#include "video_common.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>

namespace gt::bench::video {

std::vector<std::uint8_t> makeSource() {
    std::vector<std::uint8_t> nv12(kSourceFrame * kGop);
    // Rounds half to even, like numpy's rint in scripts/make-video-streams.py.
    auto clamp8 = [](double v) {
        return static_cast<std::uint8_t>(std::clamp(std::nearbyint(v), 16.0, 235.0));
    };
    for (std::uint32_t f = 0; f < kGop; ++f) {
        std::uint8_t* y = nv12.data() + f * kSourceFrame;
        std::uint8_t* uv = y + kSourceLuma;
        const std::uint32_t dx = 6 * f, dy = 3 * f; // the pattern pans by (6, 3) pixels per frame
        for (std::uint32_t py = 0; py < kImageHeight; ++py) {
            const std::uint32_t yi = std::min(py, kHeight - 1) + dy;
            const double v = yi;
            for (std::uint32_t px = 0; px < kImageWidth; ++px) {
                const std::uint32_t xi = px + dx;
                const double x = xi;
                const int bx = static_cast<int>(xi >> 6), by = static_cast<int>(yi >> 6);
                double g = 120.0 + 45.0 * std::sin(x * 0.011) * std::cos(v * 0.017);
                g += ((bx + by) & 1) ? 30.0 : -30.0;
                if ((bx * 7 + by * 3) % 5 == 0) g += ((xi >> 2) & 1) ? 18.0 : -18.0; // fine stripes
                // Texture noise fixed to the pattern, so inter frames can predict it.
                std::uint32_t h = (xi * 73856093u) ^ (yi * 19349663u);
                h = (h ^ (h >> 13)) * 1274126177u;
                g += static_cast<double>((h >> 24) & 15u) - 8.0;
                // Grain that changes every frame (so inter frames carry residual, as camera footage does).
                std::uint32_t t =
                    (px * 2654435761u) ^ (std::min(py, kHeight - 1) * 2246822519u) ^ (f * 3266489917u);
                t = (t ^ (t >> 15)) * 2246822519u;
                g += static_cast<double>((t >> 28) & 7u) - 4.0;
                y[std::size_t(py) * kImageWidth + px] = clamp8(g);
            }
        }
        for (std::uint32_t py = 0; py < kImageHeight / 2; ++py) {
            const double v = std::min(2 * py, kHeight - 2) + dy;
            for (std::uint32_t px = 0; px < kImageWidth / 2; ++px) {
                const double x = 2.0 * px + dx;
                std::uint8_t* c = uv + std::size_t(py) * kImageWidth + 2 * px;
                c[0] = clamp8(128.0 + 40.0 * std::sin(x * 0.006 + v * 0.002));
                c[1] = clamp8(128.0 + 40.0 * std::cos(v * 0.008 - x * 0.001));
            }
        }
    }
    return nv12;
}

double lumaPsnr(const std::uint8_t* decoded, const std::vector<std::uint8_t>& source, std::uint32_t f) {
    const std::uint8_t* src = source.data() + f * kSourceFrame;
    double sum = 0.0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const double d =
                double(decoded[std::size_t(y) * kWidth + x]) - double(src[std::size_t(y) * kImageWidth + x]);
            sum += d * d;
        }
    const double mse = sum / (double(kWidth) * kHeight);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void checkReference(const ReferenceStream& ref, const std::vector<std::uint8_t>& frames, std::uint32_t count,
                    CodecResult& res) {
    std::vector<std::uint32_t> wrong;
    nlohmann::json crcs = nlohmann::json::array();
    for (std::uint32_t f = 0; f < count && f < ref.crc32.size(); ++f) {
        const std::uint32_t crc = crc32(std::span(frames).subspan(f * kFrameBytes, kFrameBytes));
        crcs.push_back(std::format("{:08x}", crc));
        if (crc != ref.crc32[f]) wrong.push_back(f);
    }
    res.details["decode_crc32"] = crcs;
    if (!wrong.empty()) {
        std::string list;
        for (const std::uint32_t f : wrong)
            list += (list.empty() ? "" : ", ") + std::to_string(f);
        const double p = lumaPsnr(frames.data() + wrong.front() * kFrameBytes, makeSource(), wrong.front());
        res.failure =
            std::format("{} frame(s) {} differ from the reference decoder's (decoding is bit-exact; "
                        "frame {} is {:.1f} dB from the source, the reference {:.1f} dB)",
                        res.label, list, wrong.front(), p, ref.psnr.at(wrong.front()));
        return;
    }
    res.decodeExact = true;
}

void checkRoundTrip(const std::vector<std::uint8_t>& frames, std::uint32_t count,
                    const std::vector<std::uint8_t>& source, CodecResult& res) {
    double sum = 0.0, worst = 1e9;
    nlohmann::json perFrame = nlohmann::json::array();
    for (std::uint32_t f = 0; f < count; ++f) {
        const double p = lumaPsnr(frames.data() + f * kFrameBytes, source, f);
        perFrame.push_back(std::round(p * 100.0) / 100.0);
        sum += p;
        worst = std::min(worst, p);
    }
    res.details["round_trip_psnr"] = perFrame;
    if (count) {
        res.psnr = sum / count;
        res.psnrMin = worst;
    }
}

} // namespace gt::bench::video
