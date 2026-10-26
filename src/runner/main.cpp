#include "gt/app_info.h"
#include "gt/log.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    CLI::App app{std::string(gt::app::kDisplayName) + " benchmark runner", gt::app::kRunnerName};
    bool showVersion = false;
    bool json = false;
    app.add_flag("--version", showVersion, "Print version and exit");
    app.add_flag("--json", json, "Machine-readable output");
    CLI11_PARSE(app, argc, argv);

    if (showVersion) {
        if (json) {
            nlohmann::json j = {{"name", gt::app::kDisplayName},
                                {"id", gt::app::kId},
                                {"version", gt::app::kVersion},
                                {"revision", gt::app::kGitRevision}};
            std::printf("%s\n", j.dump().c_str());
        } else {
            std::printf("%s %s (%s)\n", gt::app::kRunnerName, gt::app::kVersion, gt::app::kGitRevision);
        }
        return 0;
    }
    gt::log::info("{} {} — nothing to do yet", gt::app::kRunnerName, gt::app::kVersion);
    return 0;
}
