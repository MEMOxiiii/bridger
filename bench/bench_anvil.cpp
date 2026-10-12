// Java world conversion, per chunk column: decode a terrain-like 1.18+ chunk (24 sections,
// real block names, 4x4x4 biomes), translate it to Bedrock, and write it to LevelDB.
// Reported as chunks per second; a 2000 x 2000 block world is ~15 600 chunks.

#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <bit>
#include <filesystem>
#include <optional>
#include <random>
#include <string>

using namespace bridger;
using namespace bridger::io::nbt;

namespace {

constexpr std::array<const char*, 12> kStates = {
    "minecraft:air", "minecraft:stone", "minecraft:deepslate", "minecraft:dirt", "minecraft:grass_block[snowy=false]",
    "minecraft:water[level=0]", "minecraft:coal_ore", "minecraft:iron_ore", "minecraft:gravel", "minecraft:andesite",
    "minecraft:oak_log[axis=y]", "minecraft:oak_leaves[distance=1,persistent=false,waterlogged=false]"};

std::vector<i64> pack(const std::vector<u32>& idx, u32 bits) {
    const usize per = 64 / bits;
    std::vector<i64> out((idx.size() + per - 1) / per, 0);
    for (usize i = 0; i < idx.size(); ++i)
        out[i / per] = static_cast<i64>(static_cast<u64>(out[i / per]) | (u64{idx[i]} << ((i % per) * bits)));
    return out;
}

std::vector<u8> make_chunk() {
    std::mt19937 rng(11);
    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.write_int("DataVersion", 5023);
    w.write_int("xPos", 0);
    w.write_int("zPos", 0);
    w.write_int("yPos", -4);
    w.write_string("Status", "minecraft:full");
    w.begin_list("sections", TagType::Compound, 24);
    for (int sy = -4; sy < 20; ++sy) {
        w.begin_compound();
        w.write_byte("Y", static_cast<i8>(sy));
        w.begin_compound("block_states");
        w.begin_list("palette", TagType::Compound, kStates.size());
        for (const char* s : kStates) {
            const std::string full(s);
            const auto br = full.find('[');
            w.begin_compound();
            w.write_string("Name", full.substr(0, br));
            if (br != std::string::npos) {
                w.begin_compound("Properties");
                std::string props = full.substr(br + 1, full.size() - br - 2);
                usize start = 0;
                while (start < props.size()) {
                    usize end = props.find(',', start);
                    if (end == std::string::npos) end = props.size();
                    const std::string kv = props.substr(start, end - start);
                    const auto eq = kv.find('=');
                    w.write_string(kv.substr(0, eq), kv.substr(eq + 1));
                    start = end + 1;
                }
                w.end_compound();
            }
            w.end_compound();
        }
        w.end_list();
        std::vector<u32> idx(4096);
        for (usize i = 0; i < 4096; ++i) {
            const int y = sy * 16 + static_cast<int>(i >> 8);
            idx[i] = y > 70 ? 0 : y > 66 ? 3 + (rng() % 2) : y > 0 ? 1 + (rng() % 20 == 0 ? 5 + rng() % 4 : 0) : 2;
        }
        w.write_long_array("data", pack(idx, 4));
        w.end_compound();
        w.begin_compound("biomes");
        w.begin_list("palette", TagType::String, 2);
        w.write_string({}, "minecraft:plains");
        w.write_string({}, "minecraft:forest");
        w.end_list();
        std::vector<u32> bidx(64);
        for (usize i = 0; i < 64; ++i) bidx[i] = static_cast<u32>(rng() % 2);
        w.write_long_array("data", pack(bidx, 1));
        w.end_compound();
        w.end_compound();
    }
    w.end_list();
    w.begin_list("block_entities", TagType::Compound, 0);
    w.end_list();
    w.end_compound();
    return std::move(w).finish().value();
}

void BM_ChunkDecode(benchmark::State& state) {
    const auto chunk = make_chunk();
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    for (auto _ : state) {
        auto c = formats::java::decode_chunk(chunk, blocks, biomes);
        if (!c) state.SkipWithError("decode failed");
        benchmark::DoNotOptimize(c);
    }
    state.counters["chunks/s"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_ChunkDecode)->Unit(benchmark::kMillisecond);

void BM_ChunkDecodeTranslateWrite(benchmark::State& state) {
    const auto chunk = make_chunk();
    const translate::MappingData& m = *translate::MappingData::embedded().value();
    const u32 air = *m.find_bedrock("minecraft:air", {});
    std::vector<std::vector<u8>> cache(m.bedrock_state_count());
    const formats::bedrock::StateNbtFn state_nbt = [&](u32 s) -> Result<std::span<const u8>> {
        if (cache[s].empty()) {
            NbtWriter w(Endian::Little);
            w.begin_root_compound("");
            w.write_string("name", m.bedrock(s).name);
            w.begin_compound("states");
            w.end_compound();
            w.write_int("version", static_cast<i32>(m.bedrock_block_version()));
            w.end_compound();
            cache[s] = std::move(w).finish().value();
        }
        return std::span<const u8>(cache[s]);
    };
    const auto dir = std::filesystem::temp_directory_path() / ("bridger-bench-anvil-" + std::to_string(std::random_device{}()));
    auto created = formats::bedrock::WorldBuilder::create(dir);
    if (!created) {
        state.SkipWithError("cannot create the world");
        return;
    }
    std::optional<formats::bedrock::WorldBuilder> builder(std::move(*created));
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    i32 n = 0;
    for (auto _ : state) {
        auto c = formats::java::decode_chunk(chunk, blocks, biomes);
        translate::ConversionReport report;
        auto b = translate::translate_to_bedrock(c->column, m, {}, report);
        // Each iteration writes a new column position.
        if (!b || !builder->write_column({n % 256, n / 256}, c->min_y, *b, air, translate::kDefaultBedrockBiome, state_nbt))
            state.SkipWithError("convert failed");
        ++n;
    }
    state.counters["chunks/s"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    builder.reset(); // close the database before removing it
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
BENCHMARK(BM_ChunkDecodeTranslateWrite)->Unit(benchmark::kMillisecond);

} // namespace
