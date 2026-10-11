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
| NBT | `bridger_bench_nbt` | parse throughput MB/s | Phase 1 |
| Region | `bridger_bench_region` | chunks/s | Phase 5 |
| Translation | `bridger_bench_translate` | sections/s | Phase 3 |
| Meshing | `bridger_bench_mesher` | sections/s | Phase 7 |
| End-to-end | `bridger_bench_e2e` | reference conversions, wall time | Phase 4 |
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

Note: varint decode goes through `Result<>` once per value. Phase 2 (Sponge reader) adds a bulk decoder for
whole arrays with a single error check, then re-measures.
