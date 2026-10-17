#pragma once

// Java -> Bedrock item stacks inside block entities: id, count, data value and the item's data (enchantments,
// damage, names, potions, books, container contents, fireworks, trims, ...). Formats and sources:
// docs/formats/bedrock-world.md, "Items". Internal to bridger_translate.

#include <bridger/core/types.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <string>
#include <string_view>

namespace bridger::translate::detail {

// Plain text of a Java text component: a plain string, a JSON string (before 1.21.5) or an NBT compound/list
// (1.21.5+). Formatting is dropped; `lossy` is set when anything besides text was present.
[[nodiscard]] std::string text_of(io::nbt::NbtView v, bool& lossy);

// Writes the Bedrock "Block" compound {name, states, version} for an item that is a block.
void write_block_tag(io::nbt::NbtWriter& w, std::string_view name, const MappingData& m, BedrockStateIndex idx);

// Java item list ("Items" of a container, or the "minecraft:container" component) -> a Bedrock list named `name`.
// Unknown items and data that is not converted are reported at `pos`.
// `slot_map` changes slot numbers where the editions' layouts differ (brewing stands).
using SlotMap = i8 (*)(i8);
void write_items(io::nbt::NbtWriter& w, std::string_view name, io::nbt::NbtView java_items, const MappingData& m, ConversionReport& report,
                 BlockPos pos, SlotMap slot_map = nullptr);

// One Java item stack (no slot) -> a Bedrock item compound written as `name` (e.g. "Book" of a lectern, "Item" of a
// decorated pot). Returns false (nothing written) when the item has no Bedrock counterpart (reported).
bool write_item(io::nbt::NbtWriter& w, std::string_view name, io::nbt::NbtCompoundView java_item, const MappingData& m,
                ConversionReport& report, BlockPos pos);

} // namespace bridger::translate::detail
