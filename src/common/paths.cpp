#include "gt/paths.h"

#include "gt/app_info.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gt::paths {
namespace {

fs::path envPath(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return fs::path(v);
    return {};
}

fs::path ensure(fs::path p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::tm localTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::tm utcTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

} // namespace

fs::path executablePath() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return fs::path(buf);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
#endif
}

fs::path executableDir() { return executablePath().parent_path(); }

// Portable mode: a "portable.txt" next to the executable keeps all state beside it.
fs::path portableRoot() {
    static const fs::path root = [] {
        fs::path dir = executableDir();
        std::error_code ec;
        return (!dir.empty() && fs::exists(dir / "portable.txt", ec)) ? dir : fs::path{};
    }();
    return root;
}

fs::path configDir() {
    if (auto p = portableRoot(); !p.empty()) return ensure(p / "config");
#ifdef _WIN32
    fs::path base = envPath("APPDATA");
#else
    fs::path base = envPath("XDG_CONFIG_HOME");
    if (base.empty()) base = envPath("HOME") / ".config";
#endif
    return ensure(base / app::kId);
}

fs::path dataDir() {
    if (auto p = portableRoot(); !p.empty()) return ensure(p / "data");
#ifdef _WIN32
    fs::path base = envPath("LOCALAPPDATA");
#else
    fs::path base = envPath("XDG_DATA_HOME");
    if (base.empty()) base = envPath("HOME") / ".local" / "share";
#endif
    return ensure(base / app::kId);
}

fs::path resultsDir() { return ensure(dataDir() / "results"); }

std::string timestampForFilename() {
    const std::tm tm = localTime(std::time(nullptr));
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

std::string isoTimestampUtc() {
    const std::tm tm = utcTime(std::time(nullptr));
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

} // namespace gt::paths
