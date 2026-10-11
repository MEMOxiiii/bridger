# 0002. Language, compilers, build system and dependency management

- Status: accepted
- Date: 2026-10-11

## Context

The project targets Windows x64 first, needs maximum performance, and must stay portable enough to add POSIX
later. The stack was decided by the maintainer in the build specification (§3); this ADR records it and the
details settled while implementing Phase 0.

## Decision

- **C++20**. Primary compiler **MSVC** (VS 2022 ≥ 17.10). **clang-cl** is the second compiler and the fuzzing
  compiler.
- **CMake ≥ 3.28** with `CMakePresets.json`. **Ninja** generator for every preset, so all presets behave the
  same and produce `compile_commands.json` for clang-tidy. Presets must run in a VS developer environment (CI
  uses `ilammy/msvc-dev-cmd`).
- Presets: `msvc-debug`, `msvc-release`, `msvc-asan`, `clangcl-release`, `clangcl-fuzz`.
  - `msvc-asan` uses **RelWithDebInfo**, because CMake's Debug flags add `/RTC1`, which is incompatible with
    `/fsanitize=address`.
- **vcpkg in manifest mode.** The baseline is pinned in `vcpkg-configuration.json` (default registry =
  microsoft/vcpkg at tag `2026.07.29`, commit `9e593bb1`). The pin lives in the registry entry instead of a
  `builtin-baseline` in `vcpkg.json`, so it works with any vcpkg tool installation.
  - The vcpkg tool must be recent: the copy bundled with VS 17.13 (2024-12) is too old for 2026 port files. Use
    a standalone checkout of the same tag.
- **Triplet `x64-windows-static-md`**: dependencies are static libraries, the CRT is dynamic (`/MD`). This gives
  a single `bridger.exe` with no dependency DLLs to ship. The dynamic CRT is the default for MSVC and
  sanitizers. Exception: vcpkg forces `vulkan-loader` to be dynamic; Bridger never ships `vulkan-1.dll`
  (ADR 0005).
- Each module is a static library target `bridger_<module>`. `/W4 /WX /permissive-` (plus extra `/w1xxxx`
  warnings) apply only to project code, through `bridger_set_warnings()`.
- Dependency port choices that differ from the specification's starting `vcpkg.json`:
  - `minizip-ng` with `default-features: false` and feature `zlib` only. The defaults pull in bzip2, lzma,
    zstd and crypto, which `.mcworld`/`.jar` files do not need.
  - `libdeflate` with explicit features `compression`, `decompression`, `gzip`, `zlib` (the same as its
    defaults, written out explicitly).
  - **`zlib` added**: leveldb-mcpe's Bedrock compressors are implemented on zlib (ADR 0004). minizip-ng uses it
    as well.

## Consequences

- Building requires a developer shell or an IDE that provides one (VS, VS Code CMake Tools).
- The first configure builds ~25 packages from source. CI caches vcpkg binaries per manifest hash.
- clang-cl presets need the VS component "C++ Clang Compiler for Windows".
