// Block states, registries, sections and structures.

#include <bridger/core/block_registry.hpp>
#include <bridger/core/block_state.hpp>
#include <bridger/core/section.hpp>
#include <bridger/core/structure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

using namespace bridger;

TEST_CASE("Block states parse to a canonical form", "[core]") {
    const auto s = BlockState::parse("minecraft:oak_stairs[waterlogged=false,half=bottom,facing=east]");
    REQUIRE(s);
    CHECK(s->id() == "minecraft:oak_stairs");
    CHECK(s->to_string() == "minecraft:oak_stairs[facing=east,half=bottom,waterlogged=false]");
    CHECK(s->property("half") == "bottom");
    CHECK_FALSE(s->property("shape"));

    CHECK(BlockState::parse("stone")->to_string() == "minecraft:stone");
    CHECK(BlockState::parse("minecraft:air")->properties().empty());
    CHECK(*BlockState::parse("a:b[y=1,x=2]") == *BlockState::parse("a:b[x=2,y=1]"));

    for (const char* bad : {"", "minecraft:", ":stone", "minecraft:stone[", "minecraft:stone[]",
                            "minecraft:stone[facing]", "minecraft:stone[a=1,a=2]", "minecraft:stone[a=]",
                            "minecraft: stone"}) {
        CAPTURE(bad);
        CHECK_FALSE(BlockState::parse(bad));
    }
}

TEST_CASE("Registry interns states to dense ids, air first", "[core]") {
    BlockStateRegistry reg;
    CHECK(reg.size() == 1);
    CHECK(reg.canonical(kAirStateId) == "minecraft:air");
    const StateId a = reg.intern(*BlockState::parse("minecraft:stone"));
    const StateId b = reg.intern(*BlockState::parse("minecraft:oak_log[axis=y]"));
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(reg.intern(*BlockState::parse("minecraft:stone")) == a);
    CHECK(reg.get(b).property("axis") == "y");
    CHECK(reg.find(*BlockState::parse("minecraft:dirt")) == std::nullopt);
}

TEST_CASE("Registry interning is thread-safe", "[core]") {
    BlockStateRegistry reg;
    std::vector<std::thread> threads;
    std::vector<std::vector<StateId>> ids(8);
    for (usize t = 0; t < 8; ++t) {
        threads.emplace_back([&reg, &ids, t] {
            for (int i = 0; i < 500; ++i)
                ids[t].push_back(reg.intern(BlockState("test:block_" + std::to_string(i), {})));
        });
    }
    for (auto& th : threads) th.join();
    CHECK(reg.size() == 501);
    for (usize t = 1; t < 8; ++t) CHECK(ids[t] == ids[0]);
}

TEST_CASE("Palette layer picks the narrowest storage", "[core]") {
    PaletteLayer layer(7);
    CHECK(layer.is_uniform());
    CHECK(layer.get(4095) == 7);

    layer.set(section_index(1, 2, 3), 9);
    CHECK_FALSE(layer.is_uniform());
    CHECK(layer.get(section_index(1, 2, 3)) == 9);
    CHECK(layer.get(0) == 7);

    // > 256 distinct values forces 16-bit indices.
    for (u32 i = 0; i < 300; ++i) layer.set(i, 1000 + i);
    for (u32 i = 0; i < 300; ++i) CHECK(layer.get(i) == 1000 + i);
    CHECK(layer.get(4000) == 7);

    layer.fill(3);
    CHECK(layer.is_uniform());
    CHECK(layer.get(123) == 3);
}

TEST_CASE("assign() and remap() implement the palette stage", "[core]") {
    std::array<u32, kSectionVolume> values{};
    for (usize i = 0; i < kSectionVolume; ++i) values[i] = (i % 3 == 0) ? 50 : 60;
    PaletteLayer layer;
    layer.assign(values);
    REQUIRE(layer.palette().size() == 2);

    // Translate the 2 palette entries; 4096 positions follow without being touched.
    std::vector<u32> translated(layer.palette().begin(), layer.palette().end());
    for (auto& v : translated) v = v == 50 ? 5 : 6;
    layer.remap(translated);
    std::array<u32, kSectionVolume> out{};
    layer.copy_to(out);
    for (usize i = 0; i < kSectionVolume; ++i) CHECK(out[i] == ((i % 3 == 0) ? 5u : 6u));

    std::array<u32, kSectionVolume> uniform{};
    uniform.fill(42);
    layer.assign(uniform);
    CHECK(layer.is_uniform());
}

TEST_CASE("compact() drops stale palette entries", "[core]") {
    PaletteLayer layer;
    for (u32 v = 1; v <= 100; ++v) layer.set(0, v); // 100 entries, only the last is used
    CHECK(layer.palette().size() == 101);
    layer.compact();
    CHECK(layer.palette().size() == 2); // fill value 0 and 100
    CHECK(layer.get(0) == 100);
}

TEST_CASE("Secondary layer and biomes are allocated only when used", "[core]") {
    Section s;
    s.set_secondary(1, 1, 1, kAirStateId);
    CHECK_FALSE(s.has_secondary());
    s.set_secondary(1, 1, 1, 5); // waterlogging
    CHECK(s.has_secondary());
    CHECK(s.secondary(1, 1, 1) == 5);
    CHECK(s.secondary(0, 0, 0) == kAirStateId);
    CHECK_FALSE(s.biome(0, 0, 0));

    const Section copy(s); // deep copy
    s.set_secondary(1, 1, 1, 6);
    CHECK(copy.secondary(1, 1, 1) == 5);
}

TEST_CASE("Structure maps local positions across section borders", "[core]") {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    auto s = Structure::create(blocks, biomes, {33, 17, 2}, {});
    REQUIRE(s);
    CHECK(s->sections_x() == 3);
    CHECK(s->sections_y() == 2);
    CHECK(s->sections_z() == 1);
    CHECK(s->set_block({32, 16, 1}, 4));
    CHECK(s->block({32, 16, 1}) == 4);
    CHECK(s->block({31, 16, 1}) == kAirStateId);
    CHECK_FALSE(s->set_block({33, 0, 0}, 4)); // outside
    CHECK_FALSE(s->set_block({-1, 0, 0}, 4));
    CHECK(s->block({-1, 0, 0}) == kAirStateId);

    CHECK_FALSE(Structure::create(blocks, biomes, {65535, 65535, 65535}, {}));
}
