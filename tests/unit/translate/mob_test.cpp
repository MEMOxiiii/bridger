// Mobs and objects between the editions (src/translate/mob_translator.cpp; docs/formats/bedrock-world.md, "Mobs"):
// each mob's look and state in both directions, the Bedrock "definitions" chosen for it, owners, what is reported, the
// entity modes and older target versions.

#include "../test_util.hpp"

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/targets.hpp>
#include <bridger/translate/translator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace bridger;
using io::nbt::NbtCompoundView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

const translate::MappingData& mapping() { return *translate::MappingData::embedded().value(); }

Structure structure(Edition edition) {
    const u32 dv = edition == Edition::Java ? mapping().java_data_version() : 0;
    auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), {16, 16, 16}, {edition, dv});
    REQUIRE(s);
    return std::move(*s);
}

// A Java entity: `fill` writes its tags (big-endian); Pos is added.
Entity java_entity(std::string id, const std::function<void(NbtWriter&)>& fill, EntityPos pos = {4.5, 2, 4.5}) {
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.begin_list("Pos", TagType::Double, 3);
    w.write_double({}, pos.x);
    w.write_double({}, pos.y);
    w.write_double({}, pos.z);
    w.end_list();
    fill(w);
    w.end_compound();
    Entity e;
    e.id = std::move(id);
    e.pos = pos;
    e.data = RawNbt{std::move(w).finish().value(), Endian::Big};
    return e;
}

Entity bedrock_actor(std::string id, const std::function<void(NbtWriter&)>& fill) {
    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_string("identifier", id);
    fill(w);
    w.end_compound();
    Entity e;
    e.id = std::move(id);
    e.pos = {4.5, 2, 4.5};
    e.data = RawNbt{std::move(w).finish().value(), Endian::Little};
    return e;
}

struct Converted {
    Structure out;
    translate::ConversionReport report;
};

Converted to_bedrock(std::vector<Entity> entities, translate::TranslateOptions o = {}) {
    if (o.entities == translate::EntityMode::Decorations) o.entities = translate::EntityMode::All;
    Structure java = structure(Edition::Java);
    java.entities = std::move(entities);
    Converted c{structure(Edition::Bedrock), {}};
    auto r = translate::translate_to_bedrock(java, mapping(), o, c.report);
    REQUIRE(r);
    c.out = std::move(*r);
    return c;
}

Converted to_java(std::vector<Entity> actors) {
    Structure bedrock = structure(Edition::Bedrock);
    bedrock.entities = std::move(actors);
    translate::TranslateOptions o;
    o.entities = translate::EntityMode::All;
    Converted c{structure(Edition::Java), {}};
    auto r = translate::translate_to_java(bedrock, mapping(), o, c.report);
    REQUIRE(r);
    c.out = std::move(*r);
    return c;
}

// The NBT of the only entity of `s` (kept alive by the caller's document).
io::nbt::NbtDocument doc_of(const Structure& s, std::string_view id) {
    const auto it = std::find_if(s.entities.begin(), s.entities.end(), [&](const Entity& e) { return e.id == id; });
    REQUIRE(it != s.entities.end());
    auto doc = io::nbt::NbtDocument::parse(it->data.bytes, it->data.endian);
    REQUIRE(doc);
    return std::move(*doc);
}

std::vector<std::string> strings(io::nbt::NbtView list) {
    std::vector<std::string> out;
    for (const auto v : list.items()) out.emplace_back(v.as_string().value_or(""));
    return out;
}

bool contains(const std::vector<std::string>& v, std::string_view s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool reported(const translate::ConversionReport& r, translate::IssueKind kind, std::string_view text) {
    const auto* subjects = r.issues(kind);
    if (subjects == nullptr) return false;
    for (const auto& [subject, entry] : *subjects)
        if (subject.find(text) != std::string::npos) return true;
    return false;
}

void write_item(NbtWriter& w, std::string_view name, std::string_view id) {
    w.begin_compound(name);
    w.write_string("id", id);
    w.write_int("count", 1);
    w.end_compound();
}

} // namespace

TEST_CASE("A Java sheep becomes a Bedrock sheep with its colour, wool, age, name and health", "[translate][entities]") {
    const auto c = to_bedrock({java_entity("minecraft:sheep", [](NbtWriter& w) {
        w.write_byte("Color", 7); // gray
        w.write_byte("Sheared", 1);
        w.write_int("Age", -1200);
        w.write_string("CustomName", "Dolly");
        w.write_float("Health", 5.0f);
    })});
    REQUIRE(c.out.entities.size() == 1);
    const auto doc = doc_of(c.out, "minecraft:sheep");
    const NbtCompoundView s = doc.root().entries();
    CHECK(s.find("identifier").as_string() == "minecraft:sheep");
    CHECK(s.find("Color").as_byte() == 7);
    CHECK(s.find("Sheared").as_byte() == 1);
    CHECK(s.find("IsBaby").as_byte() == 1);
    CHECK(s.find("CustomName").as_string() == "Dolly");
    CHECK(s.find("Persistent").as_byte() == 1); // named mobs do not despawn
    // The definitions the game gives a baby gray sheep (behavior pack: sheep_baby, sheep_gray).
    const auto defs = strings(s.find("definitions"));
    CHECK(defs.front() == "+minecraft:sheep");
    CHECK(contains(defs, "+minecraft:sheep_baby"));
    CHECK(contains(defs, "+minecraft:sheep_gray"));
    CHECK_FALSE(contains(defs, "+minecraft:sheep_adult"));
    // Health: the attribute Bedrock saves.
    bool health = false;
    for (const auto a : s.find("Attributes").items()) {
        const NbtCompoundView at = a.entries();
        if (at.find("Name").as_string() != "minecraft:health") continue;
        health = true;
        CHECK(at.find("Current").as_float() == 5.0f);
        CHECK(at.find("Max").as_float() == 8.0f); // the sheep's maximum (behavior pack)
    }
    CHECK(health);
    // Five armor slots (head, chest, legs, feet, body), empty.
    CHECK(s.find("Armor").items().size() == 5);
    CHECK(c.report.issues(translate::IssueKind::UnmappedEntity) == nullptr);
}

TEST_CASE("Horses keep coat, markings and saddle; only the world's player can stay their owner", "[translate][entities]") {
    const std::array<i32, 4> player{11, 22, 33, 44};
    translate::TranslateOptions o;
    o.player_uuid = player;
    const auto horse = [](std::array<i32, 4> owner) {
        return java_entity("minecraft:horse", [owner](NbtWriter& w) {
            w.write_int("Variant", 3 | (2 << 8)); // brown, white fields
            w.write_byte("Tame", 1);
            w.write_int_array("Owner", owner);
            w.write_float("Health", 20.0f);
            w.begin_compound("equipment");
            write_item(w, "saddle", "minecraft:saddle");
            w.end_compound();
        });
    };
    const auto mine = to_bedrock({horse(player)}, o);
    {
        const auto doc = doc_of(mine.out, "minecraft:horse");
        const NbtCompoundView h = doc.root().entries();
        CHECK(h.find("Variant").as_int() == 3);
        CHECK(h.find("MarkVariant").as_int() == 2);
        CHECK(h.find("Saddled").as_byte() == 1);
        CHECK(h.find("IsTamed").as_byte() == 1);
        CHECK(h.find("OwnerNew").as_long() == translate::kBedrockLocalPlayerId);
        const auto defs = strings(h.find("definitions"));
        CHECK(contains(defs, "+minecraft:base_brown"));
        CHECK(contains(defs, "+minecraft:markings_white_fields"));
    }
    const auto theirs = to_bedrock({horse({1, 2, 3, 4})}, o);
    const auto doc = doc_of(theirs.out, "minecraft:horse");
    CHECK(doc.root().entries().find("IsTamed").as_byte() == 0);
    CHECK(reported(theirs.report, translate::IssueKind::LossyEntity, "owner"));
}

TEST_CASE("Villagers keep profession, biome and level; their trades are reported", "[translate][entities]") {
    const auto c = to_bedrock({java_entity("minecraft:villager", [](NbtWriter& w) {
        w.begin_compound("VillagerData");
        w.write_int("level", 3);
        w.write_string("profession", "minecraft:mason");
        w.write_string("type", "minecraft:desert");
        w.end_compound();
        w.begin_compound("Offers");
        w.begin_list("Recipes", TagType::Compound, 0);
        w.end_list();
        w.end_compound();
        w.write_float("Health", 20.0f);
    })});
    const auto doc = doc_of(c.out, "minecraft:villager_v2");
    const NbtCompoundView v = doc.root().entries();
    CHECK(v.find("Variant").as_int() == 13);    // mason
    CHECK(v.find("MarkVariant").as_int() == 1); // desert
    CHECK(v.find("TradeTier").as_int() == 2);
    const auto defs = strings(v.find("definitions"));
    CHECK(contains(defs, "+mason"));
    CHECK(contains(defs, "+desert_villager"));
    CHECK(reported(c.report, translate::IssueKind::LossyEntity, "trades"));
}

TEST_CASE("Bedrock mobs become Java mobs with each look mapped back", "[translate][entities]") {
    const auto c = to_java({
        bedrock_actor("minecraft:cat", [](NbtWriter& w) { w.write_int("Variant", 1); }),       // tuxedo = Java "black"
        bedrock_actor("minecraft:axolotl", [](NbtWriter& w) { w.write_int("Variant", 1); }),   // cyan = Java 3
        bedrock_actor("minecraft:slime", [](NbtWriter& w) { w.write_byte("Size", 4); }),       // large = Java 3
        bedrock_actor("minecraft:shulker", [](NbtWriter& w) { w.write_int("Variant", 0); }),   // black = dye 15
        bedrock_actor("minecraft:tropicalfish", [](NbtWriter& w) {
            w.write_int("Variant", 1);
            w.write_int("MarkVariant", 2);
            w.write_byte("Color", 4);
            w.write_byte("Color2", 14);
        }),
        bedrock_actor("minecraft:zombie", [](NbtWriter& w) { w.write_byte("IsBaby", 1); }),
        bedrock_actor("minecraft:cow", [](NbtWriter& w) {
            w.write_byte("IsBaby", 1);
            w.begin_compound("properties");
            w.write_string("minecraft:climate_variant", "warm");
            w.write_string("minecraft:sound_variant", "moody");
            w.end_compound();
        }),
    });
    CHECK(doc_of(c.out, "minecraft:cat").root().entries().find("variant").as_string() == "minecraft:black");
    CHECK(doc_of(c.out, "minecraft:axolotl").root().entries().find("Variant").as_int() == 3);
    CHECK(doc_of(c.out, "minecraft:slime").root().entries().find("Size").as_int() == 3);
    CHECK(doc_of(c.out, "minecraft:shulker").root().entries().find("Color").as_byte() == 15);
    CHECK(doc_of(c.out, "minecraft:tropical_fish").root().entries().find("Variant").as_int() == (1 | 2 << 8 | 4 << 16 | 14 << 24));
    CHECK(doc_of(c.out, "minecraft:zombie").root().entries().find("IsBaby").as_byte() == 1);
    const auto cow = doc_of(c.out, "minecraft:cow");
    CHECK(cow.root().entries().find("Age").as_int() == -24000);
    CHECK(cow.root().entries().find("variant").as_string() == "minecraft:warm");
    CHECK(cow.root().entries().find("sound_variant").as_string() == "minecraft:moody");
}

TEST_CASE("Mobs survive Java -> Bedrock -> Java with names, equipment and dropped items", "[translate][entities]") {
    const auto there = to_bedrock({
        java_entity("minecraft:zombie", [](NbtWriter& w) {
            w.write_string("CustomName", "Bob");
            w.write_float("Health", 12.0f);
            w.begin_compound("equipment");
            write_item(w, "mainhand", "minecraft:diamond_sword");
            write_item(w, "head", "minecraft:iron_helmet");
            w.end_compound();
        }),
        java_entity("minecraft:item", [](NbtWriter& w) {
            write_item(w, "Item", "minecraft:emerald");
            w.write_short("Age", 100);
        }),
    });
    const auto back = to_java(there.out.entities);
    const auto zombie = doc_of(back.out, "minecraft:zombie");
    const NbtCompoundView z = zombie.root().entries();
    CHECK(z.find("CustomName").as_string() == "Bob");
    CHECK(z.find("Health").as_float() == 12.0f);
    CHECK(z.find("equipment").entries().find("mainhand").entries().find("id").as_string() == "minecraft:diamond_sword");
    CHECK(z.find("equipment").entries().find("head").entries().find("id").as_string() == "minecraft:iron_helmet");
    const auto item = doc_of(back.out, "minecraft:item");
    CHECK(item.root().entries().find("Item").entries().find("id").as_string() == "minecraft:emerald");
    CHECK(item.root().entries().find("Age").as_short() == 100);
}

TEST_CASE("The entity mode decides what is written", "[translate][entities]") {
    const auto pig = [] { return java_entity("minecraft:pig", [](NbtWriter& w) { w.write_float("Health", 10.0f); }); };
    translate::TranslateOptions decorations;
    decorations.entities = translate::EntityMode::Decorations;
    Structure java = structure(Edition::Java);
    java.entities = {pig()};
    translate::ConversionReport report;
    auto out = translate::translate_to_bedrock(java, mapping(), decorations, report);
    REQUIRE(out);
    CHECK(out->entities.empty()); // the default: mobs are reported, not written
    CHECK(reported(report, translate::IssueKind::UnmappedEntity, "minecraft:pig"));

    translate::TranslateOptions none;
    none.entities = translate::EntityMode::None;
    translate::ConversionReport report2;
    auto out2 = translate::translate_to_bedrock(java, mapping(), none, report2);
    REQUIRE(out2);
    CHECK(out2->entities.empty());
    CHECK(reported(report2, translate::IssueKind::UnmappedEntity, "entities off"));
}

TEST_CASE("Mobs are written for older versions: types they lack are removed, the Java format follows", "[translate][entities][targets]") {
    const auto c = to_bedrock({java_entity("minecraft:armadillo", [](NbtWriter& w) { w.write_float("Health", 12.0f); })});
    REQUIRE(c.out.entities.size() == 1);
    const auto back = to_java({c.out.entities[0],
                               bedrock_actor("minecraft:zombie", [](NbtWriter& w) {
                                   w.write_float("FallDistance", 2.0f);
                                   w.begin_list("Mainhand", TagType::Compound, 1);
                                   w.begin_compound();
                                   w.write_string("Name", "minecraft:stone_sword");
                                   w.write_byte("Count", 1);
                                   w.write_short("Damage", 0);
                                   w.end_compound();
                                   w.end_list();
                               })});
    const auto targets = translate::JavaTargets::embedded();
    REQUIRE(targets);
    const translate::JavaTarget* old = nullptr;
    for (const auto& t : (*targets)->targets())
        if (t.name == "1.20.1") old = &t;
    REQUIRE(old);
    Structure java = back.out;
    translate::ConversionReport report;
    REQUIRE(translate::rewrite_for_java_target(java, **targets, *old, *BlockState::parse("minecraft:air"), report));
    CHECK(std::none_of(java.entities.begin(), java.entities.end(), [](const Entity& e) { return e.id == "minecraft:armadillo"; }));
    CHECK(reported(report, translate::IssueKind::UnmappedEntity, "armadillo (not in Java 1.20.1)"));
    const auto zombie = doc_of(java, "minecraft:zombie");
    const NbtCompoundView z = zombie.root().entries();
    CHECK(z.find("FallDistance").as_float() == 2.0f);
    CHECK_FALSE(z.find("fall_distance"));
    CHECK_FALSE(z.find("equipment"));
    CHECK(z.find("HandItems").items().size() == 2);
    CHECK(z.find("HandItems").items().at(0).entries().find("id").as_string() == "minecraft:stone_sword");
    CHECK(z.find("HandItems").items().at(0).entries().find("Count").as_byte() == 1); // items before 1.20.5
}
