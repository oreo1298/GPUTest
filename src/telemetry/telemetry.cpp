#include "gt/telemetry/telemetry.h"

#include <chrono>
#include <cmath>

namespace gt::telemetry {
namespace {

class NullProvider final : public Provider {
public:
    std::string name() const override { return "none"; }
    void sample(results::TelemetrySample&) override {}
};

/// Merges several providers; earlier ones win per channel.
class CompositeProvider final : public Provider {
public:
    explicit CompositeProvider(std::vector<std::unique_ptr<Provider>> ps) : m_ps(std::move(ps)) {}
    std::string name() const override {
        std::string n;
        for (const auto& p : m_ps) n += (n.empty() ? "" : "+") + p->name();
        return n;
    }
    void sample(results::TelemetrySample& s) override {
        const auto& ch = results::telemetryChannels();
        for (const auto& p : m_ps) {
            results::TelemetrySample tmp;
            p->sample(tmp);
            for (std::size_t c = 0; c < ch.size(); ++c) {
                if (!std::isfinite(results::telemetryValue(s, c)))
                    results::setTelemetryValue(s, c, results::telemetryValue(tmp, c));
            }
            s.throttle |= tmp.throttle;
            if (!s.throttleRaw) s.throttleRaw = tmp.throttleRaw;
        }
    }
    void fillGpuInfo(results::GpuInfo& g) override {
        for (auto it = m_ps.rbegin(); it != m_ps.rend(); ++it) (*it)->fillGpuInfo(g);
    }

private:
    std::vector<std::unique_ptr<Provider>> m_ps;
};

} // namespace

std::unique_ptr<Provider> openProviders(const DeviceMatch& match, std::vector<std::string>* log) {
    std::vector<std::unique_ptr<Provider>> ps;
    auto tryOpen = [&](std::unique_ptr<Provider> (*fn)(const DeviceMatch&, std::string*)) {
        std::string why;
        auto p = fn(match, &why);
        if (p) {
            if (log) log->push_back("telemetry: using " + p->name());
            ps.push_back(std::move(p));
        } else if (log && !why.empty()) {
            log->push_back("telemetry: " + why);
        }
    };
    if (match.vendorId == 0x10DE) tryOpen(&openNvml);
    tryOpen(&openLinuxSysfs);
    if (ps.empty()) return std::make_unique<NullProvider>();
    if (ps.size() == 1) return std::move(ps.front());
    return std::make_unique<CompositeProvider>(std::move(ps));
}

// --- sampler ----------------------------------------------------------------------------
Sampler::Sampler(std::unique_ptr<Provider> provider, double intervalS,
                 std::function<void(results::TelemetrySample&)> extra)
    : m_provider(std::move(provider)), m_interval(intervalS), m_extra(std::move(extra)) {
    m_t0 = std::chrono::steady_clock::now();
}

Sampler::~Sampler() { stop(); }

void Sampler::setCallback(Callback cb) {
    std::lock_guard lock(m_mutex);
    m_callback = std::move(cb);
}

void Sampler::start() {
    std::lock_guard lock(m_mutex);
    if (m_running) return;
    m_running = true;
    m_thread = std::thread([this] { loop(); });
}

void Sampler::stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_running) return;
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

results::TelemetrySample Sampler::latest() const {
    std::lock_guard lock(m_mutex);
    return m_latest;
}

std::vector<results::TelemetrySample> Sampler::samples() const {
    std::lock_guard lock(m_mutex);
    return m_samples;
}

void Sampler::loop() {
    auto next = std::chrono::steady_clock::now();
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(m_interval));
    for (;;) {
        results::TelemetrySample s;
        s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count();
        try {
            m_provider->sample(s);
            if (m_extra) m_extra(s);
        } catch (...) {
            // A failing vendor library must never take the benchmark down.
        }
        Callback cb;
        {
            std::lock_guard lock(m_mutex);
            m_samples.push_back(s);
            m_latest = s;
            cb = m_callback;
        }
        if (cb) cb(s);
        next += step;
        std::unique_lock lock(m_mutex);
        if (m_cv.wait_until(lock, next, [this] { return !m_running; })) break;
    }
}

} // namespace gt::telemetry
