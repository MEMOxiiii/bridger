// End-to-end: .schem file on disk -> new .mcworld on disk (read, place, translate,
// write the world, zip, re-read for validation, atomic move). 128 x 64 x 128 structure.

#include <bridger/convert/pipeline.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/varint.hpp>

#include <benchmark/benchmark.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace bridger;
using namespace bridger::io::nbt;

namespace {

constexpr int kW = 128, kH = 64, kL = 128;

std::filesystem::path make_input(const std::filesystem::path& dir) {
    const char* palette[] = {"minecraft:air", "minecraft:stone", "minecraft:oak_planks", "minecraft:glass",
                             "minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=true]",
                             "minecraft:white_wool", "minecraft:bricks"};
    std::mt19937 rng(5);
    std::vector<u8> data;
    for (int y = 0; y < kH; ++y)
        for (int z = 0; z < kL; ++z)
            for (int x = 0; x < kW; ++x) {
                const bool wall = x == 0 || z == 0 || x == kW - 1 || z == kL - 1 || y == 0;
                io::encode_varint32(wall ? 1 + static_cast<u32>(rng() % 6) : (rng() % 50 == 0 ? 4u : 0u), data);
            }
    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.begin_compound("Schematic");
    w.write_int("Version", 3);
    w.write_int("DataVersion", 5023);
    w.write_short("Width", kW);
    w.write_short("Height", kH);
    w.write_short("Length", kL);
    w.begin_compound("Blocks");
    w.begin_compound("Palette");
    for (int i = 0; i < 7; ++i) w.write_int(palette[i], i);
    w.end_compound();
    w.write_byte_array("Data", data);
    w.end_compound();
    w.end_compound();
    w.end_compound();
    const auto gz = io::compress(std::move(w).finish().value(), io::Compression::Gzip).value();
    const auto path = dir / "bench_input.schem";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
    return path;
}

void BM_SchemToMcworld(benchmark::State& state) {
    const auto dir = std::filesystem::temp_directory_path() / "bridger-bench-e2e";
    std::filesystem::create_directories(dir);
    convert::ConvertOptions o;
    o.input = make_input(dir);
    o.output = dir / "bench_output.mcworld";
    o.overwrite = true;
    for (auto _ : state) {
        auto r = convert::convert_schem_to_new_mcworld(o);
        if (!r) state.SkipWithError(r.error().describe().c_str());
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * kW * kH * kL);
    state.SetLabel("items = blocks, 128x64x128, incl. validation re-read");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
BENCHMARK(BM_SchemToMcworld)->Unit(benchmark::kMillisecond)->Iterations(5);

} // namespace
