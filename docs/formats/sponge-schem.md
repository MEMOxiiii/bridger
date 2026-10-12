# Sponge schematic (`.schem`)

Versions covered: **1, 2 and 3** (reading), **3** (writing).
Implementation: `src/formats/java/sponge_schem.cpp`.

## Container

GZip-compressed NBT, big-endian (Java). Recommended extension `.schem`. Field names are case-sensitive [1][2][3].
Bridger also accepts uncompressed NBT. Zlib or raw deflate is rejected as `UnsupportedFormat`.

## Root

- **v3:** the `Schematic` compound is nested in an unnamed root compound: `{"": {"Schematic": {...}}}` [3].
- **v1/v2:** the specification calls the Schematic object "the root object" without naming the tag [1][2].
  WorldEdit writes a root named `Schematic` with the fields directly inside [4].

Bridger uses the `Schematic` child compound if one exists, otherwise the root itself. It then decides by the
required `Version` field (1–3; anything else is `UnsupportedVersion`).

## Fields

| Field | v1 | v2 | v3 | Type | Notes |
|---|---|---|---|---|---|
| `Version` | req | req | req | Int | |
| `DataVersion` | — | req | req | Int | Minecraft data version. A missing value in v2/v3 is a **warning**, not an error |
| `Metadata` | opt | opt | opt | Compound | `Name`, `Author` (String), `Date` (Long, ms since epoch), `RequiredMods` (List of String) [1][2][3] |
| `Width`/`Height`/`Length` | req | req | req | Short | "unsigned short", read as `value & 0xFFFF` [3] |
| `Offset` | opt | opt | opt | Int[3] | default `[0,0,0]` |
| `PaletteMax` | opt | opt | — | Int | palette size; a mismatch is a **warning** (WorldEdit throws [5]) |
| `Palette` | opt* | opt* | in `Blocks` | Compound String→Int | *"highly recommended" in v1/v2; files without one use global ids, which Bridger rejects (`UnsupportedFormat`) |
| `BlockData` / `Blocks.Data` | req | req | req | Byte array of varints | `Width*Height*Length` entries |
| `TileEntities` | opt | — | — | List of Compound | v1 name |
| `BlockEntities` | — | opt | in `Blocks` | List of Compound | |
| `Entities` | — | opt | opt | List of Compound | |
| `BiomePalette` + `BiomeData` (+ `BiomePaletteMax`) | — | opt | — | | `Width*Length` entries, one per column [2] |
| `Biomes` {`Palette`, `Data`} | — | — | opt | | `Width*Height*Length` entries, one per block [3] |

## Block data

- Each entry is a **varint** (LEB128: 7 bits per byte, low group first, high bit = continuation) indexing the
  palette [1][2][3].
- Index order: `x + z*Width + y*Width*Length` (x fastest, then z, then y) [1][2][3].
- Palette keys are block-state strings `namespace:id[key=value,...]`. v1/v2 say properties are alphabetical, v3
  says their order is "nondeterministic" [1][2][3]. Bridger parses and **canonicalizes** them (properties sorted),
  so both forms intern to the same state. A missing namespace means `minecraft:`.
- Palette indices are "recommended" to start at 0 with no gaps, which is not required. Bridger accepts sparse
  indices. A palette index used twice is `Malformed`.
- The number of entries must equal `Width*Height*Length` **exactly**. Too few, too many, an index not in the
  palette, or a varint wider than 32 bits are all `Malformed`. WorldEdit does not check any of these [5]; Bridger
  never guesses.

## Block entities and entities

| | v1 | v2 | v3 |
|---|---|---|---|
| Position | `Pos` Int[3] | `Pos` Int[3] | `Pos` Int[3] |
| Type | `Id` | `Id` | `Id` |
| Type-specific data | flattened next to `Pos`/`Id` (+ `ContentVersion`) | flattened (WorldEdit [5]); spec text says `Extra` [2] | `Data` compound [3][4] |

- **v2 discrepancy:** the v2 text describes an `Extra` compound [2], but WorldEdit, the main producer of these
  files, reads and writes the fields next to `Pos`/`Id` [4][5]. Bridger accepts both: the data is every entry
  except `Pos`/`Id`/`ContentVersion`, and if the only remaining entry is an `Extra` compound, its contents are
  used.
- Positions are relative to the schematic origin, **without** `Offset` [1][2][3]. `Pos` must have exactly 3
  values (`Malformed` otherwise).
- A block entity outside the volume is **not imported**, and a warning names it. It is never silently dropped.
- Entity `Pos` is a List of 3 Doubles [2][3].
- Type-specific data is preserved verbatim as big-endian NBT (`RawNbt`). Position keys inside it are not
  trusted: writers regenerate positions (ADR 0001).

## WorldEdit metadata

Read and shown by `bridger info`, not used for conversion:
- v1/v2: `Metadata.WEOffsetX/Y/Z`, which give the clipboard origin relative to the minimum corner [5].
- v3: `Metadata.WorldEdit.Origin` (Int[3]) [4].

## Limits we enforce on input

- Decompressed size: 1 GiB (`SchemReadOptions::decompress`).
- Volume `Width*Height*Length`: 2^31 blocks. Checked **before** allocating; the data array needs at least
  1 byte per block anyway.
- Palette index: below 2^20.
- NBT limits from `docs/formats/nbt.md`.
- Decoding runs one 16-block-high slab at a time (memory ≈ `Width*Length*16*4` bytes) into IR sections, with no
  per-block heap allocation.

## Writing (version 3)

Code: `src/formats/java/java_writers.cpp`. `{"": {Schematic: {...}}}`, gzip-compressed [3]: `Version` 3,
`DataVersion` (the pinned 5023 for translated content; a Java source keeps its own), `Width/Height/Length`
(at most 65535, else `LimitExceeded`), `Offset`, `Metadata {Name, Author, Date}`, `Blocks {Palette, Data,
BlockEntities [{Pos, Id, Data}]}`, `Biomes {Palette, Data}` (per block, when the structure has biomes) and an
empty `Entities` list. Palette indices start at 0 in order of first use. The file is re-read with Bridger's reader
and compared block by block before it is moved into place; `tests/integration/validate_java_output.py` checks it
with nbtlib.

## Open questions

- Real-world files from tools other than WorldEdit (e.g. FAWE, Amulet) have not been tested yet. The golden files
  are generated by `tests/data/generate_test_data.py`. For that reason the read status stays **in progress**
  until a set of real `.schem` files can be committed or tested locally.

## Sources

1. SpongePowered, Schematic Specification, version 1:
   https://github.com/SpongePowered/Schematic-Specification/blob/master/versions/schematic-1.md
2. SpongePowered, Schematic Specification, version 2:
   https://github.com/SpongePowered/Schematic-Specification/blob/master/versions/schematic-2.md
3. SpongePowered, Schematic Specification, version 3:
   https://github.com/SpongePowered/Schematic-Specification/blob/master/versions/schematic-3.md
4. EngineHub WorldEdit, `SpongeSchematicV3Writer.java` (GPL-3.0; behavior consulted, no code copied):
   https://github.com/EngineHub/WorldEdit/blob/master/worldedit-core/src/main/java/com/sk89q/worldedit/extent/clipboard/io/sponge/SpongeSchematicV3Writer.java
5. EngineHub WorldEdit, `SpongeSchematicV1Reader.java`, `SpongeSchematicV2Reader.java`, `ReaderUtil.java` (GPL-3.0;
   behavior consulted, no code copied): same directory as [4]

(All checked 2026-10-11.)
