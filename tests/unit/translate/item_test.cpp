// Java -> Bedrock item data inside containers (src/translate/item_translator.cpp; docs/formats/bedrock-world.md,
// "Items"). Expected Bedrock values follow Chunker's resolvers, as cited in the source.

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
    std::vector<u8> bytes; // NbtDocument is a view of these
    std::optional<io::nbt::NbtDocument> doc;
    ConversionReport report;
    NbtCompoundView item(usize i) const { return doc->root().entries().find("Items").items().at(i).entries(); }
    NbtCompoundView tag(usize i) const { return item(i).find("tag").entries(); }
    bool reported(const std::string& subject) const {
        for (const IssueKind k : {IssueKind::LossyBlock, IssueKind::UnmappedBlockEntity})
            if (const auto* s = report.issues(k); s && s->contains(subject)) return true;
        return false;
    }
};

// A chest holding the items written by `fill` (each call writes one item compound's fields).
Out chest(std::initializer_list<std::function<void(NbtWriter&)>> items) {
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.begin_list("Items", TagType::Compound, items.size());
    i8 slot = 0;
    for (const auto& fill : items) {
        w.begin_compound();
        w.write_byte("Slot", slot++);
        fill(w);
        w.end_compound();
    }
    w.end_list();
    w.end_compound();
    BlockEntity java{{0, 0, 0}, "minecraft:chest", RawNbt{std::move(w).finish().value(), Endian::Big}};
    const auto state = mapping().find_java_state(*BlockState::parse("minecraft:chest[facing=north,type=single,waterlogged=false]"));
    Out out;
    const auto be = translate_block_entity(java, *state, mapping(), out.report);
    REQUIRE(be);
    out.bytes = be->data.bytes;
    out.doc = io::nbt::NbtDocument::parse(out.bytes, be->data.endian).value();
    return out;
}

void item_head(NbtWriter& w, std::string_view id, i32 count = 1) {
    w.write_string("id", id);
    w.write_int("count", count);
}

i16 ench_level(NbtCompoundView tag, i16 id) {
    for (const auto e : tag.find("ench").items())
        if (e.entries().find("id").as_short() == id) return e.entries().find("lvl").as_short().value_or(-1);
    return -1;
}

} // namespace

TEST_CASE("Item data of 1.20.5 - 1.21.4 components converts to Bedrock", "[translate][items]") {
    const Out out = chest({[](NbtWriter& w) {
        item_head(w, "minecraft:diamond_sword");
        w.begin_compound("components");
        w.begin_compound("minecraft:enchantments");
        w.begin_compound("levels");
        w.write_int("minecraft:sharpness", 5);
        w.write_int("minecraft:unbreaking", 3);
        w.end_compound();
        w.write_byte("show_in_tooltip", 1);
        w.end_compound();
        w.write_int("minecraft:damage", 120);
        w.write_int("minecraft:repair_cost", 3);
        w.write_string("minecraft:custom_name", R"({"text":"Excalibur"})");
        w.end_compound();
    }});
    CHECK(out.report.exact());
    const NbtCompoundView tag = out.tag(0);
    CHECK(ench_level(tag, 9) == 5);  // sharpness
    CHECK(ench_level(tag, 17) == 3); // unbreaking
    CHECK(tag.find("Damage").as_int() == 120);
    CHECK(tag.find("RepairCost").as_int() == 3);
    CHECK(tag.find("display").entries().find("Name").as_string() == "Excalibur");
}

TEST_CASE("Item data of 1.21.5+ components converts to Bedrock", "[translate][items]") {
    const Out out = chest({
        [](NbtWriter& w) {
            item_head(w, "minecraft:enchanted_book");
            w.begin_compound("components");
            w.begin_compound("minecraft:stored_enchantments");
            w.write_int("minecraft:mending", 1);
            w.end_compound();
            w.end_compound();
        },
        [](NbtWriter& w) {
            item_head(w, "minecraft:splash_potion");
            w.begin_compound("components");
            w.begin_compound("minecraft:potion_contents");
            w.write_string("potion", "minecraft:strong_healing");
            w.end_compound();
            w.end_compound();
        },
        [](NbtWriter& w) {
            item_head(w, "minecraft:tipped_arrow", 16);
            w.begin_compound("components");
            w.begin_compound("minecraft:potion_contents");
            w.write_string("potion", "minecraft:long_poison");
            w.end_compound();
            w.end_compound();
        },
        [](NbtWriter& w) {
            item_head(w, "minecraft:goat_horn");
            w.begin_compound("components");
            w.write_string("minecraft:instrument", "minecraft:call_goat_horn");
            w.end_compound();
        },
        [](NbtWriter& w) {
            item_head(w, "minecraft:leather_chestplate");
            w.begin_compound("components");
            w.write_int("minecraft:dyed_color", 0x3366CC);
            w.begin_compound("minecraft:trim");
            w.write_string("material", "minecraft:gold");
            w.write_string("pattern", "minecraft:coast");
            w.end_compound();
            w.end_compound();
        },
    });
    CHECK(out.report.exact());
    CHECK(ench_level(out.tag(0), 26) == 1); // mending
    CHECK(out.item(1).find("Name").as_string() == "minecraft:splash_potion");
    CHECK(out.item(1).find("Damage").as_short() == i16{22}); // strong healing
    CHECK_FALSE(out.item(1).find("tag"));
    CHECK(out.item(2).find("Name").as_string() == "minecraft:arrow");
    CHECK(out.item(2).find("Damage").as_short() == i16{27}); // long poison (26) + 1
    CHECK(out.item(3).find("Damage").as_short() == i16{5});  // call
    CHECK(out.tag(4).find("customColor").as_int() == static_cast<i32>(0xFF3366CCu));
    CHECK(out.tag(4).find("Trim").entries().find("Material").as_string() == "gold");
    CHECK(out.tag(4).find("Trim").entries().find("Pattern").as_string() == "coast");
}

TEST_CASE("Shulker box contents and books keep their data", "[translate][items]") {
    const Out out = chest({
        [](NbtWriter& w) {
            item_head(w, "minecraft:purple_shulker_box");
            w.begin_compound("components");
            w.begin_list("minecraft:container", TagType::Compound, 1);
            w.begin_compound();
            w.write_int("slot", 7);
            w.begin_compound("item");
            item_head(w, "minecraft:netherite_pickaxe");
            w.begin_compound("components");
            w.begin_compound("minecraft:enchantments");
            w.begin_compound("levels");
            w.write_int("minecraft:efficiency", 5);
            w.end_compound();
            w.end_compound();
            w.end_compound();
            w.end_compound();
            w.end_compound();
            w.end_list();
            w.end_compound();
        },
        [](NbtWriter& w) {
            item_head(w, "minecraft:written_book");
            w.begin_compound("components");
            w.begin_compound("minecraft:written_book_content");
            w.begin_list("pages", TagType::Compound, 2);
            w.begin_compound();
            w.write_string("raw", R"({"text":"Page one"})");
            w.end_compound();
            w.begin_compound();
            w.write_string("raw", R"("Page two")");
            w.end_compound();
            w.end_list();
            w.begin_compound("title");
            w.write_string("raw", "Rules");
            w.end_compound();
            w.write_string("author", "Admin");
            w.end_compound();
            w.end_compound();
        },
        [](NbtWriter& w) { item_head(w, "minecraft:chain", 12); },
    });
    CHECK(out.report.exact());
    const auto inner = out.tag(0).find("Items").items();
    REQUIRE(inner.size() == 1);
    const NbtCompoundView pick = inner.at(0).entries();
    CHECK(pick.find("Name").as_string() == "minecraft:netherite_pickaxe");
    CHECK(pick.find("Slot").as_byte() == i8{7});
    CHECK(ench_level(pick.find("tag").entries(), 15) == 5); // efficiency
    const NbtCompoundView book = out.tag(1);
    REQUIRE(book.find("pages").items().size() == 2);
    CHECK(book.find("pages").items().at(0).entries().find("text").as_string() == "Page one");
    CHECK(book.find("pages").items().at(1).entries().find("text").as_string() == "Page two");
    CHECK(book.find("title").as_string() == "Rules");
    CHECK(book.find("author").as_string() == "Admin");
    // "chain" (before 1.21.9) is the pinned version's iron_chain.
    CHECK(out.item(2).find("Name").as_string() == "minecraft:iron_chain");
    CHECK(out.item(2).find("Count").as_byte() == i8{12});
}

TEST_CASE("Item data of the pre-1.20.5 tag converts to Bedrock", "[translate][items]") {
    const Out out = chest({[](NbtWriter& w) {
        w.write_string("id", "minecraft:shulker_box");
        w.write_byte("Count", 1);
        w.begin_compound("tag");
        w.begin_compound("display");
        w.write_string("Name", R"({"text":"Kit"})");
        w.end_compound();
        w.begin_compound("BlockEntityTag");
        w.begin_list("Items", TagType::Compound, 1);
        w.begin_compound();
        w.write_byte("Slot", 0);
        w.write_string("id", "minecraft:bow");
        w.write_byte("Count", 1);
        w.begin_compound("tag");
        w.write_int("Damage", 9);
        w.begin_list("Enchantments", TagType::Compound, 1);
        w.begin_compound();
        w.write_string("id", "minecraft:infinity");
        w.write_short("lvl", 1);
        w.end_compound();
        w.end_list();
        w.end_compound();
        w.end_compound();
        w.end_list();
        w.end_compound();
        w.end_compound();
    }});
    CHECK(out.report.exact());
    CHECK(out.tag(0).find("display").entries().find("Name").as_string() == "Kit");
    const NbtCompoundView bow = out.tag(0).find("Items").items().at(0).entries();
    CHECK(bow.find("Name").as_string() == "minecraft:bow");
    CHECK(bow.find("tag").entries().find("Damage").as_int() == 9);
    CHECK(ench_level(bow.find("tag").entries(), 22) == 1); // infinity
}

TEST_CASE("Item data Bedrock cannot hold is reported by name", "[translate][items]") {
    const Out out = chest({[](NbtWriter& w) {
        item_head(w, "minecraft:diamond_sword");
        w.begin_compound("components");
        w.begin_compound("minecraft:enchantments");
        w.write_int("minecraft:sweeping_edge", 3);
        w.write_int("minecraft:sharpness", 1);
        w.end_compound();
        w.begin_compound("minecraft:custom_data");
        w.write_int("plugin", 1);
        w.end_compound();
        w.end_compound();
    }});
    CHECK_FALSE(out.report.exact());
    CHECK(out.reported("item data not converted: enchantment minecraft:sweeping_edge"));
    CHECK(out.reported("item data not converted: minecraft:custom_data"));
    CHECK(ench_level(out.tag(0), 9) == 1); // the rest is kept
}

TEST_CASE("Fireworks keep their flight and explosions", "[translate][items]") {
    const Out out = chest({[](NbtWriter& w) {
        item_head(w, "minecraft:firework_rocket", 3);
        w.begin_compound("components");
        w.begin_compound("minecraft:fireworks");
        w.write_byte("flight_duration", 2);
        w.begin_list("explosions", TagType::Compound, 1);
        w.begin_compound();
        w.write_string("shape", "creeper");
        const std::array<i32, 1> red{0xB3312C};
        w.write_int_array("colors", red);
        w.write_byte("has_trail", 1);
        w.end_compound();
        w.end_list();
        w.end_compound();
        w.end_compound();
    }});
    CHECK(out.report.exact());
    const NbtCompoundView fw = out.tag(0).find("Fireworks").entries();
    CHECK(fw.find("Flight").as_byte() == i8{2});
    const NbtCompoundView e = fw.find("Explosions").items().at(0).entries();
    CHECK(e.find("FireworkType").as_byte() == i8{3});
    const auto colors = e.find("FireworkColor").as_byte_array();
    REQUIRE(colors);
    REQUIRE(colors->size() == 1);
    CHECK((*colors)[0] == 1); // red: dye index 14, reversed 15 - 14
    CHECK(e.find("FireworkTrail").as_byte() == i8{1});
}
