#pragma once

// Java -> Bedrock block entities and item stacks (docs/formats/bedrock-world.md,
// "Block entities"). Output NBT is little-endian and has no x/y/z: the world writer
// adds absolute positions. Position-valued fields (Bedrock chest "pairx"/"pairz")
// are written structure-local and offset by the writer.

#include <bridger/core/block_entity.hpp>
#include <bridger/core/types.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <optional>
#include <string_view>

namespace bridger::translate {

// Keys in a translated Bedrock block entity whose values are structure-local block
// coordinates (x and z axes respectively) and must be offset when placed.
inline constexpr std::string_view kLocalPairX = "pairx";
inline constexpr std::string_view kLocalPairZ = "pairz";

// Translates one Java block entity. `java_state` is the Java state at its position.
// Returns nullopt when the type is not supported (recorded in `report`).
[[nodiscard]] std::optional<BlockEntity> translate_block_entity(const BlockEntity& java, JavaStateId java_state,
                                                                const MappingData& mapping,
                                                                ConversionReport& report);

// Bedrock block entity required by a block even when the Java source has none
// (e.g. bed color, double-chest pairing). nullopt if the state needs none.
[[nodiscard]] std::optional<BlockEntity> synthesize_block_entity(BlockPos pos, JavaStateId java_state,
                                                                 const MappingData& mapping);

// Bedrock -> Java: translates one Bedrock block entity (little-endian NBT) given the Java
// state chosen for its position. The result holds big-endian Java NBT without id/x/y/z
// (BlockEntity::id is the Java id). nullopt when Java stores the data in the block itself
// (flower pots) or the type is not supported (recorded in `report`).
[[nodiscard]] std::optional<BlockEntity> translate_block_entity_to_java(const BlockEntity& bedrock, JavaStateId java_state,
                                                                        const MappingData& mapping,
                                                                        ConversionReport& report);

// True for Java states whose Bedrock form depends on more than the state itself.
[[nodiscard]] bool is_context_dependent(std::string_view java_block_id) noexcept;

} // namespace bridger::translate
