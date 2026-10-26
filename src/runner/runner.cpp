#include "runner.h"

#include "gt/app_info.h"
#include "gt/log.h"
#include "gt/paths.h"
#include "gt/scoring.h"
#include "sysreport.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>

namespace gt::runner {

Runner::Runner(Options options, bench::Suite suite, StdoutSink& sink)
    : m_opt(std::move(options)), m_suite(std::move(suite)), m_sink(sink) {}

Runner::~Runner() {
    if (m_sampler) m_sampler->stop();
    destroyDevice();
}

double Runner::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count();
}

void Runner::requestStop(results::RunStatus status, const std::string& reason) {
    std::lock_guard lock(m_stopMutex);
    if (m_stop.load()) return;
    m_stopStatus = status;
    m_stopReason = reason;
    m_stop.store(true);
}

void Runner::createDevice() {
    const rhi::Api api = *m_opt.api;
    std::string error;
    const auto adapters = rhi::enumerateAdapters(api, &error);
    if (adapters.empty()) throw rhi::Error(std::format("no {} adapters: {}", rhi::apiDisplayName(api), error));
    int index = m_opt.gpu >= 0 ? m_opt.gpu : defaultAdapter(adapters);
    if (index < 0 || index >= static_cast<int>(adapters.size()))
        throw rhi::Error(std::format("GPU index {} out of range (0..{})", index, adapters.size() - 1));
    m_adapter = adapters[static_cast<std::size_t>(index)];
    m_opt.gpu = index;

    if (!m_opt.offscreen) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            log::warn("cannot initialize video ({}); running offscreen", SDL_GetError());
            m_opt.offscreen = true;
        } else {
            int w = m_opt.windowWidth, h = m_opt.windowHeight;
            if (w <= 0 || h <= 0) {
                // Default: the render resolution scaled to fit 80% of the primary display.
                w = static_cast<int>(m_opt.settings.width);
                h = static_cast<int>(m_opt.settings.height);
                SDL_Rect usable{};
                if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0) {
                    const double s = std::min({1.0, usable.w * 0.8 / w, usable.h * 0.8 / h});
                    w = static_cast<int>(w * s);
                    h = static_cast<int>(h * s);
                }
            }
            SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            if (api == rhi::Api::Vulkan) flags |= SDL_WINDOW_VULKAN;
            if (api == rhi::Api::OpenGL) flags |= SDL_WINDOW_OPENGL;
            if (m_opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
            const std::string title = std::format("{} — {}", app::kDisplayName, m_adapter.name);
            m_window = SDL_CreateWindow(title.c_str(), w, h, flags);
            if (!m_window) {
                log::warn("cannot create a window ({}); running offscreen", SDL_GetError());
                m_opt.offscreen = true;
            }
        }
    }

    rhi::DeviceCreateInfo ci;
    ci.api = api;
    ci.adapterIndex = static_cast<std::uint32_t>(index);
    ci.validation = m_opt.validation;
    ci.gpuAssisted = m_opt.gpuValidation;
    ci.window = m_window;
    m_device = rhi::createDevice(ci);
    if (m_window) {
        try {
            m_presenter = std::make_unique<bench::Presenter>(*m_device, m_window, m_opt.vsync, m_opt.hud);
        } catch (const std::exception& e) {
            log::warn("cannot present to the window ({}); running offscreen", e.what());
            m_presenter.reset();
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            m_opt.offscreen = true;
        }
    }
}

void Runner::destroyDevice() {
    try {
        m_presenter.reset();
        if (m_device) m_device->waitIdle();
    } catch (...) {
    }
    m_device.reset();
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void Runner::startTelemetry() {
    std::vector<std::string> probeLog;
    auto provider = telemetry::openProviders(deviceMatch(m_adapter), &probeLog);
    for (const auto& l : probeLog) log::info("{}", l);
    rhi::Device* dev = m_device.get();
    m_sampler = std::make_unique<telemetry::Sampler>(std::move(provider), m_opt.telemetryIntervalS,
                                                      [dev](results::TelemetrySample& s) {
                                                          // VRAM from the graphics API when the provider has none.
                                                          if (!std::isfinite(s.vramUsedMB) && dev) {
                                                              const auto mb = dev->memoryBudget();
                                                              if (mb.valid) {
                                                                  s.vramUsedMB = float(mb.localUsage / 1048576.0);
                                                                  s.vramTotalMB = float(mb.localBudget / 1048576.0);
                                                              }
                                                          }
                                                      });
    m_sampler->setStartTime(m_t0);
    m_sampler->setCallback([this](const results::TelemetrySample& s) { onTelemetry(s); });
    m_sampler->start();
}

void Runner::onTelemetry(const results::TelemetrySample& s) {
    if (m_sink.mode() == ProgressMode::JsonLines) {
        nlohmann::json e = results::toJson(s);
        e["type"] = "telemetry";
        m_sink.emit(e);
    }
    const SafetyLimits& lim = m_opt.safety;
    if (!lim.enabled) return;
    std::string violation;
    if (std::isfinite(s.tempEdgeC) && s.tempEdgeC > lim.maxTempC)
        violation = std::format("GPU temperature {:.0f} °C exceeded the {:.0f} °C limit", s.tempEdgeC, lim.maxTempC);
    else if (std::isfinite(s.tempHotspotC) && s.tempHotspotC > lim.maxHotspotC)
        violation = std::format("hotspot temperature {:.0f} °C exceeded the {:.0f} °C limit", s.tempHotspotC,
                                lim.maxHotspotC);
    else if (std::isfinite(s.tempMemC) && s.tempMemC > lim.maxMemTempC)
        violation = std::format("memory temperature {:.0f} °C exceeded the {:.0f} °C limit", s.tempMemC,
                                lim.maxMemTempC);
    else if (lim.maxPowerW > 0 && std::isfinite(s.powerW) && s.powerW > lim.maxPowerW)
        violation = std::format("board power {:.0f} W exceeded the {:.0f} W limit", s.powerW, lim.maxPowerW);
    if (violation.empty()) {
        m_overSince = -1.0;
        return;
    }
    if (m_overSince < 0) m_overSince = s.t;
    if (s.t - m_overSince >= lim.gracePeriodS && !m_stop.load()) {
        const std::string msg = "Safety cutoff: " + violation + ". The run was stopped to protect the hardware.";
        requestStop(results::RunStatus::SafetyAbort, msg);
        m_sink.emit({{"type", "safety_abort"}, {"message", msg}, {"t", s.t}});
        log::error("{}", msg);
    }
}

std::vector<std::string> Runner::hudStatus() const {
    std::vector<std::string> lines;
    if (m_sampler) {
        const auto s = m_sampler->latest();
        std::string t;
        if (std::isfinite(s.gpuClockMhz)) t += std::format("{:.0f} MHz  ", s.gpuClockMhz);
        if (std::isfinite(s.tempEdgeC)) t += std::format("{:.0f} C  ", s.tempEdgeC);
        if (std::isfinite(s.tempHotspotC)) t += std::format("hot {:.0f} C  ", s.tempHotspotC);
        if (std::isfinite(s.powerW)) t += std::format("{:.0f} W  ", s.powerW);
        if (std::isfinite(s.fanPercent)) t += std::format("fan {:.0f}%", s.fanPercent);
        if (!t.empty()) lines.push_back(t);
    }
    if (m_opt.loop.isLooping()) lines.push_back(std::format("Loop {}{}  {}", m_loop + 1, m_loopTotal, formatDuration(elapsed())));
    return lines;
}

void Runner::checkGolden(results::TestResult& r) {
    if (m_opt.goldenDir.empty() || !r.details.contains("golden")) return;
    const std::filesystem::path refPath = std::filesystem::path(m_opt.goldenDir) / "golden.json";
    nlohmann::json ref;
    try {
        std::ifstream f(refPath);
        if (f) ref = nlohmann::json::parse(f);
    } catch (...) {
    }
    if (!ref.is_object()) ref = nlohmann::json::object();
    const int tolerance = ref.value("tolerance", 8);
    const nlohmann::json refTest = ref.contains("tests") && ref["tests"].contains(r.id) ? ref["tests"][r.id] : nlohmann::json();
    nlohmann::json report = nlohmann::json::object();
    int worst = 0;
    bool missing = false;
    for (auto& [frame, g] : r.details["golden"].items()) {
        if (!refTest.contains(frame)) {
            missing = true;
            report[frame] = "no reference";
            continue;
        }
        double mean = 0.0;
        const int diff = bench::compareSignatures(g.value("signature", ""), refTest[frame].value("signature", ""), &mean);
        const bool exact = g.value("hash", "") == refTest[frame].value("hash", "");
        report[frame] = {{"max_diff", diff}, {"mean_diff", mean}, {"exact", exact}};
        worst = diff < 0 ? 255 : std::max(worst, diff);
    }
    r.details["golden_check"] = report;
    if (missing) {
        r.verdict = results::Verdict::Warn;
        r.verdictMessage = "No golden reference for some frames (run with --dump-frames to create one).";
    } else if (worst > tolerance) {
        r.verdict = results::Verdict::Fail;
        r.verdictMessage = std::format("Rendered frames differ from the reference (max cell difference {} > {}).",
                                       worst, tolerance);
    } else if (r.verdict == results::Verdict::None) {
        r.verdict = results::Verdict::Pass;
        r.verdictMessage = std::format("Rendered frames match the reference (max cell difference {}).", worst);
    }
}

results::TestResult Runner::runTest(const std::string& id, int loop, int index, int count) {
    const bench::TestInfo* info = bench::Registry::instance().find(id);
    results::TestResult tr;
    tr.id = id;
    tr.loop = loop;
    tr.api = std::string(rhi::apiName(*m_opt.api));
    const double startS = elapsed();
    tr.startS = startS;
    if (!info) {
        tr.status = results::Status::Error;
        tr.message = "unknown test";
        return tr;
    }
    tr.name = info->name;
    tr.category = bench::categoryName(info->category);
    tr.group = info->group;
    m_currentTest = info->name;
    m_sink.emit({{"type", "test_start"}, {"test", id}, {"name", info->name}, {"category", tr.category},
                 {"index", index}, {"count", count}, {"loop", loop}, {"t", tr.startS}});
    if (m_window) SDL_SetWindowTitle(m_window, std::format("{} — {}", app::kDisplayName, info->name).c_str());

    auto finish = [&](results::TestResult& r) {
        r.startS = startS;
        r.durationS = elapsed() - startS;
        r.loop = loop;
        return r;
    };

    // Static requirements.
    const auto& apis = info->apis;
    if (!apis.empty() && std::find(apis.begin(), apis.end(), *m_opt.api) == apis.end()) {
        tr.status = results::Status::Skipped;
        std::string list;
        for (auto a : apis) list += (list.empty() ? "" : ", ") + std::string(rhi::apiDisplayName(a));
        tr.message = std::format("not implemented for {} (available on: {})", rhi::apiDisplayName(*m_opt.api), list);
        return finish(tr);
    }
    if (info->minVramMB && m_adapter.dedicatedMemory / (1024 * 1024) < info->minVramMB &&
        m_adapter.type == rhi::AdapterType::Discrete) {
        tr.status = results::Status::Skipped;
        tr.message = std::format("needs {} MB of VRAM, the GPU has {} MB", info->minVramMB,
                                 m_adapter.dedicatedMemory / (1024 * 1024));
        return finish(tr);
    }

    const auto validationBefore = m_device->validationCounts();
    auto test = bench::Registry::instance().create(id);
    bench::TestContext ctx(*m_device, m_opt.settings, *info, &m_sink, m_presenter.get(), &m_stop, &m_stopReason);
    ctx.hudStatus = [this] { return hudStatus(); };
    try {
        const bench::SupportResult sup = test->checkSupport(ctx);
        if (!sup.supported) {
            tr.status = results::Status::Skipped;
            tr.message = sup.reason;
            return finish(tr);
        }
        test->run(ctx);
        tr = ctx.result();
        tr.status = m_stop.load() ? results::Status::Aborted : results::Status::Completed;
        if (m_stop.load()) tr.message = m_stopReason;
    } catch (const bench::StopRequested& s) {
        tr = ctx.result();
        tr.status = results::Status::Aborted;
        tr.message = s.reason;
    } catch (const rhi::DeviceLostError& e) {
        tr = ctx.result();
        tr.status = results::Status::DeviceLost;
        tr.message = e.what();
        tr.verdict = results::Verdict::Fail;
        tr.verdictMessage = "The GPU stopped responding (device lost / TDR). This points to a driver problem, an "
                            "unstable overclock or failing hardware.";
    } catch (const std::exception& e) {
        tr = ctx.result();
        tr.status = results::Status::Error;
        tr.message = e.what();
    }
    const auto validationAfter = m_device ? m_device->validationCounts() : validationBefore;
    if (validationAfter.errors > validationBefore.errors)
        tr.details["validation_errors"] = validationAfter.errors - validationBefore.errors;
    if (tr.status == results::Status::Completed && info->reference > 0.0) {
        if (const results::Metric* m = tr.metric(info->primaryMetric)) {
            tr.scored = true;
            tr.primaryMetric = info->primaryMetric;
            tr.score = scoring::testScore(m->value, info->reference, info->higherIsBetter);
        }
    }
    if (tr.status == results::Status::Completed) checkGolden(tr);
    return finish(tr);
}

void Runner::writeResults() {
    m_run.durationS = elapsed();
    if (m_sampler) {
        m_run.telemetry.provider = m_sampler->providerName();
        m_run.telemetry.intervalS = m_sampler->interval();
        m_run.telemetry.samples = m_sampler->samples();
        m_run.telemetry.computeSummary();
    }
    if (m_device) {
        const auto v = m_device->validationCounts();
        m_run.validationErrors = v.errors;
        m_run.validationWarnings = v.warnings;
    }
    scoring::computeScores(m_run, m_opt.stabilityThreshold);
    std::string err;
    if (!results::writeFile(m_run, m_outPath, &err)) log::error("cannot write results to {}: {}", m_outPath, err);
}

int Runner::run() {
    m_t0 = std::chrono::steady_clock::now();
    m_sink.setStart(m_t0);
    m_outPath = m_opt.out.empty()
                    ? (paths::resultsDir() / std::format("{}_{}.json", paths::timestampForFilename(), m_suite.id)).string()
                    : m_opt.out;

    m_run.appName = app::kDisplayName;
    m_run.appVersion = app::kVersion;
    m_run.appRevision = app::kGitRevision;
    m_run.createdUtc = paths::isoTimestampUtc();
    m_run.commandLine = m_opt.commandLine;
    auto& c = m_run.config;
    c.suite = m_suite.id;
    c.api = std::string(rhi::apiName(*m_opt.api));
    c.width = static_cast<int>(m_opt.settings.width);
    c.height = static_cast<int>(m_opt.settings.height);
    c.quality = bench::qualityName(m_opt.settings.quality);
    c.duration = bench::durationName(m_opt.settings.duration);
    c.loop = m_opt.loop.toString();
    c.fullscreen = m_opt.fullscreen;
    c.vsync = m_opt.vsync;
    c.seed = m_opt.settings.seed;
    c.validation = m_opt.validation;
    c.tests = m_suite.tests;
    c.safety = {{"enabled", m_opt.safety.enabled},
                {"max_temp_c", m_opt.safety.maxTempC},
                {"max_hotspot_c", m_opt.safety.maxHotspotC},
                {"max_mem_temp_c", m_opt.safety.maxMemTempC},
                {"max_power_w", m_opt.safety.maxPowerW}};

    m_sink.emit({{"type", "hello"}, {"app", app::kDisplayName}, {"version", app::kVersion},
                 {"revision", app::kGitRevision}, {"results_file", m_outPath}});
    try {
        createDevice();
    } catch (const std::exception& e) {
        log::error("device setup failed: {}", e.what());
        m_sink.emit({{"type", "error"}, {"message", std::string("device setup failed: ") + e.what()}});
        m_run.status = results::RunStatus::Aborted;
        m_run.statusMessage = std::string("device setup failed: ") + e.what();
        writeResults();
        m_sink.emit({{"type", "run_end"}, {"status", "aborted"}, {"results_file", m_outPath}});
        return kExitSetupFailed;
    }
    c.gpuIndex = m_opt.gpu;
    c.offscreen = m_opt.offscreen;
    startTelemetry();
    m_run.system = buildSystemInfo(*m_device, &m_sampler->provider());
    m_sink.emit({{"type", "run_start"},
                 {"suite", m_suite.id},
                 {"suite_name", m_suite.name},
                 {"tests", m_suite.tests},
                 {"api", c.api},
                 {"gpu", adapterJson(m_adapter)},
                 {"resolution", {c.width, c.height}},
                 {"quality", c.quality},
                 {"duration", c.duration},
                 {"loop", c.loop},
                 {"loop_count", m_opt.loop.mode == LoopSpec::Mode::Count ? nlohmann::json(m_opt.loop.count) : nlohmann::json()},
                 {"loop_seconds", m_opt.loop.mode == LoopSpec::Mode::Timed ? nlohmann::json(m_opt.loop.seconds) : nlohmann::json()},
                 {"offscreen", m_opt.offscreen},
                 {"present_mode", m_presenter ? m_presenter->presentMode() : "offscreen"},
                 {"telemetry_provider", m_sampler->providerName()}});
    log::info("{} on {} ({}), {} test(s), {}x{} {} {}, loop {}", m_suite.name, m_adapter.name,
              rhi::apiDisplayName(*m_opt.api), m_suite.tests.size(), c.width, c.height, c.quality, c.duration, c.loop);
    writeResults();

    const LoopSpec& ls = m_opt.loop;
    const int count = static_cast<int>(m_suite.tests.size());
    if (ls.mode == LoopSpec::Mode::Count) m_loopTotal = std::format("/{}", ls.count);
    else if (ls.mode == LoopSpec::Mode::Timed) m_loopTotal = std::format(" (timed {})", formatDuration(ls.seconds));
    else if (ls.mode == LoopSpec::Mode::Infinite) m_loopTotal = " (until stopped)";
    bool deviceLost = false;
    for (m_loop = 0;; ++m_loop) {
        if (ls.mode == LoopSpec::Mode::Single && m_loop >= 1) break;
        if (ls.mode == LoopSpec::Mode::Count && m_loop >= static_cast<int>(ls.count)) break;
        if (ls.mode == LoopSpec::Mode::Timed && elapsed() >= ls.seconds) break;
        if (m_stop.load() || deviceLost) break;
        results::LoopResult lr;
        lr.index = m_loop;
        lr.startS = elapsed();
        nlohmann::json ev{{"type", "loop_start"}, {"loop", m_loop}, {"t", lr.startS}};
        if (ls.mode == LoopSpec::Mode::Timed) ev["remaining_s"] = std::max(0.0, ls.seconds - lr.startS);
        m_sink.emit(ev);
        for (int i = 0; i < count; ++i) {
            if (m_stop.load()) break;
            if (ls.mode == LoopSpec::Mode::Timed && elapsed() >= ls.seconds) break;
            results::TestResult tr = runTest(m_suite.tests[static_cast<std::size_t>(i)], m_loop, i, count);
            m_run.tests.push_back(tr);
            nlohmann::json te{{"type", "test_end"}, {"result", results::toJson(tr)}, {"summary", results::summarize(tr)}};
            m_sink.emit(te);
            log::info("{}", results::summarize(tr));
            if (tr.status == results::Status::DeviceLost) {
                m_sink.emit({{"type", "device_lost"}, {"test", tr.id}, {"message", tr.message}});
                if (m_opt.onDeviceLost == DeviceLostPolicy::Recover) {
                    log::warn("device lost during {}: recreating the device and continuing", tr.id);
                    if (m_sampler) m_sampler->stop();
                    m_sampler.reset();
                    destroyDevice();
                    try {
                        createDevice();
                        startTelemetry();
                        m_run.warnings.push_back(std::format("device lost during {}; recovered", tr.id));
                    } catch (const std::exception& e) {
                        m_run.warnings.push_back(std::string("device recovery failed: ") + e.what());
                        deviceLost = true;
                    }
                } else {
                    deviceLost = true;
                }
                if (deviceLost) break;
            }
            m_run.loops.erase(std::remove_if(m_run.loops.begin(), m_run.loops.end(),
                                             [&](const results::LoopResult& l) { return l.index == m_loop; }),
                              m_run.loops.end());
            lr.durationS = elapsed() - lr.startS;
            m_run.loops.push_back(lr);
            writeResults();
        }
        lr.durationS = elapsed() - lr.startS;
        writeResults();
        double loopScore = 0.0;
        for (const auto& l : m_run.loops)
            if (l.index == m_loop) loopScore = l.score;
        m_sink.emit({{"type", "loop_end"}, {"loop", m_loop}, {"score", loopScore}, {"t", elapsed()}});
    }

    if (m_sampler) m_sampler->stop();
    if (deviceLost) {
        m_run.status = results::RunStatus::DeviceLost;
        m_run.statusMessage = "The GPU was lost (driver reset / TDR) — partial results were saved.";
    } else if (m_stop.load()) {
        m_run.status = m_stopStatus == results::RunStatus::Completed ? results::RunStatus::Stopped : m_stopStatus;
        m_run.statusMessage = m_stopReason;
    } else {
        m_run.status = results::RunStatus::Completed;
    }
    writeResults();

    bool diagnosticFail = false;
    for (const auto& t : m_run.tests) diagnosticFail = diagnosticFail || t.verdict == results::Verdict::Fail;
    m_sink.emit({{"type", "run_end"},
                 {"status", results::toString(m_run.status)},
                 {"message", m_run.statusMessage},
                 {"results_file", m_outPath},
                 {"overall_score", m_run.scores.overall},
                 {"validation_errors", m_run.validationErrors}});
    if (m_sink.mode() == ProgressMode::Text) {
        std::printf("\n%s — %s on %s (%s)\n", app::kDisplayName, m_suite.name.c_str(), m_adapter.name.c_str(),
                    std::string(rhi::apiDisplayName(*m_opt.api)).c_str());
        for (const auto& t : m_run.tests) std::printf("  %s\n", results::summarize(t).c_str());
        if (m_run.scores.overall > 0) std::printf("Overall score: %.0f\n", m_run.scores.overall);
        if (m_run.stability.valid)
            std::printf("Stability: %.1f%% (%s)\n", m_run.stability.ratio * 100.0, m_run.stability.passed ? "pass" : "FAIL");
        if (m_run.validationErrors) std::printf("Validation errors: %llu\n", static_cast<unsigned long long>(m_run.validationErrors));
        std::printf("Results: %s\n", m_outPath.c_str());
    }
    if (m_run.status == results::RunStatus::DeviceLost) return kExitDeviceLost;
    if (m_run.status == results::RunStatus::SafetyAbort) return kExitSafetyAbort;
    if (m_run.status == results::RunStatus::Stopped) return kExitStopped;
    if (m_opt.failOnValidation && m_run.validationErrors) return kExitValidationErrors;
    if (diagnosticFail) return kExitDiagnosticFail;
    for (const auto& t : m_run.tests)
        if (t.status == results::Status::Error) return kExitSetupFailed;
    return kExitOk;
}

} // namespace gt::runner
