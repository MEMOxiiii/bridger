// Low-level I/O primitives: varint decode (Sponge block data) and endian loads (NBT).
// Phase 0 harness benchmark; NBT/region benchmarks land in bench_nbt.cpp / bench_region.cpp.

#include <bridger/io/endian.hpp>
#include <bridger/io/varint.hpp>

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

using namespace bridger;

namespace {

// A Sponge-like stream: mostly small palette indices, some larger.
std::vector<u8> make_varint_stream(usize count, u32 max_value) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<u32> dist(0, max_value);
    std::vector<u8> out;
    out.reserve(count * 2);
    for (usize i = 0; i < count; ++i) io::encode_varint32(dist(rng), out);
    return out;
}

void BM_DecodeVarint32(benchmark::State& state) {
    const auto count = static_cast<usize>(state.range(0));
    const auto max_value = static_cast<u32>(state.range(1));
    const std::vector<u8> data = make_varint_stream(count, max_value);

    for (auto _ : state) {
        std::span<const u8> rest(data);
        u64 sum = 0;
        while (!rest.empty()) {
            const auto d = io::decode_varint32(rest);
            if (!d) break;
            sum += d->value;
            rest = rest.subspan(d->length);
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(count));
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(data.size()));
}
// 16^3 = 4096 entries per section; palette sizes 100 (1 byte) and 5000 (2 bytes).
BENCHMARK(BM_DecodeVarint32)->Args({4096, 100})->Args({4096, 5000})->Args({1 << 20, 5000});

void BM_LoadBigEndianU64(benchmark::State& state) {
    std::vector<u8> data(64 * 1024);
    std::mt19937 rng(7);
    for (auto& b : data) b = static_cast<u8>(rng());

    for (auto _ : state) {
        u64 acc = 0;
        for (usize i = 0; i + 8 <= data.size(); i += 8) acc ^= io::load_be<u64>(data.data() + i);
        benchmark::DoNotOptimize(acc);
    }
    state.SetBytesProcessed(static_cast<i64>(state.iterations()) * static_cast<i64>(data.size()));
}
BENCHMARK(BM_LoadBigEndianU64);

} // namespace
