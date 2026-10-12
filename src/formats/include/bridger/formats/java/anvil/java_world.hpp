#pragma once

// A Java world folder (docs/formats/anvil.md): level.dat (gzip NBT, root.Data) and the
// Overworld's region files: dimensions/minecraft/overworld/region/ (Java 26.1+) or region/
// (older). The Nether and the End are detected and reported, not converted.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/compression.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::java {

struct JavaLevelInfo {
    std::string level_name;
    std::optional<i32> data_version;
    std::optional<std::string> version_name;
    std::optional<BlockPos> spawn;
    std::optional<i64> seed;
};

[[nodiscard]] Result<JavaLevelInfo> read_java_level_dat(std::span<const u8> bytes, const io::DecompressLimits& limits = {});

struct JavaWorld {
    std::filesystem::path root;
    JavaLevelInfo level;
    std::vector<std::filesystem::path> overworld_regions; // sorted
    bool has_nether = false;
    bool has_end = false;
    bool has_entity_regions = false; // entities/ (1.17+): not converted
    bool modern_layout = false;      // dimensions/ folder layout (Java 26.1+)
};

// `path` is the world folder or its level.dat.
[[nodiscard]] Result<JavaWorld> open_java_world(const std::filesystem::path& path);

} // namespace bridger::formats::java
