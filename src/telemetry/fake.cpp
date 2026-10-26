// Synthetic sensor data for testing the telemetry, safety and UI paths on machines
// without sensors (CI, VMs). Enabled only through the GT_FAKE_TELEMETRY environment
// variable; never used otherwise. Spec: "ramp" or "ramp:<°C per second>" — the
// temperature starts at 45 °C and rises at that rate (default 4 °C/s) up to 110 °C;
// the GPU reports power-limit throttling above 95 % of the limit and thermal
// throttling above 83 °C.

#include "gt/telemetry/telemetry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace gt::telemetry {
namespace {

class FakeProvider : public Provider {
public:
    explicit FakeProvider(double ratePerS) : m_rate(ratePerS), m_t0(std::chrono::steady_clock::now()) {}
    std::string name() const override { return "fake"; }
    void sample(results::TelemetrySample& s) override {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count();
        const double temp = std::min(110.0, 45.0 + m_rate * t);
        const double wobble = std::sin(t * 7.0);
        s.tempEdgeC = static_cast<float>(temp);
        s.tempHotspotC = static_cast<float>(temp + 12.0);
        s.tempMemC = static_cast<float>(temp + 6.0);
        s.powerLimitW = 300.0f;
        s.powerW = static_cast<float>(270.0 + 25.0 * wobble);
        s.gpuClockMhz = static_cast<float>(temp > 83.0 ? 2100.0 : 2520.0);
        s.memClockMhz = 1250.0f;
        s.gpuUtilPercent = 99.0f;
        s.memUtilPercent = 60.0f;
        s.fanPercent = static_cast<float>(std::clamp(30.0 + (temp - 45.0) * 1.2, 30.0, 100.0));
        s.fanRpm = s.fanPercent * 30.0f;
        std::uint32_t th = 0;
        if (s.powerW > s.powerLimitW * 0.95f) th |= results::throttle::kPower;
        if (temp > 83.0) th |= results::throttle::kThermal;
        s.throttle = th;
    }
    void fillGpuInfo(results::GpuInfo& g) override {
        if (g.vbios.empty()) g.vbios = "fake";
    }

private:
    double m_rate;
    std::chrono::steady_clock::time_point m_t0;
};

} // namespace

std::unique_ptr<Provider> openFake(std::string* why) {
    const char* env = std::getenv("GT_FAKE_TELEMETRY");
    if (!env || !*env) return nullptr;
    const std::string spec(env);
    double rate = 4.0;
    if (spec.rfind("ramp", 0) != 0) {
        if (why) *why = "GT_FAKE_TELEMETRY: unknown spec '" + spec + "' (use ramp or ramp:<degrees per second>)";
        return nullptr;
    }
    if (const auto colon = spec.find(':'); colon != std::string::npos) rate = std::atof(spec.c_str() + colon + 1);
    return std::make_unique<FakeProvider>(rate);
}

} // namespace gt::telemetry
