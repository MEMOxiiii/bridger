# AGENTS.md — Bridger

The single source of truth for any AI agent (or human) working on this repository.

## Purpose

Bridger is a Windows desktop tool, written in C++20, that converts Minecraft structures and worlds between Java
formats (`.schem`, `.schematic`, `.litematic`, structure `.nbt`, Anvil worlds) and Bedrock formats
(`.mcstructure`, `.mcworld`), and renders them in 3D. Speed is a core feature, and every speed claim needs a
benchmark behind it. A conversion must never corrupt or silently lose user data.
**Current phase: 0 (foundation).** Nothing converts yet; see `README.md` → "Format support".

## Commands

Run these from a **VS 2022 x64 developer shell** ("x64 Native Tools Command Prompt" or "Developer PowerShell"),
with `VCPKG_ROOT` pointing at a standalone vcpkg checkout of tag `2026.07.29`. **Set `VCPKG_ROOT` after the
developer shell has started:** the shell overwrites it with the vcpkg bundled with Visual Studio, which is too
old for this baseline. For example: `set VCPKG_ROOT=%USERPROFILE%\vcpkg`. The first configure builds every
dependency (about 7 minutes on a 16-thread laptop); later runs use vcpkg's binary cache.

| Task | Command |
|---|---|
| Configure | `cmake --preset msvc-debug` (or `msvc-release`, `msvc-asan`, `clangcl-release`, `clangcl-fuzz`) |
| Build | `cmake --build --preset msvc-debug` |
| Build Release / ASan | `cmake --build --preset msvc-release` / `cmake --build --preset msvc-asan` |
| Run all tests | `ctest --preset msvc-debug` (ASan: `ctest --preset msvc-asan`) |
| Run one test | `ctest --preset msvc-debug -R "Varint encode"`, or `build\msvc-debug\bin\bridger_unit_tests "[io]"` |
| Run benchmarks | `build\msvc-release\bin\bridger_bench_io.exe` (always use a Release build) |
| Fuzz | `cmake --build --preset clangcl-fuzz` then `build\clangcl-fuzz\bin\bridger_fuzz_varint.exe -max_total_time=60` |
| Format | `clang-format -i <files>` (config in `.clang-format`) |
| Lint | `clang-tidy -p build\msvc-debug <file.cpp>` |
| Profile | configure with `-DBRIDGER_ENABLE_TRACY=ON`, then connect the Tracy GUI |

clang-cl presets need the VS component "C++ Clang Compiler for Windows" (Microsoft.VisualStudio.Component.VC.Llvm.Clang).

## Architecture

Hub-and-spoke around a universal intermediate representation (IR). Each format has a reader (format → IR)
and/or a writer (IR → format), so adding a format never means writing N×N converters.

| Module (target) | Responsibility |
|---|---|
| `core` (`bridger_core`) | IR types, `Result`/`Error`, block-state registry, sections/chunks/worlds, logging, parallel helpers |
| `platform` (`bridger_platform`) | **The only place OS APIs are allowed**: mmap, atomic replace, durable flush, long paths, UTF-8 ⇄ native paths |
| `io` (`bridger_io`) | Binary reader/writer, endian, varint, compression (libdeflate), zip (minizip-ng), NBT (BE + LE), output writer |
| `formats` (`bridger_formats`) | One reader and/or writer per format; format detection; LevelDB (leveldb-mcpe) access |
| `translate` (`bridger_translate`) | Java ⇄ Bedrock mapping data, palette stage + context stage, `ConversionReport` |
| `convert` (`bridger_convert`) | Pipeline read → translate → write → validate; placement policy |
| `render` (`bridger_render`) | Vulkan 1.3 renderer, meshers, resource-pack assets |
| `cli` → `bridger.exe` | Command-line interface |
| `app` → `bridger-gui.exe` | Desktop app (SDL3 + Dear ImGui) |

**Dependency rule:**
`core` ← `platform` ← `io` ← `formats` ← `convert` (also uses `translate`, which depends only on `core`) ← `cli`;
`render` depends on `core` and `formats`; `app` depends on everything. Lower layers never depend on higher ones.
`core`, `platform`, `io`, `formats`, `translate` and `convert` have **no UI or GPU dependencies**, and only
`platform` may include OS headers. `cmake/BridgerModule.cmake` enforces this (`BRIDGER_ALLOWED_DEPS_<module>`):
a disallowed `DEPS` entry fails at configure time.

### Layout

Folders are created when their phase arrives. Never add files outside this layout without updating it here first.

```
CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json   # build; vcpkg baseline pinned
cmake/          CompilerWarnings, Sanitizers, Shaders, EmbedResource, BridgerModule (.cmake)
third_party/    leveldb-mcpe (git submodule) + CMakeLists.txt
src/<module>/   include/bridger/<module>/*.hpp, *.cpp, CMakeLists.txt   (platform: win32/, posix/ later)
src/render/shaders/   GLSL, compiled to SPIR-V and embedded at build time
src/cli/        bridger.exe — main.cpp, commands/{info,convert,render}.cpp
src/app/        bridger-gui.exe — main.cpp, app.{hpp,cpp}, ui/*.cpp
tools/spike/    Phase 0.5 Bedrock world experiment      tools/mapgen/  mapping data -> .brmap
tests/          unit/<module>/*_test.cpp, golden/, roundtrip/, integration/ (env-var paths), data/
fuzz/           fuzz_<target>.cpp (libFuzzer)           bench/  bench_<area>.cpp (Google Benchmark)
docs/           formats/, adr/, versions.md, safety.md, performance.md, testing.md, release.md
```

The full planned file tree (every header per module) comes from the original build specification. These
additions to it are recorded here as required:

- `third_party/CMakeLists.txt`: builds the leveldb-mcpe submodule as a static library (ADR 0004).
- `bench/bench_io.cpp`: benchmarks for low-level I/O primitives (varint, endian).
- `fuzz/fuzz_varint.cpp`: fuzz target for varint decoding.
- `src/platform/win32/paths_win32.cpp`: Windows implementation files carry a `_win32` suffix so their object
  files never clash with a future `posix/` twin.

Module pattern: public headers in `src/<m>/include/bridger/<m>/`, `.cpp` next to them in `src/<m>/`, namespace
`bridger::<m>`, one static library `bridger_<m>` (alias `bridger::<m>`) created by `bridger_add_module()`.
Public headers do not include third-party headers unless it can't be avoided. libdeflate, LevelDB, Vulkan,
spdlog and Windows.h stay out of them. (`tl::expected` in `core/result.hpp` and GLM in `render/camera.hpp` are
the deliberate exceptions.)

## Coding standards

- C++20, MSVC `/W4 /WX /permissive-` (clang-cl gets extra `-W` flags). Warnings are errors on project code only.
  Never add Bridger warning flags to third-party targets.
- Naming: `snake_case` for functions, variables and namespaces; `CamelCase` for types and enumerators;
  `kCamelCase` for constants; a trailing `_` on private members; files are `snake_case.hpp/.cpp`.
- **Errors:** return `bridger::Result<T>` (`tl::expected<T, bridger::Error>`) or `bridger::Status`. Errors carry
  context (file, offset, chunk). **No exceptions across module boundaries** (ADR 0003). Use `BRIDGER_TRY` /
  `BRIDGER_TRY_STATUS` to propagate.
- **Ownership:** RAII everywhere. No raw `new`/`delete`, and no owning raw pointers. Use `std::unique_ptr` and
  value types. Non-owning views use `std::span` / `std::string_view`.
- All strings are UTF-8. Convert to OS paths only through `bridger::platform::path_from_utf8` / `path_to_utf8`.
  Never build a `std::filesystem::path` from a narrow string on Windows (it uses the ANSI code page).
- Logging: use `bridger::log::*`, never spdlog directly. It writes to stderr, so stdout stays machine-readable.
- Prefer simple, readable code; optimize where a benchmark or Tracy shows it matters. But never adopt a design
  that blocks the performance plan.

## Performance rules

- Hot loops contain no `std::string`, `std::map`/`std::unordered_map`, virtual calls, or per-block heap
  allocations. Work with interned `uint32_t` state ids.
- Translation runs the **palette stage first** (translate each distinct palette entry once, then remap
  indices). The **context stage** visits only positions whose state is marked context-dependent. Context rules
  read from an immutable source snapshot, so the result is identical for any thread count.
- Data-oriented storage: contiguous arrays, per-section palettes, and a uniform-section fast path.
- **Measure before and after** every optimization (Release build, same machine). Update `docs/performance.md`
  with machine specs. Never write an unmeasured number in README or docs.

## Safety rules

- **Never modify input files, including existing worlds.** "Place into an existing world" always writes a
  *new* output world.
- Output goes to a temp file **in the destination's directory**. It is flushed and validated (re-read), then
  moved into place atomically: `MoveFileExW(MOVEFILE_WRITE_THROUGH)` for a new file, `ReplaceFileW` with a
  backup when overwriting. Never delete the temp file or the backup until the destination is confirmed
  valid. See `docs/safety.md`, and don't promise more than the OS guarantees.
- **All input is untrusted:** bounds-check every read, and limit NBT depth, decompressed size (zip bombs),
  array sizes and dimensions.
- **No silent data loss:** every block, block entity, entity or biome that isn't translated exactly goes into
  the `ConversionReport`, with a configurable fallback.
- Placement policy defaults (`convert/placement_policy.hpp`): `--air keep`; existing block entities are kept
  unless overwritten, and any overwritten one is reported; an overflow of build height **fails**
  (`--clip` and `--shift-to-fit` are opt-in); untouched chunks and keys are copied byte-for-byte. Changing a
  default needs the maintainer's approval.
- **No Mojang assets in the repo** (textures, models, sounds). Load them at runtime from the user's own
  installation or resource pack.
- **A writer isn't done until its output passes the in-game checklist** in `docs/testing.md`.
- **Never invent format details.** Cite every binary format detail's source in `docs/formats/<format>.md`.
  If the sources disagree or are silent, verify by experiment in the real game.

## Pinned game versions

From `docs/versions.md`: **Java Edition 26.3** and **Bedrock Edition 26.52**. Other versions are added one at a
time (Phase 9).

## How to add a new format (checklist)

1. Research the format. Write `docs/formats/<format>.md` with every byte-level detail and a cited source.
2. Add reader/writer headers under `src/formats/include/bridger/formats/<edition>/` and sources mirroring them.
3. Register the format in `src/formats/format.cpp` (`kFormats`). Its status moves Planned → InProgress.
4. Add input limits: dimensions, palette size, NBT depth, decompressed size.
5. Golden tests: small committed files in `tests/data/` (made by us, or ones we may legally commit) with
   expected decoded output in `tests/golden/`.
6. Round-trip test (A → IR → A), if both directions exist.
7. Fuzz target `fuzz/fuzz_<format>.cpp`, run clean for the agreed time budget.
8. Benchmark in `bench/` and record the baseline in `docs/performance.md`.
9. Writers: re-read validation, an independent-implementation check where possible, and the **in-game
   checklist** run recorded in `docs/testing.md`.
10. Status → Supported only after steps 1–9. Update the README support matrix and `CHANGELOG`.

## How to update mapping data for a new Minecraft version

(Mapgen arrives in Phase 3; this is the procedure it must support.)
1. Add the version to `docs/versions.md` with its release date and data version / protocol numbers.
2. Update the mapping source submodules in `tools/mapgen/sources/`, after checking that their licenses are
   unchanged (`THIRD_PARTY.md`).
3. Run mapgen and commit the new `.brmap` and the coverage report (mapped / unmapped / lossy / needs context
   rule).
4. Review the new and changed states. Add overrides in `tools/mapgen/overrides/`, each with a comment and a test.
5. Check the reverse direction (Bedrock → Java) separately.
6. Run the full test suite plus the in-game checklist for that version.

## Definition of Done (every change)

- It builds with **no warnings** under `msvc-debug`, `msvc-release`, `msvc-asan` and `clangcl-release`.
- All tests pass, including under ASan (`ctest --preset msvc-asan`).
- Benchmarks touching the changed code have not regressed (compare against `docs/performance.md`).
- Docs are updated: `docs/formats/`, an ADR for design decisions, README support matrix, and this file if
  commands or layout changed.
- No new dependency without a one-line justification and a `THIRD_PARTY.md` entry.
- Commits are small and focused, with clear messages.

## When to decide vs. when to ask

Decide and record (ADR or doc) routine choices: internal APIs, naming, data structures, test design, and format
details confirmed by sources or experiment. **Ask the maintainer** before anything that changes scope, changes
compatibility (supported versions or formats, or output the game or other tools rely on), or touches data
safety (`docs/safety.md`, placement policy).
