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
| Translation | `bridger_bench_translate` | sections/s, both directions | **baseline recorded** |
| Viewer meshing + GPU frame | `bridger_bench_render` | bake + mesh blocks/s, dense-section worst case, offscreen frame time | **baseline recorded** |
| End-to-end | `bridger_bench_e2e` | `.schem` → `.mcworld` on disk, incl. validation | **baseline recorded** |
| Viewer | `bridger-gui.exe` log (`time to first frame`, `render distance loaded`) and HUD (FPS) | time to first frame, world streaming, FPS | **baseline recorded** |

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

### Phase 6 baselines (2026-10-12, Machine A, `msvc-release`, medians of 3)

| Benchmark | What it measures | Result |
|---|---|---|
| `BM_TranslateToJava` | Bedrock → Java of the `BM_TranslateStructure` structure (8.4 M blocks; 262 k stairs get the shape rule, waterlogged ones layer 2) | 66.2 ms = 31.3 k sections/s |
| `BM_ChunkEncode` | Java chunk column → NBT → zlib (region writer) | 1.37 ms = 804 chunks/s |
| `BM_ChunkDecode` (re-run) | as in Phase 5 | 0.88 ms = 1.14 k chunks/s |
| `BM_ChunkDecodeTranslateWrite` (re-run) | as in Phase 5, now through `encode_column` + one write batch | 5.44 ms = 194 chunks/s (Phase 5: 2.54 ms; see note) |

The first `BM_TranslateToJava` measured 362 ms. A per-state cache of what the neighbour rules need (rule, facing,
half, connection flags), a cache of `with_property` results, and writing a position only when its state changed
brought it to 66 ms.

**Whole world, Bedrock → Java** (CLI, the 4096-chunk world from Phase 5 converted to `.mcworld`, then
`bridger convert big.mcworld <folder>`): `--threads 1` 21.9 s and 24.1 s; all 16 threads between 4.3 s and 17.1 s
over five runs. The machine ran at its 2.4 GHz base clock with other programs active during these runs, so the
multi-threaded figure varies widely; it needs a quiet re-measurement. Reading the world's chunk list alone
(`bridger info`) takes 1.0 s.

`bridger_bench_structures` (128 × 64 × 128 = 1 M blocks, 24 states, 64 chests; medians of 3) and the Bedrock
world reader:

| Benchmark | Result |
|---|---|
| `BM_SchemWrite` | 41.6 ms = 27.5 M blocks/s |
| `BM_StructureNbtWrite` / `Read` | 985 ms / 564 ms (one compound per block: the format itself is verbose) |
| `BM_LitematicWrite` / `Read` | 129 ms / 50.1 ms |
| `BM_McStructureWrite` / `Read` | 142 ms / 67.4 ms |
| `BM_LegacySchematicRead` | 72.2 ms = 16.0 M blocks/s |
| `BM_BedrockWorldRead` (`bench_anvil`; unzip + read 256 full columns) | 586 ms = 512 chunks/s |

Note: `BM_ChunkDecodeTranslateWrite` doubled between the Phase 5 and Phase 6 runs. The code path gained the
`EncodedColumn` copy and the 64 MB write buffer, but the same noisy conditions applied; it is recorded here as
measured and will be re-checked on a quiet machine before any claim is made.

### Phase 7–8 baselines (2026-10-13, Machine A, `msvc-release`, GPU NVIDIA GeForce RTX 4060 Laptop)

`bridger_bench_render --benchmark_repetitions=3 --benchmark_report_aggregates_only=true` (medians). Synthetic
128 × 64 × 128 terrain (≈ 512 K solid blocks with caves, grass surface, some glass) and a synthetic resource
pack; 274.5 k quads after face culling.

| Benchmark | Result |
|---|---|
| `BM_MeshStructure/0` (bake + mesh, all 16 threads) | 11.8 ms = 88.7 M blocks/s |
| `BM_MeshStructure/1` (one thread) | 82.4 ms = 12.7 M blocks/s |
| `BM_MeshSectionDense` (one 16³ section, random solid/air: 6.6 k quads, the worst case) | 1.55 ms |
| `BM_RenderFrame` (1920 × 1080 offscreen, 274.5 k quads, steady state) | 0.81 ms per frame (≈ 1230 frames/s) |

**Desktop app** (`bridger-gui.exe`, window 1440 × 900 physical pixels, vsync on a 60 Hz display; times from the
app's log, two runs each):

| Scenario | Result |
|---|---|
| Time to first frame, `tests/data/ingame/orientations.schem` (open → first frame showing it) | 19 ms |
| Time to first frame, `signs_beds.schem` (first open after start, textures read from the jar) | 392 ms |
| Real Java world "New World" (26.1.2, 7649 chunks), render distance 20: first frame | 15–21 ms |
| Same world: every chunk within 20 chunks streamed in | 2.3–3.0 s (1257 chunks, 7.6 M quads) |
| Frame rate with those 7.6 M quads loaded, orbiting | 60 fps (vsync-bound) |
| Bedrock world `AraluenBW1.21(4v4).mcworld` (36 chunks, translated to Java states on load) | first frame 44 ms; all chunks 152 ms |

What made the difference: reading files from the client jar. minizip's `locate_entry` scans the central
directory linearly; the jar has ~20 k entries and every model, blockstate and texture is a lookup. Indexing
the entry names once at open (`io::ZipReader`) cut baking for that world from 6.5 s to 84 ms (CLI
`bridger render ... --radius 12`: 441 chunks in 7.7 s before, 1.36 s after). Streaming also stopped retrying
chunks the world lists but cannot provide (partly generated), and centres on the orbit target instead of the
camera, which in orbit mode is far from what is shown.

An idle window renders nothing (the app redraws only when the camera, streaming or an animation changes), so
it uses no GPU time while you read the report.

### Experiment: greedy meshing of full cubes (2026-10-15, Machine A, not adopted)

The build specification asks for binary greedy meshing of full opaque cubes. It was implemented (equal faces of
single-variant solid cubes with the same ambient occlusion at all four corners merged into one quad with a
repeating texture; lighting identical to the per-face path) and measured against the per-face mesher, built from
the same commit and run alternately on the same machine:

| Measurement | Per-face | Greedy |
|---|---|---|
| `BM_MeshStructure/0` (synthetic terrain, all threads; medians of 3, two rounds) | 13.6–13.7 ms | 19.2–20.0 ms |
| `BM_MeshStructure/1` (one thread) | 159–168 ms | 255–281 ms |
| Quads, synthetic terrain | 274 542 | 238 035 (−13 %) |
| `BM_RenderFrame` (1080p) | 1.54–1.73 ms | 1.74–1.78 ms |
| Real Java world "New World", radius 12 (`bridger render`): quads | 2 682 082 | 2 595 565 (−3 %) |
| same: time to load and draw | 1.76 s | 2.04 s |

(The whole machine was slower than in the Phase 7–8 baselines during these runs; the two columns are comparable
with each other, not with older tables.) Real worlds barely merge: most surfaces are leaves (cutout), the common
natural blocks (grass, dirt, stone, sand) use random rotation variants, and smooth lighting differs across every
bump. Fewer than 4 % of quads disappeared while meshing got slower, so the per-face mesher stays. Greedy merging
fits distant LOD meshes, which have neither smooth lighting nor random rotations.

### Comparison with Chunker (2026-10-15, Machine A)

[Chunker](https://github.com/HiveGamesOSS/Chunker) 1.21.0 (CLI, `java -jar chunker-cli-1.21.0.jar -i <world> -o <dir>
-f BEDROCK_R26_50`, Java 26 runtime) against `bridger convert <world> out.mcworld` (`msvc-release`), on the same
Java 26.3 world ("New World", 16 region files, 50.2 MB; the Nether, the End and the entity regions removed so both
tools convert the same Overworld terrain). Five rounds, the two tools alternating, wall-clock time of the whole
process:

| Round | Bridger | Chunker |
|---|---|---|
| 1 | 100.4 s | 115.6 s |
| 2 | 24.4 s | 36.8 s |
| 3 | 15.1 s | 19.4 s |
| 4 | 14.8 s | 20.4 s |
| 5 | 9.7 s | 19.9 s |
| **Median / best** | **15.1 s / 9.7 s** | **20.4 s / 19.4 s** |

Other programs kept the CPU 40–80 % busy during the runs (round 1 especially), so the absolute times are not
baselines; the rounds are paired, and Bridger was faster in each. The outputs are not identical work: Bridger also
re-reads and validates the world and writes it zipped as one `.mcworld` (22.7 MB); Chunker writes a world folder
(22.9 MB). Chunker's time includes JVM start-up (under a second). Re-run on an idle machine before quoting a ratio.
