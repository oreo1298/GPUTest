#include "gt/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace gt::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
std::ofstream g_file;
Hook g_hook = nullptr;
void* g_hookUser = nullptr;
Level g_hookLevel = Level::Warn;
const auto g_start = std::chrono::steady_clock::now();

} // namespace

void setLevel(Level lvl) { g_level.store(lvl, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

std::string_view levelName(Level lvl) {
    switch (lvl) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
    }
    return "?";
}

bool parseLevel(std::string_view name, Level& out) {
    for (Level l : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
        if (name == levelName(l)) {
            out = l;
            return true;
        }
    }
    return false;
}

bool setFile(const std::string& path) {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    if (path.empty()) return true;
    g_file.open(path, std::ios::out | std::ios::trunc);
    return g_file.is_open();
}

void setHook(Hook hook, void* user, Level minLevel) {
    std::lock_guard lock(g_mutex);
    g_hook = hook;
    g_hookUser = user;
    g_hookLevel = minLevel;
}

void write(Level lvl, std::string_view message) {
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::string line = std::format("[{:9.3f}] {:5} {}\n", t, levelName(lvl), message);
    std::lock_guard lock(g_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
    if (g_hook && lvl >= g_hookLevel) g_hook(lvl, message, g_hookUser);
}

} // namespace gt::log
