#pragma once
// Result data model shared by the runner (writes) and the GUI (reads), plus its
// JSON serialization. The format is documented in docs/RESULTS_SCHEMA.md; bump
// kSchemaVersion on incompatible changes and keep fromJson() able to read older files.

#include "gt/stats.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gt::results {

inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kSchemaId = "gpu-benchmark-results";

enum class Status { Completed, Skipped, Error, Aborted, DeviceLost };
enum class Verdict { None, Pass, Warn, Fail };
enum class RunStatus { Running, Completed, Stopped, Aborted, SafetyAbort, DeviceLost, Crashed };

std::string toString(Status s);
std::string toString(Verdict v);
std::string toString(RunStatus s);
Status statusFromString(const std::string& s);
Verdict verdictFromString(const std::string& s);
RunStatus runStatusFromString(const std::string& s);

/// Normalized throttle reasons (vendor reasons map onto these; raw values are kept too).
namespace throttle {
inline constexpr std::uint32_t kPower = 1u << 0;       ///< power / TDP / PPT limit
inline constexpr std::uint32_t kThermal = 1u << 1;     ///< GPU core / hotspot temperature
inline constexpr std::uint32_t kMemThermal = 1u << 2;  ///< memory temperature
inline constexpr std::uint32_t kCurrent = 1u << 3;     ///< current limits (EDC/TDC)
inline constexpr std::uint32_t kVoltage = 1u << 4;     ///< voltage / reliability limit
inline constexpr std::uint32_t kHwSlowdown = 1u << 5;  ///< hardware slowdown (external signal, PSU)
inline constexpr std::uint32_t kSwPower = 1u << 6;     ///< software power capping by the driver
inline constexpr std::uint32_t kApplication = 1u << 7; ///< user / application clock limits
inline constexpr std::uint32_t kIdle = 1u << 8;        ///< idle (not a problem)
inline constexpr std::uint32_t kOther = 1u << 9;
std::vector<std::string> names(std::uint32_t mask);
std::uint32_t fromNames(const std::vector<std::string>& names);
} // namespace throttle

inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// One telemetry sample. NaN = not available from the active provider.
struct TelemetrySample {
    double t = 0.0; ///< seconds since run start
    float gpuClockMhz = kNaN;
    float memClockMhz = kNaN;
    float tempEdgeC = kNaN;
    float tempHotspotC = kNaN;
    float tempMemC = kNaN;
    float powerW = kNaN;
    float powerLimitW = kNaN;
    float fanPercent = kNaN;
    float fanRpm = kNaN;
    float gpuUtilPercent = kNaN;
    float memUtilPercent = kNaN;
    float vramUsedMB = kNaN;
    float vramTotalMB = kNaN;
    std::uint32_t throttle = 0;   ///< normalized throttle::* mask
    std::uint64_t throttleRaw = 0;///< vendor-specific raw value
};

/// Names of the float channels of TelemetrySample, in a fixed order (JSON keys).
const std::vector<std::string>& telemetryChannels();
float telemetryValue(const TelemetrySample& s, std::size_t channel);
void setTelemetryValue(TelemetrySample& s, std::size_t channel, float v);

struct ChannelSummary {
    double min = 0, avg = 0, max = 0;
    std::size_t count = 0;
};

struct ThrottleEvent {
    double start = 0.0;
    double end = 0.0;
    std::uint32_t reasons = 0;
};

struct Telemetry {
    std::string provider;           ///< e.g. "nvml", "amdgpu-sysfs", "none"
    double intervalS = 0.1;
    std::vector<TelemetrySample> samples;
    std::map<std::string, ChannelSummary> summary;
    std::vector<ThrottleEvent> throttleEvents;
    void computeSummary(); ///< fills summary + throttleEvents from samples
};

struct Metric {
    std::string name;   ///< machine id, e.g. "avg_fps", "bandwidth_read"
    std::string label;  ///< display label
    double value = 0.0;
    std::string unit;
    bool higherIsBetter = true;
};

struct TestResult {
    std::string id;
    std::string name;
    std::string category; ///< scene / synthetic / feature / diagnostic
    std::string group;
    std::string api;
    int loop = 0;
    Status status = Status::Completed;
    std::string message; ///< skip reason / error text
    Verdict verdict = Verdict::None;
    std::string verdictMessage;
    bool scored = false;
    double score = 0.0;
    std::string primaryMetric;
    std::vector<Metric> metrics;
    std::optional<FrameStats> frameStats;
    std::vector<float> frameTimesMs; ///< CPU-side frame-to-frame time
    std::vector<float> gpuTimesMs;   ///< GPU busy time per frame (timestamps)
    double gpuBusyPercent = std::numeric_limits<double>::quiet_NaN();
    double startS = 0.0;
    double durationS = 0.0;
    std::uint32_t frames = 0;
    bool timeLimited = false;
    nlohmann::json details = nlohmann::json::object();

    const Metric* metric(const std::string& name) const;
};

struct LoopResult {
    int index = 0;
    double startS = 0.0;
    double durationS = 0.0;
    double score = 0.0;
    bool complete = true;
};

struct Stability {
    bool valid = false;
    double ratio = 0.0;     ///< worst loop / best loop (0..1)
    double threshold = 0.97;
    bool passed = false;
    double trendPercentPerHour = 0.0; ///< linear trend of loop scores
};

struct Scores {
    double overall = 0.0;
    std::map<std::string, double> subscores;
    std::string comparableKey; ///< scores are only comparable when keys match
    double perWatt = std::numeric_limits<double>::quiet_NaN();
};

struct GpuInfo {
    std::string name;
    std::string vendor;
    std::uint32_t vendorId = 0, deviceId = 0, subsysVendorId = 0, subsysId = 0, revision = 0;
    std::string type;
    std::string driverName, driverVersion, driverInfo;
    std::string vbios;
    std::string pciAddress;
    std::string pcieCurrent; ///< e.g. "Gen4 x16"
    std::string pcieMax;
    int pcieCurrentGen = 0, pcieCurrentWidth = 0, pcieMaxGen = 0, pcieMaxWidth = 0;
    std::uint64_t vramBytes = 0;
    std::uint64_t sharedBytes = 0;
    std::string api, apiVersion;
    bool resizableBar = false;
};

struct HostInfo {
    std::string cpu;
    int cpuCores = 0, cpuThreads = 0;
    std::uint64_t ramBytes = 0;
    std::string os, kernel, displayServer, desktop;
    std::string hostname;
};

struct SystemInfo {
    HostInfo host;
    GpuInfo gpu;
    std::vector<std::string> extensions; ///< API extensions (full list)
    nlohmann::json capabilities = nlohmann::json::array(); ///< [{group,name,value,supported}]
};

struct RunConfig {
    std::string suite;
    std::string api;
    int gpuIndex = 0;
    int width = 1920, height = 1080;
    std::string quality = "high";
    std::string duration = "standard";
    std::string loop = "single";
    bool offscreen = false;
    bool fullscreen = false;
    bool vsync = false;
    std::uint64_t seed = 0;
    bool validation = false;
    nlohmann::json safety = nlohmann::json::object();
    std::vector<std::string> tests; ///< resolved test ids
};

struct RunResult {
    int schemaVersion = kSchemaVersion;
    std::string appName, appVersion, appRevision;
    std::string createdUtc;
    std::string commandLine;
    RunConfig config;
    SystemInfo system;
    RunStatus status = RunStatus::Running;
    std::string statusMessage;
    std::vector<TestResult> tests;
    std::vector<LoopResult> loops;
    Scores scores;
    Stability stability;
    Telemetry telemetry;
    std::uint64_t validationErrors = 0, validationWarnings = 0;
    std::vector<std::string> warnings;
    double durationS = 0.0;
};

nlohmann::json toJson(const FrameStats& s);
FrameStats frameStatsFromJson(const nlohmann::json& j);
nlohmann::json toJson(const TestResult& t);
TestResult testFromJson(const nlohmann::json& j);
nlohmann::json toJson(const TelemetrySample& s);
TelemetrySample sampleFromJson(const nlohmann::json& j);
nlohmann::json toJson(const SystemInfo& s);
SystemInfo systemFromJson(const nlohmann::json& j);
nlohmann::json toJson(const RunResult& r);
/// Throws std::runtime_error on a missing/unsupported schema.
RunResult fromJson(const nlohmann::json& j);

/// Writes atomically (temp file + rename) so a crash never leaves a truncated file.
bool writeFile(const RunResult& r, const std::string& path, std::string* error = nullptr);
std::optional<RunResult> readFile(const std::string& path, std::string* error = nullptr);

/// Human-readable one-line summary of a test result.
std::string summarize(const TestResult& t);

} // namespace gt::results
