// End-to-end conversion and placement policy.

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/convert/placement.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace bridger;
using namespace bridger::convert;

namespace {

Structure tall_structure(u32 height) {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto s = Structure::create(blocks, std::make_shared<BiomeRegistry>(), {2, height, 2}, {Edition::Java, 5023});
    REQUIRE(s);
    const StateId stone = blocks->intern(*BlockState::parse("minecraft:stone"));
    for (u32 y = 0; y < height; ++y) s->set_block({0, static_cast<i32>(y), 0}, stone);
    return std::move(*s);
}

} // namespace

TEST_CASE("Default placement: centered on 0,0, on the flat surface, spawn outside", "[convert]") {
    Structure s = tall_structure(5);
    translate::ConversionReport report;
    const auto p = place_in_new_world(s, {}, {}, report);
    REQUIRE(p);
    CHECK(p->origin == BlockPos{-1, -60, -1});
    CHECK(p->spawn.x < p->origin.x); // outside the footprint
    CHECK(report.exact());
}

TEST_CASE("Height overflow fails by default with the overflow in the message", "[convert][safety]") {
    Structure s = tall_structure(390); // taller than 384
    translate::ConversionReport report;
    const auto p = place_in_new_world(s, {}, {}, report);
    REQUIRE_FALSE(p);
    CHECK(p.error().code() == ErrorCode::OutOfBounds);
    CHECK(p.error().message().find("layers above") != std::string::npos);
}

TEST_CASE("Shift-to-fit moves the structure, clip drops and reports", "[convert][safety]") {
    {
        Structure s = tall_structure(100);
        PlacementPolicy pol;
        pol.place_at = BlockPos{0, 300, 0};
        pol.height = HeightPolicy::ShiftToFit;
        translate::ConversionReport report;
        const auto p = place_in_new_world(s, pol, {}, report);
        REQUIRE(p);
        CHECK(p->origin.y == 319 - 100 + 1);
        CHECK(report.exact());
    }
    {
        Structure s = tall_structure(100);
        PlacementPolicy pol;
        pol.place_at = BlockPos{0, 300, 0};
        pol.height = HeightPolicy::Clip;
        translate::ConversionReport report;
        const auto p = place_in_new_world(s, pol, {}, report);
        REQUIRE(p);
        CHECK(s.size().y == 20); // y 300..319 kept
        CHECK(report.total(translate::IssueKind::ClippedBlock) == 80);
    }
    {
        Structure s = tall_structure(400);
        PlacementPolicy pol;
        pol.height = HeightPolicy::ShiftToFit;
        translate::ConversionReport report;
        CHECK_FALSE(place_in_new_world(s, pol, {}, report)); // taller than the world
    }
}

TEST_CASE("In-game test structures convert with nothing lost except reported entities", "[convert][e2e]") {
    const std::string name = GENERATE("orientations", "waterlogged", "containers", "signs_beds", "borders");
    CAPTURE(name);
    test::TempDir dir;
    ConvertOptions o;
    o.input = test::data_path("ingame/" + name + ".schem");
    o.output = dir.path / (name + ".mcworld");
    const auto before = test::read_file(o.input);

    const auto summary = convert_schem_to_new_mcworld(o);
    REQUIRE(summary);
    CHECK(std::filesystem::exists(o.output));
    CHECK(test::read_file(o.input) == before); // input never modified
    CHECK(summary->report.total(translate::IssueKind::UnmappedBlock) == 0);
    CHECK(summary->report.total(translate::IssueKind::UnmappedBlockEntity) == 0);
    CHECK(summary->report.total(translate::IssueKind::LossyBlock) == 0);
    CHECK(summary->report.total() == summary->report.total(translate::IssueKind::UnmappedEntity));
    CHECK(summary->warnings.empty());

    test::TempDir check;
    formats::bedrock::WorldWriteStats stats;
    const auto bytes = test::read_file(o.output);
    REQUIRE(formats::bedrock::validate_mcworld(bytes, check.path, &stats));
    CHECK(stats.subchunks == summary->subchunks);
    CHECK(stats.block_entities == summary->block_entities);
}

TEST_CASE("Waterlogged blocks get water in layer 2 and nothing else does", "[convert][e2e]") {
    test::TempDir dir;
    ConvertOptions o;
    o.input = test::data_path("ingame/waterlogged.schem"); // 7 waterlogged blocks
    o.output = dir.path / "w.mcworld";
    REQUIRE(convert_schem_to_new_mcworld(o));

    test::TempDir check;
    REQUIRE(formats::bedrock::validate_mcworld(test::read_file(o.output), check.path));
    auto db = formats::bedrock::LevelDbStore::open(check.path / "db", false);
    REQUIRE(db);
    usize water_in_layer2 = 0;
    bool foreign = false;
    REQUIRE(db->for_each([&](std::string_view key, std::span<const u8> value) {
        if (key.size() != 10 || static_cast<u8>(key[8]) != 0x2F) return;
        const auto sub = formats::bedrock::decode_subchunk(value);
        REQUIRE(sub);
        for (usize l = 1; l < sub->layers.size(); ++l)
            for (const u16 idx : sub->layers[l].indices) {
                const std::string& name = sub->layers[l].palette[idx];
                if (name.starts_with("minecraft:water")) ++water_in_layer2;
                else if (name != "minecraft:air") foreign = true;
            }
    }));
    CHECK(water_in_layer2 == 7);
    CHECK_FALSE(foreign);
}

TEST_CASE("An existing output is replaced only with --overwrite", "[convert][safety]") {
    test::TempDir dir;
    ConvertOptions o;
    o.input = test::data_path("ingame/orientations.schem");
    o.output = dir.path / "out.mcworld";
    test::write_file(o.output, {1, 2, 3});
    const auto refused = convert_schem_to_new_mcworld(o);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code() == ErrorCode::AlreadyExists);
    CHECK(test::read_file(o.output) == std::vector<u8>{1, 2, 3});

    o.overwrite = true;
    REQUIRE(convert_schem_to_new_mcworld(o));
    CHECK(test::read_file(o.output).size() > 3);
}

TEST_CASE("Conversion is deterministic", "[convert]") {
    test::TempDir dir;
    ConvertOptions o;
    o.input = test::data_path("ingame/containers.schem");
    o.output = dir.path / "a.mcworld";
    REQUIRE(convert_schem_to_new_mcworld(o));
    o.output = dir.path / "b.mcworld";
    REQUIRE(convert_schem_to_new_mcworld(o));
    // Semantic comparison: same subchunk and block entity content (zip/LevelDB layout may differ).
    test::TempDir ca, cb;
    formats::bedrock::WorldWriteStats a, b;
    REQUIRE(formats::bedrock::validate_mcworld(test::read_file(dir.path / "a.mcworld"), ca.path, &a));
    REQUIRE(formats::bedrock::validate_mcworld(test::read_file(dir.path / "b.mcworld"), cb.path, &b));
    CHECK(a.subchunks == b.subchunks);
    CHECK(a.block_entities == b.block_entities);
}
