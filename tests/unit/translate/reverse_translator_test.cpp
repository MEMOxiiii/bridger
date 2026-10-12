// Bedrock -> Java translation rules (docs/adr/0008-translation-design.md, "Bedrock -> Java"):
// reverse overrides, nearest-state fallback, unmapped fallback, and the block-entity context
// rules for flower pots, banners, skulls and beds.

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace bridger;

namespace {

const translate::MappingData& mapping() { return *translate::MappingData::embedded().value(); }

// First pinned-palette state with this name that satisfies `pred` (all states if none given).
u32 bedrock_state(std::string_view name, const std::function<bool(const translate::BedrockState&)>& pred = {}) {
    const auto& m = mapping();
    for (u32 i = 0; i < m.bedrock_state_count(); ++i)
        if (m.bedrock(i).name == name && (!pred || pred(m.bedrock(i)))) return i;
    FAIL_CHECK("no Bedrock state " << name);
    return 0;
}

i32 int_prop(const translate::BedrockState& s, std::string_view key) {
    for (const auto& p : s.properties)
        if (p.key == key) return p.int_value;
    return -1;
}

Structure bedrock_structure(Extent size) {
    auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), size,
                               {Edition::Bedrock, mapping().bedrock_block_version()});
    REQUIRE(s);
    const u32 air = *mapping().find_bedrock("minecraft:air", {});
    for (u32 y = 0; y < s->sections_y(); ++y)
        for (u32 z = 0; z < s->sections_z(); ++z)
            for (u32 x = 0; x < s->sections_x(); ++x) s->section(x, y, z).blocks().fill(air);
    return std::move(*s);
}

BlockEntity be(BlockPos p, const char* id, const std::function<void(io::nbt::NbtWriter&)>& fields) {
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_string("id", id);
    fields(w);
    w.end_compound();
    return BlockEntity{p, id, RawNbt{std::move(w).finish().value(), Endian::Little}};
}

std::string at(const Structure& s, BlockPos p) { return std::string(s.block_registry().canonical(s.block(p))); }

} // namespace

TEST_CASE("Bedrock-only blocks use the reverse overrides or the fallback, and are reported", "[translate][reverse]") {
    Structure s = bedrock_structure({3, 1, 1});
    s.set_block({0, 0, 0}, bedrock_state("minecraft:hard_glass"));    // overrides/reverse_blocks.json -> glass
    s.set_block({1, 0, 0}, bedrock_state("minecraft:chemical_heat")); // no Java equivalent at all
    translate::ConversionReport report;
    auto java = translate::translate_to_java(s, mapping(), {}, report);
    REQUIRE(java);
    CHECK(at(*java, {0, 0, 0}) == "minecraft:glass");
    CHECK(at(*java, {1, 0, 0}) == "minecraft:air");
    CHECK(report.total(translate::IssueKind::LossyBlock) == 1);
    CHECK(report.total(translate::IssueKind::UnmappedBlock) == 1);

    translate::TranslateOptions marker;
    marker.fallback = translate::Fallback::Marker;
    translate::ConversionReport r2;
    auto with_marker = translate::translate_to_java(s, mapping(), marker, r2);
    REQUIRE(with_marker);
    CHECK(at(*with_marker, {1, 0, 0}) == "minecraft:sponge");
}

TEST_CASE("Flower pots, banners, skulls and beds take their Java block from the Bedrock block entity", "[translate][reverse]") {
    Structure s = bedrock_structure({4, 1, 1});
    s.set_block({0, 0, 0}, bedrock_state("minecraft:flower_pot"));
    s.block_entities.push_back(be({0, 0, 0}, "FlowerPot", [](io::nbt::NbtWriter& w) {
        w.begin_compound("PlantBlock");
        w.write_string("name", "minecraft:poppy");
        w.begin_compound("states");
        w.end_compound();
        w.end_compound();
    }));
    s.set_block({1, 0, 0}, bedrock_state("minecraft:standing_banner",
                                         [](const auto& b) { return int_prop(b, "ground_sign_direction") == 0; }));
    s.block_entities.push_back(be({1, 0, 0}, "Banner", [](io::nbt::NbtWriter& w) {
        w.write_int("Base", 15 - 14); // red (Bedrock stores 15 - dye index)
        w.begin_list("Patterns", io::nbt::TagType::Compound, 1);
        w.begin_compound();
        w.write_int("Color", 15 - 0); // white
        w.write_string("Pattern", "cre");
        w.end_compound();
        w.end_list();
    }));
    s.set_block({2, 0, 0}, bedrock_state("minecraft:skeleton_skull", [](const auto& b) { return int_prop(b, "facing_direction") == 1; }));
    s.block_entities.push_back(be({2, 0, 0}, "Skull", [](io::nbt::NbtWriter& w) {
        w.write_byte("SkullType", 4); // creeper
        w.write_float("Rotation", 90.0f);
    }));
    s.set_block({3, 0, 0}, bedrock_state("minecraft:bed", [](const auto& b) {
        for (const auto& p : b.properties)
            if (p.key == "head_piece_bit" && p.int_value == 1) return int_prop(b, "direction") == 0;
        return false;
    }));
    s.block_entities.push_back(be({3, 0, 0}, "Bed", [](io::nbt::NbtWriter& w) { w.write_byte("color", 11); })); // blue

    translate::ConversionReport report;
    auto java = translate::translate_to_java(s, mapping(), {}, report);
    REQUIRE(java);
    CHECK(at(*java, {0, 0, 0}) == "minecraft:potted_poppy");
    CHECK(at(*java, {1, 0, 0}).starts_with("minecraft:red_banner["));
    CHECK(at(*java, {2, 0, 0}).starts_with("minecraft:creeper_head["));
    CHECK(at(*java, {2, 0, 0}).find("rotation=4") != std::string::npos); // 90 / 22.5
    CHECK(at(*java, {3, 0, 0}).starts_with("minecraft:blue_bed["));

    // Java has no flower pot block entity; the banner keeps its pattern in Java form.
    bool banner_checked = false;
    for (const BlockEntity& e : java->block_entities) {
        CHECK(e.id != "FlowerPot");
        if (e.id != "minecraft:banner") continue;
        const auto doc = io::nbt::NbtDocument::parse(e.data.bytes, e.data.endian);
        REQUIRE(doc);
        const auto patterns = doc->root().entries().find("patterns").items();
        REQUIRE(patterns.size() == 1);
        CHECK(*patterns.at(0).entries().find("pattern").as_string() == "minecraft:creeper");
        CHECK(*patterns.at(0).entries().find("color").as_string() == "white");
        banner_checked = true;
    }
    CHECK(banner_checked);
}
