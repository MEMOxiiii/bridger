#pragma once

// Java -> Bedrock translation of a structure (docs/adr/0008-translation-design.md).
//
// 1. Palette stage: every distinct Java state of a section is translated once and
//    the section's palette is remapped; the 4096-entry index array is not touched.
// 2. Context stage: only positions whose state needs more than the state itself
//    (waterlogging -> Bedrock layer 2, double-chest pairing, bed colors) are visited.
//    Rules read the immutable source structure and write to the output, so results
//    cannot depend on processing order.
// Everything not translated exactly is recorded in the ConversionReport.

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <array>
#include <functional>
#include <optional>

namespace bridger::translate {

enum class Fallback : u8 {
    Air,    // unmapped blocks become air (default)
    Marker, // unmapped blocks become a visible marker block
};

// Which entities a conversion writes (ConvertOptions::entities, `--entities`).
enum class EntityMode : u8 {
    Decorations, // item frames and paintings only (the default)
    All,         // also mobs, dropped items, armor stands and end crystals (docs/formats/bedrock-world.md, "Mobs")
    None,        // no entities at all
};

// The UniqueID Bridger gives the converted single-player player in a Bedrock world (the "~local_player" record), so
// tamed animals can name it as their owner. Bedrock numbers actors (world start count, flipped) << 32 | counter and
// the local player is counter 1 in worlds the game saves; the world writer numbers other actors from 2 on.
inline constexpr i64 kBedrockLocalPlayerId = static_cast<i64>(0xFFFFFFFF00000001ull);

struct TranslateOptions {
    Fallback fallback = Fallback::Air;
    EntityMode entities = EntityMode::Decorations;
    // Java -> Bedrock: the UUID of the Java world's single-player player (level.dat Player.UUID). Animals it tamed stay
    // tamed, owned by the converted player (kBedrockLocalPlayerId); other owners cannot be carried and are reported.
    std::optional<std::array<i32, 4>> player_uuid;
    // Bedrock -> Java only: the Bedrock state just outside the structure (a neighbouring chunk
    // column when a world is converted column by column), so context rules see across borders.
    // nullopt = unknown or absent, treated like air. Must be thread-safe if shared.
    std::function<std::optional<u32>(BlockPos)> bedrock_outside;
};

// Input: a Java structure whose block/biome ids refer to its own registries.
// Output: a Bedrock structure of the same size where block layers hold
// BedrockStateIndex values, biome layers hold Bedrock numeric biome ids, and block
// entities hold Bedrock NBT (little-endian, without position keys: the world writer
// adds absolute x/y/z). Entities are not translated yet and are reported.
[[nodiscard]] Result<Structure> translate_to_bedrock(const Structure& java, const MappingData& mapping,
                                                     const TranslateOptions& options, ConversionReport& report);

// Bedrock -> Java (reverse_translator.cpp). Input: a Bedrock structure as produced by the
// Bedrock readers (block layers hold BedrockStateIndex values, layer 2 the liquids, biome
// layers Bedrock numeric ids, block entities little-endian Bedrock NBT with structure-local
// positions and pairx/pairz). Output: a Java structure with its own registries (pinned Java
// DataVersion) and big-endian Java block entities. The marker fallback is minecraft:sponge.
[[nodiscard]] Result<Structure> translate_to_java(const Structure& bedrock, const MappingData& mapping,
                                                  const TranslateOptions& options, ConversionReport& report);

// The Bedrock biome id used where the source has no biome data (plains).
inline constexpr i32 kDefaultBedrockBiome = 1;

} // namespace bridger::translate
