// Java -> Bedrock block entities beyond containers (src/translate/block_entity_translator.cpp; docs/formats/
// bedrock-world.md, "Block entities"). Expected Bedrock values follow Chunker's handlers, as cited in the source.

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

const MappingData& mapping() {
    static const MappingData* m = *MappingData::embedded();
    return *m;
}

struct Out {
    std::string id;
    std::vector<u8> bytes; // NbtDocument is a view of these
    std::optional<io::nbt::NbtDocument> doc;
    ConversionReport report;
    NbtCompoundView root() const { return doc->root().entries(); }
    bool reported(const std::string& subject) const {
        const auto* s = report.issues(IssueKind::LossyBlock);
        return s && s->contains(subject);
    }
};

Out convert_be(std::string_view java_id, std::string_view block_state, const std::function<void(NbtWriter&)>& fill) {
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    fill(w);
    w.end_compound();
    BlockEntity java{{0, 0, 0}, std::string(java_id), RawNbt{std::move(w).finish().value(), Endian::Big}};
    const auto state = mapping().find_java_state(*BlockState::parse(block_state));
    REQUIRE(state);
    Out out;
    const auto be = translate_block_entity(java, *state, mapping(), out.report);
    REQUIRE(be);
    out.id = be->id;
    out.bytes = be->data.bytes;
    out.doc = io::nbt::NbtDocument::parse(out.bytes, be->data.endian).value();
    return out;
}

void item(NbtWriter& w, std::string_view name, std::string_view id, i8 slot = -1) {
    w.begin_compound(name);
    if (slot >= 0) w.write_byte("Slot", slot);
    w.write_string("id", id);
    w.write_int("count", 1);
    w.end_compound();
}

} // namespace

TEST_CASE("Spawners keep their mob and timings", "[translate][blockentities]") {
    const Out out = convert_be("minecraft:mob_spawner", "minecraft:spawner", [](NbtWriter& w) {
        w.write_short("Delay", 40);
        w.write_short("MaxNearbyEntities", 6);
        w.begin_compound("SpawnData");
        w.begin_compound("entity");
        w.write_string("id", "minecraft:zombified_piglin");
        w.end_compound();
        w.end_compound();
    });
    CHECK(out.id == "MobSpawner");
    CHECK(out.report.exact());
    CHECK(out.root().find("EntityIdentifier").as_string() == "minecraft:zombie_pigman");
    CHECK(out.root().find("Delay").as_short() == i16{40});
    CHECK(out.root().find("MinSpawnDelay").as_short() == i16{200}); // Java default when absent
}

TEST_CASE("Unopened loot chests keep their loot table", "[translate][blockentities]") {
    const Out out = convert_be("minecraft:chest", "minecraft:chest[facing=north,type=single,waterlogged=false]", [](NbtWriter& w) {
        w.write_string("LootTable", "minecraft:chests/shipwreck_supply");
        w.write_long("LootTableSeed", 42);
    });
    CHECK(out.report.exact());
    CHECK(out.root().find("LootTable").as_string() == "loot_tables/chests/shipwrecksupply.json");

    const Out sand = convert_be("minecraft:brushable_block", "minecraft:suspicious_sand[dusted=0]", [](NbtWriter& w) {
        w.write_string("LootTable", "minecraft:archaeology/desert_pyramid");
    });
    CHECK(sand.id == "BrushableBlock");
    CHECK(sand.report.exact());
    CHECK(sand.root().find("type").as_string() == "minecraft:suspicious_sand");
    CHECK(sand.root().find("LootTable").as_string() == "loot_tables/archaeology/desert_pyramid.json");
}

TEST_CASE("Brewing stands, bookshelves and crafters keep their items", "[translate][blockentities]") {
    const Out brew = convert_be("minecraft:brewing_stand",
                               "minecraft:brewing_stand[has_bottle_0=true,has_bottle_1=false,has_bottle_2=false]", [](NbtWriter& w) {
                                   w.begin_list("Items", TagType::Compound, 3);
                                   item(w, {}, "minecraft:potion", 0);
                                   item(w, {}, "minecraft:nether_wart", 3);
                                   item(w, {}, "minecraft:blaze_powder", 4);
                                   w.end_list();
                                   w.write_byte("Fuel", 15);
                                   w.write_short("BrewTime", 120);
                               });
    CHECK(brew.id == "BrewingStand");
    CHECK(brew.report.exact());
    const auto items = brew.root().find("Items").items();
    REQUIRE(items.size() == 3);
    CHECK(items.at(0).entries().find("Slot").as_byte() == i8{1}); // bottle 0 -> 1
    CHECK(items.at(1).entries().find("Slot").as_byte() == i8{0}); // ingredient 3 -> 0
    CHECK(items.at(2).entries().find("Slot").as_byte() == i8{4}); // fuel stays
    CHECK(brew.root().find("FuelAmount").as_short() == i16{15});
    CHECK(brew.root().find("FuelTotal").as_short() == i16{20});
    CHECK(brew.root().find("CookTime").as_short() == i16{120});

    const Out shelf = convert_be("minecraft:chiseled_bookshelf",
                                "minecraft:chiseled_bookshelf[facing=north,slot_0_occupied=true,slot_1_occupied=false,slot_2_occupied=false,"
                                "slot_3_occupied=false,slot_4_occupied=false,slot_5_occupied=false]",
                                [](NbtWriter& w) {
                                    w.begin_list("Items", TagType::Compound, 1);
                                    item(w, {}, "minecraft:enchanted_book", 0);
                                    w.end_list();
                                });
    CHECK(shelf.id == "ChiseledBookshelf");
    CHECK(shelf.root().find("Items").items().size() == 1);

    const Out crafter = convert_be("minecraft:crafter", "minecraft:crafter[crafting=false,orientation=north_up,triggered=false]",
                                  [](NbtWriter& w) {
                                      const std::array<i32, 2> slots{0, 4};
                                      w.write_int_array("disabled_slots", slots);
                                  });
    CHECK(crafter.id == "Crafter");
    CHECK(crafter.root().find("disabled_slots").as_short() == i16{0b10001});
}

TEST_CASE("Decorated pots, campfires, lecterns and jukeboxes keep what they hold", "[translate][blockentities]") {
    const Out pot = convert_be("minecraft:decorated_pot", "minecraft:decorated_pot[cracked=false,facing=north,waterlogged=false]",
                              [](NbtWriter& w) {
                                  w.begin_list("sherds", TagType::String, 4);
                                  w.write_string({}, "minecraft:brick");
                                  w.write_string({}, "minecraft:angler_pottery_sherd");
                                  w.write_string({}, "minecraft:brick");
                                  w.write_string({}, "minecraft:skull_pottery_sherd");
                                  w.end_list();
                                  item(w, "item", "minecraft:diamond");
                              });
    CHECK(pot.report.exact());
    const auto sherds = pot.root().find("sherds").items();
    REQUIRE(sherds.size() == 4);
    CHECK(sherds.at(1).as_string() == "minecraft:angler_pottery_sherd");
    CHECK(sherds.at(3).as_string() == "minecraft:skull_pottery_sherd");
    CHECK(pot.root().find("item").entries().find("Name").as_string() == "minecraft:diamond");

    const Out fire = convert_be("minecraft:campfire", "minecraft:campfire[facing=north,lit=true,signal_fire=false,waterlogged=false]",
                               [](NbtWriter& w) {
                                   w.begin_list("Items", TagType::Compound, 1);
                                   item(w, {}, "minecraft:beef", 2);
                                   w.end_list();
                                   const std::array<i32, 4> times{0, 0, 250, 0};
                                   w.write_int_array("CookingTimes", times);
                               });
    CHECK(fire.report.exact());
    CHECK(fire.root().find("Item3").entries().find("Name").as_string() == "minecraft:beef");
    CHECK(fire.root().find("ItemTime3").as_int() == 250);

    const Out lectern = convert_be("minecraft:lectern", "minecraft:lectern[facing=north,has_book=true,powered=false]", [](NbtWriter& w) {
        w.begin_compound("Book");
        w.write_string("id", "minecraft:writable_book");
        w.write_int("count", 1);
        w.begin_compound("components");
        w.begin_compound("minecraft:writable_book_content");
        w.begin_list("pages", TagType::Compound, 3);
        for (int i = 0; i < 3; ++i) {
            w.begin_compound();
            w.write_string("raw", "text");
            w.end_compound();
        }
        w.end_list();
        w.end_compound();
        w.end_compound();
        w.end_compound();
        w.write_int("Page", 2);
    });
    CHECK(lectern.report.exact());
    CHECK(lectern.root().find("hasBook").as_byte() == i8{1});
    CHECK(lectern.root().find("totalPages").as_int() == 3);
    CHECK(lectern.root().find("page").as_int() == 1);
    CHECK(lectern.root().find("book").entries().find("tag").entries().find("pages").items().size() == 3);

    const Out jukebox = convert_be("minecraft:jukebox", "minecraft:jukebox[has_record=true]",
                                  [](NbtWriter& w) { item(w, "RecordItem", "minecraft:music_disc_cat"); });
    CHECK(jukebox.report.exact());
    CHECK(jukebox.root().find("RecordItem").entries().find("Name").as_string() == "minecraft:music_disc_cat");
}

TEST_CASE("Command blocks, beacons and data-less block entities", "[translate][blockentities]") {
    const Out cmd = convert_be("minecraft:command_block", "minecraft:command_block[conditional=false,facing=north]", [](NbtWriter& w) {
        w.write_string("Command", "say hello");
        w.write_byte("auto", 1);
    });
    CHECK(cmd.id == "CommandBlock");
    CHECK(cmd.root().find("Command").as_string() == "say hello");
    CHECK(cmd.root().find("auto").as_byte() == i8{1});
    CHECK(cmd.reported("command block: check the command in Bedrock")); // syntax differs between editions

    const Out beacon = convert_be("minecraft:beacon", "minecraft:beacon", [](NbtWriter& w) {
        w.write_string("primary_effect", "minecraft:haste");
        w.write_string("secondary_effect", "minecraft:regeneration");
    });
    CHECK(beacon.report.exact());
    CHECK(beacon.root().find("primary").as_int() == 3);
    CHECK(beacon.root().find("secondary").as_int() == 10);

    for (const auto& [java, state, bedrock] :
         {std::tuple{"minecraft:sculk_sensor", "minecraft:sculk_sensor[power=0,sculk_sensor_phase=inactive,waterlogged=false]", "SculkSensor"},
          std::tuple{"minecraft:sculk_catalyst", "minecraft:sculk_catalyst[bloom=false]", "SculkCatalyst"},
          std::tuple{"minecraft:end_portal", "minecraft:end_portal", "EndPortal"}}) {
        const Out out = convert_be(java, state, [](NbtWriter&) {});
        CHECK(out.id == bedrock);
        CHECK(out.report.exact());
    }

    const Out hive = convert_be("minecraft:beehive", "minecraft:beehive[facing=north,honey_level=0]", [](NbtWriter& w) {
        w.begin_list("bees", TagType::Compound, 2);
        w.begin_compound();
        w.end_compound();
        w.begin_compound();
        w.end_compound();
        w.end_list();
    });
    CHECK(hive.id == "Beehive");
    CHECK(hive.reported("bees in a beehive not converted (entities)"));
}
