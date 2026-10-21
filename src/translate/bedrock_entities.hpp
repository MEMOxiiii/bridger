#pragma once

// What a Bedrock actor's "definitions" list and "properties" should hold for a mob in a given state, from the vanilla
// behavior pack (data/bedrock-entities.nbt, tools/mapgen/build_bedrock_entities.py; docs/formats/bedrock-world.md,
// "Mobs"). Internal to bridger_translate.

#include <bridger/core/types.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::translate::detail {

// The state a converted mob has; unset fields do not influence the choice.
struct MobState {
    bool baby = false;
    std::optional<i32> variant, mark_variant, color, color2, skin_id;
    std::optional<bool> tamed, saddled, chested, sheared;
};

// The "definitions" list for `identifier` in `state`: "+identifier" first, then the component groups of the spawn
// outcome that matches the state best (ties: the likeliest outcome). Unknown identifiers get only "+identifier".
[[nodiscard]] std::vector<std::string> bedrock_definitions(std::string_view identifier, const MobState& state);

// An entity property the mob declares, with its default, written under "properties" as the game does.
struct BedrockProperty {
    std::string name;
    enum class Kind : u8 { Enum, Bool, Int } kind = Kind::Enum;
    std::string text; // Enum
    i32 number = 0;   // Bool (0/1), Int
};
[[nodiscard]] std::span<const BedrockProperty> bedrock_properties(std::string_view identifier);

// The mob's maximum health in the behavior pack; 0 when unknown.
[[nodiscard]] i32 bedrock_max_health(std::string_view identifier);

} // namespace bridger::translate::detail
