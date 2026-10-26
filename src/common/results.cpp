#include "gt/results.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

using nlohmann::json;

namespace gt::results {
namespace {

template <typename E, std::size_t N>
std::string enumToString(E v, const std::array<const char*, N>& names) {
    const auto i = static_cast<std::size_t>(v);
    return i < N ? names[i] : "unknown";
}
template <typename E, std::size_t N>
E enumFromString(const std::string& s, const std::array<const char*, N>& names, E fallback) {
    for (std::size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<E>(i);
    return fallback;
}

constexpr std::array<const char*, 5> kStatus{"completed", "skipped", "error", "aborted", "device_lost"};
constexpr std::array<const char*, 4> kVerdict{"none", "pass", "warn", "fail"};
constexpr std::array<const char*, 7> kRunStatus{"running", "completed", "stopped", "aborted",
                                                "safety_abort", "device_lost", "crashed"};

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

json num(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }
double getNum(const json& j, const char* key, double fallback = 0.0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    return it->get<double>();
}
std::string getStr(const json& j, const char* key, const std::string& fallback = {}) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}
bool getBool(const json& j, const char* key, bool fallback = false) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}
template <typename T>
T getInt(const json& j, const char* key, T fallback = 0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    return it->get<T>();
}

json floatArray(const std::vector<float>& v) {
    json a = json::array();
    a.get_ref<json::array_t&>().reserve(v.size());
    for (float f : v) a.push_back(std::isfinite(f) ? json(round3(f)) : json(nullptr));
    return a;
}
std::vector<float> floatArrayFrom(const json& j) {
    std::vector<float> v;
    if (!j.is_array()) return v;
    v.reserve(j.size());
    for (const auto& e : j) v.push_back(e.is_number() ? e.get<float>() : kNaN);
    return v;
}

} // namespace

std::string toString(Status s) { return enumToString(s, kStatus); }
std::string toString(Verdict v) { return enumToString(v, kVerdict); }
std::string toString(RunStatus s) { return enumToString(s, kRunStatus); }
Status statusFromString(const std::string& s) { return enumFromString(s, kStatus, Status::Error); }
Verdict verdictFromString(const std::string& s) { return enumFromString(s, kVerdict, Verdict::None); }
RunStatus runStatusFromString(const std::string& s) { return enumFromString(s, kRunStatus, RunStatus::Crashed); }

namespace throttle {
namespace {
constexpr std::array<const char*, 10> kNames{"power", "thermal", "memory_thermal", "current", "voltage",
                                             "hw_slowdown", "sw_power", "application", "idle", "other"};
}
std::vector<std::string> names(std::uint32_t mask) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (mask & (1u << i)) out.emplace_back(kNames[i]);
    return out;
}
std::uint32_t fromNames(const std::vector<std::string>& n) {
    std::uint32_t m = 0;
    for (const auto& s : n)
        for (std::size_t i = 0; i < kNames.size(); ++i)
            if (s == kNames[i]) m |= 1u << i;
    return m;
}
} // namespace throttle

// --- telemetry ------------------------------------------------------------------------------
const std::vector<std::string>& telemetryChannels() {
    static const std::vector<std::string> kChannels{
        "gpu_clock_mhz", "mem_clock_mhz", "temp_edge_c",  "temp_hotspot_c",   "temp_mem_c",
        "power_w",       "power_limit_w", "fan_percent",  "fan_rpm",          "gpu_util_percent",
        "mem_util_percent", "vram_used_mb", "vram_total_mb"};
    return kChannels;
}

const std::vector<ChannelInfo>& telemetryChannelInfo() {
    static const std::vector<ChannelInfo> kInfo{
        {"GPU clock", "MHz"},         {"Memory clock", "MHz"}, {"GPU temperature", "°C"}, {"Hotspot temperature", "°C"},
        {"Memory temperature", "°C"}, {"Board power", "W"},    {"Power limit", "W"},      {"Fan speed", "%"},
        {"Fan speed", "rpm"},         {"GPU load", "%"},       {"Memory controller load", "%"},
        {"VRAM used", "MiB"},         {"VRAM total", "MiB"}};
    return kInfo;
}

const ChannelInfo* telemetryChannelInfo(const std::string& key) {
    const auto& ch = telemetryChannels();
    for (std::size_t i = 0; i < ch.size(); ++i)
        if (ch[i] == key) return &telemetryChannelInfo()[i];
    return nullptr;
}

float telemetryValue(const TelemetrySample& s, std::size_t c) {
    switch (c) {
    case 0: return s.gpuClockMhz;
    case 1: return s.memClockMhz;
    case 2: return s.tempEdgeC;
    case 3: return s.tempHotspotC;
    case 4: return s.tempMemC;
    case 5: return s.powerW;
    case 6: return s.powerLimitW;
    case 7: return s.fanPercent;
    case 8: return s.fanRpm;
    case 9: return s.gpuUtilPercent;
    case 10: return s.memUtilPercent;
    case 11: return s.vramUsedMB;
    case 12: return s.vramTotalMB;
    default: return kNaN;
    }
}

void setTelemetryValue(TelemetrySample& s, std::size_t c, float v) {
    switch (c) {
    case 0: s.gpuClockMhz = v; break;
    case 1: s.memClockMhz = v; break;
    case 2: s.tempEdgeC = v; break;
    case 3: s.tempHotspotC = v; break;
    case 4: s.tempMemC = v; break;
    case 5: s.powerW = v; break;
    case 6: s.powerLimitW = v; break;
    case 7: s.fanPercent = v; break;
    case 8: s.fanRpm = v; break;
    case 9: s.gpuUtilPercent = v; break;
    case 10: s.memUtilPercent = v; break;
    case 11: s.vramUsedMB = v; break;
    case 12: s.vramTotalMB = v; break;
    default: break;
    }
}

void Telemetry::computeSummary() {
    summary.clear();
    const auto& ch = telemetryChannels();
    for (std::size_t c = 0; c < ch.size(); ++c) {
        ChannelSummary s;
        double sum = 0.0;
        for (const auto& smp : samples) {
            const float v = telemetryValue(smp, c);
            if (!std::isfinite(v)) continue;
            if (s.count == 0) s.min = s.max = v;
            s.min = std::min<double>(s.min, v);
            s.max = std::max<double>(s.max, v);
            sum += v;
            ++s.count;
        }
        if (s.count) {
            s.avg = sum / static_cast<double>(s.count);
            summary[ch[c]] = s;
        }
    }
    // Merge consecutive samples with throttle reasons (ignoring "idle") into events.
    throttleEvents.clear();
    const std::uint32_t ignore = throttle::kIdle;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint32_t r = samples[i].throttle & ~ignore;
        if (!r) continue;
        const double t0 = samples[i].t;
        const double t1 = i + 1 < samples.size() ? samples[i + 1].t : samples[i].t + intervalS;
        if (!throttleEvents.empty() && throttleEvents.back().reasons == r &&
            t0 - throttleEvents.back().end <= intervalS * 1.5) {
            throttleEvents.back().end = t1;
        } else {
            throttleEvents.push_back({t0, t1, r});
        }
    }
}

json toJson(const TelemetrySample& s) {
    json j;
    j["t"] = round3(s.t);
    const auto& ch = telemetryChannels();
    for (std::size_t c = 0; c < ch.size(); ++c) {
        const float v = telemetryValue(s, c);
        if (std::isfinite(v)) j[ch[c]] = std::round(static_cast<double>(v) * 10.0) / 10.0;
    }
    if (s.throttle) j["throttle"] = throttle::names(s.throttle);
    if (s.throttleRaw) j["throttle_raw"] = s.throttleRaw;
    return j;
}

TelemetrySample sampleFromJson(const json& j) {
    TelemetrySample s;
    s.t = getNum(j, "t");
    const auto& ch = telemetryChannels();
    for (std::size_t c = 0; c < ch.size(); ++c) {
        auto it = j.find(ch[c]);
        if (it != j.end() && it->is_number()) setTelemetryValue(s, c, it->get<float>());
    }
    if (auto it = j.find("throttle"); it != j.end() && it->is_array())
        s.throttle = throttle::fromNames(it->get<std::vector<std::string>>());
    s.throttleRaw = getInt<std::uint64_t>(j, "throttle_raw");
    return s;
}

// --- frame stats / tests -------------------------------------------------------------------
json toJson(const FrameStats& s) {
    return {{"frames", s.frames},       {"total_ms", round3(s.totalMs)}, {"avg_fps", round3(s.avgFps)},
            {"min_fps", round3(s.minFps)}, {"max_fps", round3(s.maxFps)}, {"low_1pct_fps", round3(s.low1Fps)},
            {"low_01pct_fps", round3(s.low01Fps)}, {"mean_ms", round3(s.meanMs)}, {"min_ms", round3(s.minMs)},
            {"max_ms", round3(s.maxMs)},   {"stddev_ms", round3(s.stddevMs)}, {"p50_ms", round3(s.p50Ms)},
            {"p90_ms", round3(s.p90Ms)},   {"p95_ms", round3(s.p95Ms)},  {"p99_ms", round3(s.p99Ms)},
            {"p999_ms", round3(s.p999Ms)}, {"stutter_count", s.stutterCount}};
}

FrameStats frameStatsFromJson(const json& j) {
    FrameStats s;
    s.frames = getInt<std::size_t>(j, "frames");
    s.totalMs = getNum(j, "total_ms");
    s.avgFps = getNum(j, "avg_fps");
    s.minFps = getNum(j, "min_fps");
    s.maxFps = getNum(j, "max_fps");
    s.low1Fps = getNum(j, "low_1pct_fps");
    s.low01Fps = getNum(j, "low_01pct_fps");
    s.meanMs = getNum(j, "mean_ms");
    s.minMs = getNum(j, "min_ms");
    s.maxMs = getNum(j, "max_ms");
    s.stddevMs = getNum(j, "stddev_ms");
    s.p50Ms = getNum(j, "p50_ms");
    s.p90Ms = getNum(j, "p90_ms");
    s.p95Ms = getNum(j, "p95_ms");
    s.p99Ms = getNum(j, "p99_ms");
    s.p999Ms = getNum(j, "p999_ms");
    s.stutterCount = getInt<std::size_t>(j, "stutter_count");
    return s;
}

const Metric* TestResult::metric(const std::string& n) const {
    for (const auto& m : metrics)
        if (m.name == n) return &m;
    return nullptr;
}

json toJson(const TestResult& t) {
    json j;
    j["id"] = t.id;
    j["name"] = t.name;
    j["category"] = t.category;
    if (!t.group.empty()) j["group"] = t.group;
    j["api"] = t.api;
    j["loop"] = t.loop;
    j["status"] = toString(t.status);
    if (!t.message.empty()) j["message"] = t.message;
    if (t.verdict != Verdict::None) {
        j["verdict"] = toString(t.verdict);
        j["verdict_message"] = t.verdictMessage;
    }
    if (t.scored) j["score"] = round3(t.score);
    if (!t.primaryMetric.empty()) j["primary_metric"] = t.primaryMetric;
    json metrics = json::array();
    for (const auto& m : t.metrics)
        metrics.push_back({{"name", m.name}, {"label", m.label}, {"value", num(m.value)}, {"unit", m.unit},
                           {"higher_is_better", m.higherIsBetter}});
    j["metrics"] = std::move(metrics);
    if (t.frameStats) j["frame_stats"] = toJson(*t.frameStats);
    if (!t.frameTimesMs.empty()) j["frame_times_ms"] = floatArray(t.frameTimesMs);
    if (!t.gpuTimesMs.empty()) j["gpu_times_ms"] = floatArray(t.gpuTimesMs);
    if (std::isfinite(t.gpuBusyPercent)) j["gpu_busy_percent"] = round3(t.gpuBusyPercent);
    j["start_s"] = round3(t.startS);
    j["duration_s"] = round3(t.durationS);
    j["frames"] = t.frames;
    if (t.timeLimited) j["time_limited"] = true;
    if (!t.details.empty()) j["details"] = t.details;
    return j;
}

TestResult testFromJson(const json& j) {
    TestResult t;
    t.id = getStr(j, "id");
    t.name = getStr(j, "name", t.id);
    t.category = getStr(j, "category");
    t.group = getStr(j, "group");
    t.api = getStr(j, "api");
    t.loop = getInt<int>(j, "loop");
    t.status = statusFromString(getStr(j, "status", "error"));
    t.message = getStr(j, "message");
    t.verdict = verdictFromString(getStr(j, "verdict", "none"));
    t.verdictMessage = getStr(j, "verdict_message");
    if (j.contains("score")) {
        t.scored = true;
        t.score = getNum(j, "score");
    }
    t.primaryMetric = getStr(j, "primary_metric");
    if (auto it = j.find("metrics"); it != j.end() && it->is_array()) {
        for (const auto& m : *it) {
            Metric mm;
            mm.name = getStr(m, "name");
            mm.label = getStr(m, "label", mm.name);
            mm.value = getNum(m, "value", std::numeric_limits<double>::quiet_NaN());
            mm.unit = getStr(m, "unit");
            mm.higherIsBetter = getBool(m, "higher_is_better", true);
            t.metrics.push_back(std::move(mm));
        }
    }
    if (auto it = j.find("frame_stats"); it != j.end() && it->is_object()) t.frameStats = frameStatsFromJson(*it);
    if (auto it = j.find("frame_times_ms"); it != j.end()) t.frameTimesMs = floatArrayFrom(*it);
    if (auto it = j.find("gpu_times_ms"); it != j.end()) t.gpuTimesMs = floatArrayFrom(*it);
    t.gpuBusyPercent = getNum(j, "gpu_busy_percent", std::numeric_limits<double>::quiet_NaN());
    t.startS = getNum(j, "start_s");
    t.durationS = getNum(j, "duration_s");
    t.frames = getInt<std::uint32_t>(j, "frames");
    t.timeLimited = getBool(j, "time_limited");
    if (auto it = j.find("details"); it != j.end()) t.details = *it;
    return t;
}

// --- system -----------------------------------------------------------------------------------
json toJson(const SystemInfo& s) {
    const auto& g = s.gpu;
    const auto& h = s.host;
    json j;
    j["host"] = {{"cpu", h.cpu},       {"cpu_cores", h.cpuCores}, {"cpu_threads", h.cpuThreads},
                 {"ram_bytes", h.ramBytes}, {"os", h.os},       {"kernel", h.kernel},
                 {"display_server", h.displayServer}, {"desktop", h.desktop}, {"hostname", h.hostname}};
    j["gpu"] = {{"name", g.name},
                {"vendor", g.vendor},
                {"vendor_id", std::format("0x{:04x}", g.vendorId)},
                {"device_id", std::format("0x{:04x}", g.deviceId)},
                {"subsys_vendor_id", std::format("0x{:04x}", g.subsysVendorId)},
                {"subsys_id", std::format("0x{:04x}", g.subsysId)},
                {"revision", std::format("0x{:02x}", g.revision)},
                {"type", g.type},
                {"driver_name", g.driverName},
                {"driver_version", g.driverVersion},
                {"driver_info", g.driverInfo},
                {"vbios", g.vbios},
                {"pci_address", g.pciAddress},
                {"pcie_current", g.pcieCurrent},
                {"pcie_max", g.pcieMax},
                {"pcie_current_gen", g.pcieCurrentGen},
                {"pcie_current_width", g.pcieCurrentWidth},
                {"pcie_max_gen", g.pcieMaxGen},
                {"pcie_max_width", g.pcieMaxWidth},
                {"vram_bytes", g.vramBytes},
                {"shared_bytes", g.sharedBytes},
                {"api", g.api},
                {"api_version", g.apiVersion},
                {"resizable_bar", g.resizableBar}};
    j["extensions"] = s.extensions;
    j["capabilities"] = s.capabilities;
    return j;
}

namespace {
std::uint32_t hexField(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number()) return it->get<std::uint32_t>();
    if (it->is_string()) {
        try {
            return static_cast<std::uint32_t>(std::stoul(it->get<std::string>(), nullptr, 0));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}
} // namespace

SystemInfo systemFromJson(const json& j) {
    SystemInfo s;
    if (auto it = j.find("host"); it != j.end()) {
        const auto& h = *it;
        s.host.cpu = getStr(h, "cpu");
        s.host.cpuCores = getInt<int>(h, "cpu_cores");
        s.host.cpuThreads = getInt<int>(h, "cpu_threads");
        s.host.ramBytes = getInt<std::uint64_t>(h, "ram_bytes");
        s.host.os = getStr(h, "os");
        s.host.kernel = getStr(h, "kernel");
        s.host.displayServer = getStr(h, "display_server");
        s.host.desktop = getStr(h, "desktop");
        s.host.hostname = getStr(h, "hostname");
    }
    if (auto it = j.find("gpu"); it != j.end()) {
        const auto& g = *it;
        auto& o = s.gpu;
        o.name = getStr(g, "name");
        o.vendor = getStr(g, "vendor");
        o.vendorId = hexField(g, "vendor_id");
        o.deviceId = hexField(g, "device_id");
        o.subsysVendorId = hexField(g, "subsys_vendor_id");
        o.subsysId = hexField(g, "subsys_id");
        o.revision = hexField(g, "revision");
        o.type = getStr(g, "type");
        o.driverName = getStr(g, "driver_name");
        o.driverVersion = getStr(g, "driver_version");
        o.driverInfo = getStr(g, "driver_info");
        o.vbios = getStr(g, "vbios");
        o.pciAddress = getStr(g, "pci_address");
        o.pcieCurrent = getStr(g, "pcie_current");
        o.pcieMax = getStr(g, "pcie_max");
        o.pcieCurrentGen = getInt<int>(g, "pcie_current_gen");
        o.pcieCurrentWidth = getInt<int>(g, "pcie_current_width");
        o.pcieMaxGen = getInt<int>(g, "pcie_max_gen");
        o.pcieMaxWidth = getInt<int>(g, "pcie_max_width");
        o.vramBytes = getInt<std::uint64_t>(g, "vram_bytes");
        o.sharedBytes = getInt<std::uint64_t>(g, "shared_bytes");
        o.api = getStr(g, "api");
        o.apiVersion = getStr(g, "api_version");
        o.resizableBar = getBool(g, "resizable_bar");
    }
    if (auto it = j.find("extensions"); it != j.end() && it->is_array())
        s.extensions = it->get<std::vector<std::string>>();
    if (auto it = j.find("capabilities"); it != j.end()) s.capabilities = *it;
    return s;
}

// --- run ------------------------------------------------------------------------------------------
json toJson(const RunResult& r) {
    json j;
    j["schema"] = kSchemaId;
    j["schema_version"] = r.schemaVersion;
    j["app"] = {{"name", r.appName}, {"version", r.appVersion}, {"revision", r.appRevision}};
    j["created_utc"] = r.createdUtc;
    j["command_line"] = r.commandLine;
    const auto& c = r.config;
    j["config"] = {{"suite", c.suite},       {"api", c.api},           {"gpu_index", c.gpuIndex},
                   {"width", c.width},       {"height", c.height},     {"quality", c.quality},
                   {"duration", c.duration}, {"loop", c.loop},         {"offscreen", c.offscreen},
                   {"fullscreen", c.fullscreen}, {"vsync", c.vsync},   {"seed", c.seed},
                   {"validation", c.validation}, {"safety", c.safety}, {"tests", c.tests}};
    j["system"] = toJson(r.system);
    j["status"] = toString(r.status);
    if (!r.statusMessage.empty()) j["status_message"] = r.statusMessage;
    j["duration_s"] = round3(r.durationS);
    json tests = json::array();
    for (const auto& t : r.tests) tests.push_back(toJson(t));
    j["tests"] = std::move(tests);
    json loops = json::array();
    for (const auto& l : r.loops)
        loops.push_back({{"index", l.index}, {"start_s", round3(l.startS)}, {"duration_s", round3(l.durationS)},
                         {"score", round3(l.score)}, {"complete", l.complete}});
    j["loops"] = std::move(loops);
    j["scores"] = {{"overall", round3(r.scores.overall)},
                   {"subscores", r.scores.subscores},
                   {"comparable_key", r.scores.comparableKey},
                   {"per_watt", num(r.scores.perWatt)}};
    if (r.stability.valid)
        j["stability"] = {{"ratio", round3(r.stability.ratio)},
                          {"threshold", r.stability.threshold},
                          {"passed", r.stability.passed},
                          {"trend_percent_per_hour", num(r.stability.trendPercentPerHour)}};
    json tel;
    tel["provider"] = r.telemetry.provider;
    tel["interval_s"] = r.telemetry.intervalS;
    json samples = json::array();
    for (const auto& s : r.telemetry.samples) samples.push_back(toJson(s));
    tel["samples"] = std::move(samples);
    json summary = json::object();
    for (const auto& [k, v] : r.telemetry.summary)
        summary[k] = {{"min", round3(v.min)}, {"avg", round3(v.avg)}, {"max", round3(v.max)}, {"count", v.count}};
    tel["summary"] = std::move(summary);
    json events = json::array();
    for (const auto& e : r.telemetry.throttleEvents)
        events.push_back({{"start_s", round3(e.start)}, {"end_s", round3(e.end)}, {"reasons", throttle::names(e.reasons)}});
    tel["throttle_events"] = std::move(events);
    j["telemetry"] = std::move(tel);
    j["validation"] = {{"errors", r.validationErrors}, {"warnings", r.validationWarnings}};
    j["warnings"] = r.warnings;
    json losses = json::array();
    for (const auto& d : r.deviceLosses) {
        json e{{"test", d.test},
               {"loop", d.loop},
               {"t", round3(d.t)},
               {"message", d.message},
               {"recovered", d.recovered},
               {"fault", {{"supported", d.faultReportSupported}, {"description", d.faultDescription}, {"details", d.faultDetails}}}};
        if (d.lastSample) e["last_sample"] = toJson(*d.lastSample);
        losses.push_back(std::move(e));
    }
    j["device_losses"] = std::move(losses);
    return j;
}

RunResult fromJson(const json& j) {
    if (!j.is_object() || getStr(j, "schema") != kSchemaId)
        throw std::runtime_error("not a results file (missing schema id)");
    RunResult r;
    r.schemaVersion = getInt<int>(j, "schema_version");
    if (r.schemaVersion < 1 || r.schemaVersion > kSchemaVersion)
        throw std::runtime_error(std::format("unsupported results schema version {}", r.schemaVersion));
    if (auto it = j.find("app"); it != j.end()) {
        r.appName = getStr(*it, "name");
        r.appVersion = getStr(*it, "version");
        r.appRevision = getStr(*it, "revision");
    }
    r.createdUtc = getStr(j, "created_utc");
    r.commandLine = getStr(j, "command_line");
    if (auto it = j.find("config"); it != j.end()) {
        const auto& c = *it;
        r.config.suite = getStr(c, "suite");
        r.config.api = getStr(c, "api");
        r.config.gpuIndex = getInt<int>(c, "gpu_index");
        r.config.width = getInt<int>(c, "width", 1920);
        r.config.height = getInt<int>(c, "height", 1080);
        r.config.quality = getStr(c, "quality");
        r.config.duration = getStr(c, "duration");
        r.config.loop = getStr(c, "loop");
        r.config.offscreen = getBool(c, "offscreen");
        r.config.fullscreen = getBool(c, "fullscreen");
        r.config.vsync = getBool(c, "vsync");
        r.config.seed = getInt<std::uint64_t>(c, "seed");
        r.config.validation = getBool(c, "validation");
        if (auto s = c.find("safety"); s != c.end()) r.config.safety = *s;
        if (auto t = c.find("tests"); t != c.end() && t->is_array()) r.config.tests = t->get<std::vector<std::string>>();
    }
    if (auto it = j.find("system"); it != j.end()) r.system = systemFromJson(*it);
    r.status = runStatusFromString(getStr(j, "status", "crashed"));
    r.statusMessage = getStr(j, "status_message");
    r.durationS = getNum(j, "duration_s");
    if (auto it = j.find("tests"); it != j.end() && it->is_array())
        for (const auto& t : *it) r.tests.push_back(testFromJson(t));
    if (auto it = j.find("loops"); it != j.end() && it->is_array()) {
        for (const auto& l : *it) {
            LoopResult lr;
            lr.index = getInt<int>(l, "index");
            lr.startS = getNum(l, "start_s");
            lr.durationS = getNum(l, "duration_s");
            lr.score = getNum(l, "score");
            lr.complete = getBool(l, "complete", true);
            r.loops.push_back(lr);
        }
    }
    if (auto it = j.find("scores"); it != j.end()) {
        r.scores.overall = getNum(*it, "overall");
        if (auto s = it->find("subscores"); s != it->end() && s->is_object())
            for (auto e = s->begin(); e != s->end(); ++e)
                if (e->is_number()) r.scores.subscores[e.key()] = e->get<double>();
        r.scores.comparableKey = getStr(*it, "comparable_key");
        r.scores.perWatt = getNum(*it, "per_watt", std::numeric_limits<double>::quiet_NaN());
    }
    if (auto it = j.find("stability"); it != j.end() && it->is_object()) {
        r.stability.valid = true;
        r.stability.ratio = getNum(*it, "ratio");
        r.stability.threshold = getNum(*it, "threshold", 0.97);
        r.stability.passed = getBool(*it, "passed");
        r.stability.trendPercentPerHour = getNum(*it, "trend_percent_per_hour", std::numeric_limits<double>::quiet_NaN());
    }
    if (auto it = j.find("telemetry"); it != j.end()) {
        r.telemetry.provider = getStr(*it, "provider");
        r.telemetry.intervalS = getNum(*it, "interval_s", 0.1);
        if (auto s = it->find("samples"); s != it->end() && s->is_array())
            for (const auto& e : *s) r.telemetry.samples.push_back(sampleFromJson(e));
        r.telemetry.computeSummary();
    }
    if (auto it = j.find("validation"); it != j.end()) {
        r.validationErrors = getInt<std::uint64_t>(*it, "errors");
        r.validationWarnings = getInt<std::uint64_t>(*it, "warnings");
    }
    if (auto it = j.find("warnings"); it != j.end() && it->is_array())
        for (const auto& w : *it)
            if (w.is_string()) r.warnings.push_back(w.get<std::string>());
    if (auto it = j.find("device_losses"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            DeviceLoss d;
            d.test = getStr(e, "test");
            d.loop = getInt<int>(e, "loop");
            d.t = getNum(e, "t");
            d.message = getStr(e, "message");
            d.recovered = getBool(e, "recovered");
            if (auto f = e.find("fault"); f != e.end() && f->is_object()) {
                d.faultReportSupported = getBool(*f, "supported");
                d.faultDescription = getStr(*f, "description");
                if (auto dd = f->find("details"); dd != f->end() && dd->is_array())
                    for (const auto& x : *dd)
                        if (x.is_string()) d.faultDetails.push_back(x.get<std::string>());
            }
            if (auto s = e.find("last_sample"); s != e.end() && s->is_object()) d.lastSample = sampleFromJson(*s);
            r.deviceLosses.push_back(std::move(d));
        }
    }
    return r;
}

bool writeFile(const RunResult& r, const std::string& path, std::string* error) {
    namespace fs = std::filesystem;
    const fs::path target(path);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot open " + tmp.string();
            return false;
        }
        f << toJson(r).dump(1, '\t', false, json::error_handler_t::replace) << '\n';
        if (!f) {
            if (error) *error = "write failed for " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Windows cannot rename over an existing file opened elsewhere; fall back to copy.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) {
            if (error) *error = ec.message();
            return false;
        }
    }
    return true;
}

std::optional<RunResult> readFile(const std::string& path, std::string* error) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            if (error) *error = "cannot open " + path;
            return std::nullopt;
        }
        json j = json::parse(f);
        return fromJson(j);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::string summarize(const TestResult& t) {
    if (t.status == Status::Skipped) return std::format("{}: skipped ({})", t.name, t.message);
    if (t.status != Status::Completed) return std::format("{}: {} — {}", t.name, toString(t.status), t.message);
    std::string s = t.name + ":";
    if (t.frameStats)
        s += std::format(" {:.1f} fps (1% low {:.1f}, p99 {:.2f} ms)", t.frameStats->avgFps, t.frameStats->low1Fps,
                         t.frameStats->p99Ms);
    else if (const Metric* m = t.metric(t.primaryMetric))
        s += std::format(" {:.4g} {}", m->value, m->unit);
    if (t.verdict != Verdict::None) s += std::format(" [{}] {}", toString(t.verdict), t.verdictMessage);
    if (t.scored) s += std::format(" — score {:.0f}", t.score);
    return s;
}

} // namespace gt::results
