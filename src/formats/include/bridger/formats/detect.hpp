#pragma once

// Format detection by file extension, confirmed by the file's leading bytes.

#include <bridger/core/result.hpp>
#include <bridger/formats/format.hpp>

#include <filesystem>
#include <span>

namespace bridger::formats {

// `head` is the start of the file (a few bytes are enough). Fails with
// UnsupportedFormat when the extension is unknown or the bytes contradict it.
[[nodiscard]] Result<FormatId> detect_format(const std::filesystem::path& path, std::span<const u8> head);

} // namespace bridger::formats
