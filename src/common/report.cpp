#include "gt/report.h"

#include "gt/stats.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace gt::report {

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

namespace {

std::string num(double v, int prec = 3) {
    if (!std::isfinite(v)) return "";
    return std::format("{:.{}f}", v, prec);
}

} // namespace

std::string testsCsv(const results::RunResult& run) {
    std::ostringstream o;
    o << "test_id,name,category,api,loop,status,message,score,avg_fps,min_fps,max_fps,low_1pct_fps,low_01pct_fps,"
         "p50_ms,p90_ms,p95_ms,p99_ms,p999_ms,stddev_ms,stutters,gpu_busy_percent,frames,duration_s,verdict,"
         "verdict_message,metrics\n";
    for (const auto& t : run.tests) {
        std::string metrics;
        for (const auto& m : t.metrics) metrics += std::format("{}={:.6g} {};", m.name, m.value, m.unit);
        const auto* f = t.frameStats ? &*t.frameStats : nullptr;
        o << csvField(t.id) << ',' << csvField(t.name) << ',' << t.category << ',' << t.api << ',' << t.loop + 1 << ','
          << results::toString(t.status) << ',' << csvField(t.message) << ',' << (t.scored ? num(t.score, 1) : "")
          << ',' << (f ? num(f->avgFps) : "") << ',' << (f ? num(f->minFps) : "") << ',' << (f ? num(f->maxFps) : "")
          << ',' << (f ? num(f->low1Fps) : "") << ',' << (f ? num(f->low01Fps) : "") << ',' << (f ? num(f->p50Ms) : "")
          << ',' << (f ? num(f->p90Ms) : "") << ',' << (f ? num(f->p95Ms) : "") << ',' << (f ? num(f->p99Ms) : "")
          << ',' << (f ? num(f->p999Ms) : "") << ',' << (f ? num(f->stddevMs) : "") << ','
          << (f ? std::to_string(f->stutterCount) : "") << ',' << num(t.gpuBusyPercent, 1) << ',' << t.frames << ','
          << num(t.durationS, 2) << ',' << (t.verdict == results::Verdict::None ? "" : results::toString(t.verdict))
          << ',' << csvField(t.verdictMessage) << ',' << csvField(metrics) << '\n';
    }
    return o.str();
}

std::string telemetryCsv(const results::RunResult& run) {
    std::ostringstream o;
    const auto& ch = results::telemetryChannels();
    o << "t_s";
    for (const auto& c : ch) o << ',' << c;
    o << ",throttle\n";
    for (const auto& s : run.telemetry.samples) {
        o << num(s.t, 2);
        for (std::size_t c = 0; c < ch.size(); ++c) o << ',' << num(results::telemetryValue(s, c), 1);
        std::string names;
        for (const auto& n : results::throttle::names(s.throttle)) names += (names.empty() ? "" : "|") + n;
        o << ',' << names << '\n';
    }
    return o.str();
}

std::string toCsv(const results::RunResult& run) {
    std::ostringstream o;
    o << "# " << run.appName << ' ' << run.appVersion << " results, " << run.createdUtc << ", "
      << csvField(run.system.gpu.name) << ", " << run.config.api << ' ' << run.config.width << 'x' << run.config.height
      << ' ' << run.config.quality << ", suite " << run.config.suite << ", overall " << num(run.scores.overall, 0)
      << "\n\n# tests\n"
      << testsCsv(run);
    // Frame times: one column per frame-based test result.
    std::vector<const results::TestResult*> ft;
    std::size_t rows = 0;
    for (const auto& t : run.tests)
        if (!t.frameTimesMs.empty()) {
            ft.push_back(&t);
            rows = std::max(rows, t.frameTimesMs.size());
        }
    if (!ft.empty()) {
        o << "\n# frame times (ms)\nframe";
        for (const auto* t : ft) o << ',' << csvField(std::format("{} loop {}", t->id, t->loop + 1));
        o << '\n';
        for (std::size_t i = 0; i < rows; ++i) {
            o << i;
            for (const auto* t : ft) o << ',' << (i < t->frameTimesMs.size() ? num(t->frameTimesMs[i]) : "");
            o << '\n';
        }
    }
    if (!run.telemetry.samples.empty()) o << "\n# telemetry (provider " << run.telemetry.provider << ")\n" << telemetryCsv(run);
    return o.str();
}

bool writeText(const std::string& path, const std::string& content, std::string* error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    f << content;
    if (!f) {
        if (error) *error = "write failed: " + path;
        return false;
    }
    return true;
}

// --- HTML -------------------------------------------------------------------------------------
namespace {

constexpr const char* kSeries[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"};

struct SvgSeries {
    std::string name;
    int slot;
    std::vector<std::pair<double, double>> pts;
};

std::vector<double> niceTicks(double lo, double hi, int target, double& step) {
    const double raw = (hi - lo) / std::max(1, target);
    const double mag = std::pow(10.0, std::floor(std::log10(raw > 0 ? raw : 1)));
    step = mag;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        step = m * mag;
        if (step >= raw) break;
    }
    std::vector<double> t;
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-6; v += step) t.push_back(std::abs(v) < step * 1e-9 ? 0 : v);
    return t;
}

std::string tickLabel(double v, double step) {
    const int dec = step < 1.0 ? std::min(3, static_cast<int>(std::ceil(-std::log10(step) - 1e-9))) : 0;
    return std::format("{:.{}f}", v, dec);
}

/// Inline SVG line chart (one y axis, thin lines, recessive grid, legend for >= 2 series).
std::string svgChart(const std::string& title, const std::string& xLabel, const std::string& unit,
                     const std::vector<SvgSeries>& series, const std::vector<results::ThrottleEvent>& bands = {},
                     const std::vector<std::pair<double, std::string>>& refs = {}, int width = 760, int height = 220) {
    double xlo = std::numeric_limits<double>::max(), xhi = std::numeric_limits<double>::lowest();
    double yhi = std::numeric_limits<double>::lowest();
    for (const auto& s : series)
        for (const auto& [x, y] : s.pts) {
            if (!std::isfinite(y)) continue;
            xlo = std::min(xlo, x);
            xhi = std::max(xhi, x);
            yhi = std::max(yhi, y);
        }
    for (const auto& r : refs) yhi = std::max(yhi, r.first);
    if (xlo > xhi) return {};
    if (xhi <= xlo) xhi = xlo + 1;
    const double ylo = 0.0;
    if (yhi <= ylo) yhi = 1.0;
    yhi *= 1.06;
    const double L = 52, R = 12, T = series.size() >= 2 ? 44 : 28, B = 40;
    const double pw = width - L - R, ph = height - T - B;
    auto X = [&](double x) { return L + (x - xlo) / (xhi - xlo) * pw; };
    auto Y = [&](double y) { return T + ph - (y - ylo) / (yhi - ylo) * ph; };
    std::ostringstream o;
    o << std::format(R"(<svg class="chart" viewBox="0 0 {} {}" width="100%" role="img" aria-label="{}">)", width, height,
                     htmlEscape(title));
    o << std::format(R"(<text x="8" y="18" class="ct">{}</text>)", htmlEscape(title));
    if (series.size() >= 2) {
        double lx = 8;
        for (const auto& s : series) {
            o << std::format(R"(<line x1="{:.1f}" y1="34" x2="{:.1f}" y2="34" stroke="{}" stroke-width="3" stroke-linecap="round"/>)", lx,
                             lx + 14, kSeries[s.slot % 8]);
            o << std::format(R"(<text x="{:.1f}" y="38" class="lg">{}</text>)", lx + 20, htmlEscape(s.name));
            lx += 34 + 7.0 * static_cast<double>(s.name.size());
        }
    }
    for (const auto& b : bands) {
        const double x0 = std::max(L, X(b.start)), x1 = std::min(L + pw, X(b.end));
        if (x1 > x0)
        {
            // Same severity colours as the GUI: power/driver limits = warning, thermal/current/voltage = serious,
            // hardware slowdown = critical.
            using namespace results::throttle;
            const char* fill = (b.reasons & kHwSlowdown)                                    ? "#d03b3b"
                               : (b.reasons & (kThermal | kMemThermal | kCurrent | kVoltage)) ? "#ec835a"
                                                                                               : "#fab219";
            o << std::format(R"(<rect x="{:.1f}" y="{:.1f}" width="{:.1f}" height="{:.1f}" fill="{}" opacity="0.18"/>)", x0, T,
                             std::max(1.5, x1 - x0), ph, fill);
        }
    }
    double ys = 1;
    for (double v : niceTicks(ylo, yhi, 4, ys)) {
        o << std::format(R"(<line x1="{:.1f}" y1="{:.1f}" x2="{:.1f}" y2="{:.1f}" class="grid"/>)", L, Y(v), L + pw, Y(v));
        o << std::format(R"(<text x="{:.1f}" y="{:.1f}" class="tick" text-anchor="end">{}</text>)", L - 6, Y(v) + 4, tickLabel(v, ys));
    }
    double xs = 1;
    for (double v : niceTicks(xlo, xhi, 6, xs))
        o << std::format(R"(<text x="{:.1f}" y="{:.1f}" class="tick" text-anchor="middle">{}</text>)", X(v), T + ph + 16, tickLabel(v, xs));
    o << std::format(R"(<text x="{:.1f}" y="{}" class="tick" text-anchor="middle">{}</text>)", L + pw / 2, height - 4,
                     htmlEscape(xLabel + (unit.empty() ? "" : "  ·  " + unit)));
    o << std::format(R"(<line x1="{:.1f}" y1="{:.1f}" x2="{:.1f}" y2="{:.1f}" class="axis"/>)", L, T + ph, L + pw, T + ph);
    for (const auto& [y, label] : refs) {
        o << std::format(R"(<line x1="{:.1f}" y1="{:.1f}" x2="{:.1f}" y2="{:.1f}" class="ref"/>)", L, Y(y), L + pw, Y(y));
        o << std::format(R"(<text x="{:.1f}" y="{:.1f}" class="tick" text-anchor="end">{}</text>)", L + pw - 2, Y(y) - 4, htmlEscape(label));
    }
    for (const auto& s : series) {
        // Decimate to min/max per pixel column to keep the file small.
        const int cols = static_cast<int>(pw);
        std::string d;
        if (static_cast<int>(s.pts.size()) > cols * 2) {
            int col = -1;
            double mn = 0, mx = 0, fx = 0;
            auto flush = [&] {
                if (col < 0) return;
                d += std::format("{}{:.1f},{:.1f}L{:.1f},{:.1f}", d.empty() ? "M" : "L", X(fx), Y(mn), X(fx), Y(mx));
            };
            for (const auto& [x, y] : s.pts) {
                if (!std::isfinite(y)) continue;
                const int c = static_cast<int>((x - xlo) / (xhi - xlo) * cols);
                if (c != col) {
                    flush();
                    col = c;
                    mn = mx = y;
                    fx = x;
                } else {
                    mn = std::min(mn, y);
                    mx = std::max(mx, y);
                }
            }
            flush();
        } else {
            for (const auto& [x, y] : s.pts)
                if (std::isfinite(y)) d += std::format("{}{:.1f},{:.1f}", d.empty() ? "M" : "L", X(x), Y(y));
        }
        if (!d.empty())
            o << std::format(R"(<path d="{}" fill="none" stroke="{}" stroke-width="1.6" stroke-linejoin="round"/>)", d,
                             kSeries[s.slot % 8]);
    }
    o << "</svg>";
    return o.str();
}

const char* kCss = R"(
:root { --bg:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781; --grid:#e1e0d9; --axis:#c3c2b7;
  --border:rgba(11,11,11,.10); --accent:#2a78d6; --good:#0ca30c; --warn:#fab219; --bad:#d03b3b; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }
main { max-width: 1040px; margin: 0 auto; padding: 28px 20px 60px; }
h1 { font-size: 26px; margin: 0 0 4px; } h2 { font-size: 18px; margin: 28px 0 10px; } h3 { font-size: 15px; margin: 18px 0 6px; }
.sub { color: var(--ink2); }
.cards { display:flex; flex-wrap:wrap; gap:10px; margin-top: 14px; }
.card { background:var(--surface); border:1px solid var(--border); border-radius:10px; padding:12px 16px; min-width:150px; }
.card .k { color:var(--ink2); font-size:12px; } .card .v { font-size:24px; font-weight:600; }
.banner { border-left:4px solid var(--warn); background:var(--surface); padding:10px 14px; border-radius:6px; margin-top:12px; }
table { border-collapse: collapse; width:100%; background:var(--surface); border:1px solid var(--border); border-radius:8px; }
th, td { text-align:left; padding:6px 10px; border-bottom:1px solid var(--grid); vertical-align: top; }
th { font-size:12px; color:var(--ink2); font-weight:600; } td.n { text-align:right; font-variant-numeric: tabular-nums; }
.pill { display:inline-block; border-radius:999px; padding:1px 9px; font-size:12px; font-weight:600; border:1px solid var(--border); }
.pass { background: rgba(12,163,12,.14); } .warn { background: rgba(250,178,25,.22); } .fail { background: rgba(208,59,59,.16); }
.chart { background:var(--surface); border:1px solid var(--border); border-radius:8px; margin: 6px 0 12px; }
.ct { font-weight:600; font-size:13px; fill: var(--ink); } .lg { font-size:12px; fill: var(--ink2); }
.tick { font-size:11px; fill: var(--muted); } .grid { stroke: var(--grid); stroke-width:1; } .axis { stroke: var(--axis); stroke-width:1; }
.ref { stroke: var(--muted); stroke-dasharray: 4 3; }
.kv td:first-child { color: var(--ink2); width: 220px; }
footer { color: var(--muted); margin-top: 40px; font-size: 12px; }
details summary { cursor: pointer; color: var(--ink2); }
@media print { body { background: #fff; } main { padding: 0; } .chart, table, .card { break-inside: avoid; } details { display:none; } }
)";

std::string verdictPill(results::Verdict v) {
    switch (v) {
    case results::Verdict::Pass: return R"(<span class="pill pass">✓ PASS</span>)";
    case results::Verdict::Warn: return R"(<span class="pill warn">⚠ WARN</span>)";
    case results::Verdict::Fail: return R"(<span class="pill fail">✗ FAIL</span>)";
    case results::Verdict::None: break;
    }
    return {};
}

std::string bytes(double b) {
    if (b >= 1024.0 * 1024 * 1024) return std::format("{:.1f} GiB", b / (1024.0 * 1024 * 1024));
    return std::format("{:.0f} MiB", b / (1024.0 * 1024));
}

} // namespace

std::string toHtml(const results::RunResult& run) {
    const auto& g = run.system.gpu;
    const auto& h = run.system.host;
    std::ostringstream o;
    o << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
         "content=\"width=device-width, initial-scale=1\">";
    o << "<title>" << htmlEscape(std::format("{} results — {}", run.appName, g.name)) << "</title><style>" << kCss
      << "</style></head><body><main>";
    o << "<h1>" << htmlEscape(g.name) << "</h1>";
    o << "<div class=\"sub\">" << htmlEscape(std::format("{} · suite {} · {}×{} {} / {} · loop {} · {}", g.api,
                                                         run.config.suite, run.config.width, run.config.height,
                                                         run.config.quality, run.config.duration, run.config.loop,
                                                         run.createdUtc))
      << "</div>";
    if (run.status != results::RunStatus::Completed)
        o << "<div class=\"banner\"><b>Run status: " << htmlEscape(results::toString(run.status)) << "</b> — "
          << htmlEscape(run.statusMessage) << "</div>";
    o << "<div class=\"cards\">";
    auto card = [&](const std::string& k, const std::string& v) {
        o << "<div class=\"card\"><div class=\"k\">" << htmlEscape(k) << "</div><div class=\"v\">" << htmlEscape(v) << "</div></div>";
    };
    card("Overall score", run.scores.overall > 0 ? std::format("{:.0f}", run.scores.overall) : "—");
    for (const auto& [cat, s] : run.scores.subscores) card(cat + " score", std::format("{:.0f}", s));
    if (std::isfinite(run.scores.perWatt)) card("Points per watt", std::format("{:.1f}", run.scores.perWatt));
    if (run.stability.valid)
        card("Stability", std::format("{:.1f} % {}", run.stability.ratio * 100.0, run.stability.passed ? "pass" : "FAIL"));
    o << "</div>";
    o << "<p class=\"sub\">Scores are only comparable between runs with the same key: <code>"
      << htmlEscape(run.scores.comparableKey) << "</code></p>";

    // Tests table
    o << "<h2>Tests</h2><table><tr><th>Test</th><th>Status</th><th>Score</th><th>Avg FPS</th><th>1% low</th>"
         "<th>p99 ms</th><th>Result</th></tr>";
    for (const auto& t : run.tests) {
        std::string result;
        if (!t.frameStats)
            if (const auto* m = t.headline()) result = htmlEscape(std::format("{}: {:.4g} {}", m->label, m->value, m->unit));
        if (t.verdict != results::Verdict::None)
            result += (result.empty() ? "" : "<br>") + verdictPill(t.verdict) + " " + htmlEscape(t.verdictMessage);
        const auto* f = t.frameStats ? &*t.frameStats : nullptr;
        o << "<tr><td>" << htmlEscape(t.name) << (run.loops.size() > 1 ? std::format(" <span class=\"sub\">(loop {})</span>", t.loop + 1) : "")
          << "</td><td>" << htmlEscape(results::toString(t.status)) << (t.message.empty() ? "" : "<br><span class=\"sub\">" + htmlEscape(t.message) + "</span>")
          << "</td><td class=\"n\">" << (t.scored ? std::format("{:.0f}", t.score) : "—") << "</td><td class=\"n\">"
          << (f ? std::format("{:.1f}", f->avgFps) : "—") << "</td><td class=\"n\">" << (f ? std::format("{:.1f}", f->low1Fps) : "—")
          << "</td><td class=\"n\">" << (f ? std::format("{:.2f}", f->p99Ms) : "—") << "</td><td>" << result << "</td></tr>";
    }
    o << "</table>";

    // Loops
    if (run.loops.size() > 1) {
        std::vector<SvgSeries> s{{"Loop score", 0, {}}};
        for (const auto& l : run.loops) s[0].pts.emplace_back(static_cast<double>(l.index + 1), l.score);
        o << "<h2>Loops</h2>" << svgChart("Score per loop", "loop", "points", s);
        if (run.stability.valid)
            o << std::format("<p>Stability (worst / best loop): <b>{:.1f} %</b> — {} (threshold {:.0f} %).{}</p>",
                             run.stability.ratio * 100.0, run.stability.passed ? "pass" : "FAIL", run.stability.threshold * 100.0,
                             std::isfinite(run.stability.trendPercentPerHour)
                                 ? std::format(" Trend: {:+.2f} % per hour.", run.stability.trendPercentPerHour)
                                 : std::string());
    }

    // Per-test details
    o << "<h2>Frame times</h2>";
    for (const auto& t : run.tests) {
        if (t.frameTimesMs.empty()) continue;
        std::vector<SvgSeries> s{{"Frame time", 0, {}}, {"GPU time", 2, {}}};
        for (std::size_t i = 0; i < t.frameTimesMs.size(); ++i) s[0].pts.emplace_back(static_cast<double>(i), t.frameTimesMs[i]);
        for (std::size_t i = 0; i < t.gpuTimesMs.size(); ++i) s[1].pts.emplace_back(static_cast<double>(i), t.gpuTimesMs[i]);
        std::vector<std::pair<double, std::string>> refs;
        if (t.frameStats) refs.emplace_back(t.frameStats->p99Ms, std::format("p99 {:.2f} ms", t.frameStats->p99Ms));
        o << "<h3>" << htmlEscape(t.name) << (run.loops.size() > 1 ? std::format(" (loop {})", t.loop + 1) : "") << "</h3>";
        if (t.frameStats) {
            const auto& f = *t.frameStats;
            o << "<div class=\"sub\">" << htmlEscape(std::format(
                     "avg {:.1f} fps · min {:.1f} · max {:.1f} · 1% low {:.1f} · 0.1% low {:.1f} · p50 {:.2f} · p90 {:.2f} · "
                     "p95 {:.2f} · p99 {:.2f} · p99.9 {:.2f} ms · σ {:.2f} ms · {} stutters",
                     f.avgFps, f.minFps, f.maxFps, f.low1Fps, f.low01Fps, f.p50Ms, f.p90Ms, f.p95Ms, f.p99Ms, f.p999Ms,
                     f.stddevMs, f.stutterCount))
              << "</div>";
        }
        o << svgChart("Frame time", "frame", "ms", s, {}, refs);
    }

    // Telemetry
    const auto& tel = run.telemetry;
    if (!tel.samples.empty()) {
        o << "<h2>Telemetry</h2><p class=\"sub\">Provider: " << htmlEscape(tel.provider)
          << ". Shaded areas mark throttling.</p>";
        struct Group {
            const char* title;
            const char* unit;
            std::vector<std::pair<std::size_t, const char*>> channels;
        };
        const Group groups[] = {{"Clocks", "MHz", {{0, "GPU"}, {1, "Memory"}}},
                                {"Temperatures", "°C", {{2, "GPU"}, {3, "Hotspot"}, {4, "Memory"}}},
                                {"Power", "W", {{5, "Board power"}, {6, "Limit"}}},
                                {"Fan", "%", {{7, "Fan"}}},
                                {"Utilization", "%", {{9, "GPU"}, {10, "Memory controller"}}},
                                {"VRAM used", "MiB", {{11, "Used"}}}};
        for (const auto& grp : groups) {
            std::vector<SvgSeries> series;
            int slot = 0;
            for (const auto& [c, name] : grp.channels) {
                SvgSeries s{name, slot++, {}};
                for (const auto& smp : tel.samples) {
                    const float v = results::telemetryValue(smp, c);
                    if (std::isfinite(v)) s.pts.emplace_back(smp.t, v);
                }
                if (!s.pts.empty()) series.push_back(std::move(s));
            }
            if (!series.empty()) o << svgChart(grp.title, "seconds", grp.unit, series, tel.throttleEvents);
        }
        o << "<table><tr><th>Channel</th><th>Min</th><th>Average</th><th>Peak</th></tr>";
        for (const auto& [k, v] : tel.summary) {
            const auto* info = results::telemetryChannelInfo(k);
            const std::string label = info ? std::format("{} ({})", info->label, info->unit) : k;
            o << std::format("<tr><td>{}</td><td class=\"n\">{:.1f}</td><td class=\"n\">{:.1f}</td><td class=\"n\">{:.1f}</td></tr>",
                             htmlEscape(label), v.min, v.avg, v.max);
        }
        o << "</table>";
        if (!tel.throttleEvents.empty()) {
            o << "<h3>Throttle events</h3><table><tr><th>From (s)</th><th>To (s)</th><th>Reasons</th></tr>";
            for (const auto& e : tel.throttleEvents) {
                std::string r;
                for (const auto& n : results::throttle::names(e.reasons)) r += (r.empty() ? "" : ", ") + n;
                o << std::format("<tr><td class=\"n\">{:.1f}</td><td class=\"n\">{:.1f}</td><td>{}</td></tr>", e.start, e.end, htmlEscape(r));
            }
            o << "</table>";
        }
    }

    // System
    o << "<h2>System</h2><table class=\"kv\">";
    auto kv = [&](const std::string& k, const std::string& v) {
        if (!v.empty()) o << "<tr><td>" << htmlEscape(k) << "</td><td>" << htmlEscape(v) << "</td></tr>";
    };
    kv("GPU", g.name);
    kv("Vendor / device", std::format("{} 0x{:04x} / 0x{:04x}", g.vendor, g.vendorId, g.deviceId));
    if (g.subsysVendorId || g.subsysId) kv("Subsystem", std::format("0x{:04x}:0x{:04x}", g.subsysVendorId, g.subsysId));
    kv("Driver", g.driverName + " " + g.driverVersion);
    kv("Driver info", g.driverInfo);
    kv("VBIOS", g.vbios);
    kv("API", g.api + " " + g.apiVersion);
    if (g.vramBytes) kv("VRAM", bytes(static_cast<double>(g.vramBytes)));
    kv("PCI address", g.pciAddress);
    if (!g.pcieCurrent.empty()) kv("PCIe link", g.pcieCurrent + " (max " + g.pcieMax + ")");
    kv("Resizable BAR", g.resizableBar ? "yes" : "no");
    kv("CPU", std::format("{} ({} cores / {} threads)", h.cpu, h.cpuCores, h.cpuThreads));
    if (h.ramBytes) kv("RAM", bytes(static_cast<double>(h.ramBytes)));
    kv("OS", h.os);
    kv("Kernel", h.kernel);
    kv("Display", h.displayServer + (h.desktop.empty() ? "" : " / " + h.desktop));
    kv("Validation", std::format("{} errors, {} warnings", run.validationErrors, run.validationWarnings));
    o << "</table>";
    if (!run.system.extensions.empty()) {
        o << "<details><summary>" << run.system.extensions.size() << " API extensions</summary><p>";
        for (const auto& e : run.system.extensions) o << htmlEscape(e) << " ";
        o << "</p></details>";
    }
    if (!run.deviceLosses.empty()) {
        o << "<h3>Device losses</h3><table class=\"kv\">";
        for (const auto& d : run.deviceLosses) {
            std::string fault = !d.faultReportSupported ? "not available (no driver fault reporting)"
                                : d.faultDescription.empty() ? "(empty)"
                                                             : d.faultDescription;
            for (const auto& line : d.faultDetails) fault += "; " + line;
            o << "<tr><td>" << htmlEscape(std::format("{} (loop {}, {:.1f} s)", d.test, d.loop + 1, d.t)) << "</td><td>"
              << htmlEscape(d.message) << (d.recovered ? " — recovered" : " — run stopped") << "<br><span class=\"sub\">"
              << htmlEscape("Driver fault report: " + fault) << "</span></td></tr>";
        }
        o << "</table>";
    }
    if (!run.warnings.empty()) {
        o << "<h3>Warnings</h3><ul>";
        for (const auto& w : run.warnings) o << "<li>" << htmlEscape(w) << "</li>";
        o << "</ul>";
    }
    o << "<footer>" << htmlEscape(std::format("Generated by {} {} ({}) · command: {}", run.appName, run.appVersion,
                                              run.appRevision, run.commandLine))
      << "</footer></main></body></html>\n";
    return o.str();
}

} // namespace gt::report
