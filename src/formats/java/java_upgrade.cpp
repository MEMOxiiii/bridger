// Java block state and biome upgrades, docs/formats/anvil.md ("Old worlds"). The rules were found
// by comparing the block lists of every release from 1.13 to 26.1 (PrismarineJS minecraft-data)
// with the pinned 26.3 list; each cites the version that made the change (minecraft.wiki).

#include "java_upgrade.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace bridger::formats::detail {
namespace {

constexpr i32 kDataVersion18w43a = 1902; // the old stone slab became smooth_stone_slab

const std::string* find_prop(const std::vector<BlockProperty>& props, std::string_view name) {
    for (const BlockProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

void erase_prop(std::vector<BlockProperty>& props, std::string_view name) {
    std::erase_if(props, [&](const BlockProperty& p) { return p.name == name; });
}

void set_prop(std::vector<BlockProperty>& props, std::string_view name, std::string value) {
    for (BlockProperty& p : props)
        if (p.name == name) {
            p.value = std::move(value);
            return;
        }
    props.push_back({std::string(name), std::move(value)});
}

struct OldBiome {
    i32 id;
    std::string_view old_name; // name in 1.17 (documentation only)
    std::string_view name;     // pinned-version name
    bool merged;
};

// Numeric ids of 1.17 (PrismarineJS minecraft-data data/pc/1.17/biomes.json, MIT); ids 0-39 and
// 127-167 are the same since Beta 1.8 / 1.7. Renamed by 1.18 (21w37a): minecraft.wiki "Java Edition
// 1.18". The variants 1.18 removed are merged into their base biome (Bridger's choice; reported).
constexpr std::array<OldBiome, 81> kOldBiomes{{
    {0, "ocean", "minecraft:ocean", false},
    {1, "plains", "minecraft:plains", false},
    {2, "desert", "minecraft:desert", false},
    {3, "mountains", "minecraft:windswept_hills", false},
    {4, "forest", "minecraft:forest", false},
    {5, "taiga", "minecraft:taiga", false},
    {6, "swamp", "minecraft:swamp", false},
    {7, "river", "minecraft:river", false},
    {8, "nether_wastes", "minecraft:nether_wastes", false},
    {9, "the_end", "minecraft:the_end", false},
    {10, "frozen_ocean", "minecraft:frozen_ocean", false},
    {11, "frozen_river", "minecraft:frozen_river", false},
    {12, "snowy_tundra", "minecraft:snowy_plains", false},
    {13, "snowy_mountains", "minecraft:snowy_plains", true},
    {14, "mushroom_fields", "minecraft:mushroom_fields", false},
    {15, "mushroom_field_shore", "minecraft:mushroom_fields", true},
    {16, "beach", "minecraft:beach", false},
    {17, "desert_hills", "minecraft:desert", true},
    {18, "wooded_hills", "minecraft:forest", true},
    {19, "taiga_hills", "minecraft:taiga", true},
    {20, "mountain_edge", "minecraft:windswept_hills", true},
    {21, "jungle", "minecraft:jungle", false},
    {22, "jungle_hills", "minecraft:jungle", true},
    {23, "jungle_edge", "minecraft:sparse_jungle", false},
    {24, "deep_ocean", "minecraft:deep_ocean", false},
    {25, "stone_shore", "minecraft:stony_shore", false},
    {26, "snowy_beach", "minecraft:snowy_beach", false},
    {27, "birch_forest", "minecraft:birch_forest", false},
    {28, "birch_forest_hills", "minecraft:birch_forest", true},
    {29, "dark_forest", "minecraft:dark_forest", false},
    {30, "snowy_taiga", "minecraft:snowy_taiga", false},
    {31, "snowy_taiga_hills", "minecraft:snowy_taiga", true},
    {32, "giant_tree_taiga", "minecraft:old_growth_pine_taiga", false},
    {33, "giant_tree_taiga_hills", "minecraft:old_growth_pine_taiga", true},
    {34, "wooded_mountains", "minecraft:windswept_forest", false},
    {35, "savanna", "minecraft:savanna", false},
    {36, "savanna_plateau", "minecraft:savanna_plateau", false},
    {37, "badlands", "minecraft:badlands", false},
    {38, "wooded_badlands_plateau", "minecraft:wooded_badlands", false},
    {39, "badlands_plateau", "minecraft:badlands", true},
    {40, "small_end_islands", "minecraft:small_end_islands", false},
    {41, "end_midlands", "minecraft:end_midlands", false},
    {42, "end_highlands", "minecraft:end_highlands", false},
    {43, "end_barrens", "minecraft:end_barrens", false},
    {44, "warm_ocean", "minecraft:warm_ocean", false},
    {45, "lukewarm_ocean", "minecraft:lukewarm_ocean", false},
    {46, "cold_ocean", "minecraft:cold_ocean", false},
    {47, "deep_warm_ocean", "minecraft:warm_ocean", true},
    {48, "deep_lukewarm_ocean", "minecraft:deep_lukewarm_ocean", false},
    {49, "deep_cold_ocean", "minecraft:deep_cold_ocean", false},
    {50, "deep_frozen_ocean", "minecraft:deep_frozen_ocean", false},
    {127, "the_void", "minecraft:the_void", false},
    {129, "sunflower_plains", "minecraft:sunflower_plains", false},
    {130, "desert_lakes", "minecraft:desert", true},
    {131, "gravelly_mountains", "minecraft:windswept_gravelly_hills", false},
    {132, "flower_forest", "minecraft:flower_forest", false},
    {133, "taiga_mountains", "minecraft:taiga", true},
    {134, "swamp_hills", "minecraft:swamp", true},
    {140, "ice_spikes", "minecraft:ice_spikes", false},
    {149, "modified_jungle", "minecraft:jungle", true},
    {151, "modified_jungle_edge", "minecraft:sparse_jungle", true},
    {155, "tall_birch_forest", "minecraft:old_growth_birch_forest", false},
    {156, "tall_birch_hills", "minecraft:old_growth_birch_forest", true},
    {157, "dark_forest_hills", "minecraft:dark_forest", true},
    {158, "snowy_taiga_mountains", "minecraft:snowy_taiga", true},
    {160, "giant_spruce_taiga", "minecraft:old_growth_spruce_taiga", false},
    {161, "giant_spruce_taiga_hills", "minecraft:old_growth_spruce_taiga", true},
    {162, "modified_gravelly_mountains", "minecraft:windswept_gravelly_hills", true},
    {163, "shattered_savanna", "minecraft:windswept_savanna", false},
    {164, "shattered_savanna_plateau", "minecraft:windswept_savanna", true},
    {165, "eroded_badlands", "minecraft:eroded_badlands", false},
    {166, "modified_wooded_badlands_plateau", "minecraft:wooded_badlands", true},
    {167, "modified_badlands_plateau", "minecraft:badlands", true},
    {168, "bamboo_jungle", "minecraft:bamboo_jungle", false},
    {169, "bamboo_jungle_hills", "minecraft:bamboo_jungle", true},
    {170, "soul_sand_valley", "minecraft:soul_sand_valley", false},
    {171, "crimson_forest", "minecraft:crimson_forest", false},
    {172, "warped_forest", "minecraft:warped_forest", false},
    {173, "basalt_deltas", "minecraft:basalt_deltas", false},
    {174, "dripstone_caves", "minecraft:dripstone_caves", false},
    {175, "lush_caves", "minecraft:lush_caves", false},
}};

} // namespace

std::optional<BlockState> upgrade_java_state(const BlockState& state, i32 data_version) {
    // Cheap test first: only these names can change.
    const std::string_view name = state.id();
    const bool candidate = name == "minecraft:grass" || name == "minecraft:grass_path" || name == "minecraft:sign" ||
                           name == "minecraft:wall_sign" || name == "minecraft:chain" || name == "minecraft:stone_slab" ||
                           name.ends_with("_wall") || name == "minecraft:cauldron" || name == "minecraft:jigsaw" ||
                           name == "minecraft:creaking_heart";
    if (!candidate) return std::nullopt;
    std::string id = state.id();
    std::vector<BlockProperty> props = state.properties();
    bool changed = false;

    // Renamed blocks: the old names no longer exist, so the rule is safe for any version.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kRenamed{{
        {"minecraft:grass", "minecraft:short_grass"},         // 1.20.3 (23w40a)
        {"minecraft:grass_path", "minecraft:dirt_path"},      // 1.17 (20w45a)
        {"minecraft:sign", "minecraft:oak_sign"},             // 1.14 (19w02a)
        {"minecraft:wall_sign", "minecraft:oak_wall_sign"},   // 1.14 (19w02a)
        {"minecraft:chain", "minecraft:iron_chain"},          // 1.21.9 (copper chains added)
    }};
    for (const auto& [from, to] : kRenamed)
        if (id == from) {
            id = std::string(to);
            changed = true;
        }
    // 1.14 (18w43a) renamed the old stone slab (smooth stone) and reused the name for a new block.
    if (id == "minecraft:stone_slab" && data_version > 0 && data_version < kDataVersion18w43a) {
        id = "minecraft:smooth_stone_slab";
        changed = true;
    }
    // 1.16 (20w06a): wall sides became none/low/tall.
    if (id.ends_with("_wall"))
        for (BlockProperty& p : props)
            if ((p.name == "north" || p.name == "east" || p.name == "south" || p.name == "west") && (p.value == "true" || p.value == "false")) {
                p.value = p.value == "true" ? "low" : "none";
                changed = true;
            }
    // 1.17 (20w45a): filled cauldrons became water_cauldron[level=1..3]; an empty one has no level.
    if (id == "minecraft:cauldron")
        if (const std::string* level = find_prop(props, "level")) {
            if (*level != "0") id = "minecraft:water_cauldron";
            else erase_prop(props, "level");
            changed = true;
        }
    // 1.16 (20w13a): the jigsaw's facing became orientation (front_top).
    if (id == "minecraft:jigsaw")
        if (const std::string* facing = find_prop(props, "facing")) {
            const std::string f = *facing;
            erase_prop(props, "facing");
            set_prop(props, "orientation", f == "up" ? "up_north" : f == "down" ? "down_north" : f + "_up");
            changed = true;
        }
    // 1.21.5 (25w02a): creaking_heart active -> creaking_heart_state; 1.21.2 snapshots had creaking.
    if (id == "minecraft:creaking_heart") {
        if (const std::string* active = find_prop(props, "active")) {
            const std::string v = *active == "true" ? "awake" : "uprooted";
            erase_prop(props, "active");
            set_prop(props, "creaking_heart_state", v);
            changed = true;
        } else if (const std::string* creaking = find_prop(props, "creaking")) {
            const std::string v = *creaking == "active" ? "awake" : *creaking == "dormant" ? "dormant" : "uprooted";
            erase_prop(props, "creaking");
            set_prop(props, "creaking_heart_state", v);
            changed = true;
        }
    }
    if (!changed) return std::nullopt;
    return BlockState(std::move(id), std::move(props));
}

std::optional<LegacyBiome> legacy_biome(i32 id) {
    const auto it = std::lower_bound(kOldBiomes.begin(), kOldBiomes.end(), id, [](const OldBiome& b, i32 v) { return b.id < v; });
    if (it == kOldBiomes.end() || it->id != id) return std::nullopt;
    return LegacyBiome{it->name, it->merged};
}

} // namespace bridger::formats::detail
