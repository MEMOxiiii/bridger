#pragma once

// CLI subcommands. Each returns a process exit code.

#include <string>

namespace bridger::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitNotImplemented = 3;

int run_info(const std::string& input_utf8);

struct ConvertArgs {
    std::string input;
    std::string output;
    std::string format;      // --format (overrides the output extension)
    std::string report_path; // --report
    std::string fallback = "air";
    std::string air = "keep";
    std::string place_at;
    std::string into_world;
    std::string level_name;
    bool clip = false;
    bool shift_to_fit = false;
    bool clear_entities = false;
    bool overwrite = false;
    std::size_t threads = 0;
};

int run_convert(const ConvertArgs& args);

} // namespace bridger::cli
