# Testing

## Layers

| Layer | Where | Runs in CI |
|---|---|---|
| Unit tests (Catch2) | `tests/unit/<module>/` | yes: `msvc-asan`, `msvc-release`, `clangcl-release` |
| Golden tests | `tests/golden/` + inputs in `tests/data/` | yes (from Phase 2) |
| Round-trip tests | `tests/roundtrip/` | yes (from Phase 6) |
| Integration (large real worlds) | `tests/integration/`, path in `BRIDGER_INTEGRATION_DATA` | no, local only |
| Fuzzing (libFuzzer, clang-cl, static CRT) | `fuzz/` | 30 s smoke per target; longer budgets locally |
| Benchmarks | `bench/` | smoke run in `msvc-release` |
| **In-game checks** | manual, recorded below | no, required for every writer |

ASan: MSVC `/fsanitize=address` (preset `msvc-asan`) and clang-cl. Third-party (vcpkg) libraries are not
instrumented. The MSVC STL's container-overflow annotations must match across every object file in a link, and
an uninstrumented library does not match: the link fails with LNK2038 `annotate_string` / `annotate_vector`.
Instrumented targets therefore define `_DISABLE_STRING_ANNOTATION` and `_DISABLE_VECTOR_ANNOTATION`
(`cmake/Sanitizers.cmake`).
**Limitation:** ASan still catches heap and stack overflows, use-after-free and double free, but it does not
catch reads or writes in the unused capacity of a `std::string` / `std::vector` (between `size()` and
`capacity()`). If that gap matters later, the fix is a custom vcpkg triplet that builds the dependencies with
ASan.

**Determinism test (from Phase 3):** converting with 1 thread and with N threads must produce semantically
identical output after decoding. Whole-world conversion is the parallel path: `tests/unit/formats/anvil_test.cpp`
converts the test Java world with 1 and 8 threads and compares every LevelDB key and value;
`tests/unit/formats/reverse_test.cpp` converts a Bedrock world to Java with 1 and 8 threads and compares the region
files byte for byte.

**Round trip (Phase 6):** `tests/unit/translate/roundtrip_test.cpp` translates every in-game test structure Java →
Bedrock → Java and requires every block state, biome, block entity id and container content back unchanged (stair
shapes excepted where the neighbours do not justify them; a separate test checks that real corners, fences and
panes are rebuilt). `reverse_test.cpp` does the same through files (`.schem` → `.mcstructure` → `.schem`/`.nbt`/
`.litematic`) and checks a Bedrock world → Java world conversion. (The specification's `tests/roundtrip/` folder
is not used: round-trip tests are ordinary unit tests next to the translator.)

**Independent validation:** where possible, Bridger's output is opened with an independent implementation (an
open-source reader for that format, or raw LevelDB tools) and compared. Such tools are used **only as test
oracles**, and none of their code is copied.

**Test data:** commit only files we made ourselves or are allowed to commit. Never commit Mojang assets or other
people's worlds.

- `tests/data/generate_test_data.py` writes all committed test files with **nbtlib** (an independent NBT
  implementation, MIT), plus the expected results in `tests/golden/` computed by the script itself. Regenerate
  with `python -m pip install nbtlib==2.0.4 && python tests/data/generate_test_data.py`. The output is
  deterministic, so commit the results.
- `tests/data/nbt/`: NBT reference files (big- and little-endian, plus gzip) for the byte-exact round-trip test.
- `tests/data/schem/`: Sponge v1/v2/v3 schematics, one crossing section borders with a 300-entry palette, and
  `malformed/` files with the expected error code in `tests/golden/schem/malformed.json`.
- `tests/data/structure/`, `litematic/`, `schematic/`: structure `.nbt` files in both palette spellings and with
  several palettes; a two-region `.litematic` with a negative size and indices spanning longs; a legacy
  `.schematic` with `AddBlocks`, unknown ids, an old-style chest and a 1.12 bed.
- `tests/data/javaworld/basic/`: a Java world (level.dat + two region files) with zlib, gzip, uncompressed and
  external (`.mcc`) chunks, both palette spellings, a waterlogged block, biomes, a block entity, negative chunk
  coordinates, a partially generated chunk and a Nether folder.
- `tests/data/javaworld/v1_16/`, `v1_12/`, `mcregion/`, `alpha/`: worlds in the layouts before 1.18 (1.13 spanning and
  1.16 packed palettes with 256- and 1024-entry biome arrays; 1.12 numeric ids with every state 1.13 moved; a McRegion
  column; an Alpha chunk file, refused). Old Bedrock layouts (LegacyTerrain, subchunk versions 0, 1 and 8 with old
  states, the experimental key offset, 2D biomes, legacy extra data) are written key by key by
  `tests/unit/formats/old_bedrock_test.cpp`.

**Independent validation of Java output:** `tests/integration/validate_java_output.py` re-reads written `.schem`,
`.nbt`, `.litematic` files and Java world folders with nbtlib (palette indices, packed arrays, chunk coordinates,
block entity positions, level.dat).

**Independent validation of written worlds:** `tests/integration/validate_mcworld.py` opens a `.mcworld` with
amulet-leveldb (test oracle only) and nbtlib, decodes every subchunk itself, checks every palette entry against the
official Bedrock palette (exact types and block version), checks that layer 2 holds only air or liquids, and
compares the counts with Bridger's `--report`. Run it on the in-game test worlds before every release.

**Real-world inputs** (not committed: they belong to their authors), run locally:

| Date | File | Source | Result |
|---|---|---|---|
| 2026-10-12 | `AraluenBW1.21Solo.schem` (405 × 140 × 405, 23 M blocks, v3, DataVersion 3955) | WorldEdit, provided by the maintainer | read OK (`bridger info`); converted in 2.7 s; every block state mapped; 38/38 block entities translated; independent validation passed |
| 2026-10-12 | All 1511 structure files in the Java 26.3 server jar (`data/minecraft/structure/**.nbt`) | Mojang (read from the local jar, never committed) | every file reads and converts to `.mcstructure`; the run found that Mojang writes `{id, properties}` palette entries |
| 2026-10-12 | `momen2.mcworld` (Bedrock world, 676 chunks, flat generator) | provided by the maintainer | `bridger info` reads it; converted to a Java world in 0.43 s: every block mapped, 38 block entities; `validate_java_output.py` passes |
| 2026-10-12 | `containers.schem` placed into a copy of the `orientations` test world with `--into-world` | generated | source world unchanged (hash); every untouched key byte-identical; overwritten block entities reported; independent validation passed |
| 2026-10-17 | A Paper survival server world (Java 1.21.4, 290,176 chunks, 7.1 G blocks) | provided by the maintainer, never committed | converted to `.mcworld` (Overworld; 76,246 partially generated chunks skipped as designed). Before the item and block entity work: 83,430 issues (55,914 block entities without data, 15,002 loot tables and 1,950 items' data dropped). After: 884 issues, all named in the report (bees in hives, vault rewards, text formatting, cosmetic item fields, map contents); 92,509 block entities written |

**Fuzz budgets:** CI runs 30 s per target. Before a phase is closed, every target runs at least **60 s** locally
with seeds (`tests/data/nbt` for `fuzz_nbt`; `tests/data/schem` and `tests/data/schem/malformed` for
`fuzz_schem`). Phase 1–2 local run (2026-10-11): varint 336k, nbt 544k, compression 1.06M, schem 384k
executions, no findings. Phase 3–4 local run (2026-10-12, 60 s each): varint 327k, nbt 414k, compression 846k,
schem 398k, subchunk 429k (seeds: subchunks extracted from a written world), zip 49k (seeds: the in-game test
worlds) executions, no findings. Phase 5 local runs (2026-10-12, seeds from `tests/data`). The first run (90 s each) found two **slow
inputs**, no crashes: a structure `.nbt` with size 0 x 196608 x 131074 and a `.mcstructure` with size
786436 x 4096 x 0. Volume 0 passed every volume limit, yet loops over the other two axes ran for minutes
(the pipeline's block count did the same). `Structure::create` now refuses a zero axis next to non-zero ones
(regression test in `tests/unit/core/ir_test.cpp`). Second run (60 s each, `-report_slow_units=5`): java_structure
485k, mcstructure 544k, anvil_chunk 495k, subchunk 692k, zip 95k, schem 651k, nbt 617k executions, no findings.
Phase 6 (2026-10-12, 60 s each): `fuzz_mcstructure` now also runs Bedrock → Java translation and the `.schem` and
`.litematic` writers on every accepted input, `fuzz_anvil_chunk` re-encodes every accepted chunk with Bridger's
writer and requires the same blocks after decoding again, and `fuzz_subchunk` also runs the raw-palette decoder.
The first run found one bug: a chunk with `yPos` −65526 decoded, but its sections (Y is a Byte) could not be
written back; the decoder now refuses a `yPos` outside −128..127 (regression input
`tests/data/javaworld/fuzz_ypos_out_of_range.nbt`). Second run: java_structure 623k, mcstructure 271k,
anvil_chunk 45k, subchunk 370k, zip 86k, schem 384k, nbt 418k executions, no findings.

## In-game checklist

The game is the final judge. A writer is not done until its output passes this checklist in the pinned game
version (`docs/versions.md`).

### Fixed test structures (`tests/data/ingame/`, generated by `tests/data/generate_test_data.py`)

Convert each with `bridger convert tests/data/ingame/<name>.schem bridger-test-<name>.mcworld --name "Bridger test: <name>"`.
The worlds open in creative mode; the structure sits on the superflat surface around x = 0, z = 0, with the
spawn point beside it.

| World | What to check |
|---|---|
| `orientations` | Stairs: 4 facings, bottom (front row) and upside-down (second row); inner/outer corner stairs; 4 doors (two open, hinges on both sides, upper and lower halves joined); logs on x/y/z axes; trapdoors (closed bottom, open top); rails (straight, curved, ascending powered rail); wall torch on the brick; wall post |
| `waterlogged` | Stairs, slab, fence, chest, lantern, glass pane, trapdoor all contain water (water visible inside the block); a water source and a lava source that stay put; the chest holds 3 cod |
| `containers` | Single chest named "Single chest" with 5 diamonds (slot 1), 64 oak logs (slot 14), a diamond sword (slot 27); **one double chest** (both halves open the same 54-slot inventory) with emeralds and gold; trapped chest with TNT; barrel with wool; furnace with iron ore + coal; hopper with apples; red shulker box with ender pearls |
| `signs_beds` | Standing sign: "Bridger / front / line 3 / line 4", back: "back … side"; wall sign on the brick; a sign with glowing text; beds in red, white, blue, lime (heads toward +z, both halves joined); the armor stand is **expected to be missing** (entities are reported, not written, in 0.1.0) |
| `borders` | 40 × 40 × 40, crossing chunk and subchunk borders and negative coordinates: checkerboard floor unbroken across chunk lines; quartz pillars at the corners and at 15/16 and 31/32 (chunk borders) straight, with glowstone every 16 blocks; glass column with red glass every 16 blocks; **no holes or seams** where written chunks meet game-generated flat terrain |

Also check for every world: it imports (double-click the `.mcworld`) and opens without errors; the spawn is beside
the structure; the ground under the structure is the same superflat as the surroundings (y = −61 surface); no
terrain regenerates over the structure after walking away and back.

1. **orientations**: stairs (all facings and halves, inner/outer shapes), doors (both halves, hinge, open),
   rails (straight, curved, ascending, powered), logs (all axes), trapdoors, signs on walls.
2. **waterlogged**: waterlogged stairs, slabs, fences, chests, and lanterns; plain water and lava sources.
3. **containers**: chests (single, double) and a barrel with items, including stacks, named items and enchanted
   items.
4. **signs**: signs with text on the front and back, colored text, glowing text.
5. **entities**: an armor stand, an item frame with an item, a painting.
6. **borders**: a structure crossing chunk borders (x/z) and sub-chunk borders (y), including negative
   coordinates.

### Phase 5–6 additions

Files in `build/ingame-test/phase56/`, made with the build under test (commands in the folder's `README.txt`):

| File | Game | What to check |
|---|---|---|
| `containers.mcstructure`, `orientations.mcstructure` | Bedrock 26.52 | Load with a structure block (put the file in a behaviour pack's `structures/` folder or use `/structure load`): same blocks, chest contents, double chest opens as one |
| `placed.mcworld` | Bedrock 26.52 | The `orientations` structure placed at −6, −60, 3 next to the build in the `containers` world (same chunks); both builds intact, nothing else changed |
| `javaworld.mcworld` | Bedrock 26.52 | Converted from the generated Java test world: terrain chunks present, chest with 3 diamonds at 3, 70, 5, desert biome on the x ≥ 8 side |
| `containers.schem`, `containers.litematic` | Java 26.3 (WorldEdit / Litematica) | Loads; same blocks and chest contents as the Bedrock world |
| `containers.nbt`, `orientations.nbt` | Java 26.3 | Copy to `<world>/generated/minecraft/structure/` and load with a structure block: blocks, stairs (including corners), chest contents |
| `Bridger containers` (folder) | Java 26.3 | Copy to `saves/`: the world opens; the structure and its flat ground are there; light is correct (no black areas); chests open with their items; chunks around it generate; note whether the seed in `/seed` matches the Bedrock world's |

### Phase 7–8: viewer and desktop app checklist

Run `bridger-gui.exe` from the release zip on a machine with Minecraft Java 26.3 installed (the textures).

| # | Check |
|---|---|
| 1 | The window opens on the Bridger wordmark, then the home screen; "Choose a file" and drag-and-drop both open `tests/data/ingame/*.schem` on its file page (no 3D view yet). "Open the 3D preview" shows it; "Back to convert" (or Esc) returns to the same file page; "Open another file" replaces it. |
| 2 | Each in-game test structure renders like the game: block models and rotations (`orientations`), glass and stained glass, water and lava levels (`waterlogged`), chests (single and double), beds, signs, shulker boxes. |
| 3 | Orbit (drag), pan (right-drag), zoom (wheel) feel smooth and stop exactly; "Frame" returns to the whole build. |
| 4 | Fly: double-click or F, WASD + Space/Shift + Ctrl, mouse look; Esc returns to the UI; with an Arabic or other non-Latin keyboard layout too. |
| 5 | Map (M) shows the build from above; Slice hides the top and the slider moves the cut. |
| 6 | A Java world (`%APPDATA%/.minecraft/saves/...`) and a Bedrock `.mcworld` stream in around the camera; flying away loads new chunks and drops old ones; frame rate stays smooth. |
| 7 | Convert a `.schem` to `.mcworld` without touching the CLI: choose the format, Browse, Convert; progress shows; "Show in folder" selects the file; the world opens in Bedrock with the build. |
| 8 | Start a large conversion and Cancel: the drawer returns to the options, and no output file (or temp file) is left. |
| 9 | Place into a copy of an existing world (x, y, z): the original world file is unchanged (compare its hash). |
| 10 | The report lists the issues of a file that has some (e.g. a `.mcstructure` with Bedrock-only blocks); clicking one outlines those blocks and flies there; clicking again clears it. |
| 11 | Settings: rename the client jar folder → the app reports the textures missing but still converts; choose the jar → textures return. Add a resource pack → its textures show. |
| 12 | Windows "Animation effects" off (reduced motion): no slide or float animations, everything still works. |
| 13 | Resize, maximise and move the window between monitors with different scaling: the UI and the 3D view stay sharp and aligned. |
| 14 | Distant terrain (Settings → View, default 32 chunks): simplified land continues past the render distance up to the fog; the HUD counts "+ N far"; moving the slider to Off removes it. Closing and reopening the same world shows the distant land at once (cache). |
| 15 | Old worlds open and convert: a Bedrock world folder from `minecraftWorlds` (the folder itself, not a `.mcworld`), a Java world from before 1.13; the original folders are unchanged afterwards. |
| 16 | The convert page shows "Saved by" with the right version for a `.schem` from an older WorldEdit, a 1.20 Java world, a `.mcworld` and a `.schematic`; the "Game version" list offers every target, newest first, and the line under it reads "source → target". |
| 17 | Target versions in the game: a world converted for Java 1.20.1 and one for 1.18.2 open in those versions (launcher installations) with the build and chests intact; a `.schem` written for 1.20.1 pastes in WorldEdit on 1.20.1. A `.mcworld` written for an older Bedrock version opens in the current Bedrock (which upgrades it). |
| 18 | Same edition: a 26.3 Java world converted to a Java 1.19.4 world opens in 1.19.4 with cherry wood as birch and a crafter as a dropper with its items; a `.mcworld` converted to 1.20.0 and back to 26.50 opens with every block intact. ⚠ Bedrock "unknown" placeholder: a world with cherry planks written for 1.19.80 still shows cherry planks after the current Bedrock opens it (the kept block comes back). |

### Procedure

1. Convert each test structure with the build under test.
2. Import into the game (Bedrock: open the `.mcworld`; Java: copy the world or use a structure block).
3. Check every item in the structure's checklist; take a screenshot of each structure.
4. Record the run below.

### Desktop app verification (0.4.x)

Driven with simulated mouse and keyboard input on Machine A (Windows 11, RTX 4060 Laptop, 1440 x 900 at 125 %),
`msvc-release` / `msvc-debug` builds, screenshots in the session's scratch folder; items refer to the Phase 7–8
app checklist above.

| Date | Commit | Items | Result | Notes |
|---|---|---|---|---|
| 2026-10-13 | `abd1860` (0.4.0 candidate, drawer UI) | 2, 3, 4, 5, 6, 7, 8 | **passed** | `.schem` → `.mcworld` through the UI with the Save dialog (file written, report shown); Cancel during a 7649-chunk world conversion left no output; fly mode with WASD/Esc (scancodes); map and slice on a real Java world; Bedrock `.mcworld` streamed; orbit/pan/zoom |
| 2026-10-13 | `e970a88` (file page UI) | 1 (part) | **passed** | in the app: splash → home, and a file opens on its file page without starting the 3D view. Preview entry and Back to convert were checked in the browser build only (the app run was disturbed by a second instance) |
| — | — | 9, 10, 11, 12, 13 and drag-and-drop | **open** | place-into-world hash check, report highlight on a file with issues, missing textures, reduced motion, multi-monitor DPI: to be run by the maintainer |

### Run log

| Date | Bridger commit | Game + version | Structures | Result | Notes |
|---|---|---|---|---|---|
| 2026-10-12 | `72d121a` | Bedrock 26.52, Java 26.3 | the five Phase 4 worlds (`build/ingame-test/`) and every Phase 5–6 file (`build/ingame-test/phase56/`) | **passed** | Run by the maintainer, who reported every item as working (release 0.3.0 gate). This answers the open questions in `docs/formats/bedrock-world.md` and `anvil.md` for these files |
| 2026-10-16 | `fe2368d` | Bedrock 26.52 and older Java releases | items 15–18: old worlds, "Saved by", target versions, same-edition conversion and the Bedrock "unknown" block | **passed** | Run by the maintainer, who reported everything as working |
