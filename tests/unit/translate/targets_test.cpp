// Target game versions: the embedded lists, block and block entity downgrades, and conversions written for an
// older version reading back as intended (docs/versions.md, "Target versions").

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/core/game_versions.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/translate/targets.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;
using namespace bridger::translate;

namespace {

const JavaTargets& java() { return **JavaTargets::embedded(); }
const BedrockTargets& bedrock() { return **BedrockTargets::embedded(); }
const MappingData& mapping() { return **MappingData::embedded(); }

std::string down(std::string_view state, std::string_view version, JavaTargets::Fit* fit = nullptr) {
    const JavaTarget* t = java().find(version);
    REQUIRE(t);
    BlockState out;
    const auto f = java().downgrade(*BlockState::parse(state), *t, out);
    if (fit) *fit = f;
    return f == JavaTargets::Fit::Missing ? std::string("<missing>") : out.to_string();
}

RawNbt be_nbt(const std::function<void(io::nbt::NbtWriter&)>& body) {
    io::nbt::NbtWriter w(Endian::Big);
    w.begin_root_compound();
    body(w);
    w.end_compound();
    return RawNbt{*std::move(w).finish(), Endian::Big};
}

} // namespace

TEST_CASE("Target lists cover every Java release from 1.18 and every Bedrock version from 1.18.30", "[translate][targets]") {
    REQUIRE(java().targets().size() == 32);
    CHECK(java().pinned().name == "26.3");
    CHECK(java().pinned().data_version == static_cast<i32>(mapping().java_data_version()));
    CHECK(java().targets().back().name == "1.18");
    CHECK(java().find("1.20.1")->data_version == 3465);
    for (const JavaTarget& t : java().targets()) {
        const auto r = find_java_release(t.name);
        REQUIRE(r);
        CHECK(r->data_version == t.data_version); // Mojang's data generator agrees with the release table
    }
    REQUIRE(bedrock().targets().size() == 37);
    CHECK(bedrock().pinned().name == "26.50");
    CHECK(bedrock().pinned().block_version == static_cast<i32>(mapping().bedrock_block_version()));
    CHECK(bedrock().targets().back().name == "1.18.30");
    const BedrockTarget* b = bedrock().find("1.20.80");
    REQUIRE(b);
    CHECK(b->protocol == 671);
    CHECK(b->chunk_version == 40);
    CHECK(bedrock().find("1.19.30")->storage_version == 9);
    CHECK(bedrock().find("1.21.120")->chunk_version == 42);
}

TEST_CASE("Java states are respelled or reported for older releases", "[translate][targets]") {
    JavaTargets::Fit fit{};
    CHECK(down("minecraft:short_grass", "1.20.2", &fit) == "minecraft:grass");
    CHECK(fit == JavaTargets::Fit::Exact);
    CHECK(down("minecraft:short_grass", "1.20.3") == "minecraft:short_grass");
    CHECK(down("minecraft:iron_chain[axis=y,waterlogged=false]", "1.21.8") == "minecraft:chain[axis=y,waterlogged=false]");
    CHECK(down("minecraft:cherry_planks", "1.19.3", &fit) == "minecraft:birch_planks"); // the closest older wood
    CHECK(fit == JavaTargets::Fit::Approximate);
    CHECK(down("minecraft:cherry_planks", "1.20") == "minecraft:cherry_planks");
    // 1.19.4 has cherry wood only behind the update_1_20 experiment (Mojang's reports list it anyway).
    CHECK(down("minecraft:cherry_planks", "1.19.4") == "minecraft:cherry_planks");
    CHECK(java().experimental_feature("minecraft:cherry_planks", *java().find("1.19.4")) == "update_1_20");
    CHECK(java().experimental_feature("minecraft:cherry_planks", *java().find("1.20")).empty());
    CHECK(java().experimental_feature("minecraft:stone", *java().find("1.19.4")).empty());
    CHECK(java().experimental_feature("minecraft:pale_oak_planks", *java().find("1.21.3")) == "winter_drop");
    // Leaves gained "waterlogged" in 1.19: a waterlogged leaf cannot be written for 1.18.2.
    CHECK(down("minecraft:oak_leaves[distance=7,persistent=true,waterlogged=true]", "1.18.2", &fit) ==
          "minecraft:oak_leaves[distance=7,persistent=true]");
    CHECK(fit == JavaTargets::Fit::Approximate);
    CHECK(down("minecraft:oak_leaves[distance=7,persistent=true,waterlogged=false]", "1.18.2", &fit) ==
          "minecraft:oak_leaves[distance=7,persistent=true]");
    CHECK(fit == JavaTargets::Fit::Exact);
    CHECK(down("minecraft:creaking_heart[axis=y,creaking_heart_state=awake,natural=false]", "1.21.4") ==
          "minecraft:creaking_heart[active=true,axis=y,natural=false]");
}

TEST_CASE("Java block entities take the target's format", "[translate][targets]") {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto s = *Structure::create(blocks, std::make_shared<BiomeRegistry>(), Extent{4, 1, 1}, {Edition::Java, 5023});
    s.set_block({0, 0, 0}, blocks->intern(*BlockState::parse("minecraft:chest[facing=north,type=single,waterlogged=false]")));
    s.set_block({1, 0, 0}, blocks->intern(*BlockState::parse("minecraft:oak_sign[rotation=0,waterlogged=false]")));
    s.set_block({2, 0, 0}, blocks->intern(*BlockState::parse("minecraft:red_banner[rotation=0]")));
    s.set_block({3, 0, 0}, blocks->intern(*BlockState::parse("minecraft:crafter[crafting=false,orientation=north_up,triggered=false]")));
    s.block_entities.push_back({{0, 0, 0}, "minecraft:chest", be_nbt([](auto& w) {
                                    w.begin_list("Items", io::nbt::TagType::Compound, 2);
                                    w.begin_compound(); w.write_byte("Slot", 0); w.write_string("id", "minecraft:diamond"); w.write_int("count", 5);
                                    w.begin_compound("components"); w.end_compound(); w.end_compound();
                                    w.begin_compound(); w.write_byte("Slot", 1); w.write_string("id", "minecraft:short_grass"); w.write_int("count", 2); w.end_compound();
                                    w.end_list();
                                })});
    s.block_entities.push_back({{1, 0, 0}, "minecraft:sign", be_nbt([](auto& w) {
                                    w.begin_compound("front_text");
                                    w.begin_list("messages", io::nbt::TagType::String, 4);
                                    for (const char* l : {"Hello", "", "", ""}) w.write_string({}, l);
                                    w.end_list();
                                    w.write_string("color", "red");
                                    w.write_byte("has_glowing_text", 1);
                                    w.end_compound();
                                    w.write_byte("is_waxed", 0);
                                })});
    s.block_entities.push_back({{2, 0, 0}, "minecraft:banner", be_nbt([](auto& w) {
                                    w.begin_list("patterns", io::nbt::TagType::Compound, 1);
                                    w.begin_compound(); w.write_string("pattern", "minecraft:border"); w.write_string("color", "black"); w.end_compound();
                                    w.end_list();
                                })});
    s.block_entities.push_back({{3, 0, 0}, "minecraft:crafter", be_nbt([](auto& w) { w.write_int("crafting_ticks_remaining", 0); })});

    ConversionReport report;
    REQUIRE(rewrite_for_java_target(s, java(), *java().find("1.19.4"), BlockState("minecraft:air", {}), report));
    auto doc_at = [&](BlockPos p) {
        for (const BlockEntity& be : s.block_entities)
            if (be.pos == p) return io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
        return Result<io::nbt::NbtDocument>(make_error(ErrorCode::FileNotFound, "no block entity"));
    };
    const auto chest = doc_at({0, 0, 0});
    REQUIRE(chest);
    const auto items = chest->root().entries().find("Items");
    REQUIRE(items.items().size() == 2);
    CHECK(items.items().at(0).entries().find("Count").as_byte() == 5);
    CHECK_FALSE(items.items().at(0).entries().find("components"));
    CHECK(items.items().at(1).entries().find("id").as_string() == "minecraft:grass");
    const auto sign = doc_at({1, 0, 0});
    REQUIRE(sign);
    CHECK(sign->root().entries().find("Text1").as_string() == "\"Hello\"");
    CHECK(sign->root().entries().find("Color").as_string() == "red");
    CHECK(sign->root().entries().find("GlowingText").as_byte() == 1);
    CHECK_FALSE(sign->root().entries().find("front_text"));
    const auto banner = doc_at({2, 0, 0});
    REQUIRE(banner);
    const auto layer = banner->root().entries().find("Patterns").items().at(0).entries();
    CHECK(layer.find("Pattern").as_string() == "bo");
    CHECK(layer.find("Color").as_int() == 15);
    // The crafter (1.21) does not exist in 1.19.4: it becomes a dropper, with a dropper block entity, reported.
    CHECK(s.block_registry().get(s.block({3, 0, 0})).id() == "minecraft:dropper");
    REQUIRE(doc_at({3, 0, 0}));
    for (const BlockEntity& be : s.block_entities)
        if (be.pos == BlockPos{3, 0, 0}) CHECK(be.id == "minecraft:dropper");
    CHECK(report.issues(IssueKind::LossyBlock) != nullptr);
}

TEST_CASE("Bedrock states and items are written as older versions spell them", "[translate][targets]") {
    const BedrockTarget& t = *bedrock().find("1.19.60");
    const auto [wool, damage] = bedrock().item_for(t, "minecraft:white_wool", 0);
    CHECK(wool == "minecraft:wool");
    CHECK(damage == 0);
    const auto [red, red_damage] = bedrock().item_for(t, "minecraft:red_wool", 0);
    CHECK(red == "minecraft:wool");
    CHECK(red_damage == 14);
    CHECK(bedrock().item_for(bedrock().pinned(), "minecraft:white_wool", 0).first == "minecraft:white_wool");

    // red_wool as a block: respelled as wool with its colour state before 1.19.70.
    const auto idx = mapping().find_bedrock("minecraft:red_wool", {});
    REQUIRE(idx);
    const auto how = bedrock().resolve(t, *idx);
    CHECK(how.kind == BedrockTargets::Written::Kind::Respelled);
    const auto nbt = bedrock().state_nbt(t, *idx, mapping());
    const auto doc = io::nbt::NbtDocument::parse(nbt, Endian::Little);
    REQUIRE(doc);
    CHECK(doc->root().entries().find("name").as_string() == "minecraft:wool");
    CHECK(doc->root().entries().find("states").entries().find("color").as_string() == "red");
    CHECK(doc->root().entries().find("version").as_int() == t.block_version);

    // cherry_planks (1.20) is kept with its own name and the pinned block version: 1.19.60 shows an unknown block that
    // turns back into cherry planks in a newer version.
    const auto cherry = mapping().find_bedrock("minecraft:cherry_planks", {});
    REQUIRE(cherry);
    CHECK(bedrock().resolve(t, *cherry).kind == BedrockTargets::Written::Kind::Missing);
    const auto kept = io::nbt::NbtDocument::parse(bedrock().state_nbt(t, *cherry, mapping()), Endian::Little);
    REQUIRE(kept);
    CHECK(kept->root().entries().find("name").as_string() == "minecraft:cherry_planks");
    CHECK(kept->root().entries().find("version").as_int() == static_cast<i32>(mapping().bedrock_block_version()));
}

TEST_CASE("Conversions written for older versions read back as intended", "[translate][targets][convert]") {
    test::TempDir dir;
    for (const auto& [format, ext, version] : {std::tuple{"schem", ".schem", "1.18.2"}, std::tuple{"litematic", ".litematic", "1.20.4"},
                                               std::tuple{"mcstructure", ".mcstructure", "1.19.80"}, std::tuple{"mcworld", ".mcworld", "1.20.0"}}) {
        INFO(format << " " << version);
        convert::ConvertOptions o;
        o.input = test::data_path("schem/v3_basic.schem");
        o.output = dir.path / (std::string("out_") + format + ext);
        o.target_version = version;
        const auto summary = convert::convert(o);
        REQUIRE(summary);
        CHECK(summary->target_version.ends_with(version));
        CHECK(std::filesystem::exists(o.output));
    }
    convert::ConvertOptions bad;
    bad.input = test::data_path("schem/v3_basic.schem");
    bad.output = dir.path / "bad.schem";
    bad.target_version = "1.17.1";
    const auto refused = convert::convert(bad);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code() == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(bad.output));

    // The 1.18.2 schematic records that version.
    const auto f = platform::MappedFile::open(dir.path / "out_schem.schem");
    REQUIRE(f);
    const auto back = formats::read_java_structure(f->data(), formats::FormatId::SpongeSchem, std::make_shared<BlockStateRegistry>(),
                                                   std::make_shared<BiomeRegistry>());
    REQUIRE(back);
    CHECK(back->data_version == 2975);
}

TEST_CASE("Worlds convert to another version of the same edition, down and back up", "[translate][targets][convert]") {
    test::TempDir dir;
    const auto run = [](const std::filesystem::path& in, const std::filesystem::path& out, const char* version) {
        convert::ConvertOptions o;
        o.input = in;
        o.output = out;
        o.target_version = version;
        return convert::convert(o);
    };

    // Java world -> Java 1.18.2 (the old level.dat era) -> Java 26.3 (the dimensions/ layout).
    const auto down = run(test::data_path("javaworld/basic"), dir.path / "java_1_18_2", "1.18.2");
    REQUIRE(down);
    CHECK(down->target_version == "Java 1.18.2");
    const auto old_world = formats::java::open_java_world(dir.path / "java_1_18_2");
    REQUIRE(old_world);
    CHECK(old_world->level.data_version == 2975);
    const auto up = run(dir.path / "java_1_18_2", dir.path / "java_26_3", "26.3");
    REQUIRE(up);
    CHECK(up->blocks == down->blocks);
    CHECK(up->chunks == down->chunks);
    const auto new_world = formats::java::open_java_world(dir.path / "java_26_3");
    REQUIRE(new_world);
    CHECK(new_world->level.data_version == 5023);

    // Bedrock world -> Bedrock 1.19.80 -> Bedrock 26.50.
    REQUIRE(run(test::data_path("javaworld/basic"), dir.path / "pinned.mcworld", ""));
    const auto bdown = run(dir.path / "pinned.mcworld", dir.path / "b_1_19_80.mcworld", "1.19.80");
    REQUIRE(bdown);
    {
        test::TempDir work;
        const auto r = formats::bedrock::BedrockWorldReader::open_path(dir.path / "b_1_19_80.mcworld", work.path);
        REQUIRE(r);
        CHECK(r->level().last_opened_version == std::vector<i32>{1, 19, 80, 0, 0});
    }
    const auto bup = run(dir.path / "b_1_19_80.mcworld", dir.path / "b_26_50.mcworld", "26.50");
    REQUIRE(bup);
    CHECK(bup->blocks == bdown->blocks);
    CHECK(bup->chunks == bdown->chunks);
}

TEST_CASE("A conversion never writes over or into its input", "[convert][safety]") {
    test::TempDir dir;
    convert::ConvertOptions o;
    o.input = test::data_path("javaworld/basic");
    o.output = test::data_path("javaworld/basic");
    o.overwrite = true;
    for (const auto& out : {o.input, o.input / "copy", o.input.parent_path()}) {
        o.output = out;
        const auto r = convert::convert(o);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::InvalidArgument);
    }
    o.input = test::data_path("schem/v3_basic.schem");
    o.output = o.input;
    const auto r = convert::convert(o);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("Every Bedrock state written for an older version reads back as itself, kept ones too", "[translate][targets][roundtrip]") {
    const auto lookup = convert::bedrock_state_lookup(mapping());
    for (const char* name : {"1.18.30", "1.19.60", "1.20.80", "1.21.50", "26.40"}) {
        INFO(name);
        const BedrockTarget& t = *bedrock().find(name);
        usize checked = 0, differ = 0;
        std::string example;
        for (BedrockStateIndex i = 0; i < mapping().bedrock_state_count(); ++i) {
            const auto how = bedrock().resolve(t, i);
            // Nearest states are rewritten before writing; missing ones keep their own spelling and read back as themselves.
            if (how.kind == BedrockTargets::Written::Kind::Nearest) continue;
            const auto doc = io::nbt::NbtDocument::parse(bedrock().state_nbt(t, i, mapping()), Endian::Little);
            REQUIRE(doc);
            const auto back = lookup(doc->root().entries());
            ++checked;
            if (back != i) {
                ++differ;
                if (example.empty()) example = std::string(mapping().bedrock(i).name);
            }
        }
        INFO("first difference: " << example);
        CHECK(checked > 5000); // 1.18.30 has about 8000 of the pinned states
        CHECK(differ == 0);
    }
}

TEST_CASE("Every pinned Java block has a substitute in every target", "[translate][targets]") {
    for (const JavaTarget& t : java().targets()) {
        INFO(t.name);
        usize missing = 0;
        std::string examples;
        std::string last;
        for (JavaStateId i = 0; i < mapping().java_state_count(); ++i) {
            const std::string name(mapping().java_block_name(i));
            if (name == last) continue; // one state per block is enough here
            last = name;
            BlockState out;
            if (java().downgrade(*BlockState::parse(mapping().java_state_string(i)), t, out) == JavaTargets::Fit::Missing) {
                ++missing;
                if (missing <= 8) examples += " " + name;
            }
        }
        INFO("without a substitute:" << examples);
        CHECK(missing == 0);
    }
    // A few of the reviewed choices.
    CHECK(down("minecraft:cherry_planks", "1.18") == "minecraft:birch_planks");
    CHECK(down("minecraft:tuff_bricks", "1.20.2") == "minecraft:stone_bricks");
    CHECK(down("minecraft:waxed_weathered_copper_grate", "1.20.2") == "minecraft:waxed_weathered_cut_copper");
    CHECK(down("minecraft:red_wool_slab[type=bottom,waterlogged=false]", "26.1") == "minecraft:red_wool");
    CHECK(down("minecraft:mangrove_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]", "1.18.2") ==
          "minecraft:jungle_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]");
}
