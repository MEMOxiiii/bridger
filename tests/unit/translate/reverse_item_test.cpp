// Bedrock -> Java item data and block entities (item_translator.cpp, reverse_block_entities.cpp), checked by going
// Java -> Bedrock -> Java, and their older Java forms (java_block_entity_downgrade.cpp through rewrite_for_java_target).

#include <bridger/core/block_registry.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/targets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace bridger;
using namespace bridger::translate;
using io::nbt::NbtCompoundView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

const MappingData& mapping() { return **MappingData::embedded(); }

struct Doc {
    std::vector<u8> bytes;
    std::optional<io::nbt::NbtDocument> doc;
    NbtCompoundView root() const { return doc->root().entries(); }
};

Doc parse(const RawNbt& raw) {
    Doc d;
    d.bytes = raw.bytes;
    d.doc = io::nbt::NbtDocument::parse(d.bytes, raw.endian).value();
    return d;
}

// A Java block entity through Bedrock and back: {Bedrock, Java} block entities and the report.
struct Trip {
    BlockEntity bedrock, java;
    ConversionReport report;
};

Trip round_trip(std::string_view java_id, std::string_view state, const std::function<void(NbtWriter&)>& fill) {
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    fill(w);
    w.end_compound();
    const BlockEntity java{{1, 2, 3}, std::string(java_id), RawNbt{std::move(w).finish().value(), Endian::Big}};
    const auto st = mapping().find_java_state(*BlockState::parse(state));
    REQUIRE(st);
    Trip t;
    auto b = translate_block_entity(java, *st, mapping(), t.report);
    REQUIRE(b);
    t.bedrock = std::move(*b);
    auto j = translate_block_entity_to_java(t.bedrock, *st, mapping(), t.report);
    REQUIRE(j);
    t.java = std::move(*j);
    return t;
}

void item(NbtWriter& w, std::string_view id, i32 count, const std::function<void(NbtWriter&)>& components = {}) {
    w.write_string("id", id);
    w.write_int("count", count);
    if (components) {
        w.begin_compound("components");
        components(w);
        w.end_compound();
    }
}

constexpr std::string_view kChest = "minecraft:chest[facing=north,type=single,waterlogged=false]";

void chest_items(NbtWriter& w) {
    w.begin_list("Items", TagType::Compound, 5);
    w.begin_compound();
    w.write_byte("Slot", 0);
    item(w, "minecraft:diamond_sword", 1, [](NbtWriter& c) {
        c.begin_compound("minecraft:enchantments");
        c.write_int("minecraft:sharpness", 5);
        c.end_compound();
        c.write_int("minecraft:damage", 40);
        c.write_string("minecraft:custom_name", "Blade");
    });
    w.end_compound();
    w.begin_compound();
    w.write_byte("Slot", 1);
    item(w, "minecraft:splash_potion", 1, [](NbtWriter& c) {
        c.begin_compound("minecraft:potion_contents");
        c.write_string("potion", "minecraft:strong_healing");
        c.end_compound();
    });
    w.end_compound();
    w.begin_compound();
    w.write_byte("Slot", 2);
    item(w, "minecraft:tipped_arrow", 8, [](NbtWriter& c) {
        c.begin_compound("minecraft:potion_contents");
        c.write_string("potion", "minecraft:long_poison");
        c.end_compound();
    });
    w.end_compound();
    w.begin_compound();
    w.write_byte("Slot", 3);
    item(w, "minecraft:red_shulker_box", 1, [](NbtWriter& c) {
        c.begin_list("minecraft:container", TagType::Compound, 1);
        c.begin_compound();
        c.write_int("slot", 4);
        c.begin_compound("item");
        item(c, "minecraft:enchanted_book", 1, [](NbtWriter& b) {
            b.begin_compound("minecraft:stored_enchantments");
            b.write_int("minecraft:mending", 1);
            b.end_compound();
        });
        c.end_compound();
        c.end_compound();
        c.end_list();
    });
    w.end_compound();
    w.begin_compound();
    w.write_byte("Slot", 4);
    item(w, "minecraft:goat_horn", 1, [](NbtWriter& c) { c.write_string("minecraft:instrument", "minecraft:feel_goat_horn"); });
    w.end_compound();
    w.end_list();
}

NbtCompoundView slot(NbtCompoundView be, i8 s) {
    for (const auto it : be.find("Items").items())
        if (it.entries().find("Slot").as_byte() == s) return it.entries();
    return io::nbt::NbtView().entries();
}

} // namespace

TEST_CASE("Item data survives Java -> Bedrock -> Java", "[translate][items][reverse]") {
    Trip t = round_trip("minecraft:chest", kChest, chest_items);
    CHECK(t.report.exact());
    const Doc j = parse(t.java.data);
    const auto sword = slot(j.root(), 0);
    CHECK(sword.find("id").as_string() == "minecraft:diamond_sword");
    const auto comps = sword.find("components").entries();
    CHECK(comps.find("minecraft:enchantments").entries().find("minecraft:sharpness").as_int() == 5);
    CHECK(comps.find("minecraft:damage").as_int() == 40);
    CHECK(comps.find("minecraft:custom_name").as_string() == "Blade");
    CHECK(slot(j.root(), 1).find("components").entries().find("minecraft:potion_contents").entries().find("potion").as_string() ==
          "minecraft:strong_healing");
    const auto arrow = slot(j.root(), 2);
    CHECK(arrow.find("id").as_string() == "minecraft:tipped_arrow");
    CHECK(arrow.find("count").as_int() == 8);
    CHECK(arrow.find("components").entries().find("minecraft:potion_contents").entries().find("potion").as_string() == "minecraft:long_poison");
    const auto inner = slot(j.root(), 3).find("components").entries().find("minecraft:container").items();
    REQUIRE(inner.size() == 1);
    CHECK(inner.at(0).entries().find("slot").as_int() == 4);
    CHECK(inner.at(0).entries().find("item").entries().find("components").entries().find("minecraft:stored_enchantments").entries().find(
              "minecraft:mending").as_int() == 1);
    CHECK(slot(j.root(), 4).find("components").entries().find("minecraft:instrument").as_string() == "minecraft:feel_goat_horn");
}

TEST_CASE("Block entities survive Java -> Bedrock -> Java", "[translate][blockentities][reverse]") {
    {
        Trip t = round_trip("minecraft:mob_spawner", "minecraft:spawner", [](NbtWriter& w) {
            w.write_short("Delay", 33);
            w.begin_compound("SpawnData");
            w.begin_compound("entity");
            w.write_string("id", "minecraft:zombified_piglin");
            w.end_compound();
            w.end_compound();
        });
        const Doc j = parse(t.java.data);
        CHECK(t.java.id == "minecraft:mob_spawner");
        CHECK(j.root().find("SpawnData").entries().find("entity").entries().find("id").as_string() == "minecraft:zombified_piglin");
        CHECK(j.root().find("Delay").as_short() == i16{33});
    }
    {
        Trip t = round_trip("minecraft:chest", kChest, [](NbtWriter& w) { w.write_string("LootTable", "minecraft:chests/shipwreck_treasure"); });
        CHECK(parse(t.java.data).root().find("LootTable").as_string() == "minecraft:chests/shipwreck_treasure");
    }
    {
        Trip t = round_trip("minecraft:brewing_stand", "minecraft:brewing_stand[has_bottle_0=true,has_bottle_1=false,has_bottle_2=false]",
                            [](NbtWriter& w) {
                                w.begin_list("Items", TagType::Compound, 2);
                                w.begin_compound();
                                w.write_byte("Slot", 0);
                                item(w, "minecraft:potion", 1);
                                w.end_compound();
                                w.begin_compound();
                                w.write_byte("Slot", 3);
                                item(w, "minecraft:nether_wart", 1);
                                w.end_compound();
                                w.end_list();
                                w.write_byte("Fuel", 7);
                            });
        const Doc j = parse(t.java.data);
        CHECK(slot(j.root(), 0).find("id").as_string() == "minecraft:potion");
        CHECK(slot(j.root(), 3).find("id").as_string() == "minecraft:nether_wart");
        CHECK(j.root().find("Fuel").as_int() == 7);
    }
    {
        Trip t = round_trip("minecraft:decorated_pot", "minecraft:decorated_pot[cracked=false,facing=north,waterlogged=false]", [](NbtWriter& w) {
            w.begin_list("sherds", TagType::String, 4);
            for (const char* s : {"minecraft:brick", "minecraft:arms_up_pottery_sherd", "minecraft:brick", "minecraft:brick"}) w.write_string({}, s);
            w.end_list();
        });
        CHECK(parse(t.java.data).root().find("sherds").entries().find("left").entries().find("id").as_string() == "minecraft:arms_up_pottery_sherd");
    }
    {
        Trip t = round_trip("minecraft:beacon", "minecraft:beacon", [](NbtWriter& w) { w.write_string("primary_effect", "minecraft:haste"); });
        CHECK(parse(t.java.data).root().find("primary_effect").as_string() == "minecraft:haste");
    }
    {
        Trip t = round_trip("minecraft:crafter", "minecraft:crafter[crafting=false,orientation=north_up,triggered=false]", [](NbtWriter& w) {
            const std::array<i32, 2> slots{2, 7};
            w.write_int_array("disabled_slots", slots);
        });
        const Doc j = parse(t.java.data); // the array view below points into it
        const auto slots = j.root().find("disabled_slots").as_int_array();
        REQUIRE(slots);
        REQUIRE(slots->size() == 2);
        CHECK((*slots)[0] == 2);
        CHECK((*slots)[1] == 7);
    }
    {
        Trip t = round_trip("minecraft:sculk_catalyst", "minecraft:sculk_catalyst[bloom=false]", [](NbtWriter&) {});
        CHECK(t.java.id == "minecraft:sculk_catalyst");
    }
}

TEST_CASE("Items take the shape of older Java versions", "[translate][items][targets]") {
    const JavaTargets& targets = **JavaTargets::embedded();
    const BlockState fallback("minecraft:air", {});
    const auto chest_for = [&](std::string_view version) {
        auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), {1, 1, 1}, {Edition::Java, 5023}).value();
        s.set_block({0, 0, 0}, s.block_registry().intern(*BlockState::parse(kChest)));
        NbtWriter w(Endian::Big);
        w.begin_root_compound();
        chest_items(w);
        w.end_compound();
        s.block_entities.push_back({{0, 0, 0}, "minecraft:chest", RawNbt{std::move(w).finish().value(), Endian::Big}});
        ConversionReport report;
        REQUIRE(rewrite_for_java_target(s, targets, *targets.find(version), fallback, report));
        return parse(s.block_entities.at(0).data);
    };
    // 1.21.4: enchantments inside "levels", the name as JSON.
    {
        const Doc d = chest_for("1.21.4");
        const auto comps = slot(d.root(), 0).find("components").entries();
        CHECK(comps.find("minecraft:enchantments").entries().find("levels").entries().find("minecraft:sharpness").as_int() == 5);
        CHECK(comps.find("minecraft:custom_name").as_string() == R"("Blade")");
    }
    // 1.20.1: the old tag {Count, tag: {Enchantments, Damage, display, Potion, BlockEntityTag.Items}}.
    {
        const Doc d = chest_for("1.20.1");
        const auto sword = slot(d.root(), 0);
        CHECK(sword.find("Count").as_byte() == i8{1});
        const auto tag = sword.find("tag").entries();
        CHECK(tag.find("Damage").as_int() == 40);
        CHECK(tag.find("Enchantments").items().at(0).entries().find("id").as_string() == "minecraft:sharpness");
        CHECK(tag.find("display").entries().find("Name").as_string() == R"("Blade")");
        CHECK(slot(d.root(), 1).find("tag").entries().find("Potion").as_string() == "minecraft:strong_healing");
        const auto box = slot(d.root(), 3).find("tag").entries().find("BlockEntityTag").entries().find("Items").items();
        REQUIRE(box.size() == 1);
        CHECK(box.at(0).entries().find("Slot").as_byte() == i8{4});
        CHECK(box.at(0).entries().find("tag").entries().find("StoredEnchantments").items().size() == 1);
    }
}
