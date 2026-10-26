#pragma once
// Analysis helpers for the diagnostic tests, kept free of GPU code so they can be
// unit-tested: VRAM error grouping, PCIe expectations and thermal-soak analysis.

#include "gt/results.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace gt::diag {

// --- VRAM tester -----------------------------------------------------------------------------
struct MemError {
    std::uint32_t chunk = 0;
    std::uint64_t word = 0; ///< 32-bit word index within the chunk
    std::uint32_t expected = 0;
    std::uint32_t actual = 0;
};

struct ErrorRange {
    std::uint32_t chunk = 0;
    std::uint64_t firstByte = 0; ///< offset within the chunk
    std::uint64_t lastByte = 0;
    std::uint32_t count = 0;
    std::uint32_t flippedBits = 0; ///< OR of expected ^ actual
};

/// Sorts the errors and merges those closer than `gapBytes` into ranges.
std::vector<ErrorRange> errorRanges(std::vector<MemError> errors, std::uint64_t gapBytes = 4096);
/// "bit 3", "bits 3, 17", or "12 different bits" for wide damage.
std::string describeBits(std::uint32_t mask);

// --- PCIe ------------------------------------------------------------------------------------
/// Usable bandwidth per lane and direction in GB/s after line encoding (0 for unknown gens).
double pcieLaneGBps(int gen);
double pcieExpectedGBps(int gen, int width);

// --- thermal soak ---------------------------------------------------------------------------
struct SoakAnalysis {
    bool hasSensors = false;
    double durationS = 0.0;
    double firstFps = 0.0;    ///< average FPS over the first 10 % (at least 5 s) of the soak
    double lastFps = 0.0;     ///< average FPS over the last 30 % (at least 5 s)
    double perfDropPct = 0.0; ///< (first - last) / first, positive = slower at the end
    double maxTempC = 0.0;
    double maxHotspotC = 0.0;
    double plateauTempC = 0.0;     ///< average GPU temperature over the last 30 %
    bool plateaued = false;        ///< the last 30 % stayed within 2 °C
    double timeToPlateauS = -1.0;  ///< first time within 1 °C of the plateau (-1 = still heating / unknown)
    double firstClockMhz = 0.0, lastClockMhz = 0.0;
    double clockDropPct = 0.0;
    std::map<std::string, double> throttlePct; ///< share of samples with each throttle reason
};

/// `frameMs` are consecutive frame times covering the soak; `samples` the telemetry
/// recorded during it (t in seconds, any origin).
SoakAnalysis analyzeSoak(std::span<const double> frameMs, std::span<const results::TelemetrySample> samples);

// --- memory latency ---------------------------------------------------------------------------
struct LatencyLevel {
    std::uint64_t upToBytes = 0; ///< largest working set still at this latency
    double latencyNs = 0.0;      ///< median latency of the plateau
};
/// Splits a latency-vs-working-set curve (ascending sizes) into plateaus: a new level
/// starts where latency rises more than `jump` (relative) above the current plateau.
/// The levels approximate the cache hierarchy (L1, L2, last-level cache, DRAM).
std::vector<LatencyLevel> latencyLevels(std::span<const std::uint64_t> sizes, std::span<const double> ns, double jump = 0.3);

} // namespace gt::diag
