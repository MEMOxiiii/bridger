// Java -> Bedrock item stacks. Every Bedrock field and id below follows Chunker (HiveGamesOSS/Chunker, MIT,
// commit 50aaf6d): BedrockItemStackResolver (the "tag" layout), BedrockItemIdentifierResolver (data values of
// potions, tipped arrows, goat horns, ominous bottles, suspicious stews), BedrockEnchantmentIDResolver,
// BedrockPotionIDResolver, BedrockTrim*Resolver and ChunkerDyeColor; the Java side follows Chunker's
// JavaComponentItemStackResolver (1.20.5+ components) and JavaItemStackResolver (older "tag").
// docs/formats/bedrock-world.md, "Items".

#include "item_translator.hpp"

#include <bridger/io/nbt/mutf8.hpp>

#include "tables.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace bridger::translate::detail {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtEntry;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

// Containers inside items inside containers: a shulker box in a chest holds a bundle at most. Deeper nesting cannot
// be made in the game; it is reported instead of followed (no recursion over untrusted nesting, AGENTS.md).
constexpr int kMaxItemDepth = 3;

std::string_view strip_ns(std::string_view id) { return id.starts_with("minecraft:") ? id.substr(10) : id; }

// Item ids renamed since 1.13: the old names no longer exist in the pinned version (minecraft.wiki, "Java Edition
// data values"; the block renames match formats/java/java_upgrade.cpp).
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kItemRenames{{
    {"minecraft:grass", "minecraft:short_grass"},                           // 1.20.3
    {"minecraft:grass_path", "minecraft:dirt_path"},                        // 1.17
    {"minecraft:sign", "minecraft:oak_sign"},                               // 1.14
    {"minecraft:chain", "minecraft:iron_chain"},                            // 1.21.9
    {"minecraft:scute", "minecraft:turtle_scute"},                          // 1.20.5
    {"minecraft:zombie_pigman_spawn_egg", "minecraft:zombified_piglin_spawn_egg"}, // 1.16
    {"minecraft:cactus_green", "minecraft:green_dye"},                      // 1.14
}};

std::optional<ItemMapping> find_item(const MappingData& m, std::string_view id) {
    if (auto found = m.find_item(id)) return found;
    for (const auto& [from, to] : kItemRenames)
        if (id == from) return m.find_item(to);
    return std::nullopt;
}

// Java enchantment -> Bedrock numeric id (BedrockEnchantmentIDResolver / JavaEnchantmentResolver). Sweeping Edge
// has no Bedrock counterpart.
constexpr std::array<std::pair<std::string_view, i32>, 42> kEnchantments{{
    {"protection", 0},      {"fire_protection", 1},   {"feather_falling", 2},  {"blast_protection", 3},
    {"projectile_protection", 4}, {"thorns", 5},      {"respiration", 6},      {"depth_strider", 7},
    {"aqua_affinity", 8},   {"sharpness", 9},         {"smite", 10},           {"bane_of_arthropods", 11},
    {"knockback", 12},      {"fire_aspect", 13},      {"looting", 14},         {"efficiency", 15},
    {"silk_touch", 16},     {"unbreaking", 17},       {"fortune", 18},         {"power", 19},
    {"punch", 20},          {"flame", 21},            {"infinity", 22},        {"luck_of_the_sea", 23},
    {"lure", 24},           {"frost_walker", 25},     {"mending", 26},         {"binding_curse", 27},
    {"vanishing_curse", 28}, {"impaling", 29},        {"riptide", 30},         {"loyalty", 31},
    {"channeling", 32},     {"multishot", 33},        {"piercing", 34},        {"quick_charge", 35},
    {"soul_speed", 36},     {"swift_sneak", 37},      {"wind_burst", 38},      {"density", 39},
    {"breach", 40},         {"lunge", 41},
}};

// Java potion -> Bedrock potion id (BedrockPotionIDResolver / JavaPotionTypeResolver). Luck has no Bedrock potion.
constexpr std::array<std::pair<std::string_view, i32>, 46> kPotions{{
    {"water", 0},              {"mundane", 1},              {"thick", 3},                {"awkward", 4},
    {"night_vision", 5},       {"long_night_vision", 6},    {"invisibility", 7},         {"long_invisibility", 8},
    {"leaping", 9},            {"long_leaping", 10},        {"strong_leaping", 11},      {"fire_resistance", 12},
    {"long_fire_resistance", 13}, {"swiftness", 14},        {"long_swiftness", 15},      {"strong_swiftness", 16},
    {"slowness", 17},          {"long_slowness", 18},       {"water_breathing", 19},     {"long_water_breathing", 20},
    {"healing", 21},           {"strong_healing", 22},      {"harming", 23},             {"strong_harming", 24},
    {"poison", 25},            {"long_poison", 26},         {"strong_poison", 27},       {"regeneration", 28},
    {"long_regeneration", 29}, {"strong_regeneration", 30}, {"strength", 31},            {"long_strength", 32},
    {"strong_strength", 33},   {"weakness", 34},            {"long_weakness", 35},       {"turtle_master", 37},
    {"long_turtle_master", 38}, {"strong_turtle_master", 39}, {"slow_falling", 40},      {"long_slow_falling", 41},
    {"strong_slowness", 42},   {"wind_charged", 43},        {"weaving", 44},             {"oozing", 45},
    {"infested", 46},          {"long_mundane", 2},
}};

// Goat horn data values (BedrockItemIdentifierResolver).
constexpr std::array<std::string_view, 8> kHorns{"ponder_goat_horn", "sing_goat_horn", "seek_goat_horn", "feel_goat_horn",
                                                 "admire_goat_horn", "call_goat_horn",  "yearn_goat_horn", "dream_goat_horn"};

// Suspicious stew data values: effect and duration in Java ticks (BedrockItemIdentifierResolver, "R13").
struct Stew {
    std::string_view effect;
    i32 duration;
    i32 data;
};
constexpr std::array<Stew, 11> kStews{{{"night_vision", 100, 0}, {"jump_boost", 120, 1}, {"weakness", 180, 2},
                                       {"blindness", 160, 3}, {"poison", 240, 4}, {"saturation", 7, 5},
                                       {"saturation", 6, 6}, {"fire_resistance", 60, 7}, {"regeneration", 160, 8},
                                       {"wither", 160, 9}, {"night_vision", 80, 10}}};

// Trim materials and patterns Bedrock knows (BedrockTrimMaterialResolver / BedrockTrimPatternResolver).
constexpr std::array<std::string_view, 11> kTrimMaterials{"amethyst", "copper", "diamond", "emerald", "gold", "iron",
                                                          "lapis",    "netherite", "quartz", "redstone", "resin"};
constexpr std::array<std::string_view, 18> kTrimPatterns{"coast", "dune", "eye",  "rib",    "sentry",  "snout",
                                                         "spire", "tide", "vex",  "ward",   "wild",    "host",
                                                         "raiser", "shaper", "silence", "wayfinder", "bolt", "flow"};

// Firework colours: Bedrock stores dye indexes (reversed, 15 - index); Java stores RGB. The closest dye by either
// edition's firework colour wins (ChunkerDyeColor.findClosestDyeColor). {bedrock RGB, java RGB} in dye order.
constexpr std::array<std::pair<u32, u32>, 16> kDyeRgb{{
    {0xF0F0F0, 0xF0F0F0}, {0xF9801D, 0xEB8844}, {0xC74EBD, 0xC354CD}, {0x3AB3DA, 0x6689D3},
    {0xFED83D, 0xDECF2A}, {0x80C71F, 0x41CD34}, {0xF38BAA, 0xD88198}, {0x474F52, 0x434343},
    {0x9D9D97, 0xABABAB}, {0x169C9C, 0x287697}, {0x8932B8, 0x7B2FBE}, {0x3C44AA, 0x253192},
    {0x835432, 0x51301A}, {0x5E7C16, 0x3B511A}, {0xB02E26, 0xB3312C}, {0x1D1D21, 0x1E1B1B},
}};
constexpr std::array<std::string_view, 5> kFireworkShapes{"small_ball", "large_ball", "star", "creeper", "burst"};

u8 closest_dye_reversed(u32 rgb) {
    const auto dist = [rgb](u32 c) {
        const i32 dr = static_cast<i32>((c >> 16) & 0xFF) - static_cast<i32>((rgb >> 16) & 0xFF);
        const i32 dg = static_cast<i32>((c >> 8) & 0xFF) - static_cast<i32>((rgb >> 8) & 0xFF);
        const i32 db = static_cast<i32>(c & 0xFF) - static_cast<i32>(rgb & 0xFF);
        return dr * dr + dg * dg + db * db;
    };
    usize best = 0;
    i32 best_d = INT32_MAX;
    for (usize i = 0; i < kDyeRgb.size(); ++i)
        for (const u32 c : {kDyeRgb[i].second, kDyeRgb[i].first})
            if (const i32 d = dist(c); d < best_d) {
                best_d = d;
                best = i;
            }
    return static_cast<u8>(15 - best);
}

template <class Table>
auto lookup(const Table& table, std::string_view key) -> std::optional<decltype(table[0].second)> {
    for (const auto& [k, v] : table)
        if (k == key) return v;
    return std::nullopt;
}

bool contains(std::span<const std::string_view> list, std::string_view s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

std::string to_mutf8(const std::string& utf8) { return io::nbt::utf8_to_mutf8(utf8).value_or(utf8); }

// A "filterable" Java string: either the value itself or {raw: value, filtered: ...} (1.20.5+).
NbtView raw_of(NbtView v) { return v.is(TagType::Compound) ? v.entries().find("raw") : v; }

// A Java item resolved to its Bedrock id; the data value may still change (potions, horns, ...).
struct Resolved {
    ItemMapping mapping;
    std::string java_id;
    i32 count = 1;
};

std::optional<Resolved> resolve(NbtCompoundView c, const MappingData& m, ConversionReport& report, BlockPos pos) {
    const auto id = c.find("id").as_string();
    if (!id) return std::nullopt;
    Resolved r;
    r.java_id = std::string(*id);
    // 1.20.5+: "count" (Int); older: "Count" (Byte).
    if (const auto n = c.find("count").as_int()) r.count = *n;
    else if (const auto b = c.find("Count").as_byte()) r.count = *b;
    if (r.java_id == "minecraft:air" || r.count <= 0) return std::nullopt;
    auto mapping = find_item(m, r.java_id);
    if (!mapping) {
        report.add(IssueKind::UnmappedBlockEntity, "item " + r.java_id, pos, static_cast<u64>(std::max(r.count, 1)));
        return std::nullopt;
    }
    r.mapping = std::move(*mapping);
    return r;
}

// Everything a Bedrock item's "tag" can hold, gathered from either Java format before writing.
struct Ctx {
    const MappingData& m;
    ConversionReport& report;
    BlockPos pos;
    int depth;
    std::string_view java_id;
    i16 data_value;
    std::vector<std::string> lost; // names of Java data not converted, reported once per item
};

void write_enchantments(NbtWriter& w, std::string_view name, const std::vector<std::pair<std::string, i32>>& list, Ctx& x) {
    std::vector<std::pair<i16, i16>> out;
    for (const auto& [id, level] : list) {
        if (const auto bid = lookup(kEnchantments, strip_ns(id)))
            out.emplace_back(static_cast<i16>(*bid), static_cast<i16>(std::clamp(level, 0, 32767)));
        else
            x.lost.push_back("enchantment " + id);
    }
    if (out.empty()) return;
    w.begin_list(name, TagType::Compound, out.size());
    for (const auto& [id, level] : out) {
        w.begin_compound();
        w.write_short("id", id);
        w.write_short("lvl", level);
        w.end_compound();
    }
    w.end_list();
}

// Java enchantments: {levels: {id: level}} (1.20.5 - 1.21.4), {id: level} (1.21.5+), or [{id, lvl}] ("tag").
std::vector<std::pair<std::string, i32>> read_enchantments(NbtView v) {
    std::vector<std::pair<std::string, i32>> out;
    if (v.is(TagType::List)) {
        for (const NbtView e : v.items()) {
            const NbtCompoundView c = e.entries();
            const auto id = c.find("id").as_string();
            if (!id) continue;
            const NbtView lvl = c.find("lvl");
            out.emplace_back(std::string(*id), lvl.as_short().value_or(static_cast<i16>(lvl.as_int().value_or(1))));
        }
        return out;
    }
    NbtView levels = v.entries().find("levels");
    if (!levels) levels = v;
    for (const NbtEntry& e : levels.entries())
        if (const auto n = e.value.as_int()) out.emplace_back(std::string(e.name), *n);
    return out;
}

struct Explosion {
    u8 shape = 0;
    std::vector<u8> colors, fades;
    bool trail = false, twinkle = false;
};

// Java explosion: {shape: "small_ball", colors: [I;..], fade_colors, has_trail, has_twinkle} (components) or
// {Type: byte, Colors, FadeColors, Trail, Flicker} ("tag").
Explosion read_explosion(NbtCompoundView c) {
    Explosion e;
    if (const auto shape = c.find("shape").as_string()) {
        for (usize i = 0; i < kFireworkShapes.size(); ++i)
            if (kFireworkShapes[i] == *shape) e.shape = static_cast<u8>(i);
    } else {
        e.shape = static_cast<u8>(std::clamp<i32>(c.find("Type").as_byte().value_or(0), 0, 4));
    }
    const auto colors = [&](std::string_view a, std::string_view b, std::vector<u8>& out) {
        NbtView v = c.find(a);
        if (!v) v = c.find(b);
        if (const auto arr = v.as_int_array())
            for (usize i = 0; i < arr->size(); ++i) out.push_back(closest_dye_reversed(static_cast<u32>((*arr)[i])));
    };
    colors("colors", "Colors", e.colors);
    colors("fade_colors", "FadeColors", e.fades);
    e.trail = c.find("has_trail").as_byte().value_or(c.find("Trail").as_byte().value_or(0)) != 0;
    e.twinkle = c.find("has_twinkle").as_byte().value_or(c.find("Flicker").as_byte().value_or(0)) != 0;
    return e;
}

void write_explosion(NbtWriter& w, std::string_view name, const Explosion& e) {
    w.begin_compound(name);
    w.write_byte("FireworkType", static_cast<i8>(e.shape));
    // Bedrock does not accept an empty colour list: black (ChunkerDyeColor.BLACK reversed = 0), as Chunker writes.
    const std::vector<u8> colors = e.colors.empty() ? std::vector<u8>{0} : e.colors;
    w.write_byte_array("FireworkColor", colors);
    w.write_byte_array("FireworkFade", e.fades);
    w.write_byte("FireworkTrail", e.trail ? 1 : 0);
    w.write_byte("FireworkFlicker", e.twinkle ? 1 : 0);
    w.end_compound();
}

void write_fireworks(NbtWriter& w, NbtCompoundView c) {
    NbtView list = c.find("explosions");
    if (!list) list = c.find("Explosions");
    std::vector<Explosion> explosions;
    for (const NbtView e : list.items()) explosions.push_back(read_explosion(e.entries()));
    i8 flight = c.find("flight_duration").as_byte().value_or(c.find("Flight").as_byte().value_or(1));
    w.begin_compound("Fireworks");
    w.write_byte("Flight", flight);
    w.begin_list("Explosions", explosions.empty() ? TagType::End : TagType::Compound, explosions.size());
    for (const Explosion& e : explosions) write_explosion(w, {}, e);
    w.end_list();
    w.end_compound();
}

// Book pages: [{raw: text}] / [text] (components) or [text] ("tag"). Written books hold text components, writable
// books plain text.
void write_pages(NbtWriter& w, NbtView pages, bool written, Ctx& x) {
    std::vector<std::string> out;
    for (const NbtView p : pages.items()) {
        const NbtView raw = raw_of(p);
        bool lossy = false;
        std::string text = written ? text_of(raw, lossy) : std::string(raw.as_string().value_or(""));
        if (!written) text = io::nbt::mutf8_to_utf8(text).value_or(text);
        if (lossy) x.lost.push_back("book text formatting");
        out.push_back(std::move(text));
    }
    w.begin_list("pages", out.empty() ? TagType::End : TagType::Compound, out.size());
    for (const std::string& t : out) {
        w.begin_compound();
        w.write_string("photoname", "");
        w.write_string("text", to_mutf8(t));
        w.end_compound();
    }
    w.end_list();
}

void write_patterns(NbtWriter& w, NbtView patterns, Ctx& x) {
    struct Layer {
        std::string code;
        i32 color;
    };
    std::vector<Layer> layers;
    for (const NbtView p : patterns.items()) {
        const NbtCompoundView c = p.entries();
        // Components: {pattern: "minecraft:stripe_top", color: "red"}; "tag": {Pattern: "ts", Color: dye index}.
        std::optional<std::string> code;
        std::optional<i32> dye;
        if (const auto pid = c.find("pattern").as_string()) {
            for (const tables::Pair& t : tables::kBannerPatterns)
                if (t.java == strip_ns(*pid)) code = std::string(t.bedrock);
            if (const auto color = c.find("color").as_string()) dye = tables::dye_index(*color);
        } else if (const auto legacy = c.find("Pattern").as_string()) {
            code = std::string(*legacy);
            dye = c.find("Color").as_int();
        }
        if (!code || !dye || *dye < 0 || *dye > 15) {
            x.lost.push_back("banner pattern");
            continue;
        }
        layers.push_back({std::move(*code), 15 - *dye});
    }
    if (layers.empty()) return;
    w.begin_list("Patterns", TagType::Compound, layers.size());
    for (const Layer& l : layers) {
        w.begin_compound();
        w.write_int("Color", l.color);
        w.write_string("Pattern", l.code);
        w.end_compound();
    }
    w.end_list();
}

void write_trim(NbtWriter& w, NbtCompoundView c, Ctx& x) {
    const auto material = c.find("material").as_string();
    const auto pattern = c.find("pattern").as_string();
    if (!material || !pattern || !contains(kTrimMaterials, strip_ns(*material)) || !contains(kTrimPatterns, strip_ns(*pattern))) {
        x.lost.push_back("armor trim");
        return;
    }
    w.begin_compound("Trim");
    w.write_string("Material", strip_ns(*material));
    w.write_string("Pattern", strip_ns(*pattern));
    w.end_compound();
}

void write_display(NbtWriter& w, NbtView name, NbtView lore, Ctx& x) {
    std::optional<std::string> text;
    if (name) {
        bool lossy = false;
        text = text_of(name, lossy);
        if (lossy) x.lost.push_back("name formatting");
    }
    std::vector<std::string> lines;
    for (const NbtView l : lore.items()) {
        bool lossy = false;
        lines.push_back(text_of(l, lossy));
        if (lossy) x.lost.push_back("lore formatting");
    }
    if (!text && lines.empty()) return;
    w.begin_compound("display");
    if (text) w.write_string("Name", to_mutf8(*text));
    if (!lines.empty()) {
        w.begin_list("Lore", TagType::String, lines.size());
        for (const std::string& l : lines) w.write_string({}, to_mutf8(l));
        w.end_list();
    }
    w.end_compound();
}

i16 potion_data(NbtView potion, Ctx& x) {
    // components: potion_contents {potion: id} (or the id alone); "tag": Potion: id.
    std::optional<std::string_view> id = potion.as_string();
    if (!id && potion.is(TagType::Compound)) {
        const NbtCompoundView c = potion.entries();
        id = c.find("potion").as_string();
        if (c.find("custom_effects")) x.lost.push_back("custom potion effects");
    }
    if (!id) return 0;
    if (const auto d = lookup(kPotions, strip_ns(*id))) return static_cast<i16>(*d);
    x.lost.push_back("potion " + std::string(*id));
    return 0;
}

i16 stew_data(NbtView effects, Ctx& x) {
    // [{id: "minecraft:night_vision", duration: 100}] (1.20.2+ and components); older {EffectId: byte} is not kept.
    for (const NbtView e : effects.items()) {
        const NbtCompoundView c = e.entries();
        const auto id = c.find("id").as_string();
        if (!id) break;
        const i32 duration = c.find("duration").as_int().value_or(160);
        std::optional<i16> any;
        for (const Stew& s : kStews)
            if (s.effect == strip_ns(*id)) {
                if (s.duration == duration) return static_cast<i16>(s.data);
                if (!any) any = static_cast<i16>(s.data);
            }
        if (any) return *any;
    }
    if (!effects.items().empty()) x.lost.push_back("suspicious stew effect");
    return 0;
}

void write_item_body(NbtWriter& w, const Resolved& r, NbtCompoundView c, Ctx& x);

// Container contents inside an item (shulker boxes, chests with data): Bedrock keeps them as "Items" in "tag".
void write_contents(NbtWriter& w, std::string_view name, NbtView list, bool component, Ctx& x, SlotMap slot_map = nullptr) {
    if (x.depth >= kMaxItemDepth) {
        if (!list.items().empty()) x.lost.push_back("items nested too deep");
        return;
    }
    std::vector<std::pair<i8, Resolved>> items;
    std::vector<NbtCompoundView> sources;
    for (const NbtView e : list.items()) {
        const NbtCompoundView entry = e.entries();
        // component: {slot: int, item: {...}}; "tag" / bundles: the item itself with "Slot".
        const NbtCompoundView item = component ? entry.find("item").entries() : entry;
        const i8 slot = component ? static_cast<i8>(entry.find("slot").as_int().value_or(static_cast<i32>(items.size())))
                                  : entry.find("Slot").as_byte().value_or(static_cast<i8>(items.size()));
        if (auto res = resolve(item, x.m, x.report, x.pos)) {
            items.emplace_back(slot, std::move(*res));
            sources.push_back(item);
        }
    }
    w.begin_list(name, items.empty() ? TagType::End : TagType::Compound, items.size());
    for (usize i = 0; i < items.size(); ++i) {
        w.begin_compound();
        w.write_byte("Slot", slot_map ? slot_map(items[i].first) : items[i].first);
        Ctx inner{x.m, x.report, x.pos, x.depth + 1, items[i].second.java_id, items[i].second.mapping.damage, {}};
        write_item_body(w, items[i].second, sources[i], inner);
        w.end_compound();
    }
    w.end_list();
}

// Writes the Bedrock "tag" of one item from its Java components or "tag" into separate bytes, so that an item
// without data gets no empty "tag". Sets x.data_value for items whose data value carries the data.
std::optional<std::vector<u8>> build_tag(NbtCompoundView c, Ctx& x) {
    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    usize written = 0;
    const auto mark = [&written] { ++written; };
    const std::string_view id = x.java_id;
    const bool potion_item = id == "minecraft:potion" || id == "minecraft:splash_potion" || id == "minecraft:lingering_potion";

    if (const NbtView comps = c.find("components"); comps.is(TagType::Compound)) {
        const NbtCompoundView k = comps.entries();
        NbtView name, lore;
        for (const NbtEntry& e : k) {
            const std::string_view n = e.name;
            if (n == "minecraft:damage") {
                w.write_int("Damage", e.value.as_int().value_or(0));
                mark();
            } else if (n == "minecraft:repair_cost") {
                w.write_int("RepairCost", e.value.as_int().value_or(0));
                mark();
            } else if (n == "minecraft:enchantments" || n == "minecraft:stored_enchantments") {
                write_enchantments(w, "ench", read_enchantments(e.value), x);
                mark();
            } else if (n == "minecraft:custom_name") {
                name = e.value;
            } else if (n == "minecraft:lore") {
                lore = e.value;
            } else if (n == "minecraft:dyed_color") {
                // An int (1.21.5+) or {rgb: int, show_in_tooltip} (1.20.5 - 1.21.4).
                const auto rgb = e.value.is(TagType::Compound) ? e.value.entries().find("rgb").as_int() : e.value.as_int();
                if (rgb) {
                    w.write_int("customColor", static_cast<i32>(0xFF000000u | static_cast<u32>(*rgb)));
                    mark();
                }
            } else if (n == "minecraft:potion_contents" && (potion_item || id == "minecraft:tipped_arrow")) {
                const i16 p = potion_data(e.value, x);
                x.data_value = id == "minecraft:tipped_arrow" ? static_cast<i16>(p >= 1 ? p + 1 : 1) : p;
            } else if (n == "minecraft:written_book_content" || n == "minecraft:writable_book_content") {
                const NbtCompoundView b = e.value.entries();
                write_pages(w, b.find("pages"), n == "minecraft:written_book_content", x);
                if (const auto title = raw_of(b.find("title")).as_string()) w.write_string("title", *title);
                if (const auto author = b.find("author").as_string()) w.write_string("author", *author);
                if (b.find("generation").as_int().value_or(0) != 0) x.lost.push_back("book generation");
                mark();
            } else if (n == "minecraft:container") {
                write_contents(w, "Items", e.value, true, x);
                mark();
            } else if (n == "minecraft:bundle_contents") {
                write_contents(w, "storage_item_component_content", e.value, false, x);
                mark();
            } else if (n == "minecraft:fireworks") {
                write_fireworks(w, e.value.entries());
                mark();
            } else if (n == "minecraft:firework_explosion") {
                write_explosion(w, "FireworksItem", read_explosion(e.value.entries()));
                mark();
            } else if (n == "minecraft:trim") {
                write_trim(w, e.value.entries(), x);
                mark();
            } else if (n == "minecraft:instrument") {
                const auto horn = e.value.as_string();
                usize i = 0;
                while (horn && i < kHorns.size() && kHorns[i] != strip_ns(*horn)) ++i;
                if (horn && i < kHorns.size()) x.data_value = static_cast<i16>(i);
                else x.lost.push_back("goat horn sound");
            } else if (n == "minecraft:ominous_bottle_amplifier") {
                x.data_value = static_cast<i16>(std::clamp(e.value.as_int().value_or(0), 0, 4));
            } else if (n == "minecraft:suspicious_stew_effects") {
                x.data_value = stew_data(e.value, x);
            } else if (n == "minecraft:banner_patterns") {
                write_patterns(w, e.value, x);
                mark();
            } else {
                x.lost.push_back(std::string(n));
            }
        }
        if (name || lore) {
            write_display(w, name, lore, x);
            mark();
        }
    } else if (const NbtView tag = c.find("tag"); tag.is(TagType::Compound)) {
        // Before 1.20.5.
        const NbtCompoundView t = tag.entries();
        for (const NbtEntry& e : t) {
            const std::string_view n = e.name;
            if (n == "Damage") {
                w.write_int("Damage", e.value.as_int().value_or(0));
                mark();
            } else if (n == "RepairCost") {
                w.write_int("RepairCost", e.value.as_int().value_or(0));
                mark();
            } else if (n == "Enchantments" || n == "StoredEnchantments") {
                write_enchantments(w, "ench", read_enchantments(e.value), x);
                mark();
            } else if (n == "display") {
                const NbtCompoundView d = e.value.entries();
                write_display(w, d.find("Name"), d.find("Lore"), x);
                if (const auto rgb = d.find("color").as_int())
                    w.write_int("customColor", static_cast<i32>(0xFF000000u | static_cast<u32>(*rgb)));
                mark();
            } else if (n == "Potion" && (potion_item || id == "minecraft:tipped_arrow")) {
                const i16 p = potion_data(e.value, x);
                x.data_value = id == "minecraft:tipped_arrow" ? static_cast<i16>(p >= 1 ? p + 1 : 1) : p;
            } else if (n == "pages") {
                write_pages(w, e.value, id == "minecraft:written_book", x);
                mark();
            } else if (n == "title" || n == "author") {
                if (const auto s = e.value.as_string()) w.write_string(n, *s);
                mark();
            } else if (n == "BlockEntityTag" && e.value.entries().find("Items")) {
                write_contents(w, "Items", e.value.entries().find("Items"), false, x);
                for (const NbtEntry& b : e.value.entries())
                    if (b.name == "Patterns") write_patterns(w, b.value, x);
                    else if (b.name != "Items" && b.name != "id") x.lost.push_back("BlockEntityTag." + std::string(b.name));
                mark();
            } else if (n == "BlockEntityTag" && e.value.entries().find("Patterns")) {
                write_patterns(w, e.value.entries().find("Patterns"), x);
                mark();
            } else if (n == "Fireworks") {
                write_fireworks(w, e.value.entries());
                mark();
            } else if (n == "Explosion") {
                write_explosion(w, "FireworksItem", read_explosion(e.value.entries()));
                mark();
            } else if (n == "Trim") {
                write_trim(w, e.value.entries(), x);
                mark();
            } else if (n == "instrument") {
                const auto horn = e.value.as_string();
                usize i = 0;
                while (horn && i < kHorns.size() && kHorns[i] != strip_ns(*horn)) ++i;
                if (horn && i < kHorns.size()) x.data_value = static_cast<i16>(i);
                else x.lost.push_back("goat horn sound");
            } else if (n == "effects") {
                x.data_value = stew_data(e.value, x);
            } else if (n == "resolved" || n == "generation") {
                if (n == "generation" && e.value.as_int().value_or(0) != 0) x.lost.push_back("book generation");
            } else {
                x.lost.push_back(std::string(n));
            }
        }
    }
    w.end_compound();
    if (written == 0) return std::nullopt;
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    // An empty tag (e.g. only unknown enchantments) is left out.
    const auto doc = io::nbt::NbtDocument::parse(*bytes, Endian::Little);
    if (!doc || doc->root().entries().size() == 0) return std::nullopt;
    return std::move(*bytes);
}

void write_item_body(NbtWriter& w, const Resolved& r, NbtCompoundView c, Ctx& x) {
    const auto tag = build_tag(c, x);
    i32 count = r.count;
    if (count > 127) {
        x.report.add(IssueKind::LossyBlock, "item count clamped: " + r.java_id, x.pos);
        count = 127;
    }
    w.write_string("Name", r.mapping.bedrock_name);
    w.write_byte("Count", static_cast<i8>(count));
    w.write_short("Damage", x.data_value);
    w.write_byte("WasPickedUp", 0);
    if (r.mapping.java_block_state) {
        const JavaStateMapping jm = x.m.map(*r.mapping.java_block_state);
        if (jm.bedrock != UINT32_MAX) write_block_tag(w, "Block", x.m, jm.bedrock);
    }
    if (tag)
        if (const auto doc = io::nbt::NbtDocument::parse(*tag, Endian::Little)) w.write_view("tag", doc->root()); // a view of *tag
    std::sort(x.lost.begin(), x.lost.end());
    x.lost.erase(std::unique(x.lost.begin(), x.lost.end()), x.lost.end());
    for (const std::string& what : x.lost) x.report.add(IssueKind::LossyBlock, "item data not converted: " + what, x.pos);
}

std::string json_text(std::string_view json, bool& lossy) {
    // Minimal scan for "text":"..." values in order; enough for names, lore, pages and sign lines.
    std::string out;
    bool any = false;
    usize i = 0;
    while ((i = json.find("\"text\"", i)) != std::string_view::npos) {
        i = json.find(':', i + 6);
        if (i == std::string_view::npos) break;
        i = json.find('"', i + 1);
        if (i == std::string_view::npos) break;
        ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                const char e = json[i + 1];
                out += e == 'n' ? '\n' : e;
                i += 2;
            } else {
                out += json[i++];
            }
        }
        any = true;
    }
    if (!any) {
        // A bare JSON string: "abc"
        if (json.size() >= 2 && json.front() == '"' && json.back() == '"') return std::string(json.substr(1, json.size() - 2));
        if (!json.empty()) lossy = true; // e.g. a translatable component
        return {};
    }
    for (std::string_view key : {"\"color\"", "\"bold\"", "\"italic\"", "\"translate\"", "\"underlined\"", "\"strikethrough\"",
                                 "\"obfuscated\"", "\"clickEvent\"", "\"hoverEvent\""})
        if (json.find(key) != std::string_view::npos) lossy = true;
    return out;
}

} // namespace

std::string text_of(NbtView v, bool& lossy) {
    if (const auto s = v.as_string()) {
        auto utf8 = io::nbt::mutf8_to_utf8(*s);
        const std::string text = utf8 ? *utf8 : std::string(*s);
        if (!text.empty() && (text.front() == '{' || text.front() == '[' || text.front() == '"')) return json_text(text, lossy);
        return text;
    }
    if (v.is(TagType::Compound)) {
        std::string out;
        for (const NbtEntry& e : v.entries()) {
            if (e.name == "text") out += text_of(e.value, lossy);
            else if (e.name == "extra")
                for (const NbtView x : e.value.items()) out += text_of(x, lossy);
            else lossy = true;
        }
        return out;
    }
    if (v.is(TagType::List)) {
        std::string out;
        for (const NbtView x : v.items()) out += text_of(x, lossy);
        return out;
    }
    lossy = true;
    return {};
}

void write_block_tag(NbtWriter& w, std::string_view name, const MappingData& m, BedrockStateIndex idx) {
    const BedrockState& s = m.bedrock(idx);
    w.begin_compound(name);
    w.write_string("name", s.name);
    w.begin_compound("states");
    for (const BedrockProperty& p : s.properties) {
        switch (p.type) {
        case NbtValueType::Byte: w.write_byte(p.key, static_cast<i8>(p.int_value)); break;
        case NbtValueType::Int: w.write_int(p.key, p.int_value); break;
        case NbtValueType::String: w.write_string(p.key, p.str_value); break;
        }
    }
    w.end_compound();
    w.write_int("version", static_cast<i32>(m.bedrock_block_version()));
    w.end_compound();
}

void write_items(NbtWriter& w, std::string_view name, NbtView java_items, const MappingData& m, ConversionReport& report, BlockPos pos,
                 SlotMap slot_map) {
    Ctx x{m, report, pos, 0, {}, 0, {}};
    write_contents(w, name, java_items, false, x, slot_map);
}

bool write_item(NbtWriter& w, std::string_view name, NbtCompoundView java_item, const MappingData& m, ConversionReport& report,
                BlockPos pos) {
    const auto r = resolve(java_item, m, report, pos);
    if (!r) return false;
    w.begin_compound(name);
    Ctx x{m, report, pos, 0, r->java_id, r->mapping.damage, {}};
    write_item_body(w, *r, java_item, x);
    w.end_compound();
    return true;
}

} // namespace bridger::translate::detail
