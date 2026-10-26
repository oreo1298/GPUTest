// Linux kernel-driver telemetry: amdgpu, i915, xe (and generic hwmon for others).
#include "gt/sysinfo.h"
#include "gt/telemetry/telemetry.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

namespace gt::telemetry {

#ifdef __linux__
namespace fs = std::filesystem;
namespace {

std::optional<double> readNumber(const fs::path& p) {
    if (p.empty()) return std::nullopt;
    std::ifstream f(p);
    double v = 0.0;
    if (f >> v) return v;
    return std::nullopt;
}

std::string readText(const fs::path& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

fs::path existing(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path{};
}

// gpu_metrics v1.3 (RDNA1..RDNA4 dGPUs): indep_throttle_status at byte offset 112.
std::uint32_t mapAmdIndepThrottle(std::uint64_t r) {
    using namespace results::throttle;
    std::uint32_t m = 0;
    if (r & 0xFFull) m |= kPower;                    // PPT0..3, SPL, FPPT, SPPT, SPPT_APU
    if (r & (0xFFull << 16)) m |= kCurrent;          // TDC_*, EDC_*, APCC
    if (r & ((1ull << 32) | (1ull << 33) | (1ull << 35) | (1ull << 36) | (1ull << 37))) m |= kThermal;
    if (r & (1ull << 34)) m |= kMemThermal;
    if (r & ((1ull << 38) | (1ull << 39) | (1ull << 40) | (1ull << 41) | (1ull << 42) | (1ull << 43)))
        m |= kThermal;                               // VR / liquid temperatures
    if (r & ((1ull << 44) | (1ull << 45) | (1ull << 46) | (1ull << 47))) m |= kHwSlowdown; // VRHOT, PROCHOT
    if (r & ((1ull << 56) | (1ull << 57))) m |= kVoltage; // PPM, FIT (reliability)
    return m;
}

class SysfsProvider final : public Provider {
public:
    explicit SysfsProvider(const fs::path& dev) : m_dev(dev) {
        std::error_code ec;
        if (auto drv = fs::read_symlink(dev / "driver", ec); !ec) m_driver = drv.filename().string();
        for (const auto& e : fs::directory_iterator(dev / "hwmon", ec)) {
            m_hwmon = e.path();
            break;
        }
        if (!m_hwmon.empty()) {
            for (int i = 1; i <= 8; ++i) {
                const fs::path input = existing(m_hwmon / ("temp" + std::to_string(i) + "_input"));
                if (input.empty()) continue;
                const std::string label = readText(m_hwmon / ("temp" + std::to_string(i) + "_label"));
                if (label == "edge" || (label.empty() && m_tempEdge.empty()) || label == "pkg" || label == "gpu")
                    m_tempEdge = m_tempEdge.empty() ? input : m_tempEdge;
                else if (label == "junction" || label == "hotspot") m_tempHotspot = input;
                else if (label == "mem" || label == "vram") m_tempMem = input;
            }
            m_freqSclk = existing(m_hwmon / "freq1_input");
            m_freqMclk = existing(m_hwmon / "freq2_input");
            m_power = existing(m_hwmon / "power1_average");
            if (m_power.empty()) m_power = existing(m_hwmon / "power1_input");
            m_energy = existing(m_hwmon / "energy1_input");
            m_powerCap = existing(m_hwmon / "power1_cap");
            if (m_powerCap.empty()) m_powerCap = existing(m_hwmon / "power1_max");
            m_fanRpm = existing(m_hwmon / "fan1_input");
            m_pwm = existing(m_hwmon / "pwm1");
        }
        m_gpuBusy = existing(dev / "gpu_busy_percent");
        m_memBusy = existing(dev / "mem_busy_percent");
        m_vramUsed = existing(dev / "mem_info_vram_used");
        m_vramTotal = existing(dev / "mem_info_vram_total");
        m_gpuMetrics = existing(dev / "gpu_metrics");

        // Intel frequency + throttle reasons.
        for (const auto& e : fs::directory_iterator(dev / "drm", ec)) {
            const std::string n = e.path().filename().string();
            if (n.rfind("card", 0) != 0) continue;
            fs::path gt0 = e.path() / "gt" / "gt0";
            if (fs::exists(gt0 / "rps_act_freq_mhz", ec)) {
                m_intelFreq = gt0 / "rps_act_freq_mhz";
                m_intelThrottleDir = gt0;
                m_intelThrottlePrefix = "throttle_reason_";
            } else if (fs::exists(e.path() / "gt_act_freq_mhz", ec)) {
                m_intelFreq = e.path() / "gt_act_freq_mhz";
            }
        }
        if (fs::exists(dev / "tile0" / "gt0" / "freq0" / "act_freq", ec)) {
            m_intelFreq = dev / "tile0" / "gt0" / "freq0" / "act_freq";
            m_intelThrottleDir = dev / "tile0" / "gt0" / "freq0" / "throttle";
            m_intelThrottlePrefix = "reason_";
        }
        m_lastEnergyTime = std::chrono::steady_clock::now();
    }

    std::string name() const override { return m_driver.empty() ? "sysfs" : m_driver + "-sysfs"; }

    bool useful() const {
        return !m_hwmon.empty() || !m_gpuBusy.empty() || !m_vramUsed.empty() || !m_intelFreq.empty();
    }

    void sample(results::TelemetrySample& s) override {
        if (auto v = readNumber(m_freqSclk)) s.gpuClockMhz = float(*v / 1e6);
        if (auto v = readNumber(m_freqMclk)) s.memClockMhz = float(*v / 1e6);
        if (auto v = readNumber(m_intelFreq)) s.gpuClockMhz = float(*v);
        if (auto v = readNumber(m_tempEdge)) s.tempEdgeC = float(*v / 1000.0);
        if (auto v = readNumber(m_tempHotspot)) s.tempHotspotC = float(*v / 1000.0);
        if (auto v = readNumber(m_tempMem)) s.tempMemC = float(*v / 1000.0);
        if (auto v = readNumber(m_power)) {
            s.powerW = float(*v / 1e6);
        } else if (auto e = readNumber(m_energy)) {
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - m_lastEnergyTime).count();
            if (m_lastEnergy && dt > 0.0 && *e >= *m_lastEnergy) s.powerW = float((*e - *m_lastEnergy) / 1e6 / dt);
            m_lastEnergy = *e;
            m_lastEnergyTime = now;
        }
        if (auto v = readNumber(m_powerCap); v && *v > 0) s.powerLimitW = float(*v / 1e6);
        if (auto v = readNumber(m_fanRpm)) s.fanRpm = float(*v);
        if (auto v = readNumber(m_pwm)) s.fanPercent = float(*v / 255.0 * 100.0);
        if (auto v = readNumber(m_gpuBusy)) s.gpuUtilPercent = float(*v);
        if (auto v = readNumber(m_memBusy)) s.memUtilPercent = float(*v);
        if (auto v = readNumber(m_vramUsed)) s.vramUsedMB = float(*v / (1024.0 * 1024.0));
        if (auto v = readNumber(m_vramTotal)) s.vramTotalMB = float(*v / (1024.0 * 1024.0));
        if (!m_gpuMetrics.empty()) readGpuMetrics(s);
        if (!m_intelThrottleDir.empty()) readIntelThrottle(s);
    }

    void fillGpuInfo(results::GpuInfo& g) override {
        const std::string vbios = readText(m_dev / "vbios_version");
        if (!vbios.empty()) g.vbios = vbios;
    }

private:
    void readGpuMetrics(results::TelemetrySample& s) {
        std::ifstream f(m_gpuMetrics, std::ios::binary);
        unsigned char buf[256] = {};
        f.read(reinterpret_cast<char*>(buf), sizeof(buf));
        const auto n = static_cast<std::size_t>(f.gcount());
        if (n < 4) return;
        std::uint16_t size = 0;
        std::memcpy(&size, buf, 2);
        const std::uint8_t format = buf[2], content = buf[3];
        if (format == 1 && content == 3 && size >= 120 && n >= 120) {
            std::uint64_t indep = 0;
            std::memcpy(&indep, buf + 112, 8);
            s.throttleRaw = indep;
            s.throttle |= mapAmdIndepThrottle(indep);
        }
    }

    void readIntelThrottle(results::TelemetrySample& s) {
        using namespace results::throttle;
        auto flag = [&](const char* reason) {
            auto v = readNumber(m_intelThrottleDir / (m_intelThrottlePrefix + reason));
            return v && *v != 0.0;
        };
        std::uint32_t m = 0;
        std::uint64_t raw = 0;
        const char* reasons[] = {"pl1", "pl2", "pl4", "thermal", "prochot", "ratl", "vr_thermalert", "vr_tdc"};
        for (std::size_t i = 0; i < std::size(reasons); ++i)
            if (flag(reasons[i])) raw |= 1ull << i;
        if (raw & 0x7) m |= kPower;
        if (raw & 0x8) m |= kThermal;
        if (raw & 0x10) m |= kHwSlowdown;
        if (raw & 0x20) m |= kThermal; // running average thermal limit
        if (raw & 0x40) m |= kThermal;
        if (raw & 0x80) m |= kCurrent;
        s.throttle |= m;
        if (raw) s.throttleRaw = raw;
    }

    fs::path m_dev, m_hwmon;
    std::string m_driver;
    fs::path m_tempEdge, m_tempHotspot, m_tempMem, m_freqSclk, m_freqMclk, m_power, m_energy, m_powerCap, m_fanRpm,
        m_pwm, m_gpuBusy, m_memBusy, m_vramUsed, m_vramTotal, m_gpuMetrics, m_intelFreq, m_intelThrottleDir;
    std::string m_intelThrottlePrefix;
    std::optional<double> m_lastEnergy;
    std::chrono::steady_clock::time_point m_lastEnergyTime;
};

} // namespace

std::unique_ptr<Provider> openLinuxSysfs(const DeviceMatch& match, std::string* why) {
    if (!match.pciAddress) {
        if (why) *why = "sysfs: no PCI address for this device";
        return nullptr;
    }
    const fs::path dev = fs::path("/sys/bus/pci/devices") / *match.pciAddress;
    std::error_code ec;
    if (!fs::exists(dev, ec)) {
        if (why) *why = "sysfs: " + dev.string() + " not found";
        return nullptr;
    }
    auto p = std::make_unique<SysfsProvider>(dev);
    if (!p->useful()) {
        if (why) *why = "sysfs: no hwmon / driver telemetry files for " + *match.pciAddress;
        return nullptr;
    }
    return p;
}

#else

std::unique_ptr<Provider> openLinuxSysfs(const DeviceMatch&, std::string* why) {
    if (why) *why = "sysfs: not Linux";
    return nullptr;
}

#endif

} // namespace gt::telemetry
