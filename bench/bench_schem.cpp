// Sponge .schem reading: end-to-end (gzip + NBT + palette + varint decode into
// IR sections) on a synthetic 256 x 128 x 256 (8.4 M blocks) v3 schematic.

#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/varint.hpp>

#include <benchmark/benchmark.h>

#include <random>
#include <string>

using namespace bridger;
using namespace bridger::io::nbt;

namespace {

constexpr int kW = 256, kH = 128, kL = 256, kPalette = 64;

std::vector<u8> make_schem(bool compressed) {
    std::mt19937 rng(7);
    // Terrain-like: mostly air above, a few dozen states below, in runs.
    std::vector<u8> data;
    data.reserve(static_cast<usize>(kW) * kH * kL);
    for (int y = 0; y < kH; ++y)
        for (int z = 0; z < kL; ++z)
            for (int x = 0; x < kW; ++x) {
                const u32 idx = y > 64 ? 0u : static_cast<u32>(1 + ((x / 4 + z / 4 + y) % (kPalette - 1)));
                io::encode_varint32((rng() % 50 == 0) ? static_cast<u32>(rng() % kPalette) : idx, data);
            }
    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.begin_compound("Schematic");
    w.write_int("Version", 3);
    w.write_int("DataVersion", 4556);
    w.write_short("Width", kW);
    w.write_short("Height", kH);
    w.write_short("Length", kL);
    w.begin_compound("Blocks");
    w.begin_compound("Palette");
    w.write_int("minecraft:air", 0);
    for (int i = 1; i < kPalette; ++i) w.write_int("minecraft:block_" + std::to_string(i) + "[facing=north]", i);
    w.end_compound();
    w.write_byte_array("Data", data);
    w.begin_list("BlockEntities", TagType::Compound, 0);
    w.end_list();
    w.end_compound();
    w.end_compound();
    w.end_compound();
    auto raw = std::move(w).finish().value();
    return compressed ? io::compress(raw, io::Compression::Gzip, 6).value() : raw;
}

void BM_SchemRead(benchmark::State& state) {
    const bool compressed = state.range(0) != 0;
    const auto file = make_schem(compressed);
    for (auto _ : state) {
        auto r = formats::java::read_sponge_schem(file, std::make_shared<BlockStateRegistry>(),
                                                  std::make_shared<BiomeRegistry>());
        if (!r) state.SkipWithError(r.error().describe().c_str());
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * kW * kH * kL); // blocks
    state.SetLabel(compressed ? "gzip, 8.4M blocks" : "raw NBT, 8.4M blocks");
}
BENCHMARK(BM_SchemRead)->Arg(1)->Arg(0)->Unit(benchmark::kMillisecond);

} // namespace
