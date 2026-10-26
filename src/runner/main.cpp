// Benchmark runner CLI. Everything the GUI can do is available here; see docs/CLI.md.
#include "events.h"
#include "gt/app_info.h"
#include "gt/bench/suites.h"
#include "gt/log.h"
#include "gt/report.h"
#include "gt/sysinfo.h"
#include "runner.h"
#include "sysreport.h"

#include <CLI/CLI.hpp>
#include <SDL3/SDL_main.h>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

using namespace gt;

namespace {

runner::Runner* g_runner = nullptr;

void onSignal(int) {
    if (g_runner) g_runner->requestStop(results::RunStatus::Stopped, "interrupted");
}

bool parseResolution(const std::string& text, std::uint32_t& w, std::uint32_t& h) {
    std::string s = text;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "720p") return w = 1280, h = 720, true;
    if (s == "1080p" || s == "fhd") return w = 1920, h = 1080, true;
    if (s == "1440p" || s == "qhd") return w = 2560, h = 1440, true;
    if (s == "4k" || s == "2160p" || s == "uhd") return w = 3840, h = 2160, true;
    const auto x = s.find('x');
    if (x == std::string::npos) return false;
    try {
        const long ww = std::stol(s.substr(0, x)), hh = std::stol(s.substr(x + 1));
        if (ww < 16 || hh < 16 || ww > 16384 || hh > 16384) return false;
        w = static_cast<std::uint32_t>(ww);
        h = static_cast<std::uint32_t>(hh);
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json testsJson() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : bench::Registry::instance().list()) {
        nlohmann::json apis = nlohmann::json::array();
        for (auto a : t.apis) apis.push_back(rhi::apiName(a));
        arr.push_back({{"id", t.id},
                       {"name", t.name},
                       {"description", t.description},
                       {"category", bench::categoryName(t.category)},
                       {"group", t.group},
                       {"apis", apis},
                       {"frame_based", t.frameBased},
                       {"primary_metric", t.primaryMetric},
                       {"primary_unit", t.primaryUnit},
                       {"scored", t.reference > 0.0},
                       {"higher_is_better", t.higherIsBetter},
                       {"min_vram_mb", t.minVramMB},
                       {"tags", t.tags},
                       {"suites", t.suites}});
    }
    return arr;
}

} // namespace

int main(int argc, char** argv) {
    runner::Options opt;
    for (int i = 0; i < argc; ++i) opt.commandLine += (i ? " " : "") + std::string(argv[i]);

    CLI::App app{std::format("{} {} — GPU benchmark, feature tester and diagnostics runner", app::kDisplayName,
                             app::kVersion),
                 app::kRunnerName};
    app.set_version_flag("--version", std::format("{} {} ({})", app::kRunnerName, app::kVersion, app::kGitRevision));

    bool listDevices = false, listTests = false, listSuites = false, showCaps = false, showSys = false,
         checkSupport = false, json = false, noSafety = false;
    std::string apiText, qualityText, durationText, progressText = "text", logLevel = "info", logFile, dumpDir,
                                                    deviceLost = "abort";
    std::vector<std::string> params, testIds;

    auto* info = app.add_option_group("Information");
    info->add_flag("--list-devices", listDevices, "List GPUs for every API");
    info->add_flag("--list-tests", listTests, "List all tests");
    info->add_flag("--list-suites", listSuites, "List preset suites");
    info->add_flag("--caps", showCaps, "Print the capability listing of the selected GPU/API");
    info->add_flag("--sysinfo", showSys, "Print system information for the selected GPU/API");
    info->add_flag("--check-support", checkSupport, "Report which tests the selected GPU/API can run, and why not");
    info->add_flag("--json", json, "Machine-readable output for the information commands");
    std::vector<std::string> exportHtml, exportCsv;
    info->add_option("--export-html", exportHtml, "Convert a results file to a self-contained HTML report: IN OUT")
        ->expected(2);
    info->add_option("--export-csv", exportCsv, "Convert a results file to CSV: IN OUT")->expected(2);

    auto* sel = app.add_option_group("Selection");
    sel->add_option("--suite,-s", opt.suite, "Preset suite (quick, standard, extreme, raytracing, legacy, compute, "
                                             "diagnostics, full, smoke) or a custom suite .json file");
    sel->add_option("--test,-t", testIds, "Run these test ids instead of a suite (repeat or comma-separate)")
        ->delimiter(',');
    sel->add_option("--api,-a", apiText, "vulkan, d3d12, d3d11 or opengl (default: best available)");
    sel->add_option("--gpu,-g", opt.gpu, "Adapter index within the API (see --list-devices; default: first discrete)");

    auto* set = app.add_option_group("Settings");
    set->add_option("--resolution,-r", opt.resolutionText, "Render resolution: 720p, 1080p, 1440p, 4k or WxH");
    set->add_option("--quality,-q", qualityText, "low, medium, high or ultra");
    set->add_option("--duration,-d", durationText, "smoke, quick, standard or extreme");
    set->add_option("--loop,-l", opt.loopText, "1 (default), N loops (\"5\"), a duration (\"20m\") or \"inf\"");
    set->add_flag("--offscreen", opt.offscreen, "Render without a window (no present)");
    set->add_flag("--fullscreen", opt.fullscreen, "Fullscreen window");
    set->add_flag("--vsync", opt.vsync, "Enable vsync (off by default)");
    set->add_flag("!--no-hud", opt.hud, "Hide the in-window HUD");
    set->add_option("--window-width", opt.windowWidth, "Window width in pixels");
    set->add_option("--window-height", opt.windowHeight, "Window height in pixels");
    set->add_option("--seed", opt.settings.seed, "Workload seed (keep the default for comparable scores)");
    set->add_option("--param", params, "Per-test parameter key=value (repeatable)");

    auto* safety = app.add_option_group("Safety");
    safety->add_option("--max-temp", opt.safety.maxTempC, "Abort above this GPU temperature (°C)");
    safety->add_option("--max-hotspot", opt.safety.maxHotspotC, "Abort above this hotspot temperature (°C)");
    safety->add_option("--max-mem-temp", opt.safety.maxMemTempC, "Abort above this memory temperature (°C)");
    safety->add_option("--max-power", opt.safety.maxPowerW, "Abort above this board power (W, 0 = off)");
    safety->add_flag("--no-safety", noSafety, "Disable safety cutoffs");
    safety->add_option("--stability-threshold", opt.stabilityThreshold, "Loop stability pass threshold (0..1)");
    safety->add_option("--on-device-lost", deviceLost, "abort (default) or recover (recreate the device, continue)");

    auto* outg = app.add_option_group("Output");
    outg->add_option("--out,-o", opt.out, "Results JSON path (default: <data dir>/results/<time>_<suite>.json)");
    outg->add_option("--progress", progressText, "Progress on stdout: text, jsonl or quiet");
    outg->add_flag("--control-stdin", opt.controlStdin, "Accept 'stop' commands on stdin (used by the GUI)");
    outg->add_option("--telemetry-interval", opt.telemetryIntervalS, "Telemetry sampling interval (seconds)");
    outg->add_option("--dump-frames", dumpDir, "Write golden frames as PNG into this directory");
    outg->add_option("--golden", opt.goldenDir, "Compare frames against golden.json in this directory");
    outg->add_flag("--validation", opt.validation, "Enable API validation (Vulkan layers / D3D debug layer)");
    outg->add_flag("--fail-on-validation", opt.failOnValidation, "Exit with code 6 if validation reported errors");
    outg->add_option("--log-level", logLevel, "trace, debug, info, warn, error");
    outg->add_option("--log-file", logFile, "Also write the log to this file");

    CLI11_PARSE(app, argc, argv);

    log::Level lvl = log::Level::Info;
    if (!log::parseLevel(logLevel, lvl)) {
        std::fprintf(stderr, "invalid --log-level '%s'\n", logLevel.c_str());
        return runner::kExitUsage;
    }
    log::setLevel(lvl);
    if (!logFile.empty() && !log::setFile(logFile)) log::warn("cannot open log file {}", logFile);

    // --- information commands ---------------------------------------------------------------
    if (exportHtml.size() == 2 || exportCsv.size() == 2) {
        const auto& args = exportHtml.size() == 2 ? exportHtml : exportCsv;
        std::string err;
        auto run = results::readFile(args[0], &err);
        if (!run) {
            std::fprintf(stderr, "cannot read %s: %s\n", args[0].c_str(), err.c_str());
            return runner::kExitUsage;
        }
        const std::string content = exportHtml.size() == 2 ? report::toHtml(*run) : report::toCsv(*run);
        if (!report::writeText(args[1], content, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return runner::kExitSetupFailed;
        }
        std::printf("wrote %s\n", args[1].c_str());
        return 0;
    }
    if (listTests) {
        if (json) std::cout << testsJson().dump(2) << "\n";
        else
            for (const auto& t : bench::Registry::instance().list())
                std::printf("%-32s %-11s %s\n", t.id.c_str(), bench::categoryName(t.category).c_str(), t.name.c_str());
        return 0;
    }
    if (listSuites) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : bench::presetSuites()) arr.push_back(bench::toJson(s));
        if (json) std::cout << arr.dump(2) << "\n";
        else
            for (const auto& s : bench::presetSuites())
                std::printf("%-12s %3zu tests  %s\n", s.id.c_str(), s.tests.size(), s.description.c_str());
        return 0;
    }
    if (listDevices) {
        const auto j = runner::devicesJson();
        if (json) {
            std::cout << j.dump(2) << "\n";
        } else {
            for (const auto& api : j["apis"]) {
                std::printf("%s:%s\n", api["display_name"].get<std::string>().c_str(),
                            api["available"].get<bool>() ? "" : (" unavailable (" + api["error"].get<std::string>() + ")").c_str());
                for (const auto& a : api["adapters"])
                    std::printf("  [%d] %s (%s, driver %s, API %s)%s\n", a["index"].get<int>(),
                                a["name"].get<std::string>().c_str(), a["type"].get<std::string>().c_str(),
                                a["driver_version"].get<std::string>().c_str(), a["api_version"].get<std::string>().c_str(),
                                a["index"] == api["default"] ? "  <- default" : "");
            }
        }
        return 0;
    }

    if (!apiText.empty()) {
        opt.api = rhi::parseApi(apiText);
        if (!opt.api) {
            std::fprintf(stderr, "unknown --api '%s'\n", apiText.c_str());
            return runner::kExitUsage;
        }
    } else {
        opt.api = runner::defaultApi();
    }

    if (showCaps || showSys || checkSupport) {
        try {
            std::string error;
            const auto adapters = rhi::enumerateAdapters(*opt.api, &error);
            if (adapters.empty()) throw rhi::Error(error.empty() ? "no adapters" : error);
            rhi::DeviceCreateInfo ci;
            ci.api = *opt.api;
            ci.adapterIndex = static_cast<std::uint32_t>(opt.gpu >= 0 ? opt.gpu : runner::defaultAdapter(adapters));
            auto dev = rhi::createDevice(ci);
            if (checkSupport) {
                nlohmann::json arr = nlohmann::json::array();
                bench::RunSettings settings;
                std::atomic<bool> stop{false};
                std::string reason;
                for (const auto& t : bench::Registry::instance().list()) {
                    bool ok = true;
                    std::string why;
                    if (!t.apis.empty() && std::find(t.apis.begin(), t.apis.end(), *opt.api) == t.apis.end()) {
                        ok = false;
                        why = std::format("not implemented for {}", rhi::apiDisplayName(*opt.api));
                    } else {
                        auto test = bench::Registry::instance().create(t.id);
                        bench::TestContext ctx(*dev, settings, t, nullptr, nullptr, &stop, &reason);
                        const auto r = test->checkSupport(ctx);
                        ok = r.supported;
                        why = r.reason;
                    }
                    arr.push_back({{"id", t.id}, {"supported", ok}, {"reason", why}});
                }
                if (json) std::cout << arr.dump(2) << "\n";
                else
                    for (const auto& e : arr)
                        std::printf("%-32s %s %s\n", e["id"].get<std::string>().c_str(),
                                    e["supported"].get<bool>() ? "yes" : "NO ", e["reason"].get<std::string>().c_str());
                return 0;
            }
            auto provider = telemetry::openProviders(runner::deviceMatch(dev->adapter()));
            const auto sys = runner::buildSystemInfo(*dev, provider.get());
            if (showSys) {
                nlohmann::json j = results::toJson(sys);
                j.erase("capabilities");
                j["telemetry_provider"] = provider->name();
                j["features"] = {{"timestamps", dev->features().timestamps},
                                 {"async_compute_queue", dev->features().asyncComputeQueue},
                                 {"copy_queue", dev->features().copyQueue},
                                 {"ray_query", dev->features().rayQuery},
                                 {"ray_tracing_pipeline", dev->features().rayTracingPipeline},
                                 {"mesh_shader", dev->features().meshShader},
                                 {"variable_rate_shading", dev->features().variableRateShading},
                                 {"shader_float16", dev->features().shaderFloat16},
                                 {"shader_float64", dev->features().shaderFloat64},
                                 {"shader_int8", dev->features().shaderInt8},
                                 {"atomic_int64", dev->features().atomicInt64},
                                 {"subgroup_size", dev->features().subgroupSize},
                                 {"resizable_bar", dev->features().resizableBar}};
                std::cout << j.dump(2) << "\n";
            } else {
                if (json) std::cout << sys.capabilities.dump(2) << "\n";
                else
                    for (const auto& c : sys.capabilities)
                        std::printf("%-28s %-56s %s\n", c["group"].get<std::string>().c_str(),
                                    c["name"].get<std::string>().c_str(), c["value"].get<std::string>().c_str());
            }
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return runner::kExitSetupFailed;
        }
    }

    // --- benchmark run -------------------------------------------------------------------------
    std::string error;
    bench::Suite suite;
    if (!testIds.empty()) {
        suite.id = "custom";
        suite.name = "Custom selection";
        for (const auto& id : testIds) {
            if (!bench::Registry::instance().find(id)) {
                std::fprintf(stderr, "unknown test id '%s' (see --list-tests)\n", id.c_str());
                return runner::kExitUsage;
            }
            suite.tests.push_back(id);
        }
    } else {
        auto s = bench::resolveSuite(opt.suite, &error);
        if (!s) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return runner::kExitUsage;
        }
        suite = *s;
    }
    // Suite defaults apply unless the CLI set the value explicitly.
    if (qualityText.empty()) qualityText = suite.quality.empty() ? "high" : suite.quality;
    if (durationText.empty()) durationText = suite.duration.empty() ? "standard" : suite.duration;
    if (app.count("--loop") == 0 && !suite.loop.empty()) opt.loopText = suite.loop;
    if (app.count("--resolution") == 0 && !suite.resolution.empty()) opt.resolutionText = suite.resolution;
    if (!bench::parseQuality(qualityText, opt.settings.quality)) {
        std::fprintf(stderr, "invalid --quality '%s'\n", qualityText.c_str());
        return runner::kExitUsage;
    }
    if (!bench::parseDuration(durationText, opt.settings.duration)) {
        std::fprintf(stderr, "invalid --duration '%s'\n", durationText.c_str());
        return runner::kExitUsage;
    }
    if (!parseResolution(opt.resolutionText, opt.settings.width, opt.settings.height)) {
        std::fprintf(stderr, "invalid --resolution '%s'\n", opt.resolutionText.c_str());
        return runner::kExitUsage;
    }
    auto loop = LoopSpec::parse(opt.loopText);
    if (!loop) {
        std::fprintf(stderr, "invalid --loop '%s' (use 1, N, 20m, 1h30m or inf)\n", opt.loopText.c_str());
        return runner::kExitUsage;
    }
    opt.loop = *loop;
    for (const auto& p : params) {
        const auto eq = p.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "invalid --param '%s' (expected key=value)\n", p.c_str());
            return runner::kExitUsage;
        }
        opt.settings.params[p.substr(0, eq)] = p.substr(eq + 1);
    }
    if (!dumpDir.empty()) opt.settings.dumpDir = dumpDir;
    opt.settings.dumpFrames = !dumpDir.empty() || !opt.goldenDir.empty();
    if (noSafety) opt.safety.enabled = false;
    if (deviceLost == "recover") opt.onDeviceLost = runner::DeviceLostPolicy::Recover;
    else if (deviceLost != "abort") {
        std::fprintf(stderr, "invalid --on-device-lost '%s'\n", deviceLost.c_str());
        return runner::kExitUsage;
    }
    if (progressText == "jsonl") opt.progress = runner::ProgressMode::JsonLines;
    else if (progressText == "quiet") opt.progress = runner::ProgressMode::Quiet;
    else if (progressText != "text") {
        std::fprintf(stderr, "invalid --progress '%s'\n", progressText.c_str());
        return runner::kExitUsage;
    }
    if (suite.tests.empty()) {
        std::fprintf(stderr, "suite '%s' has no tests\n", suite.id.c_str());
        return runner::kExitUsage;
    }

    runner::StdoutSink sink(opt.progress);
    if (opt.progress == runner::ProgressMode::JsonLines) {
        // Forward warnings and errors into the stream so the GUI can show them.
        log::setHook(
            [](log::Level l, std::string_view msg, void* user) {
                static_cast<runner::StdoutSink*>(user)->emit(
                    {{"type", "log"}, {"level", log::levelName(l)}, {"message", std::string(msg)}});
            },
            &sink, log::Level::Warn);
    }
    runner::Runner r(opt, suite, sink);
    g_runner = &r;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (opt.controlStdin) {
        std::thread([&r] {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "stop") r.requestStop(results::RunStatus::Stopped, "stopped by user");
            }
            // stdin closed: the controlling GUI went away.
            r.requestStop(results::RunStatus::Stopped, "controller disconnected");
        }).detach();
    }
    const int code = r.run();
    g_runner = nullptr;
    log::setHook(nullptr, nullptr);
    return code;
}
