# Java world (Anvil)

Editions/versions covered: Java Edition worlds saved by **1.18 or newer** (chunk DataVersion ≥ 2860), read
for the Overworld. Pinned target: Java 26.3 (DataVersion 5023, `docs/versions.md`). Older chunk layouts are
refused with a message asking the user to open and save the world in a newer version first.

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
| `dimensions/minecraft/overworld/region/r.<x>.<z>.mca` | `region/` | Overworld chunks, 32 × 32 per file | read |
| `…/region/c.<x>.<z>.mcc` | same | oversized chunk payload (absolute chunk coordinates) | read |
| `dimensions/minecraft/overworld/entities/` | `entities/` | entities (1.17+) | not converted — reported |
| `dimensions/minecraft/the_nether`, `…/the_end` | `DIM-1`, `DIM1` | Nether, End | not converted — reported |

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

Pre-1.18 chunks (a `Level` compound, or DataVersion < 2860) return `UnsupportedVersion`.
(The old "Palette entries spelled `{id, properties}`" remark below refers to the same codec.)

The column becomes a 16 × height × 16 Java structure whose local (0,0,0) is world `(x*16, yPos*16, z*16)`.
Sections not listed are air. `keepPacked` and the position tags are dropped from block entities.

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
