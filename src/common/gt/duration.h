#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gt {

/// Parses durations like "90", "90s", "20m", "1h30m", "1.5h", "500ms".
/// A bare number is seconds. Returns std::nullopt on malformed input.
std::optional<double> parseDurationSeconds(std::string_view text);

/// Formats seconds as e.g. "1h 02m 03s", "4m 05s", "12.3s".
std::string formatDuration(double seconds);

/// How often a suite repeats. Parsed from --loop:
///   "1" / "" -> single run; "N" or "Nx" -> N loops; "20m" -> timed; "inf" / "infinite" / "forever" -> until stopped.
/// A bare integer means a loop count; anything with a unit suffix is a duration.
struct LoopSpec {
    enum class Mode { Single, Count, Timed, Infinite };
    Mode mode = Mode::Single;
    std::uint64_t count = 1;     ///< for Count
    double seconds = 0.0;        ///< for Timed

    static std::optional<LoopSpec> parse(std::string_view text);
    std::string toString() const;
    bool isLooping() const { return mode != Mode::Single; }
};

} // namespace gt
