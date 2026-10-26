#pragma once
// GPU telemetry. Vendor libraries are loaded at runtime; a missing library or
// symbol just means fewer channels. Nothing here requires a vendor SDK to build.

#include "gt/results.h"

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gt::telemetry {

/// How to find the GPU the benchmark runs on.
struct DeviceMatch {
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::string name;
    std::optional<std::string> pciAddress; ///< "dddd:bb:dd.f"
    std::optional<std::array<std::uint8_t, 8>> luid;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string name() const = 0;
    /// Fills whatever channels it supports; leaves others untouched (NaN).
    virtual void sample(results::TelemetrySample& s) = 0;
    /// Adds static identity data (VBIOS, PCIe link, ...) to the GPU info.
    virtual void fillGpuInfo(results::GpuInfo&) {}
};

/// Opens every provider that matches the device, in priority order, merged into
/// one (earlier providers win per channel). Never returns null: falls back to
/// an empty provider named "none". `log` receives one line per probe for diagnostics.
std::unique_ptr<Provider> openProviders(const DeviceMatch& match, std::vector<std::string>* log = nullptr);

/// Individual providers (exposed for tests and the capability report).
std::unique_ptr<Provider> openNvml(const DeviceMatch& match, std::string* why);
std::unique_ptr<Provider> openLinuxSysfs(const DeviceMatch& match, std::string* why);
/// Synthetic sensors for tests, enabled by the GT_FAKE_TELEMETRY environment variable.
std::unique_ptr<Provider> openFake(std::string* why);

/// Samples a provider on a background thread at a fixed interval.
class Sampler {
public:
    using Callback = std::function<void(const results::TelemetrySample&)>;
    /// `extra` runs on the sampler thread after the provider (e.g. VRAM from the graphics API).
    Sampler(std::unique_ptr<Provider> provider, double intervalS,
            std::function<void(results::TelemetrySample&)> extra = {});
    ~Sampler();
    void start();
    void stop();
    /// Called on the sampler thread for every sample (keep it cheap).
    void setCallback(Callback cb);
    /// Sample timestamps are relative to this point (default: construction time).
    void setStartTime(std::chrono::steady_clock::time_point t0) { m_t0 = t0; }
    results::TelemetrySample latest() const;
    std::vector<results::TelemetrySample> samples() const;
    std::string providerName() const { return m_provider->name(); }
    double interval() const { return m_interval; }
    Provider& provider() { return *m_provider; }

private:
    void loop();
    std::unique_ptr<Provider> m_provider;
    double m_interval;
    std::function<void(results::TelemetrySample&)> m_extra;
    Callback m_callback;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<results::TelemetrySample> m_samples;
    results::TelemetrySample m_latest;
    std::thread m_thread;
    bool m_running = false;
    std::chrono::steady_clock::time_point m_t0;
};

} // namespace gt::telemetry
