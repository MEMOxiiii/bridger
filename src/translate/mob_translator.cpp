// Mobs and simple objects between the editions. Sources (docs/formats/bedrock-world.md, "Mobs"):
//  - entity types and identifiers: Chunker JavaEntityTypeResolver / BedrockEntityTypeResolver (entity_types.inc);
//  - Bedrock actor tags (identifier, Pos, Rotation, Motion, CustomName, Persistent, IsBaby, Variant, MarkVariant, Color,
//    Color2, Size, Sheared, Chested, Saddled, IsTamed, OwnerNew, Sitting, Attributes, Armor, Mainhand, Offhand, Item,
//    Age, Health, Pose): minecraft.wiki "Bedrock Edition level format/Entity format", checked against actors in worlds
//    the game saved (26.x): five Armor slots, health as {Base, Current, DefaultMax, DefaultMin, Max, Min, Name};
//  - Bedrock variant numbers and the "definitions"/"properties" of each mob: Mojang/bedrock-samples v1.26.50.4
//    behavior_pack/entities (component groups such as sheep_gray {color 7}, base_creamy {variant 1}, mason {variant 13});
//  - Java tags: minecraft.wiki "Entity format" (1.21.5+: equipment, fall_distance, CustomName as a text component) and
//    the 26.3 server's registries (cat/wolf/frog/pig/cow/chicken variants, villager professions and types).
// Only for EntityMode::All. Every piece of data that is not carried is reported as a lossy entity.

#include "mob_translator.hpp"

#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/nbt/mutf8.hpp>

#include "bedrock_entities.hpp"
#include "entity_types.hpp"
#include "item_translator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace bridger::translate::detail {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

constexpr GameVersion3 kNewest{99, 0, 0}; // the pinned versions; older targets are handled by targets.cpp

std::string_view bare(std::string_view id) { return id.starts_with("minecraft:") ? id.substr(10) : id; }

template <usize N>
std::optional<i32> index_of(const std::array<std::string_view, N>& names, std::string_view name) {
    for (usize i = 0; i < N; ++i)
        if (names[i] == bare(name)) return static_cast<i32>(i);
    return std::nullopt;
}

// Bedrock Variant numbers by Java name (behavior pack component groups: cat_white 0 ... cat_jellie 10; Java "black"
// is the tuxedo cat, "all_black" the black one).
constexpr std::array<std::string_view, 11> kCats{"white", "black", "red", "siamese", "british_shorthair", "calico",
                                                 "persian", "ragdoll", "tabby", "all_black", "jellie"};
constexpr std::array<std::string_view, 9> kWolves{"pale", "ashen", "black", "chestnut", "rusty", "snowy", "spotted", "striped", "woods"};
constexpr std::array<std::string_view, 3> kFrogs{"temperate", "cold", "warm"}; // temperate_frog 0, cold_frog 1, warm_frog 2
constexpr std::array<std::string_view, 2> kFoxes{"red", "snow"};               // fox_red 0, fox_arctic 1
constexpr std::array<std::string_view, 2> kMooshrooms{"red", "brown"};         // mooshroom_red 0, mooshroom_brown 1
constexpr std::array<std::string_view, 7> kPandaGenes{"normal", "lazy", "worried", "playful", "brown", "weak", "aggressive"};
// Villager professions (unskilled 0 ... nitwit 14) and biome types (MarkVariant: plains 0 ... taiga 6).
constexpr std::array<std::string_view, 15> kProfessions{"none",    "farmer",      "fisherman", "shepherd",      "fletcher",
                                                        "librarian", "cartographer", "cleric",    "armorer",       "weaponsmith",
                                                        "toolsmith", "butcher",     "leatherworker", "mason",     "nitwit"};
constexpr std::array<std::string_view, 7> kVillagerTypes{"plains", "desert", "jungle", "savanna", "snow", "swamp", "taiga"};
// Axolotl: Java 0 lucy, 1 wild, 2 gold, 3 cyan, 4 blue; Bedrock axolotl_lucy 0, cyan 1, gold 2, wild 3, blue 4.
constexpr std::array<i32, 5> kAxolotlToBedrock{0, 3, 2, 1, 4};
constexpr std::array<i32, 5> kAxolotlToJava{0, 3, 2, 1, 4}; // the same swap both ways
// Mobs whose Java baby flag is IsBaby (zombie family, piglins); every other baby is Age < 0.
constexpr std::array<std::string_view, 7> kIsBabyMobs{"zombie", "husk", "drowned", "zombie_villager", "zombified_piglin", "piglin", "zoglin"};
// Mobs that wear a saddle as the Bedrock Saddled flag.
constexpr std::array<std::string_view, 9> kSaddleMobs{"pig", "strider", "horse", "donkey", "mule", "skeleton_horse", "zombie_horse", "camel",
                                                      "camel_husk"};

bool in(std::span<const std::string_view> list, std::string_view v) { return std::find(list.begin(), list.end(), v) != list.end(); }

// A mob: its Bedrock behavior file gives it health. Vehicles have health too but are not converted yet (their wood
// type and position offsets are not confirmed by a source; docs/formats/bedrock-world.md, "Mobs").
bool is_mob(std::string_view bedrock_id, std::string_view java_kind) {
    if (java_kind.ends_with("boat") || java_kind.ends_with("raft") || java_kind.ends_with("minecart")) return false;
    return bedrock_max_health(bedrock_id) > 0;
}

std::array<f64, 3> vec3(NbtView v) {
    std::array<f64, 3> r{0, 0, 0};
    usize i = 0;
    for (const NbtView x : v.items()) {
        if (i >= 3) break;
        r[i++] = x.as_double().value_or(static_cast<f64>(x.as_float().value_or(0)));
    }
    return r;
}

std::array<f32, 2> rot2(NbtView v) {
    std::array<f32, 2> r{0, 0};
    usize i = 0;
    for (const NbtView x : v.items()) {
        if (i >= 2) break;
        r[i++] = x.as_float().value_or(0);
    }
    return r;
}

bool truthy(NbtView v) { return v.as_byte().value_or(0) != 0; }

// The look and state of a mob, gathered from one edition before the other edition's tags are written.
struct Look {
    MobState state;
    std::optional<i8> size; // slime and magma cube (Bedrock Size == Variant)
    std::optional<i32> strength, temper, trade_tier, trade_exp;
    std::optional<i8> sitting;
    std::vector<std::pair<std::string, std::string>> enum_props; // Bedrock "properties" overriding the defaults
};

void write_empty_bedrock_item(NbtWriter& w) {
    w.begin_compound();
    w.write_byte("Count", 0);
    w.write_short("Damage", 0);
    w.write_string("Name", "");
    w.write_byte("WasPickedUp", 0);
    w.end_compound();
}

void write_bedrock_item_or_empty(NbtWriter& w, NbtView java_item, const MappingData& map, ConversionReport& report, BlockPos where) {
    if (const auto item = java_item.as_compound(); item && !item->find("id").as_string().value_or("").empty()) {
        if (write_item(w, {}, *item, map, report, where)) return;
    }
    write_empty_bedrock_item(w);
}

// ---- Java -> Bedrock -------------------------------------------------------------------------------------------------

Look java_look(std::string_view kind, NbtCompoundView c, const Entity& e, ConversionReport& report, BlockPos where) {
    Look l;
    auto lossy = [&](std::string_view what) { report.add(IssueKind::LossyEntity, e.id + ": " + std::string(what), where); };
    if (in(kIsBabyMobs, kind)) l.state.baby = truthy(c.find("IsBaby"));
    else l.state.baby = c.find("Age").as_int().value_or(0) < 0;

    if (kind == "sheep") {
        l.state.color = c.find("Color").as_byte().value_or(0); // dye ids in both (sheep_white 0 ... sheep_black 15)
        l.state.sheared = truthy(c.find("Sheared"));
    } else if (kind == "horse") {
        const i32 v = c.find("Variant").as_int().value_or(0); // colour | markings << 8; same numbering as base_* / markings_*
        l.state.variant = v & 0xFF;
        l.state.mark_variant = (v >> 8) & 0xFF;
    } else if (kind == "llama" || kind == "trader_llama") {
        l.state.variant = c.find("Variant").as_int().value_or(0); // creamy 0, white 1, brown 2, gray 3 in both
        if (const auto s = c.find("Strength").as_int()) l.strength = *s;
    } else if (kind == "cat") {
        l.state.variant = index_of(kCats, c.find("variant").as_string().value_or("minecraft:tabby")).value_or(8);
        l.state.color = c.find("CollarColor").as_byte().value_or(14);
    } else if (kind == "wolf") {
        l.state.variant = index_of(kWolves, c.find("variant").as_string().value_or("minecraft:pale")).value_or(0);
        l.state.color = c.find("CollarColor").as_byte().value_or(14);
    } else if (kind == "parrot") {
        l.state.variant = c.find("Variant").as_int().value_or(0); // red 0, blue 1, green 2, cyan 3, gray 4 in both
    } else if (kind == "rabbit") {
        const i32 t = c.find("RabbitType").as_int().value_or(0); // brown 0 ... salt 5 in both; 99 = the killer bunny
        if (t == 99) lossy("killer bunny type");
        l.state.variant = t == 99 ? 0 : t;
    } else if (kind == "fox") {
        l.state.variant = index_of(kFoxes, c.find("Type").as_string().value_or("red")).value_or(0);
    } else if (kind == "axolotl") {
        const i32 v = c.find("Variant").as_int().value_or(0);
        l.state.variant = v >= 0 && v < 5 ? kAxolotlToBedrock[static_cast<usize>(v)] : 0;
    } else if (kind == "frog") {
        l.state.variant = index_of(kFrogs, c.find("variant").as_string().value_or("minecraft:temperate")).value_or(0);
    } else if (kind == "goat") {
        l.state.variant = truthy(c.find("IsScreamingGoat")) ? 1 : 0; // goat_default 0, goat_screamer 1
    } else if (kind == "mooshroom") {
        l.state.variant = index_of(kMooshrooms, c.find("Type").as_string().value_or("red")).value_or(0);
    } else if (kind == "slime" || kind == "magma_cube") {
        const i32 size = std::clamp(c.find("Size").as_int().value_or(0), 0, 126); // Java 0 = smallest; Bedrock 1 = smallest
        l.state.variant = size + 1;
        l.size = static_cast<i8>(size + 1);
    } else if (kind == "shulker") {
        const i8 dye = c.find("Color").as_byte().value_or(16); // 16 = undyed in both; Bedrock shulker_black 0 ... white 15
        l.state.variant = dye >= 0 && dye < 16 ? 15 - dye : 16;
    } else if (kind == "panda") {
        l.state.variant = index_of(kPandaGenes, c.find("MainGene").as_string().value_or("normal")).value_or(0);
        if (c.find("HiddenGene").as_string().value_or("normal") != c.find("MainGene").as_string().value_or("normal"))
            lossy("hidden gene");
    } else if (kind == "tropical_fish") {
        const i32 v = c.find("Variant").as_int().value_or(0); // size | pattern << 8 | body colour << 16 | pattern colour << 24
        l.state.variant = v & 0xFF;
        l.state.mark_variant = (v >> 8) & 0xFF;
        l.state.color = (v >> 16) & 0xFF;
        l.state.color2 = (v >> 24) & 0xFF;
    } else if (kind == "villager" || kind == "zombie_villager") {
        const NbtCompoundView vd = c.find("VillagerData").entries();
        l.state.variant = index_of(kProfessions, vd.find("profession").as_string().value_or("minecraft:none")).value_or(0);
        l.state.mark_variant = index_of(kVillagerTypes, vd.find("type").as_string().value_or("minecraft:plains")).value_or(0);
        l.trade_tier = std::clamp(vd.find("level").as_int().value_or(1), 1, 5) - 1;
        if (const auto xp = c.find("Xp").as_int()) l.trade_exp = *xp;
        if (c.find("Offers").as_compound()) lossy("trades (Bedrock gives the villager new ones for its level)");
        if (c.find("Gossips").as_list() && !c.find("Gossips").items().empty()) lossy("reputation (gossip)");
    } else if (kind == "pig" || kind == "cow" || kind == "chicken") {
        if (const auto v = c.find("variant").as_string()) l.enum_props.emplace_back("minecraft:climate_variant", std::string(bare(*v)));
    }
    // Sound variants (pig, cow, chicken, wolf, cat): Java "classic" is Bedrock "default".
    if (const auto sv = c.find("sound_variant").as_string()) {
        const std::string_view n = bare(*sv);
        l.enum_props.emplace_back("minecraft:sound_variant", n == "classic" ? "default" : std::string(n));
    }
    if (in(kSaddleMobs, kind)) l.state.saddled = c.find("equipment").entries().find("saddle").as_compound().has_value();
    if (const auto chested = c.find("ChestedHorse").as_byte()) l.state.chested = *chested != 0;
    if (const auto t = c.find("Temper").as_int()) l.temper = *t;
    if (const auto s = c.find("Sitting").as_byte()) l.sitting = *s;
    return l;
}

void write_definitions(NbtWriter& w, std::string_view identifier, const MobState& state) {
    const std::vector<std::string> defs = bedrock_definitions(identifier, state);
    w.begin_list("definitions", TagType::String, defs.size());
    for (const std::string& d : defs) w.write_string({}, d);
    w.end_list();
}

void write_properties(NbtWriter& w, std::string_view identifier, const Look& l) {
    const auto declared = bedrock_properties(identifier);
    if (declared.empty()) return;
    w.begin_compound("properties");
    for (const BedrockProperty& p : declared) {
        std::string text = p.text;
        for (const auto& [name, value] : l.enum_props)
            if (name == p.name) text = value;
        switch (p.kind) {
        case BedrockProperty::Kind::Enum: w.write_string(p.name, text); break;
        case BedrockProperty::Kind::Bool: w.write_byte(p.name, static_cast<i8>(p.number)); break;
        case BedrockProperty::Kind::Int: w.write_int(p.name, p.number); break;
        }
    }
    w.end_compound();
}

void write_health(NbtWriter& w, std::string_view identifier, NbtCompoundView c) {
    const auto health = c.find("Health").as_float();
    if (!health) return;
    f32 max = static_cast<f32>(bedrock_max_health(identifier));
    for (const NbtView a : c.find("attributes").items()) // a changed maximum (attribute base) is carried
        if (a.entries().find("id").as_string().value_or("") == "minecraft:max_health")
            max = static_cast<f32>(a.entries().find("base").as_double().value_or(max));
    if (max <= 0) max = std::max(*health, 1.0f);
    w.begin_list("Attributes", TagType::Compound, 1);
    w.begin_compound();
    w.write_float("Base", max);
    w.write_float("Current", std::min(*health, max));
    w.write_float("DefaultMax", max);
    w.write_float("DefaultMin", 0.0f);
    w.write_float("Max", max);
    w.write_float("Min", 0.0f);
    w.write_string("Name", "minecraft:health");
    w.end_compound();
    w.end_list();
}

void write_equipment(NbtWriter& w, NbtCompoundView c, const MappingData& map, ConversionReport& report, BlockPos where) {
    const NbtCompoundView eq = c.find("equipment").entries();
    w.begin_list("Armor", TagType::Compound, 5); // head, chest, legs, feet, body (the five slots worlds saved by 26.x have)
    for (const char* slot : {"head", "chest", "legs", "feet", "body"}) write_bedrock_item_or_empty(w, eq.find(slot), map, report, where);
    w.end_list();
    w.begin_list("Mainhand", TagType::Compound, 1);
    write_bedrock_item_or_empty(w, eq.find("mainhand"), map, report, where);
    w.end_list();
    w.begin_list("Offhand", TagType::Compound, 1);
    write_bedrock_item_or_empty(w, eq.find("offhand"), map, report, where);
    w.end_list();
}

} // namespace

bool java_mob_to_bedrock(const Entity& e, NbtCompoundView c, Structure& out, const MappingData& map, const TranslateOptions& o,
                         ConversionReport& report) {
    const EntityTypeRow* row = entity_type_by_java(e.id);
    if (row == nullptr || e.id == "minecraft:player") return false;
    const auto bedrock_id = bedrock_entity_id(*row, kNewest);
    if (!bedrock_id) return false;
    const std::string_view kind = bare(e.id);
    const bool item = kind == "item", stand = kind == "armor_stand", crystal = kind == "end_crystal";
    const bool living = !item && !crystal && is_mob(*bedrock_id, kind);
    if (!living && !item && !crystal) return false;

    const BlockPos where{static_cast<i32>(std::floor(e.pos.x)), static_cast<i32>(std::floor(e.pos.y)), static_cast<i32>(std::floor(e.pos.z))};
    auto lossy = [&](std::string_view what) { report.add(IssueKind::LossyEntity, e.id + ": " + std::string(what), where); };
    Look l = living && !stand ? java_look(kind, c, e, report, where) : Look{};

    // Owner: only the world's own player can be named in Bedrock.
    std::optional<i64> owner;
    if (const auto uuid = c.find("Owner").as_int_array(); uuid && uuid->size() == 4) {
        const std::array<i32, 4> u{(*uuid)[0], (*uuid)[1], (*uuid)[2], (*uuid)[3]};
        if (o.player_uuid && *o.player_uuid == u) owner = kBedrockLocalPlayerId;
        else lossy("owner (not the world's player; the animal is untamed)");
    }
    const bool tamed = owner.has_value(); // a Bedrock tame animal needs its owner
    l.state.tamed = tamed;

    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_string("identifier", *bedrock_id);
    if (!item && !crystal) write_definitions(w, *bedrock_id, l.state);
    const auto motion = vec3(c.find("Motion"));
    w.begin_list("Motion", TagType::Float, 3);
    for (const f64 m : motion) w.write_float({}, static_cast<f32>(m));
    w.end_list();
    const auto rot = rot2(c.find("Rotation"));
    w.begin_list("Rotation", TagType::Float, 2);
    w.write_float({}, rot[0]);
    w.write_float({}, rot[1]);
    w.end_list();
    if (c.find("CustomName")) {
        bool formatted = false;
        const std::string name = text_of(c.find("CustomName"), formatted);
        if (formatted) lossy("name formatting (colours, bold)");
        w.write_string("CustomName", name);
        w.write_byte("CustomNameVisible", truthy(c.find("CustomNameVisible")) ? 1 : 0);
    }
    w.write_byte("Invulnerable", truthy(c.find("Invulnerable")) ? 1 : 0);
    w.write_byte("OnGround", truthy(c.find("OnGround")) ? 1 : 0);
    const f64 fall = c.find("fall_distance").as_double().value_or(static_cast<f64>(c.find("FallDistance").as_float().value_or(0)));
    w.write_float("FallDistance", static_cast<f32>(fall));
    w.write_short("Fire", std::max<i16>(c.find("Fire").as_short().value_or(0), 0));
    if (const auto air = c.find("Air").as_short()) w.write_short("Air", *air);
    if (const auto tags = c.find("Tags"); tags.as_list() && !tags.items().empty()) {
        w.begin_list("Tags", TagType::String, tags.items().size());
        for (const NbtView t : tags.items()) w.write_string({}, io::nbt::mutf8_to_utf8(t.as_string().value_or("")).value_or(""));
        w.end_list();
    }
    for (const auto& [tag, what] : std::array<std::pair<const char*, const char*>, 4>{
             {{"NoAI", "no AI"}, {"Glowing", "glowing"}, {"Silent", "silent"}, {"HasVisualFire", "visual fire"}}})
        if (truthy(c.find(tag))) lossy(what);
    // Java saves flying mobs without gravity and makes some mobs left-handed at random; that only matters for a mob
    // that walks, or one that holds something.
    static constexpr std::array<std::string_view, 12> kFlyers{"bee",   "bat",   "parrot", "allay", "ghast", "happy_ghast",
                                                              "phantom", "blaze", "vex",    "wither", "ender_dragon", "breeze"};
    if (truthy(c.find("NoGravity")) && !in(kFlyers, kind)) lossy("no gravity");
    const NbtCompoundView hands = c.find("equipment").entries();
    if (truthy(c.find("LeftHanded")) && (hands.find("mainhand").as_compound() || hands.find("offhand").as_compound())) lossy("left-handed");
    if (c.find("Passengers").as_list() && !c.find("Passengers").items().empty()) lossy("passengers (riders are not linked)");
    if (c.find("leash")) lossy("leash");
    if (c.find("active_effects").as_list() && !c.find("active_effects").items().empty()) lossy("potion effects");

    if (item) {
        const auto stack = c.find("Item").as_compound();
        if (!stack || !write_item(w, "Item", *stack, map, report, where)) return false;
        w.write_short("Age", c.find("Age").as_short().value_or(0));
        w.write_short("Health", c.find("Health").as_short().value_or(5));
        w.write_short("PickupDelay", c.find("PickupDelay").as_short().value_or(0));
        if (c.find("Owner") || c.find("Thrower")) lossy("who may pick it up");
    } else if (crystal) {
        w.write_byte("ShowBottom", truthy(c.find("ShowBottom")) ? 1 : 0);
        if (c.find("beam_target") || c.find("BeamTarget")) lossy("beam target");
    } else {
        w.write_byte("Persistent", truthy(c.find("PersistenceRequired")) || c.find("CustomName") ? 1 : 0);
        w.write_byte("IsBaby", l.state.baby ? 1 : 0);
        if (l.state.variant) w.write_int("Variant", *l.state.variant);
        if (l.state.mark_variant) w.write_int("MarkVariant", *l.state.mark_variant);
        if (l.state.color) w.write_byte("Color", static_cast<i8>(*l.state.color));
        if (l.state.color2) w.write_byte("Color2", static_cast<i8>(*l.state.color2));
        if (l.size) w.write_byte("Size", *l.size);
        if (l.state.sheared) w.write_byte("Sheared", *l.state.sheared ? 1 : 0);
        if (l.state.saddled) w.write_byte("Saddled", *l.state.saddled ? 1 : 0);
        if (l.state.chested) w.write_byte("Chested", *l.state.chested ? 1 : 0);
        w.write_byte("IsTamed", tamed ? 1 : 0);
        w.write_long("OwnerNew", owner.value_or(-1));
        if (l.sitting) w.write_byte("Sitting", *l.sitting);
        if (l.strength) w.write_int("Strength", *l.strength);
        if (l.temper) w.write_int("Temper", *l.temper);
        if (l.trade_tier) w.write_int("TradeTier", *l.trade_tier);
        if (l.trade_exp) w.write_int("TradeExperience", *l.trade_exp);
        if (l.state.chested.value_or(false) && c.find("Items").as_list() && !c.find("Items").items().empty()) lossy("chest contents");
        write_properties(w, *bedrock_id, l);
        write_health(w, *bedrock_id, c);
        if (stand) {
            w.begin_compound("Pose");
            w.write_int("PoseIndex", 0);
            w.write_int("LastSignal", 0);
            w.end_compound();
            if (c.find("Pose").as_compound() && c.find("Pose").entries().size() > 0) lossy("pose (Bedrock armor stands use preset poses)");
            for (const auto& [tag, what] : std::array<std::pair<const char*, const char*>, 4>{
                     {{"Small", "small size"}, {"NoBasePlate", "no base plate"}, {"Invisible", "invisible"}, {"Marker", "marker"}}})
                if (truthy(c.find(tag))) lossy(what);
        }
        write_equipment(w, c, map, report, where);
        const NbtCompoundView eq = c.find("equipment").entries();
        if (eq.find("saddle").as_compound() && !in(kSaddleMobs, kind)) lossy("saddle");
        if (c.find("drop_chances").as_compound()) lossy("equipment drop chances");
    }
    w.end_compound();
    auto bytes = std::move(w).finish();
    if (!bytes) {
        report.add(IssueKind::UnmappedEntity, e.id + " (could not be written)", where);
        return true;
    }
    Entity actor;
    actor.id = std::string(*bedrock_id);
    actor.pos = e.pos;
    actor.data = RawNbt{std::move(*bytes), Endian::Little};
    out.entities.push_back(std::move(actor));
    return true;
}

// ---- Bedrock -> Java -------------------------------------------------------------------------------------------------

namespace {

void write_java_item_or_skip(NbtWriter& w, std::string_view slot, NbtView bedrock_list, const MappingData& map, ConversionReport& report,
                             BlockPos where, usize index = 0) {
    usize i = 0;
    for (const NbtView it : bedrock_list.items()) {
        if (i++ != index) continue;
        const NbtCompoundView item = it.entries();
        if (item.find("Name").as_string().value_or("").empty() || item.find("Count").as_byte().value_or(0) <= 0) return;
        (void)write_java_item(w, slot, item, map, report, where);
        return;
    }
}

bool has_item(NbtView bedrock_list, usize index) {
    usize i = 0;
    for (const NbtView it : bedrock_list.items())
        if (i++ == index) return !it.entries().find("Name").as_string().value_or("").empty() && it.entries().find("Count").as_byte().value_or(0) > 0;
    return false;
}

std::string java_name(std::string_view n) { return "minecraft:" + std::string(n); }

} // namespace

bool bedrock_mob_to_java(const Entity& actor, NbtCompoundView c, Structure& out, const MappingData& map, const TranslateOptions&,
                         ConversionReport& report) {
    const EntityTypeRow* row = entity_type_by_bedrock(actor.id);
    if (row == nullptr || actor.id == "minecraft:player") return false;
    const auto java_id = java_entity_id(*row, kNewest);
    if (!java_id) return false;
    const std::string_view kind = bare(*java_id);
    const bool item = kind == "item", stand = kind == "armor_stand", crystal = kind == "end_crystal";
    const bool living = !item && !crystal && is_mob(actor.id, kind);
    if (!living && !item && !crystal) return false;

    const BlockPos where{static_cast<i32>(std::floor(actor.pos.x)), static_cast<i32>(std::floor(actor.pos.y)),
                         static_cast<i32>(std::floor(actor.pos.z))};
    auto lossy = [&](std::string_view what) { report.add(IssueKind::LossyEntity, std::string(*java_id) + ": " + std::string(what), where); };

    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    const auto motion = vec3(c.find("Motion"));
    w.begin_list("Motion", TagType::Double, 3);
    for (const f64 m : motion) w.write_double({}, m);
    w.end_list();
    const auto rot = rot2(c.find("Rotation"));
    w.begin_list("Rotation", TagType::Float, 2);
    w.write_float({}, rot[0]);
    w.write_float({}, rot[1]);
    w.end_list();
    if (const auto name = c.find("CustomName").as_string(); name && !name->empty()) {
        w.write_string("CustomName", io::nbt::utf8_to_mutf8(*name).value_or("")); // a plain string is a text component
        w.write_byte("CustomNameVisible", truthy(c.find("CustomNameVisible")) ? 1 : 0);
    }
    if (truthy(c.find("Invulnerable"))) w.write_byte("Invulnerable", 1);
    w.write_byte("OnGround", truthy(c.find("OnGround")) ? 1 : 0);
    w.write_double("fall_distance", static_cast<f64>(c.find("FallDistance").as_float().value_or(0)));
    w.write_short("Fire", c.find("Fire").as_short().value_or(0) > 0 ? *c.find("Fire").as_short() : static_cast<i16>(-20));
    if (const auto air = c.find("Air").as_short()) w.write_short("Air", *air);
    if (const auto tags = c.find("Tags"); tags.as_list() && !tags.items().empty()) {
        w.begin_list("Tags", TagType::String, tags.items().size());
        for (const NbtView t : tags.items()) w.write_string({}, io::nbt::utf8_to_mutf8(t.as_string().value_or("")).value_or(""));
        w.end_list();
    }
    if (c.find("LinksTag").as_list() && !c.find("LinksTag").items().empty()) lossy("passengers (riders are not linked)");
    if (c.find("LeasherID").as_long().value_or(-1) != -1) lossy("leash");
    if (c.find("ActiveEffects").as_list() && !c.find("ActiveEffects").items().empty()) lossy("potion effects");

    if (item) {
        const auto stack = c.find("Item").as_compound();
        if (!stack || !write_java_item(w, "Item", *stack, map, report, where)) return false;
        w.write_short("Age", c.find("Age").as_short().value_or(0));
        w.write_short("Health", c.find("Health").as_short().value_or(5));
        w.write_short("PickupDelay", c.find("PickupDelay").as_short().value_or(0));
    } else if (crystal) {
        w.write_byte("ShowBottom", truthy(c.find("ShowBottom")) ? 1 : 0);
    } else {
        const bool baby = truthy(c.find("IsBaby"));
        if (in(kIsBabyMobs, kind)) w.write_byte("IsBaby", baby ? 1 : 0);
        else if (baby) w.write_int("Age", -24000); // a newborn's age (minecraft.wiki "Entity format", Breedable)
        if (truthy(c.find("Persistent"))) w.write_byte("PersistenceRequired", 1);
        const i32 variant = c.find("Variant").as_int().value_or(0), mark = c.find("MarkVariant").as_int().value_or(0);
        const i8 color = c.find("Color").as_byte().value_or(0);
        if (kind == "sheep") {
            w.write_byte("Color", color);
            w.write_byte("Sheared", truthy(c.find("Sheared")) ? 1 : 0);
        } else if (kind == "horse") {
            w.write_int("Variant", (variant & 0xFF) | ((mark & 0xFF) << 8));
        } else if (kind == "llama" || kind == "trader_llama") {
            w.write_int("Variant", std::clamp(variant, 0, 3));
            if (const auto s = c.find("Strength").as_int()) w.write_int("Strength", *s);
        } else if (kind == "cat" || kind == "wolf") {
            const std::string_view n = kind == "cat" ? (variant >= 0 && variant < 11 ? kCats[static_cast<usize>(variant)] : "tabby")
                                                     : (variant >= 0 && variant < 9 ? kWolves[static_cast<usize>(variant)] : "pale");
            w.write_string("variant", java_name(n));
            w.write_byte("CollarColor", truthy(c.find("IsTamed")) ? color : static_cast<i8>(14));
        } else if (kind == "parrot") {
            w.write_int("Variant", std::clamp(variant, 0, 4));
        } else if (kind == "rabbit") {
            w.write_int("RabbitType", std::clamp(variant, 0, 5));
        } else if (kind == "fox") {
            w.write_string("Type", variant == 1 ? "snow" : "red");
        } else if (kind == "axolotl") {
            w.write_int("Variant", variant >= 0 && variant < 5 ? kAxolotlToJava[static_cast<usize>(variant)] : 0);
        } else if (kind == "frog") {
            w.write_string("variant", java_name(variant >= 0 && variant < 3 ? kFrogs[static_cast<usize>(variant)] : "temperate"));
        } else if (kind == "goat") {
            w.write_byte("IsScreamingGoat", variant == 1 ? 1 : 0);
        } else if (kind == "mooshroom") {
            w.write_string("Type", variant == 1 ? "brown" : "red");
        } else if (kind == "slime" || kind == "magma_cube") {
            const i32 size = c.find("Size").as_byte().value_or(static_cast<i8>(variant > 0 ? variant : 1));
            w.write_int("Size", std::max(size - 1, 0));
        } else if (kind == "shulker") {
            w.write_byte("Color", static_cast<i8>(variant >= 0 && variant < 16 ? 15 - variant : 16));
        } else if (kind == "panda") {
            const std::string_view gene = variant >= 0 && variant < 7 ? kPandaGenes[static_cast<usize>(variant)] : "normal";
            w.write_string("MainGene", gene);
            w.write_string("HiddenGene", gene);
        } else if (kind == "tropical_fish") {
            const u32 packed = (static_cast<u32>(variant) & 0xFF) | ((static_cast<u32>(mark) & 0xFF) << 8) |
                               ((static_cast<u32>(static_cast<u8>(color)) & 0xFF) << 16) |
                               ((static_cast<u32>(static_cast<u8>(c.find("Color2").as_byte().value_or(0))) & 0xFF) << 24);
            w.write_int("Variant", static_cast<i32>(packed));
        } else if (kind == "villager" || kind == "zombie_villager") {
            w.begin_compound("VillagerData");
            w.write_int("level", std::clamp(c.find("TradeTier").as_int().value_or(0), 0, 4) + 1);
            w.write_string("profession", java_name(variant >= 0 && variant < 15 ? kProfessions[static_cast<usize>(variant)] : "none"));
            w.write_string("type", java_name(mark >= 0 && mark < 7 ? kVillagerTypes[static_cast<usize>(mark)] : "plains"));
            w.end_compound();
            if (const auto xp = c.find("TradeExperience").as_int()) w.write_int("Xp", *xp);
            if (c.find("Offers").as_compound()) lossy("trades (Java gives the villager new ones for its level)");
        }
        // Climate and sound variants (pig, cow, chicken, wolf, cat): Bedrock properties -> Java names.
        const NbtCompoundView props = c.find("properties").entries();
        if (const auto climate = props.find("minecraft:climate_variant").as_string(); climate && (kind == "pig" || kind == "cow" || kind == "chicken"))
            w.write_string("variant", java_name(*climate));
        if (const auto sound = props.find("minecraft:sound_variant").as_string())
            w.write_string("sound_variant", java_name(*sound == "default" ? "classic" : *sound));
        if (const auto chested = c.find("Chested").as_byte(); chested && (kind == "donkey" || kind == "mule" || kind == "llama" || kind == "trader_llama")) {
            w.write_byte("ChestedHorse", *chested);
            if (*chested && c.find("ChestItems").as_list() && !c.find("ChestItems").items().empty()) lossy("chest contents");
        }
        if (const auto t = c.find("Temper").as_int()) w.write_int("Temper", *t);
        if (const auto s = c.find("Sitting").as_byte()) w.write_byte("Sitting", *s);
        if (truthy(c.find("IsTamed"))) {
            // The Java owner is the player's account UUID, which a Bedrock world does not know.
            lossy("owner (Java needs the player's account; the animal is untamed)");
        }
        // Health and a changed maximum.
        for (const NbtView a : c.find("Attributes").items()) {
            const NbtCompoundView at = a.entries();
            if (at.find("Name").as_string().value_or("") != "minecraft:health") continue;
            w.write_float("Health", at.find("Current").as_float().value_or(1));
            const f32 max = at.find("Max").as_float().value_or(0);
            if (const i32 def = bedrock_max_health(actor.id); max > 0 && def > 0 && std::fabs(max - static_cast<f32>(def)) > 0.01f) {
                w.begin_list("attributes", TagType::Compound, 1);
                w.begin_compound();
                w.write_string("id", "minecraft:max_health");
                w.write_double("base", static_cast<f64>(max));
                w.end_compound();
                w.end_list();
            }
        }
        // Equipment: Armor head, chest, legs, feet, body; Mainhand; Offhand; the saddle flag.
        const NbtView armor = c.find("Armor"), main = c.find("Mainhand"), off = c.find("Offhand");
        const bool saddled = truthy(c.find("Saddled")) && in(kSaddleMobs, kind);
        bool any = saddled || has_item(main, 0) || has_item(off, 0);
        for (usize i = 0; i < 5; ++i) any = any || has_item(armor, i);
        if (any) {
            w.begin_compound("equipment");
            static constexpr std::array<const char*, 5> kSlots{"head", "chest", "legs", "feet", "body"};
            for (usize i = 0; i < 5; ++i) write_java_item_or_skip(w, kSlots[i], armor, map, report, where, i);
            write_java_item_or_skip(w, "mainhand", main, map, report, where);
            write_java_item_or_skip(w, "offhand", off, map, report, where);
            if (saddled) {
                w.begin_compound("saddle");
                w.write_string("id", "minecraft:saddle");
                w.write_int("count", 1);
                w.end_compound();
            }
            w.end_compound();
        }
        if (stand && c.find("Pose").entries().find("PoseIndex").as_int().value_or(0) != 0) lossy("pose (Java armor stands have free poses)");
    }
    w.end_compound();
    auto bytes = std::move(w).finish();
    if (!bytes) {
        report.add(IssueKind::UnmappedEntity, std::string(*java_id) + " (could not be written)", where);
        return true;
    }
    Entity e;
    e.id = std::string(*java_id);
    e.pos = actor.pos;
    e.data = RawNbt{std::move(*bytes), Endian::Big};
    out.entities.push_back(std::move(e));
    return true;
}

} // namespace bridger::translate::detail
