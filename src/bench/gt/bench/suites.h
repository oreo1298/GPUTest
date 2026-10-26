#pragma once

#include "gt/bench/test.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace gt::bench {

struct Suite {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tests; ///< resolved test ids, in run order
    std::string duration;           ///< default duration preset ("" = keep CLI default)
    std::string quality;
    std::string loop;
    std::string resolution;
    bool custom = false;
};

/// Preset suite ids in display order.
const std::vector<std::string>& presetSuiteIds();
/// Resolves a preset id, or loads a custom suite from a JSON file path.
std::optional<Suite> resolveSuite(const std::string& idOrPath, std::string* error = nullptr);
std::vector<Suite> presetSuites();
nlohmann::json toJson(const Suite& s);
std::optional<Suite> suiteFromJson(const nlohmann::json& j, std::string* error = nullptr);

} // namespace gt::bench
