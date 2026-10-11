# 0006. In-house NBT; choice of core libraries; license

- Status: accepted
- Date: 2026-10-11

## Context

NBT parsing is on the hot path of every format. The rest of the library set and the project license were
decided in the build specification (§3, §10).

## Decision

- **NBT is written in-house**, with no NBT library. It must support big-endian (Java) and little-endian
  (Bedrock), and Bedrock's network-varint variant if a format needs it. It provides a **zero-copy reader**
  (views into the source buffer, no allocation per tag, unneeded tags skipped without materializing) and a
  writer. No existing library offers zero-copy views for both byte orders together with our safety limits.
  Implemented in Phase 1.
- Core libraries: **libdeflate** (whole-buffer, faster than zlib), **minizip-ng**, **oneTBB**, **tl-expected**,
  **xxHash**, **spdlog**, **CLI11**; tests and benchmarks with **Catch2 v3**, **Google Benchmark**, **libFuzzer**;
  profiling with **Tracy**. Versions and licenses are in `THIRD_PARTY.md`.
- spdlog stays behind `bridger::log` (`core/log.hpp`). Logs go to **stderr**, so the CLI can print
  machine-readable output on stdout.
- **License: MPL-2.0** (the specification's default). File-level copyleft lets Bridger be used with
  permissively licensed code. All dependencies above are compatible (MIT, BSD, Zlib, Apache-2.0, BSL-1.0,
  CC0).

## Consequences

- We own NBT correctness, which is why Phase 1 requires byte-exact round-trips, fuzzing and a parse benchmark.
- No dependency may be added without a justification and a `THIRD_PARTY.md` entry.
