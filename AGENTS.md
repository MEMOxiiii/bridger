# AGENTS.md — Bridger

The single source of truth for any AI agent (or human) working on this repository.

## Purpose

Bridger is a Windows desktop tool, written in C++20, that converts Minecraft structures and worlds between Java
formats (`.schem`, `.schematic`, `.litematic`, structure `.nbt`, Anvil worlds) and Bedrock formats
(`.mcstructure`, `.mcworld`), and renders them in 3D. Speed is a core feature, and every speed claim needs a
benchmark behind it. A conversion must never corrupt or silently lose user data.
**Released: 1.1.0. Current phase: after 1.0 (items and block entities in both directions, the Nether and the End,
item frames, paintings and the player, the self-updater; next: Linux/macOS command line), on top of the Vulkan viewer
and the desktop app `bridger-gui.exe` (ADR 0009).** All
automated checks pass; the in-game and app checklists (`docs/testing.md`) gate release 0.4.0. Phase 0.5's questions are folded
into that checklist (`docs/formats/bedrock-world.md`, "Open questions").

## Commands

Run these from a **VS 2022 x64 developer shell** ("x64 Native Tools Command Prompt" or "Developer PowerShell"),
with `VCPKG_ROOT` pointing at a standalone vcpkg checkout of tag `2026.07.29`. **Set `VCPKG_ROOT` after the
developer shell has started:** the shell overwrites it with the vcpkg bundled with Visual Studio, which is too
old for this baseline. For example: `set VCPKG_ROOT=%USERPROFILE%\vcpkg`. The first configure builds every
dependency (about 7 minutes on a 16-thread laptop); later runs use vcpkg's binary cache. Building the app also
needs **Node.js 22+** on `PATH`: CMake runs `npm ci` (only when `package-lock.json` changed) and `npm run build`
in `src/app/ui`.

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
| UI in a browser | `cd src/app/ui && npm run dev` (mock bridge; `#convert`, `#done`, `#report`, ... show states) |
| Screenshot | `build\msvc-release\bin\bridger.exe render <file or world> out.png` (needs the 26.3 client jar) |

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
| `app` → `bridger-gui.exe` | Desktop app: SDL3 window, Vulkan viewer, WebView2 + React UI over it (ADR 0009) |

**Dependency rule:**
`core` ← `platform` ← `io` ← `formats` ← `convert` (also uses `translate`, which depends on `core` and `io`;
ADR 0008) ← `cli`;
`render` depends on `core` and `formats`; `app` depends on everything. Lower layers never depend on higher ones.
`core`, `platform`, `io`, `formats`, `translate` and `convert` have **no UI or GPU dependencies**, and only
`platform` may include OS headers, plus the two presentation folders `src/render/win32/` (Vulkan/D3D11 texture
import) and `src/app/win32/` (WebView2, DirectComposition), which cannot be portable (ADR 0009). `cmake/BridgerModule.cmake` enforces this (`BRIDGER_ALLOWED_DEPS_<module>`):
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
src/app/        bridger-gui.exe — main.cpp, app.{hpp,cpp}, viewer, settings, ui_bundle, win32/, ui/ (React, npm)
tools/spike/    Phase 0.5 Bedrock world experiment      tools/mapgen/  mapping data -> .brmap
tests/          unit/<module>/*_test.cpp, golden/, roundtrip/, integration/ (env-var paths), data/
fuzz/           fuzz_<target>.cpp (libFuzzer)           bench/  bench_<area>.cpp (Google Benchmark)
docs/           README.md (index), formats/, adr/, app/, assets/, mappings/, versions.md, safety.md, performance.md,
                testing.md, release.md, CHANGELOG.md, CONTRIBUTING.md, THIRD_PARTY.md
```

The full planned file tree (every header per module) comes from the original build specification. These
additions to it are recorded here as required:

- `third_party/CMakeLists.txt`: builds the leveldb-mcpe submodule as a static library (ADR 0004).
- `bench/bench_io.cpp`: benchmarks for low-level I/O primitives (varint, endian). `bench/bench_schem.cpp`:
  end-to-end `.schem` reading.
- `fuzz/fuzz_varint.cpp`, `fuzz/fuzz_compression.cpp`: fuzz targets for varint decoding and decompression.
- `src/platform/*.cpp` (outside `win32/`): OS-independent platform logic (the atomic-commit protocol, temp
  names). The OS calls live behind `platform::FileOps` in `win32/`.
- `src/cli/commands/commands.hpp`: declarations shared by `main.cpp` and the command files.
- `tests/unit/test_util.hpp`: shared test helpers. `tests/data/generate_test_data.py`: generates all committed
  test data, the golden files and the in-game test structures (`tests/data/ingame/`) with nbtlib.
- `tests/integration/validate_mcworld.py`: independent validation of written worlds (see `docs/testing.md`).
- `tools/mapgen/fetch_sources.py`, `tools/mapgen/overrides/blocks.json`, `tools/mapgen/sources/geyser-mappings`
  (submodule), `tools/mapgen/sources/cache/` (gitignored).
- `src/translate/data/*.brmap`: the generated, committed mapping blob. `docs/mappings/`: generated coverage reports.
- `cmake/bin2cpp.cpp`: build-time helper that embeds binary files (`EmbedResource.cmake`).
- `src/io/zip_archive.cpp`, `src/convert/placement.cpp`, `src/cli/commands/convert.cpp` as listed in the module
  tables of the original specification.
- `src/platform/win32/paths_win32.cpp`: Windows implementation files carry a `_win32` suffix so their object
  files never clash with a future `posix/` twin.
- `src/formats/include/bridger/formats/structure_file.hpp` (+ `structure_file.cpp`): the common result of every
  Java structure reader, so the pipeline does not depend on which reader ran. `src/formats/java/nbt_helpers.hpp`:
  internal helpers shared by the Java readers (not installed, not public).
- `formats/java/anvil/`: Java `level.dat` reading lives in `java_world.hpp` (no separate `level_dat.hpp`: it is
  three fields). Bit packing is local to each reader (`chunk_codec.cpp`, `litematic.cpp`), because the two
  formats pack differently; there is no shared `java_bit_packing.hpp`.
- `fuzz/fuzz_bedrock_subchunk.cpp` (every Bedrock subchunk version, LegacyTerrain, the state upgrader).
- `fuzz/fuzz_java_structure.cpp` (structure `.nbt`, `.litematic`, legacy `.schematic`), `fuzz/fuzz_mcstructure.cpp`,
  `fuzz/fuzz_anvil_chunk.cpp`; `bench/bench_anvil.cpp` (chunks, Bedrock world reading), `bench/bench_structures.cpp`
  (structure formats). `.github/workflows/release.yml`: builds the portable zip and the GitHub release for a `v*` tag.
- `docs/CHANGELOG.md`, `docs/CONTRIBUTING.md`, `docs/THIRD_PARTY.md` (only `README.md`, `AGENTS.md` and `CLAUDE.md`
  stay at the root, with the build and tool configuration files); `docs/README.md` indexes the docs.
- Phase 7 (`render`): `assets.hpp` (resource packs), `block_models.hpp` (baking), `mesher.hpp`, `renderer.hpp`,
  `scene.hpp` (structure meshing + framing), `streaming.hpp` (world streaming), `image_io.hpp` (PNG); internal
  `entity_models.*`, `external_image.hpp` + `win32/external_image_win32.cpp` (+ `posix/` stub), `vma_impl.cpp`,
  `stb_impl.cpp`; shaders `chunk.*`, `line.*`. `convert/view_world.hpp`: worlds opened for viewing.
  `src/cli/commands/render.cpp`: `bridger render`. `docs/formats/assets.md`. `bench/bench_render.cpp`.
- `render/lod_cache.hpp` (+ `.cpp`): the distant terrain cache (`docs/formats/assets.md`, "Distant terrain");
  `tests/unit/render/lod_test.cpp`.
- `platform/net.hpp` + `win32/net_win32.cpp` (HTTPS via WinHTTP, SHA-1 via BCrypt) and `render/asset_download.hpp`
  (the client jar download on request; `docs/safety.md`, "Network access").
- Self-update: `src/app/updater.{hpp,cpp}` (OS-free, unit-tested from `tests/unit/app/updater_test.cpp`), the UI's
  `components/UpdateCard.tsx`, `platform/process.hpp` + `win32/process_win32.cpp` (own path, start a process),
  `docs/app/updates.md`.
- Phase 8 (`app`): `viewer.*` (camera controller, OS-free, unit-tested from `tests/unit/app/`), `settings.*`,
  `ui_bundle.*` (the embedded UI zip), `win32/compositor.*`, `win32/webview_host.*`, `win32/shell*`; the UI
  in `src/app/ui/` (`package.json`, `package-lock.json`, `vite.config.ts`, `src/**`); `cmake/NpmInstall.cmake`;
  `docs/app/bridge.md` (message protocol); `docs/adr/0009-hybrid-desktop-ui.md`.
- Version names and targets: `core/game_versions.{hpp,cpp}`, `convert/source_version.{hpp,cpp}`,
  `formats/java/upgrade.hpp`, `translate/targets.{hpp,cpp}` (+ internal `java_block_entity_downgrade.*`,
  `bedrock_block_entity_downgrade.*`), data `src/translate/data/java-targets.nbt` and `bedrock-targets.nbt`, built by
  `tools/mapgen/build_java_targets.py`, `build_bedrock_targets.py` and `targetgen.cpp` (`bridger_targetgen`) with
  `tools/mapgen/overrides/bedrock_targets.json`; the UI's `components/VersionPicker.tsx`; `tests/unit/translate/targets_test.cpp`.
- `src/formats/java/java_upgrade.{hpp,cpp}` (Java block renames since 1.13, old numeric biome ids) and
  `legacy_blocks.{hpp,cpp}` (pre-1.13 block entities and the states 1.13 moved; shared by `.schematic` and old chunks):
  internal to `bridger_formats`.
- `translate/bedrock_upgrade.hpp` (+ `.cpp`), `src/translate/data/bedrock-upgrade.nbt` and
  `tools/mapgen/build_bedrock_upgrade.py`: upgrading block states of old Bedrock worlds (BedrockBlockUpgradeSchema, CC0).
- `tests/data/javaworld/`: generated Java worlds (`generate_test_data.py`, `generate_java_world`).
- `formats/java/java_writers.hpp` (+ `.cpp`): the `.schem`/`.nbt`/`.litematic` writers share palette and block
  entity code, so they live in one file instead of one per reader. `formats/java/anvil/anvil_writer.hpp`: chunk,
  region, level.dat and world_gen_settings.dat writing. `formats/bedrock/mcworld/bedrock_world_reader.hpp`.
- `src/translate/entity_translator.{hpp,cpp}` (item frames and paintings, both directions) and
  `translate/player.hpp` + `player.cpp` (the single-player player, both directions).
- `src/translate/item_translator.{hpp,cpp}` (item data in both directions, shared by every block entity),
  `tests/unit/translate/item_test.cpp` and `reverse_item_test.cpp`.
- `src/translate/reverse_translator.cpp`, `reverse_block_entities.cpp` (Bedrock → Java) and `tables.hpp`
  (internal tables shared by both directions); `tools/mapgen/overrides/reverse_blocks.json`.
- `src/core/parallel.cpp` implements `core/parallel.hpp`.
- Round-trip tests live in `tests/unit/translate/roundtrip_test.cpp` and `tests/unit/formats/reverse_test.cpp`
  (no `tests/roundtrip/` folder). `tests/integration/validate_java_output.py`: independent check of Java output.

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
- **NBT views in loops:** never `for (x : *view.as_list())` or `*view.as_compound()`. The temporary
  `std::optional` dies before the loop body (C++20). Use `view.items()` / `view.entries()`. ASan found this
  in Phase 1.
- **No recursion over untrusted nesting.** Walk trees with explicit stacks (see `docs/formats/nbt.md`). A legal
  512-deep NBT document must not overflow the stack in any build.
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
10. Status → Supported only after steps 1–9. Update the README support matrix and `docs/CHANGELOG.md`.

## How to update mapping data for a new Minecraft version

1. Add the version to `docs/versions.md` with its release date and data version / protocol numbers.
2. Update the mapping source submodules in `tools/mapgen/sources/` (GeyserMC/mappings branch for the version
   pair), after checking that their licenses are unchanged (`docs/THIRD_PARTY.md`). Update the pinned URLs and hashes in
   `tools/mapgen/fetch_sources.py` (Mojang server jar from the launcher manifest; Bedrock palette from Geyser).
3. `python tools/mapgen/fetch_sources.py`, then build `bridger_mapgen` and run it:
   `bridger_mapgen --java-blocks tools/mapgen/sources/cache/java-<v>-generated/reports/blocks.json
   --geyser tools/mapgen/sources/geyser-mappings --bedrock-palette tools/mapgen/sources/cache/block_palette.<bv>.nbt
   --overrides tools/mapgen/overrides --out src/translate/data/<name>.brmap --report docs/mappings/coverage-<name>.md`.
   Commit the new `.brmap` and the coverage report. Update the version constants in
   `src/formats/bedrock/mcworld/` and `level_dat.hpp` from the sources listed in `docs/formats/bedrock-world.md`.
4. Review the new and changed states. Add overrides in `tools/mapgen/overrides/`, each with a comment and a test.
5. Check the reverse direction (Bedrock → Java) separately.
6. Run the full test suite plus the in-game checklist for that version.

## Definition of Done (every change)

- It builds with **no warnings** under `msvc-debug`, `msvc-release`, `msvc-asan` and `clangcl-release`.
- All tests pass, including under ASan (`ctest --preset msvc-asan`).
- Benchmarks touching the changed code have not regressed (compare against `docs/performance.md`).
- Docs are updated: `docs/formats/`, an ADR for design decisions, README support matrix, and this file if
  commands or layout changed.
- No new dependency without a one-line justification and a `docs/THIRD_PARTY.md` entry.
- Commits are small and focused, with clear messages.

## When to decide vs. when to ask

Decide and record (ADR or doc) routine choices: internal APIs, naming, data structures, test design, and format
details confirmed by sources or experiment. **Ask the maintainer** before anything that changes scope, changes
compatibility (supported versions or formats, or output the game or other tools rely on), or touches data
safety (`docs/safety.md`, placement policy).
