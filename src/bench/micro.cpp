#include "gt/bench/micro.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gt::bench {

std::uint32_t microSamples(const RunSettings& s) {
    switch (s.duration) {
    case DurationPreset::Smoke: return 2;
    case DurationPreset::Quick: return 5;
    case DurationPreset::Standard: return 9;
    case DurationPreset::Extreme: return 15;
    }
    return 5;
}

MicroResult measureMicro(TestContext& ctx, const std::function<void(rhi::CommandList&, std::uint32_t)>& record,
                         double targetMs, rhi::QueueType queue, std::uint32_t maxReps) {
    const RunSettings& s = ctx.settings();
    if (s.duration == DurationPreset::Smoke) targetMs = std::min(targetMs, 2.0);
    auto submit = [&](std::uint32_t reps) {
        ctx.checkStop();
        return ctx.timeGpu(
            [&](rhi::CommandList& cmd) {
                for (std::uint32_t r = 0; r < reps; ++r) record(cmd, r);
            },
            queue);
    };
    // Calibrate in two steps: the first submission includes one-time costs, and the per-repetition
    // cost drops once the GPU is kept busy by many repetitions.
    auto repsFor = [&](double perRep) {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(targetMs / std::max(perRep, 1e-4)), 1.0, static_cast<double>(std::max(maxReps, 1u))));
    };
    submit(1);
    std::uint32_t reps = repsFor(submit(1));
    if (reps > 1) reps = repsFor(submit(reps) / reps);
    MicroResult r;
    r.reps = reps;
    std::vector<double> t;
    const std::uint32_t n = microSamples(s);
    for (std::uint32_t i = 0; i < n; ++i) t.push_back(submit(reps) / reps);
    std::sort(t.begin(), t.end());
    r.samples = n;
    r.msPerRep = t[t.size() / 2];
    r.spreadPct = r.msPerRep > 0 ? (t.back() - t.front()) / r.msPerRep * 100.0 : 0.0;
    return r;
}

} // namespace gt::bench
