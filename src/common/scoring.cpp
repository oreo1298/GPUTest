#include "gt/scoring.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace gt::scoring {

double testScore(double value, double reference, bool higherIsBetter) {
    if (!(value > 0.0) || !(reference > 0.0) || !std::isfinite(value)) return 0.0;
    return 1000.0 * (higherIsBetter ? value / reference : reference / value);
}

std::string comparableKey(const results::RunConfig& c) {
    return std::format("{}|{}|{}x{}|{}|{}", c.suite, c.api, c.width, c.height, c.quality, c.duration);
}

void computeScores(results::RunResult& run, double stabilityThreshold) {
    using results::Status;
    run.scores = {};
    run.scores.comparableKey = comparableKey(run.config);

    // Average each test's score over loops, then geomean per category.
    std::map<std::string, std::pair<double, int>> perTest; // id -> (sum, n)
    std::map<std::string, std::string> categoryOf;
    std::map<int, std::vector<double>> perLoop;
    for (const auto& t : run.tests) {
        if (!t.scored || t.status != Status::Completed || !(t.score > 0.0)) continue;
        auto& acc = perTest[t.id];
        acc.first += t.score;
        acc.second += 1;
        categoryOf[t.id] = t.category;
        perLoop[t.loop].push_back(t.score);
    }
    std::map<std::string, std::vector<double>> perCategory;
    for (const auto& [id, acc] : perTest) perCategory[categoryOf[id]].push_back(acc.first / acc.second);
    std::vector<double> subs;
    for (const auto& [cat, scores] : perCategory) {
        const double g = geometricMean(scores);
        run.scores.subscores[cat] = g;
        subs.push_back(g);
    }
    run.scores.overall = geometricMean(subs);

    // Loop scores (only loops where every scored test of the suite completed count for stability).
    std::size_t expected = 0;
    for (const auto& [l, v] : perLoop) expected = std::max(expected, v.size());
    for (auto& loop : run.loops) {
        auto it = perLoop.find(loop.index);
        loop.score = it == perLoop.end() ? 0.0 : geometricMean(it->second);
        loop.complete = it != perLoop.end() && it->second.size() == expected;
    }
    std::vector<double> xs, ys;
    double best = 0.0, worst = 0.0;
    for (const auto& loop : run.loops) {
        if (!loop.complete || !(loop.score > 0.0)) continue;
        if (xs.empty()) best = worst = loop.score;
        best = std::max(best, loop.score);
        worst = std::min(worst, loop.score);
        xs.push_back(loop.startS / 3600.0);
        ys.push_back(loop.score);
    }
    run.stability = {};
    run.stability.threshold = stabilityThreshold;
    if (xs.size() >= 2 && best > 0.0) {
        run.stability.valid = true;
        run.stability.ratio = worst / best;
        run.stability.passed = run.stability.ratio >= stabilityThreshold;
        const double mean = geometricMean(ys);
        run.stability.trendPercentPerHour = mean > 0.0 ? linearSlope(xs, ys) / mean * 100.0 : 0.0;
    }

    // Efficiency: overall score per average watt over the samples taken while scored tests ran.
    double powerSum = 0.0;
    std::size_t powerN = 0;
    for (const auto& s : run.telemetry.samples) {
        if (!std::isfinite(s.powerW)) continue;
        for (const auto& t : run.tests) {
            if (t.scored && s.t >= t.startS && s.t <= t.startS + t.durationS) {
                powerSum += s.powerW;
                ++powerN;
                break;
            }
        }
    }
    if (powerN && run.scores.overall > 0.0) run.scores.perWatt = run.scores.overall / (powerSum / powerN);
}

} // namespace gt::scoring
