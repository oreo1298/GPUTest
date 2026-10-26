#pragma once

#include "events.h"
#include "gt/bench/presenter.h"
#include "gt/bench/suites.h"
#include "gt/results.h"
#include "gt/telemetry/telemetry.h"
#include "options.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

struct SDL_Window;

namespace gt::runner {

/// Process exit codes (documented in docs/CLI.md).
enum ExitCode : int {
    kExitOk = 0,
    kExitUsage = 1,
    kExitSetupFailed = 2,
    kExitStopped = 3,
    kExitSafetyAbort = 4,
    kExitDeviceLost = 5,
    kExitValidationErrors = 6,
    kExitDiagnosticFail = 7,
};

class Runner {
public:
    Runner(Options options, bench::Suite suite, StdoutSink& sink);
    ~Runner();
    int run();
    /// Thread-safe; the first reason wins.
    void requestStop(results::RunStatus status, const std::string& reason);

private:
    void createDevice();
    void destroyDevice();
    void startTelemetry();
    results::TestResult runTest(const std::string& id, int loop, int index, int count);
    void checkGolden(results::TestResult& r);
    void writeResults();
    void onTelemetry(const results::TelemetrySample& s);
    double elapsed() const;
    std::vector<std::string> hudStatus() const;

    Options m_opt;
    bench::Suite m_suite;
    StdoutSink& m_sink;
    rhi::AdapterInfo m_adapter;
    std::unique_ptr<rhi::Device> m_device;
    SDL_Window* m_window = nullptr;
    std::unique_ptr<bench::Presenter> m_presenter;
    std::unique_ptr<telemetry::Sampler> m_sampler;
    results::RunResult m_run;
    std::atomic<bool> m_stop{false};
    std::mutex m_stopMutex;
    std::string m_stopReason;
    results::RunStatus m_stopStatus = results::RunStatus::Completed;
    std::chrono::steady_clock::time_point m_t0;
    std::string m_outPath;
    int m_loop = 0;
    std::string m_loopTotal;
    std::string m_currentTest;
    // safety state (sampler thread)
    double m_overSince = -1.0;
};

} // namespace gt::runner
