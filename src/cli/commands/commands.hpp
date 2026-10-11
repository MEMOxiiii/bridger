#pragma once

// CLI subcommands. Each returns a process exit code.

#include <string>

namespace bridger::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitNotImplemented = 3;

int run_info(const std::string& input_utf8);

} // namespace bridger::cli
