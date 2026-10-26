#pragma once
// Export of results to CSV and a self-contained, printable HTML report
// (inline CSS + SVG charts, no external resources).

#include "gt/results.h"

#include <string>

namespace gt::report {

/// One CSV file with three sections separated by blank lines: tests (metrics),
/// frame times (one column per test) and telemetry samples.
std::string toCsv(const results::RunResult& run);
/// Only the per-test summary table (one row per test result).
std::string testsCsv(const results::RunResult& run);
std::string telemetryCsv(const results::RunResult& run);

/// Self-contained HTML report.
std::string toHtml(const results::RunResult& run);

/// Writes `content` to `path`; returns false and fills `error` on failure.
bool writeText(const std::string& path, const std::string& content, std::string* error = nullptr);

/// Escapes text for HTML.
std::string htmlEscape(const std::string& s);
/// Quotes a CSV field when needed.
std::string csvField(const std::string& s);

} // namespace gt::report
