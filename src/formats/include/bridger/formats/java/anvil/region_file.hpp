#pragma once

// Java Anvil region files (r.<x>.<z>.mca), docs/formats/anvil.md:
// 4 KiB location table (3-byte big-endian sector offset + 1-byte sector count per chunk,
// index (x & 31) + (z & 31) * 32), 4 KiB timestamps, then chunk payloads: i32 BE length
// (including the compression byte), compression byte (1 gzip, 2 zlib, 3 none, 4 LZ4,
// 127 custom; +128 = payload in an external c.<x>.<z>.mcc file), data.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/platform/mapped_file.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace bridger::formats::java {

class RegionFile {
public:
    [[nodiscard]] static Result<RegionFile> open(const std::filesystem::path& path);

    // Region coordinates from the file name "r.<x>.<z>.mca".
    [[nodiscard]] i32 region_x() const noexcept { return rx_; }
    [[nodiscard]] i32 region_z() const noexcept { return rz_; }

    [[nodiscard]] bool has_chunk(u32 local_x, u32 local_z) const noexcept;
    // Decompressed chunk NBT; nullopt if the slot is empty.
    [[nodiscard]] Result<std::optional<std::vector<u8>>> read_chunk(u32 local_x, u32 local_z,
                                                                   const io::DecompressLimits& limits = {}) const;

private:
    platform::MappedFile file_;
    std::filesystem::path path_;
    i32 rx_ = 0;
    i32 rz_ = 0;
};

} // namespace bridger::formats::java
