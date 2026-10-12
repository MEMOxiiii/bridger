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
