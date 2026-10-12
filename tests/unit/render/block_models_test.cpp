// Model baking and meshing against a tiny synthetic resource pack written by the test (no
// Mojang assets in the repository; docs/formats/assets.md).

#include "../test_util.hpp"

#include <bridger/render/assets.hpp>
#include <bridger/render/block_models.hpp>
#include <bridger/render/image_io.hpp>
#include <bridger/render/mesher.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace bridger;
using namespace bridger::render;
using Catch::Approx;

namespace {

void write_text(const std::filesystem::path& p, std::string_view text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A 16 x 16 texture: `alpha` everywhere, except a fully transparent corner when `hole`.
void write_texture(const std::filesystem::path& p, u8 r, u8 g, u8 b, u8 alpha, bool hole = false) {
    std::vector<u8> rgba(16 * 16 * 4);
    for (usize i = 0; i < 256; ++i) {
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = (hole && i < 4) ? 0 : alpha;
    }
    std::filesystem::create_directories(p.parent_path());
    REQUIRE(write_png(p, 16, 16, rgba));
}

struct Pack {
    test::TempDir dir;
    std::filesystem::path root;
    Pack() : root(dir.path / "pack") {
        const auto a = root / "assets" / "minecraft";
        write_text(a / "models/block/cube_all.json", R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{
            "down":{"texture":"#all","cullface":"down"},"up":{"texture":"#all","cullface":"up"},
            "north":{"texture":"#all","cullface":"north"},"south":{"texture":"#all","cullface":"south"},
            "west":{"texture":"#all","cullface":"west"},"east":{"texture":"#all","cullface":"east"}}}]})");
        write_text(a / "models/block/stone.json", R"({"parent":"block/cube_all","textures":{"all":"block/stone"}})");
        write_text(a / "blockstates/stone.json", R"({"variants":{"":{"model":"block/stone"}}})");
        write_texture(a / "textures/block/stone.png", 120, 120, 120, 255);

        // Glass: cutout texture, 26.x object form of the texture reference.
        write_text(a / "models/block/glass.json", R"({"parent":"block/cube_all","textures":{"all":{"sprite":"block/glass"}}})");
        write_text(a / "blockstates/glass.json", R"({"variants":{"":{"model":"block/glass"}}})");
        write_texture(a / "textures/block/glass.png", 200, 230, 255, 255, true);

        // Bottom slab: half-height element, so only its bottom face occludes.
        write_text(a / "models/block/slab.json", R"({"textures":{"side":"block/stone"},"elements":[{"from":[0,0,0],"to":[16,8,16],"faces":{
            "down":{"texture":"#side","cullface":"down"},"up":{"texture":"#side"},
            "north":{"texture":"#side","cullface":"north"},"south":{"texture":"#side","cullface":"south"},
            "west":{"texture":"#side","cullface":"west"},"east":{"texture":"#side","cullface":"east"}}}]})");
        write_text(a / "blockstates/stone_slab.json",
                   R"({"variants":{"type=bottom,waterlogged=false":{"model":"block/slab"},"type=top,waterlogged=false":{"model":"block/slab","x":180}}})");

        // Multipart fence-like block: a post plus an arm when north=true.
        write_text(a / "models/block/post.json", R"({"textures":{"t":"block/stone"},"elements":[{"from":[6,0,6],"to":[10,16,10],"faces":{"up":{"texture":"#t"}}}]})");
        write_text(a / "models/block/arm.json", R"({"textures":{"t":"block/stone"},"elements":[{"from":[7,12,0],"to":[9,15,6],"faces":{"up":{"texture":"#t"}}}]})");
        write_text(a / "blockstates/test_fence.json",
                   R"({"multipart":[{"apply":{"model":"block/post"}},{"when":{"north":"true"},"apply":{"model":"block/arm"}},
                       {"when":{"OR":[{"east":"true"},{"west":"true"}]},"apply":{"model":"block/arm","y":90}}]})");
    }
};

BlockModels make_models(const Pack& pack) {
    auto assets = AssetSource::open({pack.root});
    REQUIRE(assets);
    auto models = BlockModels::create(std::make_shared<AssetSource>(std::move(*assets)));
    REQUIRE(models);
    return std::move(*models);
}

const BakedBlock& bake(BlockModels& m, std::string_view state) {
    auto s = BlockState::parse(state);
    REQUIRE(s);
    return m.bake(*s);
}

} // namespace

TEST_CASE("A full cube bakes six culled faces and occludes every side", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    const BakedBlock& stone = bake(models, "minecraft:stone");
    REQUIRE(stone.variants.size() == 1);
    CHECK(stone.variants[0].quads.size() == 6);
    CHECK(stone.layer == RenderLayer::Solid);
    for (bool o : stone.occludes) CHECK(o);
    for (const BakedQuad& q : stone.variants[0].quads) {
        CHECK(q.cull_face >= 0);
        CHECK(q.full_face);
    }
    CHECK(models.missing().empty());
    // The texture array holds the stone layer (after the built-in layers).
    const u16 layer = models.texture("block/stone");
    CHECK(layer < models.texture_count());
    CHECK(models.texture_pixels().size() == models.texture_count() * 256);
}

TEST_CASE("Textures with clear pixels make a cutout layer, and the object texture form is read", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    const BakedBlock& glass = bake(models, "minecraft:glass");
    CHECK(glass.layer == RenderLayer::Cutout);
    CHECK(glass.cull_same);
    for (bool o : glass.occludes) CHECK_FALSE(o); // see-through: neighbours stay visible
    CHECK(models.missing().empty());
}

TEST_CASE("A bottom slab occludes only downward; x=180 flips it", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    const BakedBlock& bottom = bake(models, "minecraft:stone_slab[type=bottom,waterlogged=false]");
    CHECK(bottom.occludes[0]);
    CHECK_FALSE(bottom.occludes[1]);
    const BakedBlock& top = bake(models, "minecraft:stone_slab[type=top,waterlogged=false]");
    CHECK(top.occludes[1]);
    CHECK_FALSE(top.occludes[0]);
    float max_y = 0.0f;
    for (const auto& q : top.variants[0].quads)
        for (const auto& p : q.pos) max_y = std::max(max_y, p.y);
    CHECK(max_y == Approx(1.0f));
}

TEST_CASE("Multipart applies the parts whose conditions hold, including OR", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    CHECK(bake(models, "minecraft:test_fence[east=false,north=false,west=false]").variants[0].quads.size() == 1);
    CHECK(bake(models, "minecraft:test_fence[east=false,north=true,west=false]").variants[0].quads.size() == 2);
    CHECK(bake(models, "minecraft:test_fence[east=true,north=true,west=false]").variants[0].quads.size() == 3);
}

TEST_CASE("Unknown blocks get the missing model and are listed", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    const BakedBlock& b = bake(models, "minecraft:not_a_block");
    CHECK_FALSE(b.variants.empty());
    CHECK(models.missing().size() == 1);
}

TEST_CASE("The mesher hides faces between solid blocks", "[render]") {
    Pack pack;
    BlockModels models = make_models(pack);
    const BakedBlock* stone = &bake(models, "minecraft:stone");
    std::array<const BakedBlock*, kPaddedVolume> blocks{};

    SectionMesh one;
    blocks[padded_index(5, 5, 5)] = stone;
    mesh_section(blocks, {0, 0, 0}, one);
    CHECK(one.solid.size() == 6 * 4);

    SectionMesh two;
    blocks[padded_index(6, 5, 5)] = stone; // the shared face disappears on both blocks
    mesh_section(blocks, {0, 0, 0}, two);
    CHECK(two.solid.size() == 10 * 4);

    // A neighbour in the padding (another section) also hides the face.
    SectionMesh edge;
    std::array<const BakedBlock*, kPaddedVolume> b2{};
    b2[padded_index(15, 0, 0)] = stone;
    b2[padded_index(16, 0, 0)] = stone;
    mesh_section(b2, {0, 0, 0}, edge);
    CHECK(edge.solid.size() == 5 * 4);
    CHECK(edge.cutout.empty());
    CHECK(edge.translucent.empty());
}
