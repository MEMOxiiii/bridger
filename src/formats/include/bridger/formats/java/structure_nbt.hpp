#pragma once

// Java structure files (.nbt, written by structure blocks), docs/formats/structure-nbt.md.

#include <bridger/formats/structure_file.hpp>

namespace bridger::formats::java {

[[nodiscard]] Result<StructureFile> read_structure_nbt(std::span<const u8> file_bytes,
                                                       std::shared_ptr<BlockStateRegistry> blocks,
                                                       std::shared_ptr<BiomeRegistry> biomes,
                                                       const StructureReadOptions& options = {});

} // namespace bridger::formats::java
