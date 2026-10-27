// Video engines (feature.video): throughput and correctness of the GPU's fixed-function video decoder and
// encoder, through Vulkan Video (video_vk.h).
//
// Per codec:
//  - Decode: a fixed 1080p reference stream (data/video, made with FFmpeg's software encoders from a
//    synthetic pattern) is decoded back to back; every GPU decodes the same bits, so the frame rates are
//    comparable, and each decoded frame must match FFmpeg's decode bit for bit (CRC-32), as the codecs
//    define decoding exactly.
//  - Encode: the same pattern is encoded on the GPU (IDR + P-frames at a constant quantizer where the encoder
//    allows it); the stream it wrote is decoded on the GPU with the parameter sets and headers it actually
//    produced (encoders may override what they were asked for), and compared with the source (luma PSNR).
// Every step is gated on what the driver reports (queues, extensions, profiles, formats, capabilities), so
// missing pieces are reported in the details and the test skips only when there is nothing to run.

#include "gt/bench/context.h"
#include "gt/bench/test.h"

#include "video_vk.h"

#include <format>
#include <string>
#include <vector>

namespace gt::bench {
namespace {

class VideoTest : public Test {
public:
    VideoTest() {
        m_info.id = "feature.video";
        m_info.name = "Video engines";
        m_info.description =
            "Decodes 1080p reference streams on the GPU's video decoder (checked bit for bit) and "
            "encodes 1080p on its video encoder (checked by decoding it again), reporting frames "
            "per second and the codecs the video engines offer.";
        m_info.category = Category::Feature;
        m_info.group = "Video";
        m_info.apis = {rhi::Api::Vulkan};
        m_info.frameBased = false;
        m_info.primaryMetric = "h264_decode_fps";
        m_info.primaryUnit = "fps";
        m_info.reference = 0.0; // fixed-function throughput: shown, not scored
        m_info.tags = {"video"};
        m_info.suites = {"standard", "extreme"};
        m_info.order = 97;
    }
    const TestInfo& info() const override { return m_info; }

#if GT_HAS_VULKAN
    static std::vector<vkvideo::CodecHooks> codecs() { return {vkvideo::h264Hooks(), vkvideo::h265Hooks()}; }
#endif

    SupportResult checkSupport(TestContext& ctx) override {
#if GT_HAS_VULKAN
        if (ctx.api() != rhi::Api::Vulkan)
            return SupportResult::no("video engines are tested through Vulkan Video");
        const rhi::NativeDevice n = ctx.device().nativeDevice();
        if (std::string why = vkvideo::blocker(n); !why.empty()) return SupportResult::no(why);
        for (const auto& c : codecs())
            if (vkvideo::canDecode(n, c) || vkvideo::canEncode(n, c)) return SupportResult::yes();
        return SupportResult::no("the video queues offer none of the tested codecs (H.264, H.265)");
#else
        (void)ctx;
        return SupportResult::no("built without Vulkan");
#endif
    }

    void run(TestContext& ctx) override {
#if GT_HAS_VULKAN
        rhi::Device& dev = ctx.device();
        const rhi::NativeDevice n = dev.nativeDevice();
        auto& d = ctx.details();
        const auto pd = static_cast<VkPhysicalDevice>(n.physicalDevice);
        d["decode_codecs"] = vkvideo::codecList(vkvideo::familyCodecs(pd, n.videoDecodeFamily));
        d["encode_codecs"] = vkvideo::codecList(vkvideo::familyCodecs(pd, n.videoEncodeFamily));

        const DurationPreset dur = ctx.settings().duration;
        const double seconds = dur == DurationPreset::Smoke     ? 0.5
                               : dur == DurationPreset::Quick   ? 2.0
                               : dur == DurationPreset::Extreme ? 8.0
                                                                : 4.0;
        const std::vector<std::uint8_t> source = vkvideo::makeSource();
        const auto all = codecs();
        std::vector<vkvideo::CodecResult> results;
        dev.waitIdle();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const double base = double(i) / double(all.size()), span = 1.0 / double(all.size());
            auto progress = [&](double f, const std::string& s) { ctx.progress(base + f * span, s); };
            results.push_back(vkvideo::runCodec(ctx, n, all[i], source, seconds, progress));
            ctx.checkStop();
        }
        dev.waitIdle();

        std::vector<std::string> failures, warnings, summary;
        for (const auto& r : results) {
            nlohmann::json& cd = d[r.key];
            cd = r.details;
            if (!r.decodeNote.empty()) cd["decode_note"] = r.decodeNote;
            if (!r.encodeNote.empty()) cd["encode_note"] = r.encodeNote;
            if (r.decodeFps)
                ctx.addMetric(r.key + "_decode_fps", r.label + " 1080p decode", *r.decodeFps, "fps");
            if (r.encodeFps)
                ctx.addMetric(r.key + "_encode_fps", r.label + " 1080p encode", *r.encodeFps, "fps");
            if (r.psnr) {
                ctx.addMetric(r.key + "_psnr", r.label + " round trip (luma PSNR)", *r.psnr, "dB");
                ctx.addMetric(r.key + "_psnr_min", r.label + " round trip, worst frame", *r.psnrMin, "dB");
            }
            if (!r.failure.empty()) {
                failures.push_back(r.failure);
                continue;
            }
            std::string s = r.label + ":";
            if (r.decodeFps)
                s += std::format(" decode {:.0f} fps{}", *r.decodeFps, r.decodeExact ? " (bit-exact)" : "");
            else
                s += " no decode";
            if (r.encodeFps) {
                s += std::format(", encode {:.0f} fps", *r.encodeFps);
                if (r.psnr) s += std::format(" ({:.1f} dB)", *r.psnr);
            } else {
                s += ", no encode";
            }
            summary.push_back(s);
            if (r.psnrMin && *r.psnrMin < 25.0)
                failures.push_back(
                    std::format("the {} encoder's output decodes to wrong pictures (worst frame {:.1f} "
                                "dB PSNR)",
                                r.label, *r.psnrMin));
            else if (r.psnrMin && *r.psnrMin < 30.0)
                warnings.push_back(
                    std::format("the {} encoder's output is of low quality (worst frame {:.1f} dB)", r.label,
                                *r.psnrMin));
        }
        auto join = [](const std::vector<std::string>& v, const char* sep) {
            std::string s;
            for (const auto& x : v)
                s += (s.empty() ? "" : sep) + x;
            return s;
        };
        if (!failures.empty()) {
            ctx.setVerdict(results::Verdict::Fail, join(failures, "; ") + ".");
            return;
        }
        const std::string text = join(summary, "; ") + ". The details list what was not measured and why.";
        ctx.setVerdict(warnings.empty() ? results::Verdict::Pass : results::Verdict::Warn,
                       warnings.empty() ? text : join(warnings, "; ") + ". " + text);
#else
        (void)ctx;
#endif
    }

private:
    TestInfo m_info;
};

} // namespace
} // namespace gt::bench

GT_REGISTER_TEST(gt::bench::VideoTest)
