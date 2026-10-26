// NVIDIA NVML provider. Prototypes and constants are declared here from the
// public NVML API documentation (nvml.h is not redistributed).
#include "dynlib.h"
#include "gt/sysinfo.h"
#include "gt/telemetry/telemetry.h"

#include <cstdlib>
#include <cstring>
#include <format>

namespace gt::telemetry {
namespace {

using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
enum NvmlClock : int { NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_SM = 1, NVML_CLOCK_MEM = 2 };
constexpr int NVML_TEMPERATURE_GPU = 0;

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};
struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};
union nvmlValue_t {
    double dVal;
    unsigned int uiVal;
    unsigned long ulVal;
    unsigned long long ullVal;
    signed long long sllVal;
    signed int siVal;
    unsigned short usVal;
};
struct nvmlFieldValue_t {
    unsigned int fieldId;
    unsigned int scopeId;
    long long timestamp;
    long long latencyUsec;
    int valueType;
    nvmlReturn_t nvmlReturn;
    nvmlValue_t value;
};
constexpr unsigned int NVML_FI_DEV_MEMORY_TEMP = 82;

// Clock event (throttle) reasons.
constexpr unsigned long long kIdle = 0x1ull;
constexpr unsigned long long kAppClocks = 0x2ull;
constexpr unsigned long long kSwPowerCap = 0x4ull;
constexpr unsigned long long kHwSlowdown = 0x8ull;
constexpr unsigned long long kSyncBoost = 0x10ull;
constexpr unsigned long long kSwThermal = 0x20ull;
constexpr unsigned long long kHwThermal = 0x40ull;
constexpr unsigned long long kHwPowerBrake = 0x80ull;
constexpr unsigned long long kDisplayClock = 0x100ull;

struct Api {
    DynLib lib;
    nvmlReturn_t (*init)() = nullptr;
    nvmlReturn_t (*shutdown)() = nullptr;
    const char* (*errorString)(nvmlReturn_t) = nullptr;
    nvmlReturn_t (*getCount)(unsigned int*) = nullptr;
    nvmlReturn_t (*byIndex)(unsigned int, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*byPci)(const char*, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*getName)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*clockInfo)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (*temperature)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (*powerUsage)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*powerLimit)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*fanSpeed)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*utilization)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (*memoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
    nvmlReturn_t (*throttleReasons)(nvmlDevice_t, unsigned long long*) = nullptr;
    nvmlReturn_t (*fieldValues)(nvmlDevice_t, int, nvmlFieldValue_t*) = nullptr;
    nvmlReturn_t (*currGen)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*currWidth)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*maxGen)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*maxWidth)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*gpuMaxGen)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*vbios)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*driverVersion)(char*, unsigned int) = nullptr;
};

std::uint32_t mapReasons(unsigned long long r) {
    std::uint32_t m = 0;
    if (r & kIdle) m |= results::throttle::kIdle;
    if (r & (kAppClocks | kDisplayClock | kSyncBoost)) m |= results::throttle::kApplication;
    if (r & kSwPowerCap) m |= results::throttle::kPower;
    if (r & (kHwSlowdown | kHwPowerBrake)) m |= results::throttle::kHwSlowdown;
    if (r & (kSwThermal | kHwThermal)) m |= results::throttle::kThermal;
    return m;
}

class NvmlProvider final : public Provider {
public:
    NvmlProvider(std::unique_ptr<Api> api, nvmlDevice_t dev) : m_api(std::move(api)), m_dev(dev) {}
    ~NvmlProvider() override {
        if (m_api->shutdown) m_api->shutdown();
    }
    std::string name() const override { return "nvml"; }

    void sample(results::TelemetrySample& s) override {
        const Api& a = *m_api;
        unsigned int v = 0;
        if (a.clockInfo && a.clockInfo(m_dev, NVML_CLOCK_GRAPHICS, &v) == NVML_SUCCESS) s.gpuClockMhz = float(v);
        if (a.clockInfo && a.clockInfo(m_dev, NVML_CLOCK_MEM, &v) == NVML_SUCCESS) s.memClockMhz = float(v);
        if (a.temperature && a.temperature(m_dev, NVML_TEMPERATURE_GPU, &v) == NVML_SUCCESS) s.tempEdgeC = float(v);
        if (a.powerUsage && a.powerUsage(m_dev, &v) == NVML_SUCCESS) s.powerW = float(v) / 1000.0f;
        if (a.powerLimit && a.powerLimit(m_dev, &v) == NVML_SUCCESS) s.powerLimitW = float(v) / 1000.0f;
        if (a.fanSpeed && a.fanSpeed(m_dev, &v) == NVML_SUCCESS) s.fanPercent = float(v);
        nvmlUtilization_t u{};
        if (a.utilization && a.utilization(m_dev, &u) == NVML_SUCCESS) {
            s.gpuUtilPercent = float(u.gpu);
            s.memUtilPercent = float(u.memory);
        }
        nvmlMemory_t mem{};
        if (a.memoryInfo && a.memoryInfo(m_dev, &mem) == NVML_SUCCESS) {
            s.vramUsedMB = float(double(mem.used) / (1024.0 * 1024.0));
            s.vramTotalMB = float(double(mem.total) / (1024.0 * 1024.0));
        }
        unsigned long long reasons = 0;
        if (a.throttleReasons && a.throttleReasons(m_dev, &reasons) == NVML_SUCCESS) {
            s.throttleRaw = reasons;
            s.throttle = mapReasons(reasons);
        }
        if (a.fieldValues && m_memTempSupported) {
            nvmlFieldValue_t fv{};
            fv.fieldId = NVML_FI_DEV_MEMORY_TEMP;
            if (a.fieldValues(m_dev, 1, &fv) == NVML_SUCCESS && fv.nvmlReturn == NVML_SUCCESS && fv.value.uiVal > 0 &&
                fv.value.uiVal < 200)
                s.tempMemC = float(fv.value.uiVal);
            else
                m_memTempSupported = false;
        }
    }

    void fillGpuInfo(results::GpuInfo& g) override {
        const Api& a = *m_api;
        char buf[96] = {};
        if (a.vbios && a.vbios(m_dev, buf, sizeof(buf)) == NVML_SUCCESS) g.vbios = buf;
        unsigned int cg = 0, cw = 0, mg = 0, mw = 0, gmg = 0;
        if (a.currGen) a.currGen(m_dev, &cg);
        if (a.currWidth) a.currWidth(m_dev, &cw);
        if (a.maxGen) a.maxGen(m_dev, &mg);
        if (a.maxWidth) a.maxWidth(m_dev, &mw);
        if (a.gpuMaxGen && a.gpuMaxGen(m_dev, &gmg) == NVML_SUCCESS && gmg > 0) mg = std::min(mg ? mg : gmg, gmg);
        if (cg && cw) {
            g.pcieCurrentGen = int(cg);
            g.pcieCurrentWidth = int(cw);
            g.pcieCurrent = sysinfo::formatLink(int(cg), int(cw));
        }
        if (mg && mw) {
            g.pcieMaxGen = int(mg);
            g.pcieMaxWidth = int(mw);
            g.pcieMax = sysinfo::formatLink(int(mg), int(mw));
        }
    }

private:
    std::unique_ptr<Api> m_api;
    nvmlDevice_t m_dev;
    bool m_memTempSupported = true;
};

} // namespace

std::unique_ptr<Provider> openNvml(const DeviceMatch& match, std::string* why) {
    auto api = std::make_unique<Api>();
#ifdef _WIN32
    bool ok = api->lib.open("nvml.dll");
    if (!ok) {
        std::string pf = std::getenv("ProgramW6432") ? std::getenv("ProgramW6432") : "C:\\Program Files";
        ok = api->lib.open(pf + "\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
#else
    bool ok = api->lib.open("libnvidia-ml.so.1");
#endif
    if (!ok) {
        if (why) *why = "NVML not available: " + api->lib.error();
        return nullptr;
    }
    auto& L = api->lib;
    if (!L.load(api->init, "nvmlInit_v2")) {
        if (why) *why = "NVML: nvmlInit_v2 missing";
        return nullptr;
    }
    L.load(api->shutdown, "nvmlShutdown");
    L.load(api->errorString, "nvmlErrorString");
    L.load(api->getCount, "nvmlDeviceGetCount_v2");
    L.load(api->byIndex, "nvmlDeviceGetHandleByIndex_v2");
    L.load(api->byPci, "nvmlDeviceGetHandleByPciBusId_v2");
    L.load(api->getName, "nvmlDeviceGetName");
    L.load(api->clockInfo, "nvmlDeviceGetClockInfo");
    L.load(api->temperature, "nvmlDeviceGetTemperature");
    L.load(api->powerUsage, "nvmlDeviceGetPowerUsage");
    L.load(api->powerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    L.load(api->fanSpeed, "nvmlDeviceGetFanSpeed");
    L.load(api->utilization, "nvmlDeviceGetUtilizationRates");
    L.load(api->memoryInfo, "nvmlDeviceGetMemoryInfo");
    if (!L.load(api->throttleReasons, "nvmlDeviceGetCurrentClocksEventReasons"))
        L.load(api->throttleReasons, "nvmlDeviceGetCurrentClocksThrottleReasons");
    L.load(api->fieldValues, "nvmlDeviceGetFieldValues");
    L.load(api->currGen, "nvmlDeviceGetCurrPcieLinkGeneration");
    L.load(api->currWidth, "nvmlDeviceGetCurrPcieLinkWidth");
    L.load(api->maxGen, "nvmlDeviceGetMaxPcieLinkGeneration");
    L.load(api->maxWidth, "nvmlDeviceGetMaxPcieLinkWidth");
    L.load(api->gpuMaxGen, "nvmlDeviceGetGpuMaxPcieLinkGeneration");
    L.load(api->vbios, "nvmlDeviceGetVbiosVersion");
    L.load(api->driverVersion, "nvmlSystemGetDriverVersion");

    if (nvmlReturn_t r = api->init(); r != NVML_SUCCESS) {
        if (why) *why = std::format("nvmlInit failed ({})", api->errorString ? api->errorString(r) : "?");
        return nullptr;
    }
    nvmlDevice_t dev = nullptr;
    if (match.pciAddress && api->byPci) {
        // NVML expects an 8-digit domain ("00000000:01:00.0"); accept the short form too.
        const std::string& a = *match.pciAddress;
        if (api->byPci(("0000" + a).c_str(), &dev) != NVML_SUCCESS) {
            dev = nullptr;
            if (api->byPci(a.c_str(), &dev) != NVML_SUCCESS) dev = nullptr;
        }
    }
    if (!dev && api->getCount && api->byIndex && api->getName) {
        unsigned int count = 0;
        api->getCount(&count);
        // Without a PCI address, accept a unique name match (or the only GPU).
        nvmlDevice_t candidate = nullptr;
        int matches = 0;
        for (unsigned int i = 0; i < count; ++i) {
            nvmlDevice_t d = nullptr;
            if (api->byIndex(i, &d) != NVML_SUCCESS) continue;
            char name[96] = {};
            api->getName(d, name, sizeof(name));
            if (count == 1 || match.name == name) {
                candidate = d;
                ++matches;
            }
        }
        if (matches == 1) dev = candidate;
    }
    if (!dev) {
        if (api->shutdown) api->shutdown();
        if (why) *why = "NVML: no NVML device matches the selected GPU";
        return nullptr;
    }
    return std::make_unique<NvmlProvider>(std::move(api), dev);
}

} // namespace gt::telemetry
