#pragma once

// Bedrock level.dat: 8-byte header (i32 LE storage version, i32 LE payload length)
// followed by little-endian NBT (docs/formats/bedrock-world.md, "level.dat").

#include <bridger/core/types.hpp>

#include <array>
#include <string>
#include <vector>

namespace bridger::formats::bedrock {

struct LevelDatOptions {
    std::string level_name = "Bridger World";
    BlockPos spawn{0, -60, 0};
    std::string flat_world_layers; // FlatWorldLayers JSON
    i64 last_played = 0;           // seconds since the epoch
    i64 random_seed = 0;
    i32 game_type = 1; // creative: converted builds are for exploring
    i32 generator = 2; // 2 flat (new worlds for structures), 1 infinite (converted Java worlds)
    // Pinned Bedrock version 26.50 (docs/versions.md); sources in bedrock-world.md.
    i32 storage_version = 10;
    i32 network_version = 2192;
    std::array<i32, 5> version{1, 26, 50, 0, 0};
};

[[nodiscard]] std::vector<u8> encode_level_dat(const LevelDatOptions& options);

// Default superflat layers (bedrock, 2 dirt, grass block) as written by the game;
// the surface block is at y = -61, so the first free y is -60.
[[nodiscard]] std::string default_flat_world_layers();
inline constexpr i32 kDefaultFlatSurfaceY = -61;

} // namespace bridger::formats::bedrock
