// Java world (Anvil) reading and Java world -> .mcworld conversion, on the world written
// by nbtlib in tests/data/generate_test_data.py (generate_java_world).

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/zip_archive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace bridger;
using namespace bridger::formats;

namespace {

std::vector<u8> chunk_bytes(const std::string& region, u32 lx, u32 lz) {
    auto r = java::RegionFile::open(test::data_path("javaworld/" + region));
    REQUIRE(r);
    auto c = r->read_chunk(lx, lz);
    REQUIRE(c);
    REQUIRE(c->has_value());
    return **c;
}

java::JavaChunk decode(const std::vector<u8>& bytes) {
    auto c = java::decode_chunk(bytes, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
    REQUIRE(c);
    return std::move(*c);
}

std::string block_at(const java::JavaChunk& c, i32 x, i32 world_y, i32 z) {
    const Structure& s = c.column;
    return std::string(s.block_registry().canonical(s.block({x, world_y - c.min_y, z})));
}

} // namespace

TEST_CASE("Region files expose their chunks with every compression type", "[anvil]") {
    auto r = java::RegionFile::open(test::data_path("javaworld/basic/region/r.0.0.mca"));
    REQUIRE(r);
    CHECK(r->region_x() == 0);
    CHECK(r->region_z() == 0);
    CHECK(r->has_chunk(0, 0));  // zlib
    CHECK(r->has_chunk(1, 0));  // gzip
    CHECK(r->has_chunk(2, 0));  // uncompressed
    CHECK(r->has_chunk(0, 1));  // external .mcc
    CHECK_FALSE(r->has_chunk(5, 5));
    for (const auto& [x, z] : {std::pair{0u, 0u}, {1u, 0u}, {2u, 0u}, {0u, 1u}}) {
        const auto c = r->read_chunk(x, z);
        REQUIRE(c);
        REQUIRE(c->has_value());
        const auto chunk = decode(**c);
        CHECK(chunk.pos == ChunkPos{static_cast<i32>(x), static_cast<i32>(z)});
    }
    CHECK_FALSE(java::RegionFile::open(test::data_path("javaworld/basic/level.dat"))); // not a region name
}

TEST_CASE("Chunk sections, biomes and block entities decode", "[anvil]") {
    const auto c = decode(chunk_bytes("basic/region/r.0.0.mca", 0, 0));
    CHECK(c.full);
    CHECK(c.data_version == 5023);
    CHECK(c.min_y == -64);
    CHECK(c.column.size() == Extent{16, 384, 16});
    CHECK(block_at(c, 5, -64, 5) == "minecraft:stone");
    CHECK(block_at(c, 7, 64, 9) == "minecraft:oak_planks");
    CHECK(block_at(c, 0, 65, 3) == "minecraft:glass");
    CHECK(block_at(c, 1, 65, 3) == "minecraft:air");
    CHECK(block_at(c, 3, 70, 5) == "minecraft:chest[facing=north,type=single,waterlogged=false]");
    CHECK(block_at(c, 3, 200, 5) == "minecraft:air");

    const auto biome = [&](i32 x, i32 y, i32 z) {
        const auto b = c.column.biome({x, y - c.min_y, z});
        REQUIRE(b);
        return std::string(c.column.biome_registry().name(*b));
    };
    CHECK(biome(2, 70, 2) == "minecraft:plains");
    CHECK(biome(10, 70, 2) == "minecraft:desert");
    CHECK(biome(10, -60, 2) == "minecraft:plains"); // single-entry palette section

    REQUIRE(c.column.block_entities.size() == 1);
    const BlockEntity& be = c.column.block_entities[0];
    CHECK(be.id == "minecraft:chest");
    CHECK(be.pos == BlockPos{3, 70 + 64, 5});
    const auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    REQUIRE(doc);
    CHECK(doc->root().entries().find("Items"));
    CHECK_FALSE(doc->root().entries().find("keepPacked"));
    CHECK_FALSE(doc->root().entries().find("x"));
}

TEST_CASE("Chunks with {id, properties} palettes, partial status and old layouts", "[anvil]") {
    const auto stairs = decode(chunk_bytes("basic/region/r.0.0.mca", 1, 0));
    CHECK(block_at(stairs, 2, 0, 3) == "minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=true]");

    const auto partial = decode(chunk_bytes("basic/region/r.0.0.mca", 1, 1));
    CHECK_FALSE(partial.full);

    const auto negative = decode(chunk_bytes("basic/region/r.-1.0.mca", 31, 0));
    CHECK(negative.pos == ChunkPos{-1, 0});
    CHECK(block_at(negative, 0, -16, 0) == "minecraft:emerald_block");

    const auto old = java::decode_chunk(chunk_bytes("old_layout/region/r.0.0.mca", 0, 0), std::make_shared<BlockStateRegistry>(),
                                        std::make_shared<BiomeRegistry>());
    REQUIRE_FALSE(old);
    CHECK(old.error().code() == ErrorCode::UnsupportedVersion);
}

TEST_CASE("Truncated or corrupted chunks fail cleanly", "[anvil][fuzz]") {
    const auto bytes = chunk_bytes("basic/region/r.0.0.mca", 0, 0);
    for (usize n = 0; n < bytes.size(); n += 7) {
        const std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        (void)java::decode_chunk(cut, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
    }
    for (usize i = 0; i < bytes.size(); i += 13) {
        std::vector<u8> flipped = bytes;
        flipped[i] ^= 0x5A;
        (void)java::decode_chunk(flipped, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
    }
}

TEST_CASE("Java level.dat and world folders are read", "[anvil]") {
    const auto w = java::open_java_world(test::data_path("javaworld/basic"));
    REQUIRE(w);
    CHECK(w->level.level_name == "Bridger Test World");
    CHECK(w->level.data_version == 5023);
    CHECK(w->level.version_name == "26.3");
    CHECK(w->level.seed == 12345);
    CHECK(w->level.spawn == BlockPos{8, 70, 8});
    CHECK(w->overworld_regions.size() == 2);
    CHECK(w->has_nether);
    CHECK_FALSE(w->has_end);
    // The level.dat path itself is accepted too.
    CHECK(java::open_java_world(test::data_path("javaworld/basic/level.dat")));
    CHECK_FALSE(java::open_java_world(test::data_path("schem")));
}

TEST_CASE("Java 26.1+ worlds keep dimensions in dimensions/ and the seed in data/", "[anvil]") {
    const auto w = java::open_java_world(test::data_path("javaworld/modern_layout"));
    REQUIRE(w);
    CHECK(w->modern_layout);
    CHECK(w->level.seed == -987654321);
    REQUIRE(w->overworld_regions.size() == 1);
    CHECK(w->has_end);
    CHECK_FALSE(w->has_nether);

    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path("javaworld/modern_layout");
    o.output = dir.path / "modern.mcworld";
    const auto summary = convert::convert(o);
    REQUIRE(summary);
    CHECK(summary->chunks == 1);
}

TEST_CASE("A Java world converts to a valid .mcworld", "[anvil][convert][e2e]") {
    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path("javaworld/basic");
    o.output = dir.path / "world.mcworld";
    const auto summary = convert::convert(o);
    REQUIRE(summary);
    CHECK(summary->input_format == FormatId::JavaWorld);
    CHECK(summary->chunks == 5); // (0,0) (1,0) (2,0) (0,1) (-1,0); (1,1) is partial
    CHECK(summary->block_entities == 1);
    CHECK(summary->report.total(translate::IssueKind::UnmappedBlock) == 0);
    CHECK(summary->report.total(translate::IssueKind::DroppedSecondaryLayer) == 0);
    const auto has_warning = [&](std::string_view needle) {
        return std::any_of(summary->warnings.begin(), summary->warnings.end(),
                           [&](const std::string& w) { return w.find(needle) != std::string::npos; });
    };
    CHECK(has_warning("partially generated"));
    CHECK(has_warning("Nether"));

    // Independent checks on the zip: the world validates, and the right keys exist.
    const auto bytes = test::read_file(o.output);
    test::TempDir check;
    bedrock::WorldWriteStats st;
    REQUIRE(bedrock::validate_mcworld(bytes, check.path, &st));
    CHECK(st.chunks == 5);
    auto db = bedrock::LevelDbStore::open(check.path / "db", false);
    REQUIRE(db);
    using bedrock::ChunkTag;
    using bedrock::Dimension;
    const auto has = [&](i32 cx, i32 cz, ChunkTag tag, std::optional<i8> sy = std::nullopt) {
        const auto v = sy ? db->get(bedrock::chunk_key(cx, cz, Dimension::Overworld, tag, *sy))
                          : db->get(bedrock::chunk_key(cx, cz, Dimension::Overworld, tag));
        REQUIRE(v);
        return v->has_value();
    };
    CHECK(has(0, 1, ChunkTag::SubChunkPrefix, i8{1}));   // the externally stored chunk
    CHECK(has(-1, 0, ChunkTag::SubChunkPrefix, i8{-1})); // negative coordinates
    CHECK(has(0, 0, ChunkTag::BlockEntity));
    CHECK_FALSE(has(1, 1, ChunkTag::Version)); // partial chunk left to the game
    CHECK_FALSE(has(0, 0, ChunkTag::SubChunkPrefix, i8{19})); // all-air section not written

    // Waterlogged stairs: water on layer 2.
    const auto sub = db->get(bedrock::chunk_key(1, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{0}));
    REQUIRE(sub);
    REQUIRE(sub->has_value());
    const auto decoded = bedrock::decode_subchunk(**sub);
    REQUIRE(decoded);
    CHECK(decoded->layers.size() == 2);

    // Converting again without --overwrite is refused; the input is never touched.
    CHECK_FALSE(convert::convert(o));
}

TEST_CASE("World conversion output does not depend on the thread count", "[anvil][convert][determinism]") {
    // Compared after decoding: every LevelDB key and value (LevelDB's own file layout may differ).
    test::TempDir dir;
    const auto convert_with = [&](usize threads) {
        convert::ConvertOptions o;
        o.input = test::data_path("javaworld/basic");
        o.output = dir.path / ("t" + std::to_string(threads) + ".mcworld");
        o.threads = threads;
        const auto summary = convert::convert(o);
        REQUIRE(summary);
        const std::filesystem::path unpacked = dir.path / ("u" + std::to_string(threads));
        std::filesystem::create_directories(unpacked);
        REQUIRE(bedrock::validate_mcworld(test::read_file(o.output), unpacked));
        auto db = bedrock::LevelDbStore::open(unpacked / "db", false);
        REQUIRE(db);
        std::vector<std::pair<std::string, std::vector<u8>>> kv;
        REQUIRE(db->for_each([&](std::string_view k, std::span<const u8> v) {
            kv.emplace_back(std::string(k), std::vector<u8>(v.begin(), v.end()));
        }));
        return std::pair{kv, summary->report.total()};
    };
    const auto one = convert_with(1);
    const auto many = convert_with(8);
    CHECK(one.first.size() > 10);
    CHECK(one.first == many.first);
    CHECK(one.second == many.second);
}

TEST_CASE("A pre-1.18 Java world is refused with a clear message", "[anvil][convert]") {
    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path("javaworld/old_layout");
    o.output = dir.path / "old.mcworld";
    const auto summary = convert::convert(o);
    REQUIRE_FALSE(summary);
    CHECK_FALSE(std::filesystem::exists(o.output));
}

TEST_CASE("A chunk whose yPos does not fit a section byte is refused", "[anvil][fuzz]") {
    // Found by fuzz_anvil_chunk: yPos -65526 decoded, but could not be written back.
    const auto bytes = test::read_file(test::data_path("javaworld/fuzz_ypos_out_of_range.nbt"));
    const auto c = java::decode_chunk(bytes, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
    REQUIRE_FALSE(c);
    CHECK(c.error().code() == ErrorCode::Malformed);
}
