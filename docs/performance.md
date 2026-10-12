# Performance

**Rule:** no number appears here, or in the README, unless it was measured, with the machine and command stated.

## How to measure

1. Build Release: `cmake --preset msvc-release && cmake --build --preset msvc-release`.
2. Close other heavy programs and plug in laptops (power plan "Best performance").
3. Run `build\msvc-release\bin\bridger_bench_<area>.exe --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`.
4. Record the median here with the date, commit hash and machine.

## Benchmarks

| Area | Executable | Measures | Status |
|---|---|---|---|
| I/O primitives | `bridger_bench_io` | varint decode (items/s, MB/s), BE loads (MB/s) | harness only (Phase 0) |
| NBT | `bridger_bench_nbt` | parse / walk / materialize / write MB/s, gzip decompress | **baseline recorded** |
| Sponge `.schem` | `bridger_bench_schem` | end-to-end read, blocks/s | **baseline recorded** |
| Java chunks | `bridger_bench_anvil` | chunk decode, decode + translate + encode + store (chunks/s) | **baseline recorded** |
| Translation | `bridger_bench_translate` | sections/s | **baseline recorded** |
| Meshing | `bridger_bench_mesher` | sections/s | Phase 7 |
| End-to-end | `bridger_bench_e2e` | `.schem` → `.mcworld` on disk, incl. validation | **baseline recorded** |
| Viewer | (in-app) | time to first frame, FPS | Phase 7 |

## Baselines

Phase 0 only proves the harness runs. These numbers come from a first, unoptimized varint implementation. They
are a reference point, **not targets**. Targets are set once real code exists (Phase 1 onward).

**Machine A:** Intel Core i7-13620H (16 threads, 24 MiB L3), 32 GB RAM, Windows 11 Pro 26200, MSVC 19.43
(VS 17.13), `msvc-release`. Command:
`bridger_bench_io.exe --benchmark_repetitions=3 --benchmark_report_aggregates_only=true` (medians shown).

| Date | Commit | Machine | Benchmark | Result |
|---|---|---|---|---|
| 2026-10-11 | Phase 0 | A | `BM_DecodeVarint32/4096/100` (1-byte varints) | 51.5 M items/s, 49 MiB/s |
| 2026-10-11 | Phase 0 | A | `BM_DecodeVarint32/4096/5000` (mostly 2-byte) | 47.9 M items/s, 90 MiB/s |
| 2026-10-11 | Phase 0 | A | `BM_DecodeVarint32/1048576/5000` | 50.7 M items/s, 95 MiB/s |
| 2026-10-11 | Phase 0 | A | `BM_LoadBigEndianU64` (64 KiB) | 16.9 GiB/s |

Note: `BM_DecodeVarint32` goes through `Result<>` once per value. The Sponge reader uses an inline bulk decoder
instead (see `BM_SchemRead` below).

### Phase 1–2 baselines (2026-10-11, Machine A, `msvc-release`)

Synthetic inputs built inside the benchmarks: the NBT document is ~9.8 MB (20,000 block-entity compounds with
item lists, a 4 MB byte array, a 512 KB long array); the schematic is v3, 256 × 128 × 256 = 8.4 M blocks, with
a 64-entry palette. Medians of 3 repetitions.

| Benchmark | What it measures | Result |
|---|---|---|
| `BM_NbtParse/0` | full validation, big-endian (zero-copy) | 1.37 GiB/s |
| `BM_NbtParse/1` | full validation, little-endian | 1.29 GiB/s |
| `BM_NbtParseAndWalk` | validate + visit every block entity's `Pos` | 1.08 GiB/s |
| `BM_NbtMaterialize` | owning tree from a parsed document | 182 MiB/s |
| `BM_NbtWrite` | owning tree → bytes | 523 MiB/s |
| `BM_GzipDecompress` | libdeflate gzip, decompressed bytes | 613 MiB/s |
| `BM_SchemRead/1` | `.schem` gzip file → IR structure | 54.7 ms = 155 M blocks/s |
| `BM_SchemRead/0` | same, raw NBT (no gzip) | 39.4 ms = 217 M blocks/s |

Observations, not yet acted on: materializing is ~7× slower than validating (one heap allocation per tag);
readers should keep using views. Gzip accounts for ~30% of `.schem` read time.

### Phase 3–4 baselines (2026-10-12, Machine A, `msvc-release`)

| Benchmark | What it measures | Result |
|---|---|---|
| `BM_TranslateStructure` | Java → Bedrock translation of 256 × 128 × 256 (8.4 M blocks, 24 states, some waterlogged) | 34.4 ms = 60.7 k sections/s |
| `BM_SchemToMcworld` | 128 × 64 × 128 `.schem` file → `.mcworld` file: read, translate, LevelDB, zip, validation re-read, atomic move | 353 ms (1 M blocks) |

Real file (not a benchmark, one run): `AraluenBW1.21Solo.schem`, 405 × 140 × 405, converted in 2.7 s wall time.
Single-threaded everywhere so far; the pipeline has not been parallelized (performance plan §4).

### Phase 5 baselines (2026-10-12, Machine A, `msvc-release`)

`bridger_bench_anvil` uses a synthetic 1.18+ chunk: 24 sections, 12 real block states, two biomes (one run):

| Benchmark | What it measures | Result |
|---|---|---|
| `BM_ChunkDecode` | chunk NBT → IR column | 0.98 ms = 1.06 k chunks/s |
| `BM_ChunkDecodeTranslateWrite` | decode + translate + encode + LevelDB put, one thread | 2.54 ms = 407 chunks/s |

**Whole world, end to end** (CLI, one run each): a synthetic Java world of 4 regions × 1024 full chunks
(16 MB of region files) → `.mcworld` (6.9 MB), including validation, zip and the atomic move:
`bridger -v convert <world> out.mcworld [--threads N]`. The `-v` log prints the time of each phase.

| Step (time since start) | `--threads 1` | all 16 threads |
|---|---|---|
| chunks read, decoded, translated, encoded and stored | 20.7 s | 1.6 s |
| database flushed | 23.4 s | 6.6 s |
| world validated (every subchunk decoded) | 25.2 s | 8.4 s |
| zipped and moved into place | 26.0 s | 9.2 s |

What changed to get there (same world, all threads): one LevelDB write batch per column and a 64 MB write
buffer for new worlds took the chunk phase from 9.9 s to 1.7 s; flushing the memtable instead of a full
`CompactRange` took the database step from 11.4 s to 4.9 s. What remains is single-threaded deflate inside
LevelDB: with deflate level 1 instead of the default the total was 6.0 s, but the world grew from 6.9 MB to
10.1 MB, so the default level was kept.
