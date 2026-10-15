// Distant terrain: column summaries, LOD meshes and the on-disk cache (no GPU, no game assets).

#include "../test_util.hpp"

#include <bridger/render/lod_cache.hpp>
#include <bridger/render/mesher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace bridger;
using namespace bridger::render;

namespace {

BakedBlock solid(u32 color, Tint tint = Tint::None) {
    BakedBlock b;
    b.layer = RenderLayer::Solid;
    b.map_color = color;
    b.tint = tint;
    b.map_tinted = tint != Tint::None;
    return b;
}

ColumnSummary flat(i16 top, u32 color) {
    ColumnSummary s;
    s.top.fill(top);
    s.color.fill(color);
    return s;
}

usize quads(const std::map<int, SectionMesh>& m) {
    usize n = 0;
    for (const auto& [y, s] : m) n += (s.solid.size() + s.cutout.size() + s.translucent.size()) / 4;
    return n;
}

} // namespace

TEST_CASE("A column summary keeps the top visible block and its colour", "[render][lod]") {
    const BakedBlock stone = solid(0xFF808080u), grass = solid(0xFF7F7F7Fu, Tint::Grass);
    BakedBlock glass_like = solid(0);  // no map colour: skipped
    const ColumnSummary s = summarize_column(
        [&](int x, int y, int z) -> const BakedBlock* {
            if (x == 3 && z == 4 && y == 20) return &glass_like;
            if (x == 3 && z == 4 && y == 11) return &grass;
            if (x == 5 && z == 5) return nullptr; // an empty column
            return y <= 10 ? &stone : nullptr;
        },
        -64, 384);
    CHECK(s.top[0] == 10);
    CHECK(s.color[0] == 0xFF808080u);
    CHECK(s.top[4 * 16 + 3] == 11);
    CHECK(s.color[4 * 16 + 3] != 0xFF7F7F7Fu); // tinted green
    CHECK(s.top[5 * 16 + 5] == ColumnSummary::kNoTop);
}

TEST_CASE("Summarizing a loaded column matches the block-by-block summary", "[render][lod]") {
    const BakedBlock stone = solid(0xFF808080u), grass = solid(0xFF7F7F7Fu, Tint::Grass);
    auto blocks = std::make_shared<BlockStateRegistry>();
    const StateId sid = blocks->intern(BlockState("minecraft:stone", {})), gid = blocks->intern(BlockState("minecraft:grass_block", {}));
    auto column = Structure::create(blocks, std::make_shared<BiomeRegistry>(), Extent{16, 256, 16}, {Edition::Java, 0});
    REQUIRE(column);
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            if (x == 7 && z == 7) continue; // one empty column
            const int h = 40 + x * 3 + z; // crosses several sections
            for (int y = 0; y <= h; ++y) column->set_block({x, y, z}, y == h ? gid : sid);
        }
    std::vector<const BakedBlock*> table(blocks->size(), nullptr);
    table[sid] = &stone;
    table[gid] = &grass;
    const ColumnSummary fast = summarize_column(*column, table, 0);
    const ColumnSummary slow = summarize_column(
        [&](int x, int y, int z) -> const BakedBlock* {
            const StateId id = column->block({x, y, z});
            return id < table.size() ? table[id] : nullptr;
        },
        0, 256);
    CHECK(fast == slow);
    CHECK(fast.top[0] == 40);
    CHECK(fast.top[7 * 16 + 7] == ColumnSummary::kNoTop);
}

TEST_CASE("LOD meshes merge equal tops and close borders with skirts or neighbours", "[render][lod]") {
    const ColumnSummary s = flat(10, 0xFF808080u);
    std::map<int, SectionMesh> alone;
    mesh_lod(s, {nullptr, nullptr, nullptr, nullptr}, {0, -64, 0}, 2, 7, alone);
    // One merged top + a skirt on each border cell (8 per side, 8 blocks deep, inside one section).
    CHECK(quads(alone) == 1 + 4 * 8);
    for (const auto& [y, m] : alone)
        for (const Vertex& v : m.solid) {
            CHECK(v.layer == 7);
            CHECK(v.y >= 0.0f);
            CHECK(v.y <= 16.0f); // section-local
        }

    // Neighbours at the same height: no sides at all.
    std::map<int, SectionMesh> joined;
    mesh_lod(s, {&s, &s, &s, &s}, {0, -64, 0}, 2, 7, joined);
    CHECK(quads(joined) == 1);

    // A step inside the column: the higher half gets a side down to the lower one.
    ColumnSummary step = s;
    for (int z = 0; z < 16; ++z)
        for (int x = 8; x < 16; ++x) step.top[static_cast<usize>(z * 16 + x)] = 14;
    const ColumnSummary high = flat(14, 0xFF808080u); // neighbours never lower than the column
    std::map<int, SectionMesh> stepped;
    mesh_lod(step, {&high, &high, &high, &high}, {0, -64, 0}, 4, 7, stepped);
    CHECK(quads(stepped) == 2 + 4); // two tops, one side per cell row along the step (16 / 4 rows)

    // A tall side is split at section boundaries so each quad stays inside its culling box.
    ColumnSummary tall = flat(40, 0xFF808080u);
    std::map<int, SectionMesh> split;
    mesh_lod(tall, {&s, &s, &s, &s}, {0, -64, 0}, 16, 7, split);
    // sides from y 11 to 40: sections [0,16), [16,32), [32,48) -> 3 pieces per side, 4 sides, + top
    CHECK(quads(split) == 1 + 4 * 3);
}

TEST_CASE("The LOD cache survives a restart and ignores damaged files", "[render][lod]") {
    test::TempDir dir;
    ColumnSummary a = flat(64, 0xFF112233u), b = flat(-20, 0xFF445566u);
    a.top[17] = ColumnSummary::kNoTop;
    {
        LodCache c(dir.path / "cache");
        c.put({0, 0}, a);
        c.put({-1, 33}, b); // another region (r.-1.1)
        CHECK(c.get({0, 0}) == a);
        REQUIRE(c.flush());
    }
    {
        LodCache c(dir.path / "cache");
        CHECK(c.get({0, 0}) == a);
        CHECK(c.get({-1, 33}) == b);
        CHECK_FALSE(c.get({1, 0}));
    }
    // Flip a byte: the checksum fails and the region reads as empty.
    const auto file = dir.path / "cache" / "r.0.0.blod";
    REQUIRE(std::filesystem::exists(file));
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(200);
        f.put('\x7f');
    }
    LodCache c(dir.path / "cache");
    CHECK_FALSE(c.get({0, 0}));
    CHECK(c.get({-1, 33}) == b);
}
