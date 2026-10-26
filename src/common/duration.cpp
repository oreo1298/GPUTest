#include "gt/duration.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace gt {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

} // namespace

std::optional<double> parseDurationSeconds(std::string_view text) {
    const std::string s = lower(trim(text));
    if (s.empty()) return std::nullopt;
    double total = 0.0;
    std::size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        // number
        std::size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
        if (i == start) return std::nullopt;
        double value = 0.0;
        auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + i, value);
        if (ec != std::errc{} || ptr != s.data() + i) return std::nullopt;
        // unit
        std::size_t ustart = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        std::string_view unit(s.data() + ustart, i - ustart);
        double mult = 0.0;
        if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs") mult = 1.0;
        else if (unit == "ms") mult = 0.001;
        else if (unit == "m" || unit == "min" || unit == "mins") mult = 60.0;
        else if (unit == "h" || unit == "hr" || unit == "hrs") mult = 3600.0;
        else return std::nullopt;
        if (unit.empty() && i != s.size()) return std::nullopt; // "10 20" etc.
        total += value * mult;
        any = true;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    if (!any || !std::isfinite(total) || total < 0.0) return std::nullopt;
    return total;
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds)) return "∞";
    if (seconds < 0) seconds = 0;
    if (seconds < 60.0) return std::format("{:.1f}s", seconds);
    auto total = static_cast<long long>(std::llround(seconds));
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long sec = total % 60;
    if (h > 0) return std::format("{}h {:02}m {:02}s", h, m, sec);
    return std::format("{}m {:02}s", m, sec);
}

std::optional<LoopSpec> LoopSpec::parse(std::string_view text) {
    const std::string s = lower(trim(text));
    LoopSpec spec;
    if (s.empty() || s == "1" || s == "once" || s == "single") return spec;
    if (s == "inf" || s == "infinite" || s == "forever" || s == "0") {
        spec.mode = Mode::Infinite;
        return spec;
    }
    // "N" or "Nx" -> count
    std::string_view digits = s;
    if (!digits.empty() && digits.back() == 'x') digits.remove_suffix(1);
    bool allDigits = !digits.empty();
    for (char c : digits) allDigits = allDigits && std::isdigit(static_cast<unsigned char>(c));
    if (allDigits) {
        std::uint64_t n = 0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
        if (ec != std::errc{} || n == 0) return std::nullopt;
        spec.mode = n == 1 ? Mode::Single : Mode::Count;
        spec.count = n;
        return spec;
    }
    auto secs = parseDurationSeconds(s);
    if (!secs || *secs <= 0.0) return std::nullopt;
    spec.mode = Mode::Timed;
    spec.seconds = *secs;
    return spec;
}

std::string LoopSpec::toString() const {
    switch (mode) {
    case Mode::Single: return "single";
    case Mode::Count: return std::format("{}x", count);
    case Mode::Timed: return std::format("timed {}", formatDuration(seconds));
    case Mode::Infinite: return "infinite";
    }
    return "?";
}

} // namespace gt
