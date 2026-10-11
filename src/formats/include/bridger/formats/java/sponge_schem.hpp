#pragma once

// Sponge schematic (.schem) reader, versions 1, 2 and 3.
// Format details and sources: docs/formats/sponge-schem.md.

#include <bridger/core/block_registry.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::java {

struct SchemReadOptions {
    io::DecompressLimits decompress{};
    io::nbt::NbtLimits nbt{};
    u64 max_volume = u64{1} << 31;     // blocks; the data array has >= 1 byte per block anyway
    u32 max_palette_size = u32{1} << 20;
};

// Facts about the file itself (as opposed to the converted structure).
struct SchemInfo {
    i32 version = 0;                    // 1, 2 or 3
    std::optional<i32> data_version;    // Minecraft DataVersion (required by v2/v3, absent in v1)
    io::Compression compression = io::Compression::None;
    u16 width = 0;
    u16 height = 0;
    u16 length = 0;
    std::array<i32, 3> offset{0, 0, 0};
    usize palette_size = 0;
    usize biome_palette_size = 0;       // 0 if the file has no biomes
    bool biomes_3d = false;             // v3: per block; v2: per column
    usize block_entity_count = 0;
    usize entity_count = 0;
    std::optional<std::array<i32, 3>> worldedit_offset; // v1/v2 Metadata.WEOffsetX/Y/Z
    std::optional<std::array<i32, 3>> worldedit_origin; // v3 Metadata.WorldEdit.Origin
    // Non-fatal oddities (e.g. PaletteMax mismatch, block entity outside the volume).
    // Never silently dropped: callers show them in the conversion report.
    std::vector<std::string> warnings;
};

struct SchemFile {
    SchemInfo info;
    Structure structure;
};

// `file_bytes` is the file as stored on disk (gzip-compressed or raw NBT).
[[nodiscard]] Result<SchemFile> read_sponge_schem(std::span<const u8> file_bytes,
                                                  std::shared_ptr<BlockStateRegistry> block_registry,
                                                  std::shared_ptr<BiomeRegistry> biome_registry,
                                                  const SchemReadOptions& options = {});

} // namespace bridger::formats::java
