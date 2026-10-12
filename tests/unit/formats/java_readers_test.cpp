// Structure .nbt, .litematic and legacy .schematic readers on files written by nbtlib
// (tests/data/generate_test_data.py), plus .mcstructure and existing-world placement.

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace bridger;
using namespace bridger::formats;

namespace {

const translate::MappingData& mapping() { return *translate::MappingData::embedded().value(); }

java::LegacyTables legacy_tables() {
    return {[](u16 id, u8 d) -> std::optional<std::string> {
                if (const auto s = mapping().legacy_block(id, d)) return mapping().java_state_string(*s);
                return std::nullopt;
            },
            [](u16 id, u16 dmg) -> std::optional<std::string> {
                if (const auto i = mapping().legacy_item(id, dmg)) return std::string(*i);
                return std::nullopt;
            }};
}

Result<StructureFile> read(const std::string& file, FormatId f) {
    const auto bytes = test::read_file(test::data_path(file));
    const auto tables = legacy_tables();
    return read_java_structure(bytes, f, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), &tables);
}

std::string at(const Structure& s, BlockPos p) { return std::string(s.block_registry().canonical(s.block(p))); }

std::vector<std::pair<std::string, i32>> items_of(const BlockEntity& be) {
    std::vector<std::pair<std::string, i32>> out;
    const auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    REQUIRE(doc);
    for (const auto item : doc->root().entries().find("Items").items())
        out.emplace_back(std::string(*item.entries().find("id").as_string()), *item.entries().find("count").as_int());
    return out;
}

} // namespace

TEST_CASE("Structure .nbt: both palette spellings read identically", "[formats][nbt-structure]") {
    const std::string file = GENERATE("structure/modern_keys.nbt", "structure/legacy_keys.nbt");
    CAPTURE(file);
    const auto f = read(file, FormatId::StructureNbt);
    REQUIRE(f);
    const Structure& s = f->structure;
    CHECK(f->data_version == 5023);
    CHECK(s.size() == Extent{3, 2, 2});
    CHECK(at(s, {0, 0, 0}) == "minecraft:stone");
    CHECK(at(s, {2, 0, 0}) == "minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]");
    CHECK(at(s, {1, 1, 0}) == "minecraft:chest[facing=north,type=single,waterlogged=true]");
    CHECK(at(s, {2, 1, 1}) == "minecraft:red_wool");
    CHECK(at(s, {0, 1, 1}) == "minecraft:air"); // not listed: structure void, read as air
    REQUIRE(s.block_entities.size() == 1);
    CHECK(s.block_entities[0].id == "minecraft:chest");
    CHECK(items_of(s.block_entities[0]) == std::vector<std::pair<std::string, i32>>{{"minecraft:diamond", 7}});
    REQUIRE(s.entities.size() == 1);
    CHECK(s.entities[0].id == "minecraft:armor_stand");
    CHECK(f->warnings.empty());
}

TEST_CASE("Structure .nbt with several palettes uses the first and warns", "[formats][nbt-structure]") {
    const auto f = read("structure/multi_palette.nbt", FormatId::StructureNbt);
    REQUIRE(f);
    REQUIRE(f->warnings.size() == 1);
    CHECK(at(f->structure, {0, 0, 0}) == "minecraft:stone");
}

TEST_CASE("Litematic: regions merge, negative sizes, entries spanning longs", "[formats][litematic]") {
    const auto f = read("litematic/two_regions.litematic", FormatId::Litematic);
    REQUIRE(f);
    const Structure& s = f->structure;
    CHECK(f->data_version == 5023);
    CHECK(s.size() == Extent{11, 2, 3}); // x 0..10 (region B: Position 10, Size -3 -> 8..10)
    const char* cycle[] = {"minecraft:stone", "minecraft:oak_planks", "minecraft:glass", "minecraft:bricks"};
    for (i32 z = 0; z < 3; ++z)
        for (i32 x = 0; x < 5; ++x) {
            CAPTURE(x, z);
            CHECK(at(s, {x, 0, z}) == cycle[(x + z) % 4]); // 3-bit entries cross long boundaries
        }
    CHECK(at(s, {2, 1, 1}) == "minecraft:chest[facing=south,type=single,waterlogged=false]");
    for (i32 x = 8; x <= 10; ++x)
        for (i32 y = 0; y < 2; ++y)
            for (i32 z = 0; z < 2; ++z) CHECK(at(s, {x, y, z}) == "minecraft:gold_block");
    CHECK(at(s, {7, 0, 0}) == "minecraft:air"); // between the regions
    REQUIRE(s.block_entities.size() == 1);
    CHECK(s.block_entities[0].pos == BlockPos{2, 1, 1});
    CHECK(items_of(s.block_entities[0]) == std::vector<std::pair<std::string, i32>>{{"minecraft:emerald", 9}});
    CHECK(s.metadata.name == "Two regions");
}

TEST_CASE("Legacy .schematic: numeric ids, renames, AddBlocks, bed color, items", "[formats][schematic]") {
    const auto f = read("schematic/legacy_basic.schematic", FormatId::LegacySchematic);
    REQUIRE(f);
    const Structure& s = f->structure;
    CHECK(s.size() == Extent{4, 2, 3});
    CHECK(s.offset == BlockPos{0, -1, 0});
    CHECK(at(s, {0, 0, 0}) == "minecraft:stone");
    CHECK(at(s, {0, 1, 0}).starts_with("minecraft:oak_stairs[facing=west"));
    CHECK(at(s, {1, 1, 0}) == "minecraft:red_wool");
    CHECK(at(s, {2, 1, 0}).starts_with("minecraft:chest[facing=south"));
    CHECK(at(s, {3, 1, 0}).starts_with("minecraft:smooth_stone_slab[type=bottom")); // 1.14 rename
    CHECK(at(s, {0, 1, 1}) == "minecraft:short_grass");                             // 1.20.3 rename
    CHECK(at(s, {1, 1, 1}).starts_with("minecraft:blue_bed["));                     // color from the block entity
    CHECK(at(s, {3, 1, 2}) == "bridger:legacy_300_0");                               // AddBlocks; unknown id kept visible
    REQUIRE(s.block_entities.size() == 2);
    const BlockEntity* chest = nullptr;
    for (const auto& be : s.block_entities)
        if (be.id == "minecraft:chest") chest = &be;
    REQUIRE(chest != nullptr);
    CHECK(items_of(*chest) == std::vector<std::pair<std::string, i32>>{{"minecraft:diamond", 5}, {"minecraft:blue_wool", 3}});
}

TEST_CASE("Java reader errors and limits", "[formats]") {
    const auto tables = legacy_tables();
    const std::vector<u8> junk{'n', 'o', 't', ' ', 'n', 'b', 't'};
    for (const FormatId f : {FormatId::StructureNbt, FormatId::Litematic, FormatId::LegacySchematic})
        CHECK_FALSE(read_java_structure(junk, f, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), &tables));
    // Truncations of every real file never crash.
    for (const char* file : {"structure/modern_keys.nbt", "litematic/two_regions.litematic", "schematic/legacy_basic.schematic"}) {
        const auto bytes = test::read_file(test::data_path(file));
        const FormatId f = std::string(file).find("litematic") != std::string::npos ? FormatId::Litematic
                           : std::string(file).find("schematic") != std::string::npos ? FormatId::LegacySchematic
                                                                                       : FormatId::StructureNbt;
        for (usize len = 0; len < bytes.size(); len += 5)
            CHECK_FALSE(read_java_structure(std::span<const u8>(bytes.data(), len), f, std::make_shared<BlockStateRegistry>(),
                                            std::make_shared<BiomeRegistry>(), &tables));
    }
}

TEST_CASE("Every Java format converts to .mcstructure and validates", "[convert][e2e]") {
    const std::string file = GENERATE("structure/modern_keys.nbt", "litematic/two_regions.litematic",
                                      "schematic/legacy_basic.schematic", "ingame/containers.schem");
    CAPTURE(file);
    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path(file);
    o.output = dir.path / "out.mcstructure";
    const auto summary = convert::convert(o);
    REQUIRE(summary);
    CHECK(summary->output_format == FormatId::McStructure);

    // Read it back independently of the pipeline's own validation.
    const auto bytes = test::read_file(o.output);
    const auto& m = mapping();
    const auto lookup = [&m](io::nbt::NbtCompoundView e) -> std::optional<u32> {
        std::vector<translate::BedrockProperty> props;
        for (const auto& st : e.find("states").entries()) {
            translate::BedrockProperty p;
            p.key = st.name;
            if (const auto b = st.value.as_byte()) { p.type = translate::NbtValueType::Byte; p.int_value = *b; }
            else if (const auto i = st.value.as_int()) { p.type = translate::NbtValueType::Int; p.int_value = *i; }
            else { p.type = translate::NbtValueType::String; p.str_value = *st.value.as_string(); }
            props.push_back(p);
        }
        return m.find_bedrock(*e.find("name").as_string(), props);
    };
    const auto air = m.find_bedrock("minecraft:air", {});
    const auto back = bedrock::read_mcstructure(bytes, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                                m.bedrock_block_version(), lookup, *air);
    REQUIRE(back);
    CHECK(back->warnings.empty());
    CHECK(back->structure.block_entities.size() == summary->block_entities);
}

TEST_CASE("Placing into an existing world keeps everything else byte-identical", "[convert][safety]") {
    test::TempDir dir;
    convert::ConvertOptions base;
    base.input = test::data_path("ingame/containers.schem");
    base.output = dir.path / "base.mcworld";
    REQUIRE(convert::convert(base));
    const auto base_bytes = test::read_file(base.output);

    convert::ConvertOptions place;
    place.input = test::data_path("ingame/orientations.schem");
    place.output = dir.path / "placed.mcworld";
    place.into_world = base.output;
    place.placement.place_at = BlockPos{-6, -59, -2}; // overlaps the single chest at local (1,1,1) of containers
    place.placement.air = convert::AirMode::Keep;
    const auto summary = convert::convert(place);
    REQUIRE(summary);
    CHECK(test::read_file(base.output) == base_bytes); // the source world is never modified

    // An overwritten block entity is reported, never silently removed.
    CHECK(summary->report.total(translate::IssueKind::RemovedBlockEntity) >= 1);

    // Tampering with an untouched key must be detected by the validator.
    test::TempDir tamper;
    auto entries = io::read_zip(test::read_file(place.output));
    REQUIRE(entries);
    std::vector<io::ZipEntry> modified = *entries;
    for (auto& e : modified)
        if (e.name == "level.dat") e.data.back() ^= 1;
    const auto zipped = io::write_zip(modified);
    REQUIRE(zipped);
    CHECK_FALSE(bedrock::validate_placed_world(*zipped, base_bytes, {}, tamper.path));
}

TEST_CASE("Placing into a world without a position is refused", "[convert][safety]") {
    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path("ingame/orientations.schem");
    o.output = dir.path / "x.mcworld";
    o.into_world = dir.path / "does_not_matter.mcworld";
    const auto r = convert::convert(o);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::InvalidArgument);
}

namespace {

std::vector<u8> entity_nbt(const char* id, f32 x, f32 y, f32 z) {
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound("");
    w.write_string("identifier", id);
    w.begin_list("Pos", io::nbt::TagType::Float, 3);
    w.write_float({}, x);
    w.write_float({}, y);
    w.write_float({}, z);
    w.end_list();
    w.end_compound();
    return std::move(w).finish().value();
}

} // namespace

TEST_CASE("Clearing entities removes entities from both storages and reports them", "[convert][safety]") {
    test::TempDir dir;
    convert::ConvertOptions base;
    base.input = test::data_path("ingame/containers.schem");
    base.output = dir.path / "base.mcworld";
    REQUIRE(convert::convert(base));

    // Add entities to the base world: one in the legacy per-chunk key, one as a modern actor
    // (digp list + actorprefix record), both inside the placement box, plus one outside it.
    const std::filesystem::path unpacked = dir.path / "unpacked";
    {
        auto entries = io::read_zip(test::read_file(base.output));
        REQUIRE(entries);
        for (const auto& e : *entries) {
            const auto p = unpacked / e.name;
            std::filesystem::create_directories(p.parent_path());
            test::write_file(p, e.data);
        }
        auto db = bedrock::LevelDbStore::open(unpacked / "db", false);
        REQUIRE(db);
        std::vector<u8> legacy = entity_nbt("minecraft:pig", -5.5f, -58.0f, -1.5f);
        const auto far = entity_nbt("minecraft:cow", -15.5f, -58.0f, -15.5f); // chunk (-1,-1), outside the box
        legacy.insert(legacy.end(), far.begin(), far.end());
        REQUIRE(db->put(bedrock::chunk_key(-1, -1, bedrock::Dimension::Overworld, bedrock::ChunkTag::Entity), legacy));
        const std::string pos_key = bedrock::chunk_key(-1, -1, bedrock::Dimension::Overworld, bedrock::ChunkTag::Version).substr(0, 8);
        const std::string actor_id("\x01\x00\x00\x00\x02\x00\x00\x00", 8);
        REQUIRE(db->put("digp" + pos_key, std::span(reinterpret_cast<const u8*>(actor_id.data()), 8)));
        REQUIRE(db->put("actorprefix" + actor_id, entity_nbt("minecraft:sheep", -4.5f, -58.0f, -0.5f)));
        REQUIRE(db->compact());
    }
    const auto with_entities = dir.path / "with_entities.mcworld";
    REQUIRE(io::write_zip_file_from_directory(unpacked, with_entities));

    convert::ConvertOptions place;
    place.input = test::data_path("ingame/orientations.schem");
    place.output = dir.path / "placed.mcworld";
    place.into_world = with_entities;
    place.placement.place_at = BlockPos{-6, -59, -2};
    place.placement.clear_entities = true;
    const auto summary = convert::convert(place);
    REQUIRE(summary);
    CHECK(summary->report.total(translate::IssueKind::RemovedEntity) == 2); // pig + sheep, not the cow

    test::TempDir check;
    auto out = io::read_zip(test::read_file(place.output));
    REQUIRE(out);
    for (const auto& e : *out) {
        const auto p = check.path / e.name;
        std::filesystem::create_directories(p.parent_path());
        test::write_file(p, e.data);
    }
    auto db = bedrock::LevelDbStore::open(check.path / "db", false);
    REQUIRE(db);
    const auto legacy = db->get(bedrock::chunk_key(-1, -1, bedrock::Dimension::Overworld, bedrock::ChunkTag::Entity));
    REQUIRE(legacy);
    REQUIRE(legacy->has_value());
    CHECK(**legacy == entity_nbt("minecraft:cow", -15.5f, -58.0f, -15.5f)); // the cow is kept verbatim
    const auto actor = db->get("actorprefix" + std::string("\x01\x00\x00\x00\x02\x00\x00\x00", 8));
    REQUIRE(actor);
    CHECK_FALSE(actor->has_value());
}
