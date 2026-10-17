#pragma once

// Java chunk NBT, docs/formats/anvil.md. A chunk column becomes a 16 x height x 16 Java
// structure whose local (0,0,0) is world (cx*16, min_y, cz*16).

#include <bridger/core/block_registry.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::java {

struct JavaChunk {
    ChunkPos pos;
    i32 data_version = 0;
    i32 min_y = -64; // 0 for chunks older than 1.18
    bool full = false; // generation status "full": only full chunks are converted
    Structure column;
    std::vector<std::string> warnings;
};

inline constexpr i32 kFirstFlatteningChunkDataVersion = 2860; // Java 1.18 (21w43a+) section layout

// Entities of a chunk from Java 1.17's entity region files (entities/r.<x>.<z>.mca: {DataVersion, Position, Entities}),
// as core Entity values: id, position relative to the column at (pos.x*16, min_y, pos.z*16), and the whole compound
// (Pos included) as data. Chunks older than 1.17 keep "Entities" in the chunk itself; decode_chunk reads those.
[[nodiscard]] Result<std::vector<Entity>> decode_entity_chunk(std::span<const u8> nbt, ChunkPos pos, i32 min_y,
                                                              const io::nbt::NbtLimits& limits = {});

// Every Java chunk layout since McRegion (Beta 1.3): 1.18+ sections, the pre-1.18 "Level" compound
// with 1.13+ palettes, numeric-id sections (1.2 - 1.12) and McRegion columns (Beta 1.3 - 1.1).
// Numeric ids need `legacy` (the caller's legacy table); without it such chunks fail with
// UnsupportedVersion. States and biomes are upgraded to the pinned version (docs/formats/anvil.md).
[[nodiscard]] Result<JavaChunk> decode_chunk(std::span<const u8> nbt, std::shared_ptr<BlockStateRegistry> blocks,
                                             std::shared_ptr<BiomeRegistry> biomes, const io::nbt::NbtLimits& limits = {},
                                             const LegacyTables* legacy = nullptr);

} // namespace bridger::formats::java
