#pragma once

// Upgrading Java block states and biome ids saved by older versions to the pinned version
// (docs/formats/anvil.md, "Old worlds"). Internal to bridger_formats.

#include <bridger/core/block_state.hpp>
#include <bridger/core/types.hpp>

#include <optional>
#include <string_view>

namespace bridger::formats::detail {

// A 1.13+ block state saved at `data_version` (0 = unknown) -> the same block in the pinned version.
// Only renames and value changes since 1.13 apply; properties the old version did not have take
// their default later (MappingData::find_java_state).
// nullopt: nothing to change (the common case; no copy is made).
[[nodiscard]] std::optional<BlockState> upgrade_java_state(const BlockState& state, i32 data_version);
// The state to use: the upgraded one, or `state` itself.
[[nodiscard]] inline BlockState upgraded(BlockState state, i32 data_version) {
    if (auto u = upgrade_java_state(state, data_version)) return std::move(*u);
    return state;
}

// Numeric biome id of 1.17 and older (Biomes arrays) -> pinned-version name. `merged` is set for
// the variant biomes 1.18 removed, which become their base biome (not exact).
struct LegacyBiome {
    std::string_view name; // "minecraft:windswept_hills"
    bool merged = false;
};
[[nodiscard]] std::optional<LegacyBiome> legacy_biome(i32 id);

} // namespace bridger::formats::detail
