// Bedrock -> Java through files: .mcstructure -> .schem/.nbt/.litematic, Java -> Java between the
// structure formats, and .mcworld -> Java world folder, each read back independently.

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_view.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>

using namespace bridger;
using namespace bridger::formats;

namespace {

StructureFile read(const std::filesystem::path& p, FormatId f) {
    auto r = read_java_structure(test::read_file(p), f, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
    REQUIRE(r);
    return std::move(*r);
}

std::string strip_shape(std::string s) {
    if (s.find("_stairs[") == std::string::npos) return s;
    const auto at = s.find("shape=");
    const auto end = s.find_first_of(",]", at);
    s.erase(at, end - at + (s[end] == ',' ? 1 : 0));
    return s;
}

void check_same_blocks(const Structure& a, const Structure& b) {
    REQUIRE(a.size() == b.size());
    usize diff = 0;
    for (u32 y = 0; y < a.size().y; ++y)
        for (u32 z = 0; z < a.size().z; ++z)
            for (u32 x = 0; x < a.size().x; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                const std::string sa = strip_shape(std::string(a.block_registry().canonical(a.block(p))));
                const std::string sb = strip_shape(std::string(b.block_registry().canonical(b.block(p))));
                if (sa != sb && diff++ < 5) UNSCOPED_INFO(p.x << "," << p.y << "," << p.z << ": " << sa << " vs " << sb);
            }
    CHECK(diff == 0);
}

FormatId format_of(const std::string& ext) { return *format_from_extension(ext); }

} // namespace

TEST_CASE("Java structures survive .mcstructure and every Java writer", "[convert][reverse][e2e]") {
    const std::string target = GENERATE(".schem", ".nbt", ".litematic");
    CAPTURE(target);
    test::TempDir dir;
    const auto source = test::data_path("ingame/containers.schem");

    convert::ConvertOptions to_bedrock;
    to_bedrock.input = source;
    to_bedrock.output = dir.path / "c.mcstructure";
    REQUIRE(convert::convert(to_bedrock));

    convert::ConvertOptions to_java;
    to_java.input = to_bedrock.output;
    to_java.output = dir.path / ("back" + target);
    const auto summary = convert::convert(to_java);
    REQUIRE(summary);
    CHECK(summary->input_format == FormatId::McStructure);

    const StructureFile original = read(source, FormatId::SpongeSchem);
    const StructureFile back = read(to_java.output, format_of(target));
    CHECK(back.data_version == 5023);
    check_same_blocks(original.structure, back.structure);
    CHECK(back.structure.block_entities.size() == original.structure.block_entities.size());

    // Java -> Java (no translation) between the structure formats.
    convert::ConvertOptions java_java;
    java_java.input = source;
    java_java.output = dir.path / ("direct" + target);
    REQUIRE(convert::convert(java_java));
    check_same_blocks(original.structure, read(java_java.output, format_of(target)).structure);
}

TEST_CASE("A Bedrock world converts to a Java world folder", "[convert][reverse][e2e]") {
    test::TempDir dir;
    convert::ConvertOptions to_bedrock;
    to_bedrock.input = test::data_path("ingame/containers.schem");
    to_bedrock.output = dir.path / "c.mcworld";
    to_bedrock.placement.place_at = BlockPos{0, -60, 0};
    REQUIRE(convert::convert(to_bedrock));

    convert::ConvertOptions to_java;
    to_java.input = to_bedrock.output;
    to_java.output = dir.path / "java world";
    const auto summary = convert::convert(to_java);
    REQUIRE(summary);
    CHECK(summary->output_format == FormatId::JavaWorld);
    CHECK(summary->chunks >= 1);

    const auto world = java::open_java_world(to_java.output);
    REQUIRE(world);
    CHECK(world->modern_layout);
    CHECK(world->level.level_name == "containers"); // the Bedrock world's name (from the .schem)
    REQUIRE_FALSE(world->overworld_regions.empty());

    // The structure's chest content is in the chunk at the placement origin.
    std::map<std::string, usize> be_ids;
    bool flat_layer = false;
    for (const auto& rp : world->overworld_regions) {
        auto region = java::RegionFile::open(rp);
        REQUIRE(region);
        for (u32 lz = 0; lz < 32; ++lz)
            for (u32 lx = 0; lx < 32; ++lx) {
                auto raw = region->read_chunk(lx, lz);
                REQUIRE(raw);
                if (!*raw) continue;
                auto chunk = java::decode_chunk(**raw, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
                REQUIRE(chunk);
                CHECK(chunk->full);
                for (const auto& be : chunk->column.block_entities) ++be_ids[be.id];
                // The superflat surface (grass at y = -61) is part of the written chunks.
                flat_layer |= chunk->column.block_registry().canonical(chunk->column.block({0, -61 - chunk->min_y, 0})) ==
                              "minecraft:grass_block[snowy=false]";
            }
    }
    CHECK(be_ids["minecraft:chest"] >= 2);
    CHECK(flat_layer);

    // An existing destination folder is never replaced.
    CHECK_FALSE(convert::convert(to_java));
}

TEST_CASE("Bedrock world -> Java world output does not depend on the thread count", "[convert][reverse][determinism]") {
    test::TempDir dir;
    convert::ConvertOptions to_bedrock;
    to_bedrock.input = test::data_path("ingame/borders.schem"); // 40^3, several chunks and regions' worth of borders
    to_bedrock.output = dir.path / "b.mcworld";
    REQUIRE(convert::convert(to_bedrock));
    std::vector<std::vector<u8>> regions[2];
    for (const usize threads : {usize{1}, usize{8}}) {
        convert::ConvertOptions o;
        o.input = to_bedrock.output;
        o.output = dir.path / ("w" + std::to_string(threads));
        o.threads = threads;
        REQUIRE(convert::convert(o));
        const auto world = java::open_java_world(o.output);
        REQUIRE(world);
        for (const auto& rp : world->overworld_regions) regions[threads == 1 ? 0 : 1].push_back(test::read_file(rp));
    }
    // Region files are deterministic byte for byte (level.dat differs only in LastPlayed).
    CHECK(regions[0].size() >= 1);
    CHECK(regions[0] == regions[1]);
}
