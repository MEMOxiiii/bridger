# 0004. Bedrock world database: Amulet-Team/leveldb-mcpe, built by our CMake

- Status: accepted
- Date: 2026-10-11

## Context

Bedrock worlds are LevelDB databases with Mojang-specific compressors: raw deflate (id 4) and zlib (id 2). The
build specification names "leveldb-mcpe (Mojang's LevelDB fork)" as a git submodule and asks to verify the
maintained fork and its license first.

Findings (2026-10-11):
- `github.com/Mojang/leveldb-mcpe` is **no longer available** (`git ls-remote` reports "Repository not found",
  HTTP 404).
- `github.com/Amulet-Team/leveldb-mcpe` is a maintained fork of Mojang's code: not archived, last push
  2024-11-22, **BSD-3-Clause** (the original Google LevelDB license). It includes Mojang's `ZlibCompressor` and
  `ZlibCompressorRaw`, Windows ports (`port_win`, `env_win`), and Mojang's sample setup
  (`mcpe_sample_setup.cpp`).
- Its `CMakeLists.txt` builds a **shared** library for Amulet's Python wrapper and links a `zlibstatic` target
  that only exists when zlib is built as a sub-project.

## Options

1. Patch the fork's CMakeLists.txt (a local diff on a submodule is fragile).
2. Fork the fork (more repos to maintain).
3. **Keep the submodule pristine and compile its sources from `third_party/CMakeLists.txt`.**
4. Plain Google LevelDB: lacks the Bedrock compressors, so we would reimplement them.

## Decision

Option 3. `third_party/CMakeLists.txt` builds a **static** `leveldb_mcpe` target (alias `leveldb::mcpe`) from the
same source list as upstream, linked to vcpkg's `ZLIB::ZLIB`. Details:
- `DLLX` (the fork's export macro) is defined empty as a PUBLIC definition.
- The zstd and snappy compressors are excluded (`zstd_compressor.cc` is guarded by `#ifdef ZSTD`;
  `snappy_compressor.cc` is not compiled). Bedrock worlds use zlib/raw deflate, so zstd/snappy would only be
  added if a real world needs them.
- Compiled as C++17 with warnings off. It is third-party code, so Bridger's `/WX` never applies.
- The submodule is pinned at commit `4846fc72c7eda860b1bcf6efc58920a9273da928`.

## Consequences

- Only `bridger_formats` links it (privately); LevelDB headers never appear in Bridger's public headers.
- Phase 0 has a smoke test that writes, compacts and re-reads a binary key with the raw-zlib compressor.
- If Amulet's fork stops being maintained, we can switch the submodule URL without changing our build.
