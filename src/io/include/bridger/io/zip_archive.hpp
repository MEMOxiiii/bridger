#pragma once

// In-memory zip archives (minizip-ng): .mcworld files are zipped world folders.
// Reading is guarded against zip bombs and path tricks: entry count and total
// uncompressed size are capped, and entry names must be relative paths without "..".

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bridger::io {

struct ZipEntry {
    std::string name; // '/'-separated relative path, UTF-8
    std::vector<u8> data;
};

// Builds a deflate-compressed zip of `entries` (timestamps fixed for reproducible output).
[[nodiscard]] Result<std::vector<u8>> write_zip(std::span<const ZipEntry> entries);

struct ZipLimits {
    usize max_entries = 100'000;
    u64 max_total_bytes = u64{4} << 30; // 4 GiB uncompressed
};

[[nodiscard]] Result<std::vector<ZipEntry>> read_zip(std::span<const u8> archive, const ZipLimits& limits = {});

// Streams every file under `dir` into a new zip file at `zip_path` (one file in memory at a
// time; names relative to `dir`, '/'-separated, sorted; fixed timestamps). For large worlds.
[[nodiscard]] Status write_zip_file_from_directory(const std::filesystem::path& dir, const std::filesystem::path& zip_path);

struct ZipListing {
    std::string name;
    u64 size = 0;
};
// Lists a zip file's entries without extracting them.
[[nodiscard]] Result<std::vector<ZipListing>> list_zip_file(const std::filesystem::path& zip_path, const ZipLimits& limits = {});

} // namespace bridger::io
