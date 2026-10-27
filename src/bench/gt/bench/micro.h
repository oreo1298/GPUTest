#pragma once
// Timing for micro-benchmarks: calibrates how many repetitions of a workload make one
// submission last about `targetMs` (long enough to dwarf submission overhead and
// timer resolution, short enough to stay far from OS GPU watchdogs), then times
// several submissions and reports the median time per repetition.

#include "gt/bench/context.h"

#include <functional>

namespace gt::bench {

struct MicroResult {
    double msPerRep = 0.0;   ///< median GPU time of one repetition
    std::uint32_t reps = 1;  ///< repetitions per submission
    std::uint32_t samples = 0;
    double spreadPct = 0.0;  ///< (slowest - fastest) / median of the samples
};

/// Minimum number of timed submissions for the duration preset (smoke 2, quick 5, standard 9, extreme 15).
std::uint32_t microSamples(const RunSettings& s);
/// Minimum time spent on the timed submissions of one measurement (smoke 0, quick 1 s, standard 3 s, extreme
/// 8 s): long enough for clocks to settle, so a fast GPU does not finish a test in a blink.
double microSeconds(const RunSettings& s);

/// `record(cmd, rep)` records one repetition. Throws StopRequested when the run is stopped.
/// `maxReps` bounds repetitions per submission (for workloads that use per-list transient memory).
MicroResult measureMicro(TestContext& ctx, const std::function<void(rhi::CommandList&, std::uint32_t rep)>& record,
                         double targetMs = 10.0, rhi::QueueType queue = rhi::QueueType::Graphics,
                         std::uint32_t maxReps = 4096);

} // namespace gt::bench
