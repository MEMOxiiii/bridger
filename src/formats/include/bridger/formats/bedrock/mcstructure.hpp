#pragma once

// Bedrock structure files (.mcstructure), docs/formats/mcstructure.md.
// Uncompressed little-endian NBT; two block layers sharing one palette; index order
// (x * size_y + y) * size_z + z (z fastest).

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::bedrock {

// Writes a Bedrock structure (block layers hold Bedrock state ids resolvable by
// `state_nbt`; block entities hold Bedrock NBT without positions).
[[nodiscard]] Result<std::vector<u8>> write_mcstructure(const Structure& structure, u32 air_state,
                                                        const StateNbtFn& state_nbt);

// Resolves a palette entry {name, states, version} to a Bedrock state id, or nullopt.
using StateLookupFn = std::function<std::optional<u32>(io::nbt::NbtCompoundView entry)>;

struct McStructureFile {
    Structure structure;
    std::vector<std::string> warnings; // unknown states, structure void, ...
    std::array<i32, 3> world_origin{0, 0, 0};
};

struct McStructureReadOptions {
    io::nbt::NbtLimits nbt{};
    u64 max_volume = u64{1} << 31;
};

[[nodiscard]] Result<McStructureFile> read_mcstructure(std::span<const u8> bytes,
                                                       std::shared_ptr<BlockStateRegistry> blocks,
                                                       std::shared_ptr<BiomeRegistry> biomes, u32 bedrock_data_version,
                                                       const StateLookupFn& lookup, u32 air_state,
                                                       const McStructureReadOptions& options = {});

} // namespace bridger::formats::bedrock
