#pragma once

// A Java world folder (docs/formats/anvil.md): level.dat (gzip NBT, root.Data) and the region files of each
// dimension: dimensions/minecraft/<overworld|the_nether|the_end>/region/ (Java 26.1+), or region/, DIM-1/region/ and
// DIM1/region/ (older).

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
    std::optional<i32> storage_format; // level.dat "version": 19132 McRegion, 19133 Anvil
    std::optional<BlockPos> spawn;
    std::optional<i64> seed;
    std::optional<RawNbt> player; // Data.Player (single player), big-endian NBT
};

[[nodiscard]] Result<JavaLevelInfo> read_java_level_dat(std::span<const u8> bytes, const io::DecompressLimits& limits = {});

struct JavaWorld {
    std::filesystem::path root;
    JavaLevelInfo level;
    std::vector<std::filesystem::path> overworld_regions; // sorted
    std::vector<std::filesystem::path> nether_regions;    // sorted
    std::vector<std::filesystem::path> end_regions;       // sorted
    bool has_nether = false;
    bool has_end = false;
    // Bukkit/Spigot/Paper servers keep the Nether and the End in separate worlds next to this one
    // (<name>_nether/DIM-1, <name>_the_end/DIM1); they are not read (as Chunker), only reported.
    bool server_nether_folder = false;
    bool server_end_folder = false;
    bool has_entity_regions = false; // entities/ (1.17+): not converted
    bool modern_layout = false;      // dimensions/ folder layout (Java 26.1+)
};

// `path` is the world folder or its level.dat.
[[nodiscard]] Result<JavaWorld> open_java_world(const std::filesystem::path& path);

} // namespace bridger::formats::java
