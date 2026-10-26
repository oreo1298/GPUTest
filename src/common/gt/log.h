#pragma once

#include <format>
#include <string>
#include <string_view>

namespace gt::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

void setLevel(Level level);
Level level();
std::string_view levelName(Level level);
bool parseLevel(std::string_view name, Level& out);

/// Optional file sink (in addition to stderr). Pass an empty path to close it.
bool setFile(const std::string& path);

/// Optional callback invoked for every message at or above `minLevel` (e.g. to
/// forward warnings into the JSON progress stream). Called with the log mutex held.
using Hook = void (*)(Level, std::string_view message, void* user);
void setHook(Hook hook, void* user, Level minLevel = Level::Warn);

void write(Level level, std::string_view message);

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Trace) write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Debug) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Info) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Warn) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Error) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace gt::log
