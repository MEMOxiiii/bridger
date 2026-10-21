# Java world (Anvil)

Editions/versions covered: Java Edition worlds from **Beta 1.3 (McRegion) to 26.3**, all three dimensions;
blocks and biomes of older versions are upgraded to the pinned target, Java 26.3 (DataVersion 5023,
`docs/versions.md`). See "Old worlds" for everything before 1.18. Alpha worlds (before Beta 1.3) are refused
with a message asking the user to open them once in a later version, which converts them.

Code: `src/formats/java/anvil/` (`region_file`, `chunk_codec`, `java_world`), conversion in
`src/convert/pipeline.cpp` (`convert_java_world`).

## Container

A world is a folder [3]. **Java 26.1 changed the layout** [4]: each dimension moved under
`dimensions/<namespace>/<name>/`, and world generation settings moved out of `level.dat`. Both layouts are read;
the 26.1+ one is detected by `dimensions/minecraft/overworld/`.

| Path (26.1+) | Path (before 26.1) | Content | Bridger |
|---|---|---|---|
| `level.dat` | same | gzip big-endian NBT, root → `Data` | read: name, DataVersion, version name, spawn (and seed before 26.1) |
| `data/minecraft/world_gen_settings.dat` | `level.dat` → `Data.WorldGenSettings` | gzip NBT; 26.1+: saved-data wrapper `{DataVersion, data: {seed, generate_structures, bonus_chest, dimensions}}` | read: seed |
| `dimensions/minecraft/overworld/region/r.<x>.<z>.mca` | `region/` | Overworld chunks, 32 × 32 per file (sections y -4..19) | read and written |
| `…/region/c.<x>.<z>.mcc` | same | oversized chunk payload (absolute chunk coordinates) | read |
| `dimensions/minecraft/<dim>/entities/r.<x>.<z>.mca` | `entities/`, `DIM-1/entities/`, `DIM1/entities/` | entities (1.17+): region files whose chunks are `{DataVersion, Position: [I; x, z], Entities: [...]}` | read; written for item frames and paintings (each entity with `id`, `Pos` doubles, `UUID` int[4]), and with `--entities all` for mobs, dropped items, armor stands and end crystals (`docs/formats/bedrock-world.md`, "Mobs", lists every tag); every other entity is reported |
| `level.dat` → `Data.Player` | same | the single-player player | read and written (`docs/formats/bedrock-world.md`, "Player") |
| `dimensions/minecraft/the_nether/region`, `…/the_end/region` | `DIM-1/region`, `DIM1/region` | Nether, End chunks (`yPos` 0, sections 0..15, y 0..255) [3] | read and written (Anvil only) |
| `<world>_nether/DIM-1`, `<world>_the_end/DIM1` next to the world | | a Bukkit/Spigot/Paper server's separate Nether and End worlds | not read, as in Chunker (`JavaLevelDirectoryResolver`) — reported |

The field names in `world_gen_settings.dat` (`seed`, `generate_structures`, `bonus_chest`) and the saved-data
wrapper key `data` were read from the string constants of the 26.3 server's `WorldOptions` and
`SavedDataStorage` classes (the 26.x server is not obfuscated); no code was copied. The 26.3 server falls back to
a random seed when this file is missing ("Unable to read or access the world gen settings file! Falling back to
the default settings with a random world seed").

The input may be the folder or its `level.dat`.

## Region file

From [1]:

- Bytes 0–4095: 1024 location entries, big-endian, index `(x & 31) + (z & 31) * 32`. Each entry is a 3-byte
  sector offset and a 1-byte sector count (sectors are 4096 bytes). `0` = chunk absent.
- Bytes 4096–8191: 1024 big-endian timestamps (ignored).
- At the chunk's sector: `i32` BE length (counting the compression byte), compression byte, payload.
  Compression: `1` gzip, `2` zlib, `3` uncompressed, `4` LZ4 (1.20.5+ option), `127` custom (server
  plugins). `+128` means the payload is in `c.<x>.<z>.mcc` next to the region file.
- LZ4 and custom compression are refused per chunk with `UnsupportedFormat`; the chunk is counted under
  "chunks could not be read" in the summary, and conversion continues.

Region coordinates come from the file name. An empty (0-byte) region file has no chunks; a non-empty file
shorter than the two header tables is malformed.

## Chunk NBT (1.18+)

From [2], cross-checked against the test world written by nbtlib (`tests/data/generate_test_data.py`):

| Tag | Meaning |
|---|---|
| `DataVersion` | Int |
| `xPos`, `zPos`, `yPos` | chunk coordinates; `yPos` = lowest section Y |
| `Status` | generation status; only `minecraft:full` (or `full`) chunks are converted |
| `sections` | list of `{Y: Byte, block_states, biomes}` |
| `block_entities` | list of compounds with `id` and **absolute** `x`, `y`, `z` |

`block_states`: `palette` and `data` (long array). Palette entries are written by Java 26.3's
`BlockState.CODEC`, a "constant or dispatch" codec (read from the 26.3 server): a **bare block-name string** for a
block without properties, otherwise `{id, properties}` (the dispatch field is `id`, from `StateHolder.codec`).
Older versions write `{Name, Properties}` [2]. All three forms are read. If the palette has one
entry, `data` is absent and the section is uniform. Otherwise each index uses
`max(4, ceil(log2(palette size)))` bits; `64 / bits` indices are packed per long, LSB first, and **never span
two longs** (the 1.16+ rule). Index order `y * 256 + z * 16 + x`. Palette entries spelled `{id, properties}`
(as in 26.3 structure files, `structure-nbt.md`) and bare strings are accepted too.

`biomes`: `palette` (list of strings) and `data`. 4 × 4 × 4 cells per section: `ceil(log2(size))` bits
(0 bits and no `data` for a single entry), same packing, index `y * 16 + z * 4 + x` in cells. Bridger expands
the cells to per-block biomes.

Chunks with a `Level` compound are read as described under "Old worlds"; a chunk with a DataVersion below 2860
and no `Level` compound is malformed.

The column becomes a 16 × height × 16 Java structure whose local (0,0,0) is world `(x*16, yPos*16, z*16)`.
Sections not listed are air. `keepPacked` and the position tags are dropped from block entities.

## Old worlds

Every layout since McRegion is read (`chunk_codec.cpp`, `java_world.cpp`; data and rules in
`java/java_upgrade.cpp` and `java/legacy_blocks.cpp`). The layout is recognised from the chunk itself, not from
version numbers: a `Level` compound means pre-1.18; inside it a section `Palette` means 1.13 – 1.17, a section
`Blocks` array means 1.2 – 1.12, and `Level.Blocks` (no sections) means McRegion.

| Versions | Files | Chunk | Blocks | Biomes |
|---|---|---|---|---|
| Alpha (before Beta 1.3) | `<base36>/<base36>/c.<x>.<z>.dat` | — | — | refused (`UnsupportedVersion`) |
| Beta 1.3 – 1.1 | `region/r.<x>.<z>.mcr` (same container as `.mca`) | `Level` | `Level.Blocks` (32 768) + `Data`, index `x*2048 + z*128 + y`, 128 high | none (plains) |
| 1.2 – 1.12 | `region/*.mca` | `Level` | per section `Blocks` (4096) + `Data` + optional `Add`, index `y*256 + z*16 + x` | `Biomes` 256 bytes |
| 1.13 – 1.14 | `region/*.mca` | `Level` | per section `Palette` + `BlockStates` | `Biomes` 256 ints |
| 1.15 – 1.17 | `region/*.mca` | `Level` | same | `Biomes` 1024 ints |
| 1.18 + | `region/*.mca` (26.1+: `dimensions/…`) | root | `sections[].block_states` | `sections[].biomes` |

Sources: [1][2][5]; McRegion and Anvil index orders [6]; nibble order, the 1.13 packing and the biome arrays
from Chunker's Java readers [7] (`JavaChunkReader`, `PaletteUtil`, `v1_13`/`v1_15` `ColumnReader`), checked with
worlds Chunker wrote (below).

- **Nibble arrays** (`Data`, `Add`): the value of an even index is in the low four bits; id = `Blocks[i] |
  Add[i] << 8`.
- **`BlockStates` before 20w17a** (1.13 – 1.15) is one run of `bits`-bit fields that **spans** long boundaries;
  from 20w17a (1.16) entries never span. Bits are `max(4, ceil(log2(palette size)))` in both. The two are told
  apart by the array length (`4096 * bits / 64` vs `ceil(4096 / floor(64 / bits))`); for 4, 8 and 16 bits they
  are the same array.
- **Biomes**: 1024 ints are 4 × 4 × 4 cells, index `(y/4)*16 + (z/4)*4 + x/4` for y 0–255; 256 entries are
  per column, index `z*16 + x`. Ids are the numeric biome ids of 1.17 (PrismarineJS minecraft-data
  `pc/1.17/biomes.json` [8]); 1.18 renamed eleven of them (`mountains` → `windswept_hills`, …) [5] and removed 26
  variants without naming a successor; Bridger merges each into its base biome (`desert_hills` → `desert`, …)
  and warns. `-1`/`255` (not generated) read as plains, as the game fills them.
- **Generation status**: `Level.Status` `full` (1.14+), `postprocessed`/`fullchunk` (1.13); without `Status`,
  `TerrainPopulated` (absent = populated). Only finished chunks are converted, as for 1.18+.
- **Sections** with `Y` outside 0–15 (1.14+ keeps light-only sections at −1 and 16) are skipped. Columns are 256
  (McRegion: 128) high from y 0.
- **McRegion**: when a world has `.mca` files its `.mcr` files are ignored (1.2 left them next to the converted
  ones).

**Numeric ids (before 1.13)** go through the same legacy table as `.schematic` files
(`docs/formats/legacy-schematic.md`: minecraft-data `legacy.json` plus Bridger's renames, with the same
nearest-data-value fallback). 1.13 moved some state out of block entities or across blocks; Bridger restores it
after reading the column (`fix_legacy_blocks`), after the rules of the 1.13 flattening as Chunker documents them
[7] (`JavaLegacy*PreTransformHandler`, `JavaLegacy*BlockEntityHandler`):

| Block | 1.12 storage | Restored |
|---|---|---|
| doors | lower half: facing, open; upper half: hinge, powered | both halves get all four |
| tall plants (sunflower … peony) | upper half: data 8, no plant | the plant of the lower half |
| beds | block entity `color` (dye index) | `<colour>_bed` |
| flower pots | block entity `Item` (string; int before 1.8) + `Data` | `potted_<plant>`; the block entity is dropped |
| skulls | block entity `SkullType` (0–5) + `Rot` | `<type>_skull`/`_head` (wall variants keep facing) |
| banners | block entity `Base` (dye damage: 0 black … 15 white) | `<colour>_banner` |
| note blocks | block entity `note` | `note_block[note]`; the block entity is dropped |

Pre-1.11 block entity ids (`Chest`, `Trap`, `MobSpawner`, …) are namespaced, and item stacks with numeric ids
are renamed with the legacy item table (as for `.schematic`).

**State renames since 1.13** apply to every palette (old chunks, 1.18+ chunks, and `.schem`/`.nbt`/`.litematic`
files). They were found by comparing the block lists of every release from 1.13 to 26.1 (minecraft-data [8])
with the pinned 26.3 report; each is safe for any DataVersion because the old name or value no longer exists,
except `stone_slab`, which is renamed only below DataVersion 1902 (18w43a):

| Change | Version |
|---|---|
| `sign` → `oak_sign`, `wall_sign` → `oak_wall_sign`; `stone_slab` → `smooth_stone_slab` | 1.14 (19w02a, 18w43a) |
| wall sides `true`/`false` → `low`/`none` | 1.16 (20w06a) |
| `jigsaw[facing]` → `orientation` (`<f>_up`, `up_north`, `down_north`) | 1.16 (20w13a) |
| `grass_path` → `dirt_path`; `cauldron[level=1..3]` → `water_cauldron`, `level=0` dropped | 1.17 (20w45a) |
| `grass` → `short_grass` | 1.20.3 (23w40a) |
| `creaking_heart[active]` / `[creaking]` → `creaking_heart_state` | 1.21.5 (25w02a) |
| `chain` → `iron_chain` | 1.21.9 |

Properties added since a block's version take their 26.3 default (`MappingData::find_java_state`).

**Below y 0.** Pre-1.18 columns start at y 0. Converted chunks have nothing below it (as in the source), while
chunks the source did not have are generated by the target game with today's terrain, which does not match
pre-1.18 terrain at the edges. (The games themselves regenerate below y 0 when they upgrade an old world; that
is not reproduced.)

**Checked:** worlds written by Chunker [7] as `JAVA_1_8_8`, `JAVA_1_12_2` and `JAVA_1_16_5` from a 26.3 world
convert with every chunk read and no unknown block; a 3D render of the 1.12 world matches the original except
for blocks 1.12 does not have. Generated test worlds (`tests/data/javaworld/v1_16`, `v1_12`, `mcregion`, `alpha`,
written by nbtlib) cover both packings, each biome array, every restored state and the refusal of Alpha worlds.

## Conversion to `.mcworld`

1. Every region is processed in batches of 128 chunk slots. Worker threads (`--threads`, default all cores)
   each read, decompress, decode, translate (`translate_to_bedrock`) and encode one column into LevelDB
   key/value pairs, with their own registries and conversion report. The main thread then stores the batch
   in slot order with one LevelDB write batch per column. Output does not depend on the thread count (test:
   `World conversion output does not depend on the thread count`).
2. The world has **no flat layers**: `level.dat` uses the infinite generator (`Generator` 1) and the Java
   seed, so chunks the Java world never generated (and partially generated ones, which are skipped) are
   generated by the game. Java and Bedrock share terrain generation for a given seed since 1.18 only
   approximately; the seams this can cause at the edge of converted terrain are expected.
3. Spawn comes from `level.dat` (`SpawnX/Y/Z`, or `spawn.pos` in newer versions), else (0, 64, 0).
4. The database is flushed (no full compaction), validated in place (every subchunk and block entity list
   decodes, counts match), zipped straight to a temp file next to the output, the zip listing is checked,
   and the temp file is moved into place atomically (`docs/safety.md`).

Biomes are translated per block. Blocks outside the Bedrock Overworld height (−64..319) are dropped and
counted in a warning.

## Writing (Bedrock world → Java world)

Code: `src/formats/java/anvil/anvil_writer.cpp`, pipeline `convert_bedrock_world`.

- **Layout:** the Java 26.1+ folder layout: `level.dat`, `data/minecraft/world_gen_settings.dat`,
  `dimensions/minecraft/overworld/region/r.<x>.<z>.mca`.
- **Chunks** (DataVersion 5023): `xPos`, `yPos` (−4), `zPos`, `Status` `minecraft:full`, `LastUpdate` 0,
  `InhabitedTime` 0, `isLightOn` 0 (no light data is written, so the game computes it), 24 `sections` (Y −4..19),
  each with `block_states` (`{id, properties}` palette; `data` packed with `max(4, bits)` bits, no spanning, omitted
  for one entry) and `biomes` (4 × 4 × 4 cells: the biome at each cell's lowest corner; `ceil(log2)` bits),
  `block_entities` (absolute `x/y/z`, `id`, `keepPacked` 0), and an empty `structures` compound. Heightmaps are not
  written; whether the game rebuilds them for full chunks is part of the in-game check.
- **Region files:** chunks zlib-compressed (type 2), padded to 4 KiB sectors in slot order; a payload over 255
  sectors goes to `c.<x>.<z>.mcc` with type `2 + 128` [1].
- **level.dat:** `Data` with `DataVersion`, `version` 19133, `Version {Id, Name, Series "main", Snapshot 0}`,
  `LevelName`, `LastPlayed`, `GameType` 1 (creative), `allowCommands` 1, `initialized` 1, `Time` 0,
  `spawn {dimension, pos (Int[3]), yaw (Float), pitch (Float)}` and `DataPacks {Enabled ["vanilla"], Disabled []}`.
  The minecraft.wiki table lists `spawn.yaw/pitch` as Int, but the 26.3 server's `LevelData.RespawnData` record
  holds floats; Bridger follows the game.
- **world_gen_settings.dat:** `{DataVersion, data: {seed, generate_structures 1, bonus_chest 0}}` with the Bedrock
  world's seed. `dimensions` is not written. The 26.3 server falls back to default settings with a **random seed**
  when it cannot read this file; whether it accepts the file without `dimensions` (and so keeps the seed) is an
  open in-game check. Either way, chunks the Bedrock world did not have are generated by Java.
- **Validation before the move:** the folder is re-opened as a Java world and every chunk is read back and decoded
  (count compared with what was written). The world is built in a temp folder **next to the destination** and
  renamed into place; an existing destination folder is never replaced (`docs/safety.md`).
- Independent check: `tests/integration/validate_java_output.py` (nbtlib) re-reads level.dat and every chunk.

## Limits we enforce on input

- Decompression and NBT limits as everywhere (`io::DecompressLimits`, `io::nbt::NbtLimits`).
- Section Y range ≤ 256 sections; palettes ≤ 4096 block entries, ≤ 64 biome entries; packed arrays must be
  long enough; every index must be inside its palette.
- Location entries must point inside the file and after the header; payload lengths must fit their sectors.

## Open questions

- Written worlds open in Java 26.3 with correct blocks, light and chests: **passed** in the 2026-10-12 in-game run
  (`docs/testing.md`), including the seed check of the checklist.
- Java 26.3 world with real terrain: not yet tested (needs a world saved by the game; the Mojang server
  can generate one but running it requires accepting the EULA, which is the user's decision).
- In-game check of a converted world in Bedrock 26.52: **passed** (2026-10-12).

## Sources

1. minecraft.wiki, "Region file format" — <https://minecraft.wiki/w/Region_file_format>
2. minecraft.wiki, "Chunk format" — <https://minecraft.wiki/w/Chunk_format>
3. minecraft.wiki, "Java Edition level format" — <https://minecraft.wiki/w/Java_Edition_level_format>
4. minecraft.wiki, "Java Edition 26.1 Snapshot 6" (save-format changes) — <https://minecraft.wiki/w/Java_Edition_26.1_Snapshot_6>
5. minecraft.wiki, "Java Edition 1.18" (biome renames and removals) and "Data version" — <https://minecraft.wiki/w/Java_Edition_1.18>,
   <https://minecraft.wiki/w/Data_version> (checked 2026-10-15)
6. minecraft.wiki, "Anvil file format" (McRegion/Anvil index order, `Add`) — <https://minecraft.wiki/w/Anvil_file_format>
7. HiveGamesOSS/Chunker (MIT), branch `main` (checked 2026-10-15), consulted only, no code copied:
   `java/base/reader/JavaChunkReader.java`, `java/util/PaletteUtil.java`, `java/v1_13/reader/{ChunkReader,ColumnReader}.java`,
   `java/v1_15/reader/ColumnReader.java`, `java/base/reader/pretransform/legacy/handlers/*`,
   `java/base/resolver/blockentity/legacy/handlers/*`. The Chunker CLI 1.21.0 wrote the old test worlds.
8. PrismarineJS/minecraft-data (MIT): `data/pc/<version>/blocks.json` (1.13 – 26.1) and `data/pc/1.17/biomes.json`
   (commit `21951b85`) — <https://github.com/PrismarineJS/minecraft-data>
