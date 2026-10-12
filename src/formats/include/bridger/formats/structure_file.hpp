#pragma once

// Common result of reading any structure format into the IR, so the conversion
// pipeline does not depend on which reader produced it.

#include <bridger/core/block_registry.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/formats/format.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats {

struct StructureReadOptions {
    io::DecompressLimits decompress{};
    io::nbt::NbtLimits nbt{};
    u64 max_volume = u64{1} << 31;
    u32 max_palette_size = u32{1} << 20;
};

struct StructureFile {
    FormatId format = FormatId::SpongeSchem;
    std::optional<i32> data_version; // Java DataVersion when the format stores one
    std::vector<std::string> warnings;
    Structure structure;
};

namespace java {
struct LegacyTables;
}

// Reads a Java structure file (.schem, .nbt, .litematic, .schematic) given its format.
// `legacy` is required for .schematic (numeric ids).
[[nodiscard]] Result<StructureFile> read_java_structure(std::span<const u8> file_bytes, FormatId format,
                                                        std::shared_ptr<BlockStateRegistry> blocks,
                                                        std::shared_ptr<BiomeRegistry> biomes,
                                                        const java::LegacyTables* legacy = nullptr,
                                                        const StructureReadOptions& options = {});

} // namespace bridger::formats
