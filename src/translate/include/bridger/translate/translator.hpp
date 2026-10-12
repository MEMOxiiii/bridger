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

namespace bridger::translate {

enum class Fallback : u8 {
    Air,    // unmapped blocks become air (default)
    Marker, // unmapped blocks become a visible marker block
};

struct TranslateOptions {
    Fallback fallback = Fallback::Air;
};

// Input: a Java structure whose block/biome ids refer to its own registries.
// Output: a Bedrock structure of the same size where block layers hold
// BedrockStateIndex values, biome layers hold Bedrock numeric biome ids, and block
// entities hold Bedrock NBT (little-endian, without position keys: the world writer
// adds absolute x/y/z). Entities are not translated yet and are reported.
[[nodiscard]] Result<Structure> translate_to_bedrock(const Structure& java, const MappingData& mapping,
                                                     const TranslateOptions& options, ConversionReport& report);

// The Bedrock biome id used where the source has no biome data (plains).
inline constexpr i32 kDefaultBedrockBiome = 1;

} // namespace bridger::translate
