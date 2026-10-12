// Structure file formats, each read and written on the same synthetic 128 x 64 x 128 Java
// structure (1 M blocks, 24 states, 64 chests with items): .schem (write; reading is in
// bench_schem), structure .nbt, .litematic, .mcstructure (Bedrock form) and legacy .schematic
// (read only).

#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/java/java_writers.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <benchmark/benchmark.h>

#include <random>
#include <string>

using namespace bridger;
using namespace bridger::formats;

namespace {

constexpr u32 kW = 128, kH = 64, kL = 128;

const translate::MappingData& mapping() { return *translate::MappingData::embedded().value(); }

Structure make_java() {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto s = Structure::create(blocks, std::make_shared<BiomeRegistry>(), {kW, kH, kL}, {Edition::Java, 5023}).value();
    std::vector<StateId> states;
    for (const char* f : {"north", "south", "east", "west"})
        for (const char* h : {"top", "bottom"})
            states.push_back(blocks->intern(*BlockState::parse(std::string("minecraft:oak_stairs[facing=") + f + ",half=" + h +
                                                               ",shape=straight,waterlogged=false]")));
    for (const char* b : {"stone", "dirt", "grass_block[snowy=false]", "oak_planks", "cobblestone", "glass", "white_wool",
                          "bricks", "sand", "gravel", "andesite", "diorite", "granite", "deepslate", "tuff", "calcite"})
        states.push_back(blocks->intern(*BlockState::parse(std::string("minecraft:") + b)));
    std::mt19937 rng(9);
    for (u32 y = 0; y < kH; ++y)
        for (u32 z = 0; z < kL; ++z)
            for (u32 x = 0; x < kW; ++x)
                s.set_block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)},
                            y < 32 ? states[8 + rng() % 16] : (rng() % 10 == 0 ? states[rng() % 8] : kAirStateId));
    const StateId chest = blocks->intern(*BlockState::parse("minecraft:chest[facing=north,type=single,waterlogged=false]"));
    for (i32 i = 0; i < 64; ++i) {
        const BlockPos p{i * 2, 40, 64};
        s.set_block(p, chest);
        io::nbt::NbtWriter w(Endian::Big);
        w.begin_root_compound();
        w.begin_list("Items", io::nbt::TagType::Compound, 3);
        for (i8 slot = 0; slot < 3; ++slot) {
            w.begin_compound();
            w.write_byte("Slot", slot);
            w.write_string("id", "minecraft:diamond");
            w.write_int("count", 10 + slot);
            w.end_compound();
        }
        w.end_list();
        w.end_compound();
        s.block_entities.push_back({p, "minecraft:chest", RawNbt{std::move(w).finish().value(), Endian::Big}});
    }
    return s;
}

const Structure& java() {
    static const Structure s = make_java();
    return s;
}

const java::JavaWriteOptions kOptions{5023, "bench", "bench", 0};

void report(benchmark::State& state) {
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(java().size().volume()));
    state.SetLabel("items = blocks");
}

template <typename Write>
void bench_write(benchmark::State& state, Write write) {
    for (auto _ : state) {
        auto bytes = write(java(), kOptions);
        if (!bytes) state.SkipWithError("write failed");
        benchmark::DoNotOptimize(bytes);
    }
    report(state);
}

void bench_read(benchmark::State& state, FormatId format, const std::vector<u8>& bytes) {
    for (auto _ : state) {
        auto f = read_java_structure(bytes, format, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
        if (!f) state.SkipWithError("read failed");
        benchmark::DoNotOptimize(f);
    }
    report(state);
}

void BM_SchemWrite(benchmark::State& state) { bench_write(state, java::write_sponge_schem); }
void BM_StructureNbtWrite(benchmark::State& state) { bench_write(state, java::write_structure_nbt); }
void BM_StructureNbtRead(benchmark::State& state) {
    bench_read(state, FormatId::StructureNbt, java::write_structure_nbt(java(), kOptions).value());
}
void BM_LitematicWrite(benchmark::State& state) { bench_write(state, java::write_litematic); }
void BM_LitematicRead(benchmark::State& state) {
    bench_read(state, FormatId::Litematic, java::write_litematic(java(), kOptions).value());
}

// .mcstructure on the Bedrock form of the structure.
struct BedrockFixture {
    Structure bedrock;
    u32 air = 0;
    bedrock::StateNbtFn fn;
    std::vector<u8> file;
};
const BedrockFixture& bedrock_fixture() {
    static const BedrockFixture f = [] {
        BedrockFixture b;
        const auto& m = mapping();
        translate::ConversionReport r;
        b.bedrock = translate::translate_to_bedrock(java(), m, {}, r).value();
        b.air = *m.find_bedrock("minecraft:air", {});
        static std::vector<std::vector<u8>> state_nbt(m.bedrock_state_count()); // outlives the fixture
        auto* cache = &state_nbt;
        b.fn = [cache, &m](u32 s) -> Result<std::span<const u8>> {
            if ((*cache)[s].empty()) {
                io::nbt::NbtWriter w(Endian::Little);
                w.begin_root_compound("");
                w.write_string("name", m.bedrock(s).name);
                w.begin_compound("states");
                for (const auto& p : m.bedrock(s).properties) {
                    if (p.type == translate::NbtValueType::Byte) w.write_byte(p.key, static_cast<i8>(p.int_value));
                    else if (p.type == translate::NbtValueType::Int) w.write_int(p.key, p.int_value);
                    else w.write_string(p.key, p.str_value);
                }
                w.end_compound();
                w.write_int("version", static_cast<i32>(m.bedrock_block_version()));
                w.end_compound();
                (*cache)[s] = std::move(w).finish().value();
            }
            return std::span<const u8>((*cache)[s]);
        };
        b.file = bedrock::write_mcstructure(b.bedrock, b.air, b.fn).value();
        return b;
    }();
    return f;
}

void BM_McStructureWrite(benchmark::State& state) {
    const auto& f = bedrock_fixture();
    for (auto _ : state) {
        auto bytes = bedrock::write_mcstructure(f.bedrock, f.air, f.fn);
        if (!bytes) state.SkipWithError("write failed");
        benchmark::DoNotOptimize(bytes);
    }
    report(state);
}

void BM_McStructureRead(benchmark::State& state) {
    const auto& f = bedrock_fixture();
    const auto& m = mapping();
    const bedrock::StateLookupFn lookup = [&m](io::nbt::NbtCompoundView e) -> std::optional<u32> {
        std::vector<translate::BedrockProperty> props;
        for (const auto& st : e.find("states").entries()) {
            translate::BedrockProperty p;
            p.key = st.name;
            if (const auto b = st.value.as_byte()) { p.type = translate::NbtValueType::Byte; p.int_value = *b; }
            else if (const auto i = st.value.as_int()) { p.type = translate::NbtValueType::Int; p.int_value = *i; }
            else { p.type = translate::NbtValueType::String; p.str_value = st.value.as_string().value_or(""); }
            props.push_back(p);
        }
        return m.find_bedrock(e.find("name").as_string().value_or(""), props);
    };
    for (auto _ : state) {
        auto s = bedrock::read_mcstructure(f.file, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                           m.bedrock_block_version(), lookup, f.air);
        if (!s) state.SkipWithError("read failed");
        benchmark::DoNotOptimize(s);
    }
    report(state);
}

// Legacy .schematic: numeric ids, built directly (Bridger has no legacy writer).
void BM_LegacySchematicRead(benchmark::State& state) {
    std::mt19937 rng(4);
    std::vector<u8> ids(static_cast<usize>(kW) * kH * kL), data(ids.size());
    for (usize i = 0; i < ids.size(); ++i) {
        ids[i] = static_cast<u8>((i / (kW * kL)) < 32 ? 1 + rng() % 5 : 0); // stone, grass, dirt, cobblestone, planks
        data[i] = 0;
    }
    io::nbt::NbtWriter w(Endian::Big);
    w.begin_root_compound("Schematic");
    w.write_short("Width", static_cast<i16>(kW));
    w.write_short("Height", static_cast<i16>(kH));
    w.write_short("Length", static_cast<i16>(kL));
    w.write_string("Materials", "Alpha");
    w.write_byte_array("Blocks", ids);
    w.write_byte_array("Data", data);
    w.begin_list("TileEntities", io::nbt::TagType::End, 0);
    w.end_list();
    w.begin_list("Entities", io::nbt::TagType::End, 0);
    w.end_list();
    w.end_compound();
    const auto file = io::compress(std::move(w).finish().value(), io::Compression::Gzip, 6).value();
    const auto& m = mapping();
    const java::LegacyTables tables{
        [&m](u16 id, u8 d) -> std::optional<std::string> {
            if (const auto s = m.legacy_block(id, d)) return m.java_state_string(*s);
            return std::nullopt;
        },
        [&m](u16 id, u16 dmg) -> std::optional<std::string> {
            if (const auto i = m.legacy_item(id, dmg)) return std::string(*i);
            return std::nullopt;
        }};
    for (auto _ : state) {
        auto f = read_java_structure(file, FormatId::LegacySchematic, std::make_shared<BlockStateRegistry>(),
                                     std::make_shared<BiomeRegistry>(), &tables);
        if (!f) state.SkipWithError("read failed");
        benchmark::DoNotOptimize(f);
    }
    report(state);
}

BENCHMARK(BM_SchemWrite)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StructureNbtWrite)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StructureNbtRead)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_LitematicWrite)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_LitematicRead)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_McStructureWrite)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_McStructureRead)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_LegacySchematicRead)->Unit(benchmark::kMillisecond);

} // namespace
