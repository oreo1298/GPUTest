#pragma once

#include "gt/bench/test.h"
#include "gt/bench/upscale.h"
#include "gt/results.h"
#include "gt/rhi/rhi.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace gt::bench {

enum class Quality { Low, Medium, High, Ultra };
enum class DurationPreset { Smoke, Quick, Standard, Extreme };
std::string qualityName(Quality q);
std::string durationName(DurationPreset d);
bool parseQuality(const std::string& s, Quality& out);
bool parseDuration(const std::string& s, DurationPreset& out);

struct RunSettings {
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    Quality quality = Quality::High;
    DurationPreset duration = DurationPreset::Standard;
    std::uint64_t seed = 0x5EEDu;
    Upscaler upscaler = Upscaler::Off; ///< scenes render at a lower resolution and are upscaled to width x height
    UpscaleMode upscaleMode = UpscaleMode::Quality;
    float sharpness = 0.2f;      ///< FSR 1 RCAS: 0 is the sharpest, each stop halves the sharpening
    bool dumpFrames = false;     ///< write golden frames as PNG
    std::string dumpDir;
    std::map<std::string, std::string> params; ///< free-form per-test overrides (--param key=value)

    /// Frame-loop sizing for the current duration preset.
    std::uint32_t framesPerPass(std::uint32_t standardFrames) const;
    double minSeconds() const;
    double maxSeconds() const;
    std::string param(const std::string& key, const std::string& fallback = {}) const;
};

/// Receives JSON events (progress, frame-time batches, logs) for the progress stream.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(const nlohmann::json& event) = 0;
};

/// Thrown by TestContext::checkStop() when the run must end (user stop, safety cutoff, window closed).
struct StopRequested {
    std::string reason;
};

class Presenter; // window + swapchain + HUD (optional)

struct FrameContext {
    rhi::CommandList& cmd;
    std::uint32_t frame;     ///< index within the pass (0..framesPerPass-1), -1 style warmups use 0
    std::uint32_t pass;
    double simTime;          ///< deterministic simulated time for this frame
    double dt;               ///< simulated time step
    bool warmup;
    std::uint32_t slot = 0;  ///< frame-in-flight index (0..kFramesInFlight-1) for per-frame resources
};
inline constexpr std::uint32_t kFramesInFlight = 2;

struct FrameLoopDesc {
    std::uint32_t framesPerPass = 600;
    std::uint32_t warmupFrames = 30;
    double simDuration = 20.0; ///< simulated seconds covered by one pass
    std::function<void(FrameContext&)> record;
    /// Optional async-compute stage: recorded into a compute-queue list that waits for `record`'s
    /// submission and overlaps the next frame's graphics work; `recordPost` (graphics) waits for it
    /// and finishes the frame (composite, final image). Resources shared across the queues need
    /// TextureDesc::concurrentQueues and per-frame copies (FrameContext::slot).
    std::function<void(FrameContext&)> recordAsync;
    std::function<void(FrameContext&)> recordPost;
    rhi::Texture* output = nullptr; ///< final image (shown in the window, dumped as golden frames)
    std::vector<std::uint32_t> goldenFrames; ///< frame indices (of pass 0) to dump/hash
    double minSeconds = -1.0; ///< overrides the duration preset's minimum run time when >= 0
    double maxSeconds = -1.0; ///< overrides the preset's hard time limit when > 0
};

struct FrameLoopResult {
    std::vector<double> frameMs;
    std::vector<double> gpuMs;
    std::uint32_t passes = 0;
    bool timeLimited = false;
    double elapsedS = 0.0;
};

class TestContext {
public:
    TestContext(rhi::Device& device, const RunSettings& settings, const TestInfo& info, EventSink* events,
                Presenter* presenter, std::atomic<bool>* stopFlag, std::string* stopReason);

    rhi::Device& device() { return m_device; }
    rhi::Api api() const { return m_device.api(); }
    /// The run's settings as this test sees them: when scenes are upscaled, width and height are the
    /// render resolution and the frame loop upscales its output to outputWidth() x outputHeight().
    const RunSettings& settings() const { return m_settings; }
    bool upscaling() const { return m_upscaling; }
    std::uint32_t outputWidth() const { return m_outputWidth; }
    std::uint32_t outputHeight() const { return m_outputHeight; }
    const TestInfo& info() const { return m_info; }
    std::mt19937_64 rng() const { return std::mt19937_64(m_settings.seed); }

    // --- results
    void addMetric(const std::string& name, const std::string& label, double value, const std::string& unit,
                   bool higherIsBetter = true);
    void setVerdict(results::Verdict v, const std::string& message);
    nlohmann::json& details() { return m_result.details; }
    void setFrameResult(const FrameLoopResult& r);
    results::TestResult& result() { return m_result; }

    // --- control
    bool stopRequested() const;
    void checkStop() const;
    void progress(double fraction, const std::string& status = {});
    void log(const std::string& message);

    // --- harness services
    /// Deterministic frame loop: renders whole passes of `framesPerPass` frames until
    /// the duration preset's minimum time is reached (or the time limit / a stop).
    FrameLoopResult runFrames(const FrameLoopDesc& desc);
    /// Records with `record`, submits on `queue`, waits, and returns the GPU time in ms
    /// measured with timestamps (falls back to CPU wall time when unsupported). `submitCpuMs`
    /// receives the CPU time of the submit call itself (where OpenGL and D3D11 issue the commands).
    double timeGpu(const std::function<void(rhi::CommandList&)>& record,
                   rhi::QueueType queue = rhi::QueueType::Graphics, double* submitCpuMs = nullptr);
    /// Reads back a texture (RGBA8/BGRA8 only), records its hash + signature under
    /// details.golden.<name>, and writes <dumpDir>/<test-id>_<name>.png when a dump dir is set.
    /// Returns the 64-bit FNV-1a hash of the pixels (0 if unsupported).
    std::uint64_t dumpTexture(rhi::Texture* texture, const std::string& name);
    Presenter* presenter() { return m_presenter; }
    /// Extra HUD lines (telemetry, loop counter) supplied by the runner.
    std::function<std::vector<std::string>()> hudStatus;
    /// Telemetry samples recorded since this test started (set by the runner; may be empty).
    std::function<std::vector<results::TelemetrySample>()> telemetrySamples;
    /// The GPU's identity with the PCIe link state re-read now (set by the runner).
    std::function<results::GpuInfo()> gpuInfo;

private:
    rhi::Device& m_device;
    RunSettings m_settings; ///< width/height are the render size when the test is upscaled
    bool m_upscaling = false;
    std::uint32_t m_outputWidth = 0, m_outputHeight = 0;
    const TestInfo& m_info;
    EventSink* m_events;
    Presenter* m_presenter;
    std::atomic<bool>* m_stopFlag;
    std::string* m_stopReason;
    results::TestResult m_result;
    std::chrono::steady_clock::time_point m_lastProgress{};
    std::string m_lastStatus;
    std::unique_ptr<rhi::CommandList> m_timerList;
    std::unique_ptr<rhi::QueryPool> m_timerQueries;
};

/// Coarse image signature for tolerant golden-image comparison: mean RGB of a
/// 16x9 grid of cells, hex encoded (864 chars).
std::string imageSignature(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);
/// Compares two signatures; returns the largest per-channel cell difference (0..255), or -1 if malformed.
int compareSignatures(const std::string& a, const std::string& b, double* meanDiff = nullptr);

/// FNV-1a 64-bit hash.
std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t seed = 0xcbf29ce484222325ull);
/// Writes RGBA8 pixels as PNG. Returns false on failure.
bool writePng(const std::string& path, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);

} // namespace gt::bench
