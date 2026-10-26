#include "gt/bench/suites.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace gt::bench {
namespace {

struct Preset {
    const char* id;
    const char* name;
    const char* description;
    const char* duration;
    const char* quality;
};

const Preset kPresets[] = {
    {"quick", "Quick", "A few minutes: the main scenes and core synthetic tests at short durations.", "quick", "high"},
    {"standard", "Standard", "The recommended suite: scenes, synthetic and feature tests at standard durations.",
     "standard", "high"},
    {"extreme", "Extreme", "Heavier settings and longer runs for high-end GPUs.", "extreme", "ultra"},
    {"raytracing", "Ray tracing", "Every ray-tracing scene and feature test.", "standard", "high"},
    {"legacy", "Legacy", "Tests that run on older GPUs and legacy APIs (OpenGL 3.3, D3D11 FL10).", "standard", "medium"},
    {"compute", "Compute", "Compute throughput, memory bandwidth/latency and matrix tests.", "standard", "high"},
    {"diagnostics", "Diagnostics", "Repair-bench checks: VRAM test, artifact and compute-error detection, thermals, PCIe.",
     "standard", "high"},
    {"full", "Full", "Every test that the selected GPU and API support.", "standard", "high"},
    {"smoke", "Smoke", "Tiny run used by CI to check that everything works (not for scoring).", "smoke", "low"},
};

} // namespace

const std::vector<std::string>& presetSuiteIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (const auto& p : kPresets) v.emplace_back(p.id);
        return v;
    }();
    return ids;
}

std::vector<Suite> presetSuites() {
    std::vector<Suite> out;
    const auto tests = Registry::instance().list();
    for (const auto& p : kPresets) {
        Suite s;
        s.id = p.id;
        s.name = p.name;
        s.description = p.description;
        s.duration = p.duration;
        s.quality = p.quality;
        for (const auto& t : tests) {
            const bool member = s.id == "full" || std::find(t.suites.begin(), t.suites.end(), s.id) != t.suites.end();
            if (member) s.tests.push_back(t.id);
        }
        // Run order: diagnostics first (fail fast on bad hardware), then synthetic, features, scenes.
        auto rank = [](const TestInfo* i) {
            if (!i) return 9;
            switch (i->category) {
            case Category::Diagnostic: return 0;
            case Category::Synthetic: return 1;
            case Category::Feature: return 2;
            case Category::Scene: return 3;
            }
            return 9;
        };
        std::stable_sort(s.tests.begin(), s.tests.end(), [&](const std::string& a, const std::string& b) {
            return rank(Registry::instance().find(a)) < rank(Registry::instance().find(b));
        });
        out.push_back(std::move(s));
    }
    return out;
}

nlohmann::json toJson(const Suite& s) {
    nlohmann::json j{{"id", s.id}, {"name", s.name}, {"description", s.description}, {"tests", s.tests}};
    if (!s.duration.empty()) j["duration"] = s.duration;
    if (!s.quality.empty()) j["quality"] = s.quality;
    if (!s.loop.empty()) j["loop"] = s.loop;
    if (!s.resolution.empty()) j["resolution"] = s.resolution;
    if (s.custom) j["custom"] = true;
    return j;
}

std::optional<Suite> suiteFromJson(const nlohmann::json& j, std::string* error) {
    if (!j.is_object() || !j.contains("tests") || !j["tests"].is_array()) {
        if (error) *error = "suite file must be an object with a \"tests\" array";
        return std::nullopt;
    }
    Suite s;
    s.custom = true;
    s.id = j.value("id", std::string("custom"));
    s.name = j.value("name", s.id);
    s.description = j.value("description", std::string());
    s.duration = j.value("duration", std::string());
    s.quality = j.value("quality", std::string());
    s.loop = j.value("loop", std::string());
    s.resolution = j.value("resolution", std::string());
    for (const auto& t : j["tests"]) {
        if (!t.is_string()) continue;
        const std::string id = t.get<std::string>();
        if (!Registry::instance().find(id)) {
            if (error) *error = "unknown test id in suite: " + id;
            return std::nullopt;
        }
        s.tests.push_back(id);
    }
    return s;
}

std::optional<Suite> resolveSuite(const std::string& idOrPath, std::string* error) {
    for (auto& s : presetSuites())
        if (s.id == idOrPath) return s;
    std::error_code ec;
    if (std::filesystem::exists(idOrPath, ec)) {
        try {
            std::ifstream f(idOrPath);
            return suiteFromJson(nlohmann::json::parse(f), error);
        } catch (const std::exception& e) {
            if (error) *error = std::string("cannot parse suite file: ") + e.what();
            return std::nullopt;
        }
    }
    if (error) *error = "unknown suite '" + idOrPath + "' (presets: quick, standard, extreme, raytracing, legacy, compute, diagnostics, full, smoke; or a .json file)";
    return std::nullopt;
}

} // namespace gt::bench
