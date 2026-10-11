// NBT throughput: validation (zero-copy parse), materialization, writing, and
// gzip decompression, on a synthetic document shaped like real structure data
// (many small block-entity compounds plus large arrays).

#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/nbt_value.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <benchmark/benchmark.h>

#include <random>
#include <string>
#include <vector>

using namespace bridger;
using namespace bridger::io::nbt;

namespace {

std::vector<u8> make_document(Endian endian) {
    std::mt19937 rng(42);
    NbtWriter w(endian);
    w.begin_root_compound("");
    w.write_int("DataVersion", 4556);
    // 20k block entities, each a small compound with an item list.
    w.begin_list("BlockEntities", TagType::Compound, 20000);
    for (int i = 0; i < 20000; ++i) {
        w.begin_compound();
        w.write_int_array("Pos", std::vector<i32>{i % 64, i / 4096, (i / 64) % 64});
        w.write_string("Id", "minecraft:chest");
        w.begin_compound("Data");
        w.begin_list("Items", TagType::Compound, 3);
        for (int k = 0; k < 3; ++k) {
            w.begin_compound();
            w.write_byte("Slot", static_cast<i8>(k));
            w.write_string("id", "minecraft:diamond_sword");
            w.write_int("count", static_cast<i32>(rng() % 64));
            w.end_compound();
        }
        w.end_list();
        w.write_string("CustomName", "{\"text\":\"Chest " + std::to_string(i) + "\"}");
        w.end_compound();
        w.end_compound();
    }
    w.end_list();
    // 4 MB of block data and 512 KB of packed longs.
    std::vector<u8> bytes(4 << 20);
    for (auto& b : bytes) b = static_cast<u8>(rng() & 0x3F);
    w.write_byte_array("BlockData", bytes);
    std::vector<i64> longs(64 * 1024);
    for (auto& l : longs) l = static_cast<i64>(rng()) << 32 | rng();
    w.write_long_array("States", longs);
    w.end_compound();
    return std::move(w).finish().value();
}

const std::vector<u8>& document(Endian endian) {
    static const std::vector<u8> be = make_document(Endian::Big);
    static const std::vector<u8> le = make_document(Endian::Little);
    return endian == Endian::Big ? be : le;
}

void BM_NbtParse(benchmark::State& state) {
    const Endian endian = state.range(0) == 0 ? Endian::Big : Endian::Little;
    const auto& doc = document(endian);
    for (auto _ : state) {
        auto parsed = NbtDocument::parse(doc, endian);
        benchmark::DoNotOptimize(parsed);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(doc.size()));
    state.SetLabel(endian == Endian::Big ? "big-endian" : "little-endian");
}
BENCHMARK(BM_NbtParse)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

void BM_NbtParseAndWalk(benchmark::State& state) {
    const auto& doc = document(Endian::Big);
    for (auto _ : state) {
        auto parsed = NbtDocument::parse(doc, Endian::Big);
        i64 sum = 0;
        for (const NbtView be : parsed->root().entries().find("BlockEntities").items())
            sum += be.as_compound()->find("Pos").as_int_array()->operator[](0);
        benchmark::DoNotOptimize(sum);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(doc.size()));
}
BENCHMARK(BM_NbtParseAndWalk)->Unit(benchmark::kMillisecond);

void BM_NbtMaterialize(benchmark::State& state) {
    const auto& doc = document(Endian::Big);
    const auto parsed = NbtDocument::parse(doc, Endian::Big).value();
    for (auto _ : state) {
        auto tree = materialize(parsed);
        benchmark::DoNotOptimize(tree);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(doc.size()));
}
BENCHMARK(BM_NbtMaterialize)->Unit(benchmark::kMillisecond);

void BM_NbtWrite(benchmark::State& state) {
    const auto& doc = document(Endian::Big);
    const NbtRoot tree = read_nbt(doc, Endian::Big).value();
    for (auto _ : state) {
        auto bytes = write_nbt(tree, Endian::Big);
        benchmark::DoNotOptimize(bytes);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(doc.size()));
}
BENCHMARK(BM_NbtWrite)->Unit(benchmark::kMillisecond);

void BM_GzipDecompress(benchmark::State& state) {
    const auto& doc = document(Endian::Big);
    const auto packed = io::compress(doc, io::Compression::Gzip, 6).value();
    for (auto _ : state) {
        auto out = io::decompress(packed, io::Compression::Gzip);
        benchmark::DoNotOptimize(out);
    }
    // Throughput in decompressed bytes.
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(doc.size()));
}
BENCHMARK(BM_GzipDecompress)->Unit(benchmark::kMillisecond);

} // namespace
