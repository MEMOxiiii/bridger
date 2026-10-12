// Java -> Bedrock -> Java round trip (Phase 6 acceptance): every block state, block entity and
// biome that both editions can represent must come back unchanged; anything else must be in
// the conversion report.

#include "../test_util.hpp"

#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>

using namespace bridger;

namespace {

const translate::MappingData& mapping() { return *translate::MappingData::embedded().value(); }

Structure read(const std::string& file) {
    const auto bytes = test::read_file(test::data_path(file));
    auto f = formats::read_java_structure(bytes, formats::FormatId::SpongeSchem, std::make_shared<BlockStateRegistry>(),
                                          std::make_shared<BiomeRegistry>());
    REQUIRE(f);
    return std::move(f->structure);
}

// Item ids and counts of a Java container, slot -> "id x count".
std::map<i8, std::string> items_of(const BlockEntity& be) {
    std::map<i8, std::string> out;
    if (be.data.empty()) return out;
    auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    REQUIRE(doc);
    for (const auto it : doc->root().entries().find("Items").items()) {
        const auto c = it.entries();
        const i32 count = c.find("count").as_int().value_or(c.find("Count").as_byte().value_or(1));
        out[c.find("Slot").as_byte().value_or(0)] = std::string(c.find("id").as_string().value_or("?")) + " x" + std::to_string(count);
    }
    return out;
}

std::string without_shape(std::string s) {
    const auto at = s.find("shape=");
    if (at == std::string::npos) return s;
    const auto end = s.find_first_of(",]", at);
    s.erase(at, end - at + (s[end] == ',' ? 1 : 0));
    return s;
}

Structure java_structure(Extent size) {
    auto s = Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), size,
                               {Edition::Java, mapping().java_data_version()});
    REQUIRE(s);
    return std::move(*s);
}

void put(Structure& s, BlockPos p, const char* state) { s.set_block(p, s.block_registry().intern(*BlockState::parse(state))); }

std::string at(const Structure& s, BlockPos p) { return std::string(s.block_registry().canonical(s.block(p))); }

Structure round_trip(const Structure& java, translate::ConversionReport& report) {
    auto bedrock = translate::translate_to_bedrock(java, mapping(), {}, report);
    REQUIRE(bedrock);
    auto back = translate::translate_to_java(*bedrock, mapping(), {}, report);
    REQUIRE(back);
    return std::move(*back);
}

} // namespace

TEST_CASE("Stair shapes and fence connections are rebuilt from neighbours", "[translate][roundtrip]") {
    // An L of stairs: the corner takes an outer shape from its neighbours, as in Java.
    Structure s = java_structure({4, 1, 4});
    put(s, {1, 0, 1}, "minecraft:oak_stairs[facing=north,half=bottom,shape=outer_left,waterlogged=false]");
    put(s, {0, 0, 1}, "minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]");
    put(s, {1, 0, 0}, "minecraft:oak_stairs[facing=west,half=bottom,shape=straight,waterlogged=false]");
    // A fence between a plank and another fence; a pane next to glass.
    put(s, {3, 0, 0}, "minecraft:oak_planks");
    put(s, {3, 0, 1}, "minecraft:oak_fence[east=false,north=true,south=true,waterlogged=false,west=false]");
    put(s, {3, 0, 2}, "minecraft:oak_fence[east=false,north=true,south=false,waterlogged=false,west=false]");
    put(s, {2, 0, 3}, "minecraft:glass");
    put(s, {3, 0, 3}, "minecraft:glass_pane[east=false,north=false,south=false,waterlogged=false,west=true]");
    translate::ConversionReport report;
    const Structure back = round_trip(s, report);
    for (const BlockPos p : {BlockPos{1, 0, 1}, BlockPos{0, 0, 1}, BlockPos{1, 0, 0}, BlockPos{3, 0, 1}, BlockPos{3, 0, 2},
                             BlockPos{3, 0, 3}}) {
        CAPTURE(p.x, p.z);
        CHECK(at(back, p) == at(s, p));
    }
}

TEST_CASE("Java -> Bedrock -> Java preserves every representable block state", "[translate][roundtrip]") {
    const std::string file = GENERATE("ingame/orientations.schem", "ingame/waterlogged.schem", "ingame/containers.schem",
                                      "ingame/signs_beds.schem", "ingame/borders.schem", "schem/v3_basic.schem");
    CAPTURE(file);
    const Structure java = read(file);
    translate::ConversionReport to_bedrock, to_java;
    auto bedrock = translate::translate_to_bedrock(java, mapping(), {}, to_bedrock);
    REQUIRE(bedrock);
    auto back = translate::translate_to_java(*bedrock, mapping(), {}, to_java);
    REQUIRE(back);
    REQUIRE(back->size() == java.size());

    // Block states, position by position.
    std::map<std::string, u64> differences; // "original -> result" -> count
    const Extent e = java.size();
    for (u32 y = 0; y < e.y; ++y)
        for (u32 z = 0; z < e.z; ++z)
            for (u32 x = 0; x < e.x; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                std::string a(java.block_registry().canonical(java.block(p)));
                std::string b(back->block_registry().canonical(back->block(p)));
                // Bedrock stores no stair shape: Java's own neighbour rule recomputes it (tested
                // separately below). The test structures set some corner shapes without neighbours.
                if (a.find("_stairs[") != std::string::npos) {
                    a = without_shape(a);
                    b = without_shape(b);
                }
                if (a != b) ++differences[a + "  ->  " + b];
            }
    for (const auto& [d, n] : differences) UNSCOPED_INFO(n << "x " << d);
    CHECK(differences.empty());

    // Biomes.
    for (u32 y = 0; y < e.y; y += 4)
        for (u32 z = 0; z < e.z; z += 4)
            for (u32 x = 0; x < e.x; x += 4) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                const auto a = java.biome(p), b = back->biome(p);
                if (!a) continue;
                REQUIRE(b);
                CHECK(java.biome_registry().name(*a) == back->biome_registry().name(*b));
            }

    // Block entities: same positions and ids, container contents preserved.
    std::map<std::tuple<i32, i32, i32>, const BlockEntity*> back_bes;
    for (const BlockEntity& be : back->block_entities) back_bes[{be.pos.x, be.pos.y, be.pos.z}] = &be;
    for (const BlockEntity& be : java.block_entities) {
        CAPTURE(be.id, be.pos.x, be.pos.y, be.pos.z);
        const auto it = back_bes.find({be.pos.x, be.pos.y, be.pos.z});
        if (it == back_bes.end()) {
            // Only block entities Bridger does not translate may be missing, and they must be reported.
            CHECK(to_bedrock.total(translate::IssueKind::UnmappedBlockEntity) > 0);
            continue;
        }
        CHECK(it->second->id == be.id);
        CHECK(items_of(*it->second) == items_of(be));
    }
}
