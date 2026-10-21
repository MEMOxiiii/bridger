#pragma once

// Java entities -> Bedrock (docs/formats/bedrock-world.md, "Entities"): item frames become a frame block with an
// ItemFrame block entity (Bedrock stores them as blocks), paintings become Bedrock actors; every other entity is
// reported. Internal to bridger_translate.

#include <bridger/core/entity.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace bridger::translate::detail {

// The first Bedrock version with a painting motive (BedrockPaintingMotiveResolver); {0,0,0} for the old ones. Checked
// when writing for an older target.
[[nodiscard]] std::optional<std::array<i32, 3>> painting_motive_since(std::string_view bedrock_motive) noexcept;

// Translates one Java entity of the column `java` into `out` (the Bedrock column of the same size): a frame block and
// block entity, or an actor in out.entities (Entity::data = little-endian actor NBT without "Pos"; the world writer
// adds the absolute position). `air` and `water` are Bedrock state indices. options.entities decides what is written:
// frames and paintings (Decorations), also mobs and objects (All, mob_translator.hpp) or nothing (None); the rest is
// reported.
void translate_entity(const Entity& e, Structure& out, const MappingData& map, BedrockStateIndex air, BedrockStateIndex water,
                      const TranslateOptions& options, ConversionReport& report);

// Bedrock -> Java. Java entities in Structure::entities carry big-endian NBT without "id", "Pos" and "UUID" (the
// Java writer adds them) and with their tile ("block_pos") column-local, offset by the writer like block entities.
[[nodiscard]] bool is_bedrock_frame(std::string_view bedrock_block_name) noexcept;
// A frame block (Bedrock state `frame_state`) with its ItemFrame/GlowItemFrame block entity -> a Java item frame.
void frame_to_java(const BlockEntity& be, BedrockStateIndex frame_state, Structure& out, const MappingData& map, ConversionReport& report);
// A Bedrock actor -> a Java entity: paintings, and with EntityMode::All mobs and objects; everything else is reported.
void actor_to_java(const Entity& actor, Structure& out, const MappingData& map, const TranslateOptions& options, ConversionReport& report);
// The first Java DataVersion with a painting variant (JavaPaintingMotiveResolver: 1.19, 1.21, 1.21.7); 0 for old ones.
[[nodiscard]] i32 java_painting_since(std::string_view java_variant) noexcept;

} // namespace bridger::translate::detail
