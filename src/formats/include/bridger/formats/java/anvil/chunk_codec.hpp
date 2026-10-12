#pragma once

// Java chunk NBT (1.18+ layout), docs/formats/anvil.md. A chunk column becomes a
// 16 x height x 16 Java structure whose local (0,0,0) is world (cx*16, min_y, cz*16).

#include <bridger/core/block_registry.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::java {

struct JavaChunk {
    ChunkPos pos;
    i32 data_version = 0;
    i32 min_y = -64;
    bool full = false; // generation status "full": only full chunks are converted
    Structure column;
    std::vector<std::string> warnings;
};

inline constexpr i32 kFirstFlatteningChunkDataVersion = 2860; // Java 1.18 (21w43a+) section layout

[[nodiscard]] Result<JavaChunk> decode_chunk(std::span<const u8> nbt, std::shared_ptr<BlockStateRegistry> blocks,
                                             std::shared_ptr<BiomeRegistry> biomes, const io::nbt::NbtLimits& limits = {});

} // namespace bridger::formats::java
