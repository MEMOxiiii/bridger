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
identical output after decoding.

**Independent validation:** where possible, Bridger's output is opened with an independent implementation (an
open-source reader for that format, or raw LevelDB tools) and compared. Such tools are used **only as test
oracles**, and none of their code is copied.

**Test data:** commit only files we made ourselves or are allowed to commit. Never commit Mojang assets or other
people's worlds.

## In-game checklist

The game is the final judge. A writer is not done until its output passes this checklist in the pinned game
version (`docs/versions.md`).

### Fixed test structures (created in Phase 2–4, stored in `tests/data/ingame/`)

1. **orientations**: stairs (all facings and halves, inner/outer shapes), doors (both halves, hinge, open),
   rails (straight, curved, ascending, powered), logs (all axes), trapdoors, signs on walls.
2. **waterlogged**: waterlogged stairs, slabs, fences, chests, and lanterns; plain water and lava sources.
3. **containers**: chests (single, double) and a barrel with items, including stacks, named items and enchanted
   items.
4. **signs**: signs with text on the front and back, colored text, glowing text.
5. **entities**: an armor stand, an item frame with an item, a painting.
6. **borders**: a structure crossing chunk borders (x/z) and sub-chunk borders (y), including negative
   coordinates.

### Procedure

1. Convert each test structure with the build under test.
2. Import into the game (Bedrock: open the `.mcworld`; Java: copy the world or use a structure block).
3. Check every item in the structure's checklist; take a screenshot of each structure.
4. Record the run below.

### Run log

| Date | Bridger commit | Game + version | Structures | Result | Notes |
|---|---|---|---|---|---|
| — | — | — | — | — | No writer exists yet (Phase 0) |
