#pragma once

#include "gt/bench/context.h"
#include "options.h"

#include <mutex>

namespace gt::runner {

/// Writes events as JSON lines (machine mode) or short human-readable lines (text mode) to stdout.
class StdoutSink final : public bench::EventSink {
public:
    explicit StdoutSink(ProgressMode mode) : m_mode(mode) {}
    void emit(const nlohmann::json& event) override;
    ProgressMode mode() const { return m_mode; }

private:
    ProgressMode m_mode;
    std::mutex m_mutex;
};

} // namespace gt::runner
