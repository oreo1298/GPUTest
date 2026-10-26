#pragma once

#include <filesystem>
#include <string>

namespace gt::paths {

/// Absolute path of the running executable (empty if unknown).
std::filesystem::path executablePath();
/// Directory containing the running executable.
std::filesystem::path executableDir();
/// Non-empty when running in portable mode ("portable.txt" next to the executable).
std::filesystem::path portableRoot();
/// Per-user config dir, e.g. ~/.config/<app-id> or %APPDATA%\<app-id>. Created on demand.
std::filesystem::path configDir();
/// Per-user data dir (history, results), e.g. ~/.local/share/<app-id> or %LOCALAPPDATA%\<app-id>.
std::filesystem::path dataDir();
/// Default directory for result files: <dataDir>/results.
std::filesystem::path resultsDir();
/// Filesystem-safe timestamp "YYYYMMDD-HHMMSS" (local time).
std::string timestampForFilename();
/// ISO-8601 UTC timestamp "YYYY-MM-DDTHH:MM:SSZ".
std::string isoTimestampUtc();

} // namespace gt::paths
