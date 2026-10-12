#pragma once

// Litematica schematics (.litematic), docs/formats/litematic.md. All regions are merged
// into one structure covering their enclosing box (later regions win where they overlap).

#include <bridger/formats/structure_file.hpp>

namespace bridger::formats::java {

[[nodiscard]] Result<StructureFile> read_litematic(std::span<const u8> file_bytes,
                                                   std::shared_ptr<BlockStateRegistry> blocks,
                                                   std::shared_ptr<BiomeRegistry> biomes,
                                                   const StructureReadOptions& options = {});

} // namespace bridger::formats::java
