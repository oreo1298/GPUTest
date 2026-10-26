#include "gt/sysinfo.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gt::sysinfo {
namespace {

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !ws(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !ws(static_cast<unsigned char>(c)); }).base(),
            s.end());
    return s;
}

[[maybe_unused]] std::string readFirstLine(const fs::path& p) {
    std::ifstream f(p);
    std::string line;
    if (f) std::getline(f, line);
    return trim(line);
}

[[maybe_unused]] std::uint32_t readHex(const fs::path& p) {
    const std::string s = readFirstLine(p);
    if (s.empty()) return 0;
    try {
        return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

[[maybe_unused]] int readInt(const fs::path& p) {
    const std::string s = readFirstLine(p);
    try {
        return s.empty() ? 0 : std::stoi(s);
    } catch (...) {
        return 0;
    }
}

const char* envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

} // namespace

int pcieGenFromGts(double gts) {
    if (gts >= 60.0) return 6;
    if (gts >= 30.0) return 5;
    if (gts >= 15.0) return 4;
    if (gts >= 7.5) return 3;
    if (gts >= 4.5) return 2;
    if (gts >= 2.0) return 1;
    return 0;
}

std::string formatLink(int gen, int width) {
    if (gen <= 0 && width <= 0) return {};
    if (gen <= 0) return std::format("x{}", width);
    if (width <= 0) return std::format("Gen{}", gen);
    return std::format("Gen{} x{}", gen, width);
}

#ifdef _WIN32

results::HostInfo collectHost() {
    results::HostInfo h;
    char buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        h.cpu = trim(buf);
    h.cpuThreads = static_cast<int>(std::thread::hardware_concurrency());
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
        std::vector<char> info(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                             reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(info.data()),
                                             &len)) {
            int cores = 0;
            for (DWORD off = 0; off < len;) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(info.data() + off);
                if (p->Relationship == RelationProcessorCore) ++cores;
                off += p->Size;
            }
            h.cpuCores = cores;
        }
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) h.ramBytes = ms.ullTotalPhys;

    char product[256] = {}, display[64] = {}, build[64] = {};
    DWORD s1 = sizeof(product), s2 = sizeof(display), s3 = sizeof(build);
    const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProductName", RRF_RT_REG_SZ, nullptr, product, &s1);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &s2);
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &s3);
    std::string name = product;
    // ProductName still says "Windows 10" on Windows 11; builds >= 22000 are Windows 11.
    int buildNo = 0;
    try {
        buildNo = std::stoi(build);
    } catch (...) {
    }
    if (buildNo >= 22000 && name.find("Windows 10") != std::string::npos) name.replace(name.find("10"), 2, "11");
    h.os = trim(std::format("{} {}", name, display));
    h.kernel = std::format("NT build {}", build);
    h.displayServer = "Windows (DWM)";
    h.desktop = "Windows";
    char host[256];
    DWORD hs = sizeof(host);
    if (GetComputerNameA(host, &hs)) h.hostname = host;
    return h;
}

PciDeviceInfo readPciDevice(const std::string&) {
    return {}; // Windows: filled from vendor telemetry (NVML) or SetupAPI in the telemetry module.
}

#else

results::HostInfo collectHost() {
    results::HostInfo h;
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        std::set<std::pair<int, int>> cores;
        int physId = 0;
        while (std::getline(f, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim(line.substr(0, colon));
            const std::string val = trim(line.substr(colon + 1));
            if (key == "model name" && h.cpu.empty()) h.cpu = val;
            else if (key == "physical id") physId = std::atoi(val.c_str());
            else if (key == "core id") cores.emplace(physId, std::atoi(val.c_str()));
        }
        h.cpuCores = static_cast<int>(cores.size());
    }
    h.cpuThreads = static_cast<int>(std::thread::hardware_concurrency());
    if (h.cpuCores == 0) h.cpuCores = h.cpuThreads;
    {
        std::ifstream f("/proc/meminfo");
        std::string key;
        std::uint64_t kb = 0;
        while (f >> key >> kb) {
            if (key == "MemTotal:") {
                h.ramBytes = kb * 1024;
                break;
            }
            f.ignore(256, '\n');
        }
    }
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                std::string v = line.substr(12);
                if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
                h.os = v;
            }
        }
    }
    utsname u{};
    if (uname(&u) == 0) {
        h.kernel = std::format("{} {}", u.sysname, u.release);
        h.hostname = u.nodename;
    }
    const std::string session = envOr("XDG_SESSION_TYPE", "");
    if (session == "wayland" || std::getenv("WAYLAND_DISPLAY")) h.displayServer = "Wayland";
    else if (session == "x11" || std::getenv("DISPLAY")) h.displayServer = "X11";
    else h.displayServer = "none (headless)";
    if (h.displayServer == "Wayland" && std::getenv("DISPLAY")) h.displayServer += " (XWayland available)";
    h.desktop = envOr("XDG_CURRENT_DESKTOP", "");
    return h;
}

PciDeviceInfo readPciDevice(const std::string& address) {
    PciDeviceInfo info;
    const fs::path dev = fs::path("/sys/bus/pci/devices") / address;
    std::error_code ec;
    if (!fs::exists(dev, ec)) return info;
    info.valid = true;
    info.sysfsPath = dev.string();
    info.vendorId = readHex(dev / "vendor");
    info.deviceId = readHex(dev / "device");
    info.subsysVendorId = readHex(dev / "subsystem_vendor");
    info.subsysId = readHex(dev / "subsystem_device");
    info.revision = readHex(dev / "revision");
    info.vbios = readFirstLine(dev / "vbios_version");
    if (auto drv = fs::read_symlink(dev / "driver", ec); !ec) info.driver = drv.filename().string();

    auto readLink = [](const fs::path& p, int& gen, int& width, const char* speedFile, const char* widthFile) {
        const std::string speed = readFirstLine(p / speedFile); // "16.0 GT/s PCIe"
        double gts = 0.0;
        try {
            gts = speed.empty() ? 0.0 : std::stod(speed);
        } catch (...) {
        }
        gen = pcieGenFromGts(gts);
        width = readInt(p / widthFile);
    };
    readLink(dev, info.currentGen, info.currentWidth, "current_link_speed", "current_link_width");
    readLink(dev, info.maxGen, info.maxWidth, "max_link_speed", "max_link_width");

    // AMD dGPUs sit behind an on-package PCIe switch (upstream + downstream port with the
    // same vendor). The GPU's own link is internal; the slot link is the switch's upstream port.
    fs::path up = fs::canonical(dev, ec).parent_path();
    for (int depth = 0; depth < 2 && !ec; ++depth) {
        if (!fs::exists(up / "vendor")) break;
        if (readHex(up / "vendor") != info.vendorId) break;
        int g = 0, w = 0, mg = 0, mw = 0;
        readLink(up, g, w, "current_link_speed", "current_link_width");
        readLink(up, mg, mw, "max_link_speed", "max_link_width");
        if (g > 0 && w > 0) {
            info.currentGen = g;
            info.currentWidth = w;
            info.maxGen = mg;
            info.maxWidth = mw;
        }
        up = up.parent_path();
    }
    return info;
}

#endif

} // namespace gt::sysinfo
