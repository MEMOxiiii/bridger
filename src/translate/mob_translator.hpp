#pragma once

// Mobs and simple objects between the editions (docs/formats/bedrock-world.md, "Mobs"): every vanilla mob, dropped
// items, armor stands and end crystals, with what both editions can hold: position, motion, rotation, name, health,
// baby/adult, persistence, scoreboard tags, equipment and each mob's look (sheep colour, horse coat, cat, wolf, frog,
// parrot, rabbit, fox, axolotl, llama, mooshroom, goat, slime size, shulker colour, panda, tropical fish, villager
// profession/biome/level, pig/cow/chicken climate and sound). Whatever is not carried is reported per entity type.
// Only when the conversion asks for entities (EntityMode::All). Internal to bridger_translate.

#include <bridger/core/entity.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

namespace bridger::translate::detail {

// Java entity (column-local position, big-endian NBT with "Pos") -> a Bedrock actor in out.entities (little-endian
// NBT without "Pos" and "UniqueID", which the world writer adds). False when the type is not one this converts.
bool java_mob_to_bedrock(const Entity& e, io::nbt::NbtCompoundView c, Structure& out, const MappingData& map, const TranslateOptions& options,
                         ConversionReport& report);

// Bedrock actor -> a Java entity in out.entities (big-endian NBT without "id", "Pos" and "UUID"). False when the type is
// not one this converts.
bool bedrock_mob_to_java(const Entity& actor, io::nbt::NbtCompoundView c, Structure& out, const MappingData& map,
                         const TranslateOptions& options, ConversionReport& report);

} // namespace bridger::translate::detail
