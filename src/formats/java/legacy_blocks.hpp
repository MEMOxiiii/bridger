#pragma once

// Pre-1.13 (numeric id) block data shared by the legacy .schematic reader and old Java chunks
// (docs/formats/legacy-schematic.md, docs/formats/anvil.md "Old worlds"). Internal to bridger_formats.

#include <bridger/core/structure.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/io/nbt/nbt_view.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::formats::detail {

inline constexpr std::array<std::string_view, 16> kDyeColors{"white", "orange", "magenta", "light_blue", "yellow", "lime",
                                                             "pink", "gray", "light_gray", "cyan", "purple", "blue",
                                                             "brown", "green", "red", "black"};

// Pre-1.11 block entity ids ("Chest", "Trap", ...) -> namespaced ids.
[[nodiscard]] std::string normalize_block_entity_id(std::string_view id);

// A pre-1.13 block entity at local position `pos` rewritten into the form the translator expects:
// namespaced id, item stacks {id: String, count: Int, Slot}. Numeric item ids use the legacy
// table. Returns nullopt (with a warning) for entries that cannot be imported.
[[nodiscard]] Result<std::optional<BlockEntity>> legacy_block_entity(io::nbt::NbtCompoundView c, BlockPos pos,
                                                                     const java::LegacyTables& tables,
                                                                     std::vector<std::string>& warnings);

// Block states that pre-1.13 versions split across two blocks or kept in a block entity:
// door halves, the upper half of tall plants, bed colours, flower pot contents, skull types and
// rotation, banner base colours and note block pitches. Block entities that 1.13 removed
// (flower pots, note blocks) are dropped after use. Needs the block entities already imported.
void fix_legacy_blocks(Structure& s, std::vector<std::string>& warnings);

} // namespace bridger::formats::detail
