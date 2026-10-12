#pragma once

// PNG output for screenshots (stb_image_write).

#include <bridger/core/result.hpp>

#include <filesystem>
#include <span>

namespace bridger::render {

// RGBA8 rows, top to bottom.
[[nodiscard]] Status write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);
// Same, into memory.
[[nodiscard]] Result<std::vector<u8>> encode_png(u32 width, u32 height, std::span<const u8> rgba);

} // namespace bridger::render
