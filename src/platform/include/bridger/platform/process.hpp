#pragma once

// The running program and new processes (the desktop app's self-update restarts itself).

#include <bridger/core/result.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace bridger::platform {

// Full path of the running executable.
[[nodiscard]] Result<std::filesystem::path> executable_path();

// Starts `exe` with `args` (UTF-8) as an independent process and returns without waiting for it.
[[nodiscard]] Status launch_detached(const std::filesystem::path& exe, std::span<const std::string> args);

} // namespace bridger::platform
