// Mapping data and Java -> Bedrock translation. Expected Bedrock states were derived
// from the sources (Mojang's blocks report + GeyserMC mappings), not from Bridger.

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;
using namespace bridger::translate;

namespace {

const MappingData& mapping() {
    auto m = MappingData::embedded();
    REQUIRE(m);
    return **m;
}

const BedrockState& bedrock_of(std::string_view java) {
    const auto id = mapping().find_java_state(*BlockState::parse(java));
    REQUIRE(id);
    const JavaStateMapping m = mapping().map(*id);
    REQUIRE(m.bedrock != UINT32_MAX);
    return mapping().bedrock(m.bedrock);
}

std::string prop(const BedrockState& s, std::string_view key) {
    for (const auto& p : s.properties)
        if (p.key == key) return p.type == NbtValueType::String ? std::string(p.str_value) : std::to_string(p.int_value);
    return "<missing>";
}

io::nbt::NbtDocument parse(const RawNbt& raw) {
    auto doc = io::nbt::NbtDocument::parse(raw.bytes, raw.endian);
    REQUIRE(doc);
    return *doc;
}

} // namespace

TEST_CASE("Embedded mapping data matches the pinned versions", "[translate]") {
    const MappingData& m = mapping();
    CHECK(m.java_version() == "26.3");
    CHECK(m.java_data_version() == 5023);
    CHECK(m.bedrock_version() == "26.50");
    CHECK(m.bedrock_block_version() == 18168865);
    CHECK(m.java_state_count() == 35723);
    CHECK(m.bedrock_state_count() == 22091);
}

TEST_CASE("Java states map to the source-defined Bedrock states", "[translate]") {
    const auto& stairs = bedrock_of("minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]");
    CHECK(stairs.name == "minecraft:oak_stairs");
    CHECK(prop(stairs, "weirdo_direction") == "0");
    CHECK(prop(stairs, "upside_down_bit") == "0");

    CHECK(prop(bedrock_of("minecraft:oak_log[axis=x]"), "pillar_axis") == "x");
    const auto& bed = bedrock_of("minecraft:red_bed[facing=south,occupied=false,part=head]");
    CHECK(bed.name == "minecraft:bed");
    CHECK(prop(bed, "head_piece_bit") == "1");
    CHECK(bedrock_of("minecraft:water[level=0]").name == "minecraft:water");
}

TEST_CASE("Waterlogged states are flagged for Bedrock layer 2", "[translate]") {
    const auto wet = mapping().find_java_state(*BlockState::parse("minecraft:chest[facing=south,type=single,waterlogged=true]"));
    const auto dry = mapping().find_java_state(*BlockState::parse("minecraft:chest[facing=south,type=single,waterlogged=false]"));
    REQUIRE(wet);
    REQUIRE(dry);
    CHECK((mapping().map(*wet).flags & kFlagWaterlogged) != 0);
    CHECK((mapping().map(*dry).flags & kFlagWaterlogged) == 0);
    CHECK(mapping().map(*wet).bedrock == mapping().map(*dry).bedrock); // same block, water lives in layer 2
}

TEST_CASE("Java state lookup: defaults, unknowns, round-trip", "[translate]") {
    const MappingData& m = mapping();
    bool missing = false;
    const auto stone = m.find_java_state(*BlockState::parse("minecraft:stone"), &missing);
    REQUIRE(stone);
    CHECK_FALSE(missing);
    CHECK(m.java_state_string(*stone) == "minecraft:stone");

    // Missing properties take the block's default state.
    const auto stairs = m.find_java_state(*BlockState::parse("minecraft:oak_stairs[facing=west]"), &missing);
    REQUIRE(stairs);
    CHECK(missing);
    CHECK(m.java_state_string(*stairs) == "minecraft:oak_stairs[facing=west,half=bottom,shape=straight,waterlogged=false]");

    CHECK_FALSE(m.find_java_state(*BlockState::parse("examplemod:machine")));
    CHECK_FALSE(m.find_java_state(*BlockState::parse("minecraft:oak_stairs[facing=up]")));
    CHECK_FALSE(m.find_java_state(*BlockState::parse("minecraft:stone[color=red]")));
}

TEST_CASE("Items and biomes", "[translate]") {
    const auto diamond = mapping().find_item("minecraft:diamond");
    REQUIRE(diamond);
    CHECK(diamond->bedrock_name == "minecraft:diamond");
    CHECK_FALSE(diamond->java_block_state);
    const auto log = mapping().find_item("minecraft:oak_log");
    REQUIRE(log);
    REQUIRE(log->java_block_state);
    CHECK(mapping().find_biome("minecraft:plains") == 1);
    CHECK_FALSE(mapping().find_item("examplemod:widget"));
}

TEST_CASE("Translation: palette stage, waterlogging, context rules and report", "[translate]") {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    auto java = Structure::create(blocks, biomes, {20, 2, 3}, {Edition::Java, 5023});
    REQUIRE(java);
    auto set = [&](BlockPos p, std::string_view s) { java->set_block(p, blocks->intern(*BlockState::parse(s))); };
    set({0, 0, 0}, "minecraft:chest[facing=south,type=single,waterlogged=true]");
    set({2, 0, 0}, "minecraft:chest[facing=north,type=left,waterlogged=false]");
    set({3, 0, 0}, "minecraft:chest[facing=north,type=right,waterlogged=false]");
    set({5, 0, 1}, "minecraft:white_bed[facing=south,occupied=false,part=foot]");
    set({5, 0, 2}, "minecraft:white_bed[facing=south,occupied=false,part=head]");
    set({7, 0, 0}, "examplemod:machine[on=true]");
    set({18, 1, 2}, "minecraft:stone"); // second section along x
    java->entities.push_back(Entity{{1.5, 1, 1.5}, "minecraft:armor_stand", {}});

    ConversionReport report;
    auto out = translate_to_bedrock(*java, mapping(), {}, report);
    REQUIRE(out);
    CHECK(out->source().edition == Edition::Bedrock);

    const MappingData& m = mapping();
    CHECK(m.bedrock(out->block({0, 0, 0})).name == "minecraft:chest");
    CHECK(m.bedrock(out->secondary({0, 0, 0})).name == "minecraft:water"); // waterlogged -> layer 2
    CHECK(m.bedrock(out->secondary({2, 0, 0})).name == "minecraft:air");
    CHECK(m.bedrock(out->block({18, 1, 2})).name == "minecraft:stone");
    CHECK(m.bedrock(out->block({7, 0, 0})).name == "minecraft:air"); // fallback

    CHECK(report.total(IssueKind::UnmappedBlock) == 1);
    CHECK(report.total(IssueKind::UnmappedEntity) == 1);
    REQUIRE(report.issues(IssueKind::UnmappedBlock) != nullptr);
    CHECK(report.issues(IssueKind::UnmappedBlock)->contains("examplemod:machine[on=true]"));

    // Synthesized block entities: chest pairing (north facing: left half pairs with x+1) and bed color.
    const BlockEntity* left = nullptr;
    const BlockEntity* bed = nullptr;
    usize chests = 0;
    for (const auto& be : out->block_entities) {
        if (be.pos == BlockPos{2, 0, 0}) left = &be;
        if (be.pos == BlockPos{5, 0, 2}) bed = &be;
        if (be.id == "Chest") ++chests;
    }
    CHECK(chests == 3);
    REQUIRE(left != nullptr);
    {
        const auto doc = parse(left->data);
        const auto c = doc.root().entries();
        CHECK(c.find("pairx").as_int() == 3);
        CHECK(c.find("pairz").as_int() == 0);
        CHECK_FALSE(c.find("pairlead")); // left half is not the lead
    }
    REQUIRE(bed != nullptr);
    CHECK(bed->id == "Bed");
    CHECK(parse(bed->data).root().entries().find("color").as_byte() == i8{0}); // white
}

TEST_CASE("Marker fallback makes unmapped blocks visible", "[translate]") {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto java = Structure::create(blocks, std::make_shared<BiomeRegistry>(), {1, 1, 1}, {Edition::Java, 5023});
    REQUIRE(java);
    java->set_block({0, 0, 0}, blocks->intern(*BlockState::parse("examplemod:thing")));
    ConversionReport report;
    TranslateOptions opt;
    opt.fallback = Fallback::Marker;
    auto out = translate_to_bedrock(*java, mapping(), opt, report);
    REQUIRE(out);
    CHECK(mapping().bedrock(out->block({0, 0, 0})).name == "minecraft:info_update");
}

TEST_CASE("Chest contents translate to Bedrock items", "[translate]") {
    // Java 1.20.5+ item format.
    io::nbt::NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.begin_list("Items", io::nbt::TagType::Compound, 2);
    w.begin_compound();
    w.write_byte("Slot", 3);
    w.write_string("id", "minecraft:diamond");
    w.write_int("count", 5);
    w.end_compound();
    w.begin_compound();
    w.write_byte("Slot", 4);
    w.write_string("id", "minecraft:oak_log");
    w.write_int("count", 64);
    w.end_compound();
    w.end_list();
    w.write_string("CustomName", R"({"text":"Loot"})");
    w.end_compound();
    BlockEntity java{{0, 0, 0}, "minecraft:chest", RawNbt{std::move(w).finish().value(), Endian::Big}};
    const auto state = mapping().find_java_state(*BlockState::parse("minecraft:chest[facing=north,type=single,waterlogged=false]"));
    REQUIRE(state);

    ConversionReport report;
    const auto out = translate_block_entity(java, *state, mapping(), report);
    REQUIRE(out);
    CHECK(out->id == "Chest");
    CHECK(report.exact());
    const auto doc = parse(out->data);
    CHECK(out->data.endian == Endian::Little);
    const auto c = doc.root().entries();
    CHECK(c.find("id").as_string() == "Chest");
    CHECK(c.find("CustomName").as_string() == "Loot");
    const auto items = c.find("Items").items();
    REQUIRE(items.size() == 2);
    const auto first = items.at(0).entries();
    CHECK(first.find("Name").as_string() == "minecraft:diamond");
    CHECK(first.find("Count").as_byte() == i8{5});
    CHECK(first.find("Slot").as_byte() == i8{3});
    const auto block = items.at(1).entries().find("Block").entries();
    CHECK(block.find("name").as_string() == "minecraft:oak_log");
    CHECK(block.find("version").as_int() == 18168865);
    CHECK_FALSE(c.find("x")); // positions are added by the world writer
}
