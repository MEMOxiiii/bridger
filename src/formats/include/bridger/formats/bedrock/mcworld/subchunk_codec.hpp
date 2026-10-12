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

} // namespace bridger::formats::bedrock
