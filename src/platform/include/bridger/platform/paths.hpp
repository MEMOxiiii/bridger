#pragma once

// Path helpers. All strings inside Bridger are UTF-8; these functions are the
// only sanctioned way to cross into OS path types.
//
// Why: on Windows, constructing std::filesystem::path from a narrow string uses
// the active ANSI code page, which corrupts non-ASCII file names.

#include <bridger/core/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace bridger::platform {

// UTF-8 -> path. Fails on invalid UTF-8.
[[nodiscard]] Result<std::filesystem::path> path_from_utf8(std::string_view utf8);

// path -> UTF-8. Fails if the native path cannot be represented (e.g. unpaired surrogates).
[[nodiscard]] Result<std::string> path_to_utf8(const std::filesystem::path& path);

// Absolute path usable beyond MAX_PATH. On Windows: prefixes "\\?\" (or "\\?\UNC\")
// to an absolute, normalized path. Elsewhere: returns the absolute path unchanged.
[[nodiscard]] Result<std::filesystem::path> to_long_path(const std::filesystem::path& path);

} // namespace bridger::platform
