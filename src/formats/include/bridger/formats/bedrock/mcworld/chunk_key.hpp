#pragma once

// Bedrock LevelDB chunk keys (docs/formats/bedrock-world.md, "LevelDB keys"):
//   x (i32 LE), z (i32 LE), [dimension (i32 LE), omitted for the Overworld], tag (u8),
//   [subchunk y (i8), SubChunkPrefix only]

#include <bridger/core/types.hpp>

#include <optional>
#include <string>

namespace bridger::formats::bedrock {

enum class ChunkTag : u8 {
    Data3D = 0x2B,         // heightmap + 3D biomes
    Version = 0x2C,        // chunk storage version (1 byte)
    SubChunkPrefix = 0x2F, // 16x16x16 terrain
    BlockEntity = 0x31,    // concatenated little-endian NBT roots
    Entity = 0x32,         // legacy entity storage (worlds not re-saved since actorprefix keys), same encoding
    FinalizedState = 0x36, // i32 LE
};

enum class Dimension : i32 { Overworld = 0, Nether = 1, End = 2 };

[[nodiscard]] std::string chunk_key(i32 chunk_x, i32 chunk_z, Dimension dim, ChunkTag tag,
                                    std::optional<i8> subchunk_y = std::nullopt);

// Overworld vertical range (Caves & Cliffs and later): y -64..319, subchunks -4..19.
inline constexpr i32 kOverworldMinY = -64;
inline constexpr i32 kOverworldMaxY = 319;
inline constexpr i32 kOverworldMinSubchunk = -4;
inline constexpr i32 kOverworldSubchunks = 24;

// Vertical range of each dimension in subchunks: the Overworld -4..19, the Nether 0..7 (y 0..127), the End 0..15
// (y 0..255). Data3D holds one biome storage per subchunk of the range (Chunker Dimension.biomeHeight: 24, 8, 16;
// minecraft.wiki "Bedrock Edition level format"). docs/formats/bedrock-world.md, "Dimensions".
struct DimensionRange {
    i32 min_subchunk;
    i32 subchunks;
    [[nodiscard]] constexpr i32 min_y() const noexcept { return min_subchunk * 16; }
    [[nodiscard]] constexpr i32 max_y() const noexcept { return (min_subchunk + subchunks) * 16 - 1; }
    [[nodiscard]] constexpr bool contains_subchunk(i32 sy) const noexcept { return sy >= min_subchunk && sy < min_subchunk + subchunks; }
};
[[nodiscard]] constexpr DimensionRange dimension_range(Dimension d) noexcept {
    switch (d) {
    case Dimension::Nether: return {0, 8};
    case Dimension::End: return {0, 16};
    case Dimension::Overworld: break;
    }
    return {kOverworldMinSubchunk, kOverworldSubchunks};
}

// Values for the pinned Bedrock version (26.50), sources in docs/formats/bedrock-world.md.
inline constexpr u8 kChunkVersion = 42;
inline constexpr i32 kFinalizedDone = 2;
inline constexpr u8 kSubchunkFormat = 9;

} // namespace bridger::formats::bedrock
