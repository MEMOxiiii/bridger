// Old Bedrock worlds: every historical chunk layout (docs/formats/bedrock-world.md, "Old worlds"),
// written key by key into a LevelDB, then read with the world reader and the state lookup that
// conversions use (block state upgrades included).

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/translate/bedrock_upgrade.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>

using namespace bridger;
using namespace bridger::formats::bedrock;
namespace fs = std::filesystem;

namespace {

const translate::MappingData& mapping() {
    static const translate::MappingData* m = *translate::MappingData::embedded();
    return *m;
}

std::vector<u8> nbt_entry(const std::function<void(io::nbt::NbtWriter&)>& body) {
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound();
    body(w);
    w.end_compound();
    return *std::move(w).finish();
}

// One persistent storage, 4 bits per index; index(x, y, z) picks the palette entry.
void write_storage(io::BinaryWriter& w, const std::vector<std::vector<u8>>& palette, const std::function<u32(u32, u32, u32)>& index) {
    w.write<u8>(4 << 1);
    for (u32 word = 0; word < 512; ++word) {
        u32 v = 0;
        for (u32 k = 0; k < 8; ++k) {
            const u32 i = word * 8 + k; // XZY: i = x*256 + z*16 + y
            v |= index(i >> 8, i & 15, (i >> 4) & 15) << (k * 4);
        }
        w.write<u32>(v);
    }
    w.write<i32>(static_cast<i32>(palette.size()));
    for (const auto& e : palette) w.write_bytes(e);
}

std::string bedrock_name_at(const BedrockColumn& c, i32 x, i32 y, i32 z) {
    return std::string(mapping().bedrock(c.column.block({x, y - c.min_y, z})).name);
}

struct OldWorld {
    test::TempDir dir;
    std::vector<u8> mcworld;

    OldWorld() {
        const fs::path db_dir = dir.path / "src" / "db";
        fs::create_directories(db_dir);
        std::optional<LevelDbStore> db;
        {
            auto opened = LevelDbStore::open(db_dir, /*create_new=*/true);
            REQUIRE(opened);
            db.emplace(std::move(*opened));
        }
        auto put = [&](const std::string& key, std::span<const u8> v) { REQUIRE(db->put(key, v)); };
        auto tag = [](u8 t) { return static_cast<ChunkTag>(t); };
        const std::array<u8, 1> v2{2}, v3{3}, v8{8}, v22{22}, v25{25};

        // Chunk (0, 0): pre-1.0 LegacyTerrain (chunk version 2 under the LegacyVersion key).
        {
            std::vector<u8> t(32768 + 16384 + 32768 + 256 + 1024, 0);
            auto set = [&](usize x, usize y, usize z, u8 id, u8 meta) {
                const usize i = (x << 11) | (z << 7) | y;
                t[i] = id;
                t[32768 + (i >> 1)] |= static_cast<u8>(meta << ((i & 1) * 4));
            };
            for (usize x = 0; x < 16; ++x)
                for (usize z = 0; z < 16; ++z)
                    for (usize y = 0; y < 4; ++y) set(x, y, z, 1, 0); // stone
            set(1, 4, 2, 35, 14);                                      // red wool
            set(15, 127, 15, 5, 2);                                    // birch planks at the top
            const usize b = 32768 + 16384 + 32768 + 256;
            for (usize i = 0; i < 256; ++i) t[b + i * 4] = 2; // desert
            put(chunk_key(0, 0, Dimension::Overworld, tag(0x76)), v2);
            put(chunk_key(0, 0, Dimension::Overworld, tag(0x30)), t);
        }
        // Chunk (1, 0): 1.0 subchunk version 0, Data2D biomes, one legacy extra-data block.
        {
            std::vector<u8> sub(1 + 4096 + 2048 + 4096, 0);
            sub[0] = 0;
            sub[1 + ((3 << 8) | (2 << 4) | 1)] = 5; // birch planks at x=3, z=2, y=1
            sub[1 + 4096 + (((3 << 8) | (2 << 4) | 1) >> 1)] = 2 << 4; // odd index: high nibble
            put(chunk_key(1, 0, Dimension::Overworld, tag(0x76)), v3);
            put(chunk_key(1, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{0}), sub);
            std::vector<u8> d2(512 + 256, 1); // plains
            put(chunk_key(1, 0, Dimension::Overworld, tag(0x2D)), d2);
            io::BinaryWriter extra(Endian::Little);
            extra.write<i32>(1);
            extra.write<u32>((3u << 12) | (2u << 8) | 1u); // x=3, z=2, y=1
            extra.write<u16>(78);                          // snow layer
            put(chunk_key(1, 0, Dimension::Overworld, tag(0x34)), std::move(extra).take());
        }
        // Chunk (2, 0): 1.2.13 subchunk version 1 with a {name, val} palette.
        {
            io::BinaryWriter w(Endian::Little);
            w.write<u8>(1);
            const auto air = nbt_entry([](auto& n) { n.write_string("name", "minecraft:air"); n.write_short("val", 0); });
            const auto granite = nbt_entry([](auto& n) { n.write_string("name", "minecraft:stone"); n.write_short("val", 1); });
            write_storage(w, {air, granite}, [](u32 x, u32 y, u32 z) { return x == 0 && y == 0 && z == 0 ? 1u : 0u; });
            put(chunk_key(2, 0, Dimension::Overworld, ChunkTag::Version), v8);
            put(chunk_key(2, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{0}), std::move(w).take());
        }
        // Chunk (3, 0): 1.16 subchunk version 8 with an old-version state (wool with a colour state).
        {
            io::BinaryWriter w(Endian::Little);
            w.write<u8>(8);
            w.write<u8>(1);
            const auto wool = nbt_entry([](auto& n) {
                n.write_string("name", "minecraft:wool");
                n.begin_compound("states");
                n.write_string("color", "red");
                n.end_compound();
                n.write_int("version", 17694723); // 1.14.0.3
            });
            write_storage(w, {wool}, [](u32, u32, u32) { return 0u; });
            put(chunk_key(3, 0, Dimension::Overworld, ChunkTag::Version), v22);
            put(chunk_key(3, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{2}), std::move(w).take());
        }
        // Chunk (4, 0): experimental Caves & Cliffs world (chunk version 25): subchunk y stored at key y + 4.
        {
            io::BinaryWriter w(Endian::Little);
            w.write<u8>(8);
            w.write<u8>(1);
            const auto stone = nbt_entry([](auto& n) {
                n.write_string("name", "minecraft:stone");
                n.begin_compound("states");
                n.write_string("stone_type", "stone");
                n.end_compound();
                n.write_int("version", 17959425); // 1.18.10
            });
            write_storage(w, {stone}, [](u32, u32, u32) { return 0u; });
            put(chunk_key(4, 0, Dimension::Overworld, ChunkTag::Version), v25);
            put(chunk_key(4, 0, Dimension::Overworld, ChunkTag::SubChunkPrefix, i8{4}), std::move(w).take()); // y = 0
        }
        REQUIRE(db->flush());
        db.reset();

        // level.dat: i32 storage version, i32 length, LE NBT.
        const auto ld = nbt_entry([](auto& n) { n.write_string("LevelName", "Old"); n.write_int("SpawnX", 0); n.write_int("SpawnY", 64); n.write_int("SpawnZ", 0); });
        std::vector<io::ZipEntry> zip;
        io::BinaryWriter level(Endian::Little);
        level.write<i32>(3);
        level.write<i32>(static_cast<i32>(ld.size()));
        level.write_bytes(ld);
        const std::vector<u8> level_bytes = std::move(level).take();
        std::ofstream(dir.path / "src" / "level.dat", std::ios::binary)
            .write(reinterpret_cast<const char*>(level_bytes.data()), static_cast<std::streamsize>(level_bytes.size()));
        zip.push_back({"level.dat", level_bytes});
        for (const auto& e : fs::recursive_directory_iterator(db_dir)) {
            if (!e.is_regular_file()) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
            zip.push_back({"db/" + e.path().filename().string(), std::move(bytes)});
        }
        mcworld = *io::write_zip(zip);
    }
};

} // namespace

TEST_CASE("The upgrade data loads and upgrades each historical entry form", "[translate][bedrock][old]") {
    const auto up = translate::BedrockUpgrader::embedded();
    REQUIRE(up);
    CHECK((*up)->schema_count() >= 36);

    auto name_of = [&](std::optional<translate::UpgradedState> s) { return s ? s->name : std::string("<none>"); };
    CHECK(name_of((*up)->from_legacy("minecraft:wool", 14)) == "minecraft:red_wool");
    CHECK(name_of((*up)->from_legacy("stone", 1)) == "minecraft:granite");
    CHECK(name_of((*up)->from_legacy_id(5, 2)) == "minecraft:birch_planks");
    CHECK(name_of((*up)->from_legacy_id(17, 0)) == "minecraft:oak_log");
    CHECK_FALSE((*up)->from_legacy_id(100000, 0)); // not a block id
    // Unknown data values fall back to data 0.
    CHECK(name_of((*up)->from_legacy("minecraft:planks", 15)) == "minecraft:oak_planks");
}

TEST_CASE("Every pre-1.13 Bedrock block id upgrades to a state of the pinned palette", "[translate][bedrock][old]") {
    const auto up = *translate::BedrockUpgrader::embedded();
    usize total = 0, missing = 0;
    std::string examples;
    for (i32 id = 0; id < 256; ++id)
        for (i32 data = 0; data < 16; ++data) {
            const auto st = up->from_legacy_id(id, data);
            if (!st) continue;
            ++total;
            const auto props = st->properties();
            if (!mapping().find_bedrock(st->name, props)) {
                ++missing;
                if (missing <= 5) examples += " " + st->name;
            }
        }
    INFO("states missing from the pinned palette:" << examples);
    CHECK(total > 1000);
    CHECK(missing == 0);
}

TEST_CASE("Old subchunk layouts decode", "[formats][bedrock][old]") {
    SECTION("version 0: numeric ids and data nibbles") {
        std::vector<u8> sub(1 + 4096 + 2048, 0);
        sub[1 + 1] = 35;           // x=0, z=0, y=1 (index 1, odd)
        sub[1 + 4096 + 0] = 0xE0;  // data 14 in the high nibble
        const auto r = decode_subchunk_raw(sub);
        REQUIRE(r);
        CHECK(r->legacy);
        REQUIRE(r->layers.size() == 1);
        const auto& layer = r->layers[0];
        CHECK(layer.palette.size() == 2);
        const u16 at = layer.indices[(1 << 8)]; // Bridger order y*256 + z*16 + x
        CHECK(at == 1);
        CHECK(layer.palette[1] == legacy_palette_entry(35, 14));
    }
    SECTION("truncated version 0 and unknown versions are refused") {
        CHECK_FALSE(decode_subchunk_raw(std::vector<u8>(100, 0)));
        CHECK_FALSE(decode_subchunk_raw(std::vector<u8>{10, 0}));
    }
    SECTION("bits 0 storage (one entry, no words)") {
        io::BinaryWriter w(Endian::Little);
        w.write<u8>(8);
        w.write<u8>(1);
        w.write<u8>(0);
        w.write_bytes(nbt_entry([](auto& n) { n.write_string("name", "minecraft:stone"); }));
        const auto r = decode_subchunk_raw(std::move(w).take());
        REQUIRE(r);
        CHECK(r->layers[0].palette.size() == 1);
    }
}

TEST_CASE("Old Bedrock worlds read with upgraded blocks and biomes", "[formats][bedrock][old]") {
    OldWorld w;
    auto reader = BedrockWorldReader::open(w.mcworld, w.dir.path / "work");
    REQUIRE(reader);
    CHECK(reader->overworld_chunks().size() == 5);

    const auto& m = mapping();
    const auto lookup = convert::bedrock_state_lookup(m);
    const auto air = m.find_bedrock("minecraft:air", {});
    REQUIRE(air);
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    auto read = [&](i32 cx) {
        auto c = reader->read_column({cx, 0}, blocks, biomes, m.bedrock_block_version(), lookup, *air, 1);
        REQUIRE(c);
        INFO("unknown blocks: " << c->unknown_blocks.size());
        CHECK(c->unknown_blocks.empty());
        return std::move(*c);
    };

    const BedrockColumn legacy_terrain = read(0);
    CHECK(bedrock_name_at(legacy_terrain, 0, 0, 0) == "minecraft:stone");
    CHECK(bedrock_name_at(legacy_terrain, 1, 4, 2) == "minecraft:red_wool");
    CHECK(bedrock_name_at(legacy_terrain, 15, 127, 15) == "minecraft:birch_planks");
    CHECK(bedrock_name_at(legacy_terrain, 0, 4, 0) == "minecraft:air");
    CHECK(legacy_terrain.column.biome({5, 0 - legacy_terrain.min_y, 5}) == BiomeId{2});

    const BedrockColumn v0 = read(1);
    CHECK(bedrock_name_at(v0, 3, 1, 2) == "minecraft:birch_planks");
    CHECK(v0.column.biome({0, 70 - v0.min_y, 0}) == BiomeId{1});
    const Section& sec = v0.column.section(0, static_cast<u32>((0 - v0.min_y) / 16), 0);
    REQUIRE(sec.has_secondary());
    CHECK(m.bedrock(sec.secondary(3, 1, 2)).name == "minecraft:snow_layer");

    CHECK(bedrock_name_at(read(2), 0, 0, 0) == "minecraft:granite");
    CHECK(bedrock_name_at(read(3), 5, 40, 5) == "minecraft:red_wool");
    const BedrockColumn caves = read(4);
    CHECK(bedrock_name_at(caves, 0, 0, 0) == "minecraft:stone");
    CHECK(bedrock_name_at(caves, 0, -64, 0) == "minecraft:air");
}

TEST_CASE("A Bedrock world folder opens from a copy and stays unchanged", "[formats][bedrock][old][safety]") {
    OldWorld w;
    const fs::path folder = w.dir.path / "src";
    CHECK(is_bedrock_world_folder(folder));
    CHECK(is_bedrock_world_folder(folder / "level.dat"));
    CHECK_FALSE(is_bedrock_world_folder(w.dir.path));

    auto listing = [&] {
        std::vector<std::pair<std::string, u64>> out;
        for (const auto& e : fs::recursive_directory_iterator(folder))
            out.emplace_back(e.path().lexically_relative(folder).generic_string(), e.is_regular_file() ? e.file_size() : 0);
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto before = listing();
    {
        auto reader = BedrockWorldReader::open_path(folder, w.dir.path / "work");
        REQUIRE(reader);
        CHECK(reader->overworld_chunks().size() == 5);
        CHECK(reader->level().level_name == "Old");
    }
    CHECK(listing() == before);
}
