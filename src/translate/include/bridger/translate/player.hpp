#pragma once

// The single-player player (Java level.dat "Player" <-> Bedrock "~local_player"): position, motion, rotation,
// dimension, game mode, inventory, armor and offhand, as Chunker converts it (docs/formats/bedrock-world.md, "Player").
// Ender chest, experience, health, hunger, effects and abilities are not converted and are reported. Server players
// (playerdata/) are not read (as in Chunker).

#include <bridger/core/block_entity.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <optional>
#include <span>
#include <vector>

namespace bridger::translate {

// Java Player compound (big-endian NBT) -> Bedrock ~local_player value (little-endian NBT).
[[nodiscard]] std::optional<std::vector<u8>> java_player_to_bedrock(const RawNbt& java_player, const MappingData& mapping,
                                                                   ConversionReport& report);

// Bedrock ~local_player value -> Java Player compound for `java_data_version` (equipment compound from 1.21.5, inventory
// slots 100-103 and -106 before).
[[nodiscard]] std::optional<RawNbt> bedrock_player_to_java(std::span<const u8> bedrock_player, i32 java_data_version,
                                                           const MappingData& mapping, ConversionReport& report);

} // namespace bridger::translate
