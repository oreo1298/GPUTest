#include "events.h"

#include <cmath>
#include <cstdio>
#include <format>

namespace gt::runner {

void StdoutSink::emit(const nlohmann::json& e) {
    if (m_mode == ProgressMode::Quiet) return;
    std::lock_guard lock(m_mutex);
    if (m_mode == ProgressMode::JsonLines) {
        std::string line;
        if (!e.contains("t")) {
            nlohmann::json stamped = e;
            stamped["t"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count() * 1000.0) / 1000.0;
            line = stamped.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        } else {
            line = e.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return;
    }
    // Text mode: only the events a human cares about.
    const std::string type = e.value("type", "");
    std::string line;
    if (type == "test_start") {
        line = std::format("[{}/{}] {} ...", e.value("index", 0) + 1, e.value("count", 0), e.value("name", ""));
    } else if (type == "test_end") {
        line = "    " + e.value("summary", std::string());
    } else if (type == "loop_end") {
        line = std::format("loop {} finished: score {:.0f}", e.value("loop", 0) + 1, e.value("score", 0.0));
    } else if (type == "safety_abort" || type == "device_lost" || type == "error") {
        line = std::format("!! {}: {}", type, e.value("message", ""));
    } else if (type == "run_end") {
        line = std::format("done: {} — results: {}", e.value("status", ""), e.value("results_file", ""));
    }
    if (!line.empty()) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
}

} // namespace gt::runner
