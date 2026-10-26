#pragma once

#include "gt/bench/context.h"
#include "options.h"

#include <chrono>
#include <mutex>

namespace gt::runner {

/// Writes events as JSON lines (machine mode) or short human-readable lines (text mode) to stdout.
class StdoutSink final : public bench::EventSink {
public:
    explicit StdoutSink(ProgressMode mode) : m_mode(mode), m_t0(std::chrono::steady_clock::now()) {}
    void emit(const nlohmann::json& event) override;
    ProgressMode mode() const { return m_mode; }
    /// Events without a "t" field get seconds since this point (the run start).
    void setStart(std::chrono::steady_clock::time_point t0) { m_t0 = t0; }

private:
    ProgressMode m_mode;
    std::chrono::steady_clock::time_point m_t0;
    std::mutex m_mutex;
};

} // namespace gt::runner
