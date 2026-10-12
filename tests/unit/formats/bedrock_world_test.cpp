// Bedrock world writing: keys, subchunk codec, level.dat, the full world, validation.

#include "../test_util.hpp"

#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/bedrock/mcworld/level_dat.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace bridger;
using namespace bridger::formats::bedrock;

namespace {

// Fake state domain for codec tests: state n -> {name: "test:n", states: {}, version: 1}.
struct FakeStates {
    std::map<u32, std::vector<u8>> cache;
    StateNbtFn fn() {
        return [this](u32 s) -> Result<std::span<const u8>> {
            auto& bytes = cache[s];
            if (bytes.empty()) {
                io::nbt::NbtWriter w(Endian::Little);
                w.begin_root_compound("");
                w.write_string("name", "test:" + std::to_string(s));
                w.begin_compound("states");
                w.end_compound();
                w.write_int("version", 1);
                w.end_compound();
                bytes = std::move(w).finish().value();
            }
            return std::span<const u8>(bytes);
        };
    }
};

} // namespace

TEST_CASE("Chunk keys follow the LevelDB layout", "[formats][bedrock]") {
    const std::string k = chunk_key(-1, 2, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{-4});
    REQUIRE(k.size() == 10);
    CHECK(io::load_le<i32>(k.data()) == -1);
    CHECK(io::load_le<i32>(k.data() + 4) == 2);
    CHECK(static_cast<u8>(k[8]) == 0x2F);
    CHECK(static_cast<i8>(k[9]) == -4);
    CHECK(chunk_key(0, 0, Dimension::Nether, ChunkTag::Version).size() == 13);
    CHECK(chunk_key(0, 0, Dimension::Overworld, ChunkTag::Version).size() == 9);
}

TEST_CASE("Subchunk encode/decode round-trip", "[formats][bedrock]") {
    FakeStates states;
    PaletteLayer blocks;
    std::array<u32, kSectionVolume> values{};
    for (usize i = 0; i < kSectionVolume; ++i) values[i] = static_cast<u32>(i % 300); // 300 states -> 16 bits
    blocks.assign(values);
    PaletteLayer liquids(0);
    liquids.set(section_index(1, 2, 3), 7);
    const PaletteLayer* layers[] = {&blocks, &liquids};

    const auto bytes = encode_subchunk(-2, layers, states.fn());
    REQUIRE(bytes);
    CHECK((*bytes)[0] == 9);
    CHECK((*bytes)[1] == 2);
    CHECK(static_cast<i8>((*bytes)[2]) == -2);
    CHECK((*bytes)[3] == (16 << 1)); // bits per index, persistent palette

    const auto decoded = decode_subchunk(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->y_index == -2);
    REQUIRE(decoded->layers.size() == 2);
    for (usize i = 0; i < kSectionVolume; ++i)
        CHECK(decoded->layers[0].palette[decoded->layers[0].indices[i]] == "test:" + std::to_string(i % 300));
    const auto& l1 = decoded->layers[1];
    CHECK(l1.palette.size() == 2);
    CHECK(l1.palette[l1.indices[section_index(1, 2, 3)]] == "test:7");
    CHECK(l1.palette[l1.indices[0]] == "test:0");

    // Bedrock order: y changes fastest. A layer with one block at (x=0,y=1,z=0) sets bit index 1.
    PaletteLayer one(0);
    one.set(section_index(0, 1, 0), 5);
    const PaletteLayer* single[] = {&one};
    const auto enc = encode_subchunk(0, single, states.fn());
    REQUIRE(enc);
    CHECK((*enc)[3] == (1 << 1));         // 2 palette entries -> 1 bit
    CHECK(io::load_le<u32>(enc->data() + 4) == 0b10); // index 1 set in the first word
}

TEST_CASE("Corrupt subchunks are rejected", "[formats][bedrock]") {
    CHECK_FALSE(decode_subchunk({}));
    const std::vector<u8> bad_version{7, 1, 0};
    CHECK_FALSE(decode_subchunk(bad_version));
    const std::vector<u8> bad_bits{9, 1, 0, 7 << 1};
    CHECK_FALSE(decode_subchunk(bad_bits));
    const std::vector<u8> runtime{9, 1, 0, (1 << 1) | 1};
    CHECK_FALSE(decode_subchunk(runtime));
}

TEST_CASE("level.dat has the header and the required fields", "[formats][bedrock]") {
    LevelDatOptions o;
    o.level_name = "Test \xD8\xAC";
    o.spawn = {-5, -60, 7};
    o.flat_world_layers = default_flat_world_layers();
    const auto bytes = encode_level_dat(o);
    REQUIRE(bytes.size() > 8);
    CHECK(io::load_le<i32>(bytes.data()) == 10);
    CHECK(io::load_le<i32>(bytes.data() + 4) == static_cast<i32>(bytes.size() - 8));
    const auto doc = io::nbt::NbtDocument::parse(std::span<const u8>(bytes).subspan(8), Endian::Little);
    REQUIRE(doc);
    const auto c = doc->root().entries();
    CHECK(c.find("StorageVersion").as_int() == 10);
    CHECK(c.find("NetworkVersion").as_int() == 2192);
    CHECK(c.find("Generator").as_int() == 2);
    CHECK(c.find("SpawnX").as_int() == -5);
    CHECK(c.find("SpawnZ").as_int() == 7);
    CHECK(c.find("LevelName").as_string() == "Test \xD8\xAC");
    const auto version = c.find("lastOpenedWithVersion").as_list();
    REQUIRE(version);
    CHECK(version->at(1).as_int() == 26);
    CHECK(version->at(2).as_int() == 50);
}

TEST_CASE("A written world validates and stores blocks at the right world positions", "[formats][bedrock]") {
    FakeStates states;
    auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), {18, 3, 2},
                               {Edition::Bedrock, 1});
    REQUIRE(s);
    // States: 0 = air, 1..3 = flat layers, 10+ = structure blocks.
    for (u32 x = 0; x < 18; ++x)
        for (u32 y = 0; y < 3; ++y)
            for (u32 z = 0; z < 2; ++z) s->set_block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}, 0);
    s->set_block({0, 0, 0}, 10);
    s->set_block({17, 2, 1}, 11);
    s->set_secondary({0, 0, 0}, 12);

    NewWorldOptions o;
    o.origin = {-1, -60, 3}; // crosses the x chunk border at 0
    o.air_state = 0;
    o.flat_layers = {{1, 1}, {2, 2}, {3, 1}};
    o.level.flat_world_layers = default_flat_world_layers();
    test::TempDir work;
    WorldWriteStats written;
    const auto world = write_new_world(*s, o, states.fn(), work.path / "build", &written);
    REQUIRE(world);
    CHECK(written.chunks == 3); // chunk x -1, 0, 1 at z 0
    test::TempDir check;
    WorldWriteStats reread;
    REQUIRE(validate_mcworld(*world, check.path, &reread));
    CHECK(reread.subchunks == written.subchunks);
    CHECK(reread.chunks == written.chunks);

    // Look at the subchunk holding world (-1, -60, 3): chunk x -1, subchunk y -4.
    auto db = LevelDbStore::open(check.path / "db", false);
    REQUIRE(db);
    const auto raw = db->get(chunk_key(-1, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{-4}));
    REQUIRE(raw);
    REQUIRE(raw->has_value());
    const auto sub = decode_subchunk(**raw);
    REQUIRE(sub);
    REQUIRE(sub->layers.size() == 2);
    const auto& l0 = sub->layers[0];
    // world y -60 is local y 4 in subchunk -4; world x -1 -> 15, z 3 -> 3.
    CHECK(l0.palette[l0.indices[section_index(15, 4, 3)]] == "test:10");
    CHECK(sub->layers[1].palette[sub->layers[1].indices[section_index(15, 4, 3)]] == "test:12");
    // Flat layers underneath: y -64 bedrock(1), -63/-62 dirt(2), -61 grass(3).
    CHECK(l0.palette[l0.indices[section_index(15, 0, 3)]] == "test:1");
    CHECK(l0.palette[l0.indices[section_index(15, 2, 3)]] == "test:2");
    CHECK(l0.palette[l0.indices[section_index(15, 3, 3)]] == "test:3");
    // Version and finalized state.
    const auto version = db->get(chunk_key(-1, 0, Dimension::Overworld, ChunkTag::Version));
    REQUIRE(version);
    REQUIRE(version->has_value());
    CHECK((**version)[0] == 42);
}

TEST_CASE("Structures outside the build height are refused", "[formats][bedrock]") {
    FakeStates states;
    auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), {1, 10, 1},
                               {Edition::Bedrock, 1});
    REQUIRE(s);
    NewWorldOptions o;
    o.origin = {0, 315, 0};
    test::TempDir work;
    const auto r = write_new_world(*s, o, states.fn(), work.path / "w");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::OutOfBounds);
}

TEST_CASE("Zip archives round-trip and reject unsafe entries", "[io][zip]") {
    const std::vector<io::ZipEntry> entries{{"level.dat", {1, 2, 3}}, {"db/CURRENT", {}}, {"db/000005.ldb", std::vector<u8>(5000, 7)}};
    const auto zip = io::write_zip(entries);
    REQUIRE(zip);
    const auto back = io::read_zip(*zip);
    REQUIRE(back);
    REQUIRE(back->size() == 3);
    CHECK((*back)[0].name == "level.dat");
    CHECK((*back)[2].data == entries[2].data);

    const std::vector<io::ZipEntry> evil{{"../evil.txt", {1}}};
    CHECK_FALSE(io::write_zip(evil));

    io::ZipLimits tiny;
    tiny.max_total_bytes = 100;
    const auto limited = io::read_zip(*zip, tiny);
    REQUIRE_FALSE(limited);
    CHECK(limited.error().code() == ErrorCode::LimitExceeded);
    CHECK_FALSE(io::read_zip(std::vector<u8>{'n', 'o', 't', 'z', 'i', 'p'}));
}
