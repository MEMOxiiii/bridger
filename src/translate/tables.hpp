#pragma once

// Java <-> Bedrock block-entity tables shared by both translation directions (internal).
// Sources are cited where each table is used (block_entity_translator.cpp) and in
// docs/formats/bedrock-world.md, "Block entities".

#include <bridger/core/types.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace bridger::translate::tables {

// Dye colors in Bedrock's numeric order (bed "color" byte; banner colors are 15 - index).
inline constexpr std::array<std::string_view, 16> kDyeColors{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

inline std::optional<i32> dye_index(std::string_view color) {
    for (usize i = 0; i < kDyeColors.size(); ++i)
        if (kDyeColors[i] == color) return static_cast<i32>(i);
    return std::nullopt;
}

struct Pair {
    std::string_view java;
    std::string_view bedrock;
};

// Java banner pattern id (without namespace) <-> Bedrock short code (Java ids:
// data/minecraft/banner_pattern in the 26.3 server jar; codes and pairing: Chunker
// JavaBannerPatternResolver + BedrockBannerPatternResolver).
inline constexpr std::array<Pair, 43> kBannerPatterns{{
    {"base", "b"}, {"square_bottom_left", "bl"}, {"square_bottom_right", "br"}, {"square_top_left", "tl"},
    {"square_top_right", "tr"}, {"stripe_bottom", "bs"}, {"stripe_top", "ts"}, {"stripe_left", "ls"},
    {"stripe_right", "rs"}, {"stripe_center", "cs"}, {"stripe_middle", "ms"}, {"stripe_downright", "drs"},
    {"stripe_downleft", "dls"}, {"small_stripes", "ss"}, {"cross", "cr"}, {"straight_cross", "sc"},
    {"triangle_bottom", "bt"}, {"triangle_top", "tt"}, {"triangles_bottom", "bts"}, {"triangles_top", "tts"},
    {"diagonal_left", "ld"}, {"diagonal_up_right", "rd"}, {"diagonal_up_left", "lud"}, {"diagonal_right", "rud"},
    {"circle", "mc"}, {"rhombus", "mr"}, {"half_vertical", "vh"}, {"half_horizontal", "hh"},
    {"half_vertical_right", "vhr"}, {"half_horizontal_bottom", "hhb"}, {"border", "bo"}, {"curly_border", "cbo"},
    {"gradient", "gra"}, {"gradient_up", "gru"}, {"bricks", "bri"}, {"globe", "glb"}, {"creeper", "cre"},
    {"skull", "sku"}, {"flower", "flo"}, {"mojang", "moj"}, {"piglin", "pig"}, {"flow", "flw"}, {"guster", "gus"}}};

// Containers whose slot layout is the same in both editions: Java block entity id <-> Bedrock id
// (Bedrock ids as used by Chunker's block entity handlers). The brewing stand is not listed: its
// slot order differs. Java trapped chests are also stored as Bedrock "Chest".
inline constexpr std::array<Pair, 9> kContainers{{{"minecraft:barrel", "Barrel"},
                                                  {"minecraft:furnace", "Furnace"},
                                                  {"minecraft:blast_furnace", "BlastFurnace"},
                                                  {"minecraft:smoker", "Smoker"},
                                                  {"minecraft:hopper", "Hopper"},
                                                  {"minecraft:dispenser", "Dispenser"},
                                                  {"minecraft:dropper", "Dropper"},
                                                  {"minecraft:shulker_box", "ShulkerBox"},
                                                  {"minecraft:chest", "Chest"}}};

// Block entities written with their id only (their data is transient or rebuilt by the game).
inline constexpr std::array<Pair, 10> kMarkers{{{"minecraft:campfire", "Campfire"},
                                               {"minecraft:bell", "Bell"},
                                               {"minecraft:comparator", "Comparator"},
                                               {"minecraft:lectern", "Lectern"},
                                               {"minecraft:decorated_pot", "DecoratedPot"},
                                               {"minecraft:daylight_detector", "DaylightDetector"},
                                               {"minecraft:jukebox", "Jukebox"},
                                               {"minecraft:beacon", "Beacon"},
                                               {"minecraft:conduit", "Conduit"},
                                               {"minecraft:enchanting_table", "EnchantTable"}}};

// Loot tables Bedrock still names the old way (Chunker BedrockRandomizableContainerBlockEntityHandler): {Java, Bedrock}.
inline constexpr std::array<Pair, 5> kLootTableRenames{{
    {"minecraft:chests/buried_treasure", "minecraft:chests/buriedtreasure"},
    {"minecraft:chests/jungle_temple_dispenser", "minecraft:chests/dispenser_trap"},
    {"minecraft:chests/shipwreck_map", "minecraft:chests/shipwreck"},
    {"minecraft:chests/shipwreck_supply", "minecraft:chests/shipwrecksupply"},
    {"minecraft:chests/shipwreck_treasure", "minecraft:chests/shipwrecktreasure"},
}};

// Entities named differently in the two editions (Chunker JavaEntityTypeResolver / BedrockEntityTypeResolver):
// {Java, Bedrock}. Everything else has the same id.
inline constexpr std::array<Pair, 17> kEntityRenames{{
    {"minecraft:breeze_wind_charge", "minecraft:breeze_wind_charge_projectile"},
    {"minecraft:end_crystal", "minecraft:ender_crystal"},
    {"minecraft:evoker", "minecraft:evocation_illager"},
    {"minecraft:evoker_fangs", "minecraft:evocation_fang"},
    {"minecraft:experience_bottle", "minecraft:xp_bottle"},
    {"minecraft:experience_orb", "minecraft:xp_orb"},
    {"minecraft:eye_of_ender", "minecraft:eye_of_ender_signal"},
    {"minecraft:firework_rocket", "minecraft:fireworks_rocket"},
    {"minecraft:fishing_bobber", "minecraft:fishing_hook"},
    {"minecraft:oak_boat", "minecraft:boat"},
    {"minecraft:oak_chest_boat", "minecraft:chest_boat"},
    {"minecraft:trident", "minecraft:thrown_trident"},
    {"minecraft:tropical_fish", "minecraft:tropicalfish"},
    {"minecraft:villager", "minecraft:villager_v2"},
    {"minecraft:wind_charge", "minecraft:wind_charge_projectile"},
    {"minecraft:zombie_villager", "minecraft:zombie_villager_v2"},
    {"minecraft:zombified_piglin", "minecraft:zombie_pigman"},
}};

// Beacon effects by Bedrock id (Chunker BedrockEffectIDResolver); Java names them (1.20.2+) or uses the same numbers.
inline constexpr std::array<std::string_view, 12> kBeaconEffects{"",         "speed",          "slowness",       "haste",
                                                                "mining_fatigue", "strength", "instant_health", "instant_damage",
                                                                "jump_boost",     "nausea",   "regeneration",   "resistance"};

// Block entities written with their id only in both directions (Chunker EmptyBlockEntityHandler): {Java, Bedrock}.
inline constexpr std::array<Pair, 9> kEmptyBlockEntities{{
    {"minecraft:sculk_catalyst", "SculkCatalyst"},
    {"minecraft:sculk_shrieker", "SculkShrieker"},
    {"minecraft:calibrated_sculk_sensor", "CalibratedSculkSensor"},
    {"minecraft:trial_spawner", "TrialSpawner"},
    {"minecraft:vault", "Vault"},
    {"minecraft:creaking_heart", "CreakingHeart"},
    {"minecraft:end_portal", "EndPortal"},
    {"minecraft:beehive", "Beehive"},
    {"minecraft:conduit", "Conduit"},
}};

// Bedrock SkullType by Java block (Chunker BedrockSkullBlockEntityHandler.SKULL_TO_ID).
struct Skull {
    std::string_view floor, wall;
    i8 type;
};
inline constexpr std::array<Skull, 7> kSkulls{{{"minecraft:skeleton_skull", "minecraft:skeleton_wall_skull", 0},
                                              {"minecraft:wither_skeleton_skull", "minecraft:wither_skeleton_wall_skull", 1},
                                              {"minecraft:zombie_head", "minecraft:zombie_wall_head", 2},
                                              {"minecraft:player_head", "minecraft:player_wall_head", 3},
                                              {"minecraft:creeper_head", "minecraft:creeper_wall_head", 4},
                                              {"minecraft:dragon_head", "minecraft:dragon_wall_head", 5},
                                              {"minecraft:piglin_head", "minecraft:piglin_wall_head", 6}}};

// Bedrock pairing of a Java double chest (GeyserMC DoubleChestBlockEntityTranslator): the
// partner of a "left"/"right" chest facing `facing` is beside it.
inline std::optional<BlockPos> chest_partner(std::string_view type, std::string_view facing, BlockPos pos) {
    if (type != "left" && type != "right") return std::nullopt;
    const bool left = type == "left";
    BlockPos p = pos;
    if (facing == "east") p.z += left ? 1 : -1;
    else if (facing == "west") p.z += left ? -1 : 1;
    else if (facing == "south") p.x += left ? -1 : 1;
    else if (facing == "north") p.x += left ? 1 : -1;
    else return std::nullopt;
    return p;
}

} // namespace bridger::translate::tables
