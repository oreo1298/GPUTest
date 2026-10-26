#pragma once

#include "gt/bench/context.h"
#include "gt/duration.h"
#include "gt/rhi/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gt::runner {

enum class ProgressMode { Text, JsonLines, Quiet };
enum class DeviceLostPolicy { Abort, Recover };

struct SafetyLimits {
    bool enabled = true;
    double maxTempC = 90.0;     ///< edge / core temperature
    double maxHotspotC = 105.0; ///< junction / hotspot
    double maxMemTempC = 100.0;
    double maxPowerW = 0.0;     ///< 0 = no explicit limit (the GPU's own power limit applies)
    int gracePeriodS = 2;       ///< limit must be exceeded continuously this long
};

struct Options {
    // selection
    std::optional<rhi::Api> api;
    int gpu = -1; ///< -1 = automatic
    std::string suite = "standard";
    std::vector<std::string> tests;
    // settings
    bench::RunSettings settings;
    std::string resolutionText = "1080p";
    bool qualityExplicit = false;
    bool durationExplicit = false;
    bool loopExplicit = false;
    bool resolutionExplicit = false;
    LoopSpec loop;
    std::string loopText = "1";
    bool offscreen = false;
    bool fullscreen = false;
    bool vsync = false;
    bool hud = true;
    int windowWidth = 0, windowHeight = 0;
    bool validation = false;
    bool gpuValidation = false;
    SafetyLimits safety;
    double stabilityThreshold = 0.97;
    double telemetryIntervalS = 0.1;
    DeviceLostPolicy onDeviceLost = DeviceLostPolicy::Abort;
    std::string goldenDir; ///< compare dumped frames against references here
    // output
    std::string out;
    ProgressMode progress = ProgressMode::Text;
    bool controlStdin = false;
    bool failOnValidation = false;
    std::string commandLine;
};

} // namespace gt::runner
