#pragma once

// Bedrock SubChunkPrefix encoding, format version 9 (docs/formats/bedrock-world.md):
//   u8 version (9), u8 layer count, i8 subchunk y,
//   per layer: u8 (bits_per_index << 1 | runtime flag 0), ceil(4096 / floor(32/bits)) u32 LE words
//              (indices packed from the low bits, XZY order: i = x*256 + z*16 + y),
//              i32 LE palette count, palette entries as little-endian NBT roots {name, states, version}.

#include <bridger/core/result.hpp>
#include <bridger/core/section.hpp>
#include <bridger/core/types.hpp>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::formats::bedrock {

// Encoded little-endian NBT root compound for a block state id (the caller owns the mapping).
using StateNbtFn = std::function<Result<std::span<const u8>>(u32 state)>;

// Layers in Bridger order (y*256 + z*16 + x). Layer 0 = blocks, layer 1 = liquids.
[[nodiscard]] Result<std::vector<u8>> encode_subchunk(i8 y_index, std::span<const PaletteLayer* const> layers,
                                                      const StateNbtFn& state_nbt);

struct DecodedLayer {
    std::vector<std::string> palette;        // canonical "name[key=value,...]" of each NBT entry
    std::array<u16, kSectionVolume> indices; // palette index per block, Bridger order
};
struct DecodedSubchunk {
    u8 version = 0;
    i8 y_index = 0;
    std::vector<DecodedLayer> layers;
};

// Decodes versions 8 and 9 with persistent (NBT) palettes; used for validation and tests.
[[nodiscard]] Result<DecodedSubchunk> decode_subchunk(std::span<const u8> data);

// Same, but palette entries are kept as their exact little-endian NBT bytes, so existing
// world data can be re-encoded unchanged (placement into existing worlds).
struct RawSubchunkLayer {
    std::vector<std::vector<u8>> palette;    // raw NBT root per entry
    std::array<u16, kSectionVolume> indices; // Bridger order
};
struct RawSubchunk {
    u8 version = 0;
    i8 y_index = 0;
    bool legacy = false; // pre-1.2.13 numeric ids: palette entries are legacy_palette_entry() compounds
    std::vector<RawSubchunkLayer> layers;
};
// Versions 0-9: 0 and 2-7 (numeric ids, 1.0 - 1.2.x), 1 (one storage, 1.2.13), 8 and 9.
[[nodiscard]] Result<RawSubchunk> decode_subchunk_raw(std::span<const u8> data);

// Numeric block ids (before 1.2.13) have no palette; Bridger stands in a palette entry
// {"bridger:legacy_id": Int id, "val": Short data}, which the state lookup upgrades
// (translate/bedrock_upgrade.hpp).
inline constexpr std::string_view kLegacyIdEntryKey = "bridger:legacy_id";
[[nodiscard]] std::vector<u8> legacy_palette_entry(u8 id, u8 data);
// 16 x 16 x 16 ids and data nibbles; bedrock_offset(x, y, z, base) gives the array index.
[[nodiscard]] RawSubchunkLayer legacy_layer(std::span<const u8> ids, std::span<const u8> data,
                                            usize (*bedrock_offset)(usize x, usize y, usize z, usize base), usize base);

// The pre-1.0 LegacyTerrain column (key tag 0x30): eight subchunks of numeric ids from y 0, and
// 2D biome ids (index z*16 + x).
struct LegacyTerrain {
    std::array<RawSubchunkLayer, 8> subchunks;
    std::array<u8, 256> biomes{};
};
[[nodiscard]] Result<LegacyTerrain> decode_legacy_terrain(std::span<const u8> value);

} // namespace bridger::formats::bedrock
