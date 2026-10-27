#pragma once
// Scoring (documented in docs/SCORING.md).
//   test score  = 1000 * value / reference          (higher-is-better metrics)
//               = 1000 * reference / value          (lower-is-better metrics)
//   subscore    = geometric mean of the category's test scores (scores averaged over loops first)
//   overall     = geometric mean of the subscores
//   loop score  = geometric mean of that loop's test scores
//   stability   = worst loop score / best loop score

#include "gt/results.h"

namespace gt::scoring {

double testScore(double value, double reference, bool higherIsBetter);

/// Revision of the test workloads: bumped whenever test content changes what a score measures, so that runs
/// of different workloads are never compared (it is the last part of the comparable key).
inline constexpr int kWorkloadRevision = 2;

/// Key that must match for two runs' scores to be comparable:
/// `suite|api|WxH|quality|duration|upscaler|rN` (upscaler "off" for native rendering, N = kWorkloadRevision).
std::string comparableKey(const results::RunConfig& config);

/// Fills scores, loop scores, stability and efficiency from the test results.
void computeScores(results::RunResult& run, double stabilityThreshold = 0.97);

} // namespace gt::scoring
