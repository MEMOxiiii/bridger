# MCEdit legacy schematic (`.schematic`)

Editions/versions covered: the pre-1.13 numeric-id format written by MCEdit, WorldEdit and Schematica
(`Materials` = `Alpha`). Read only. Code: `src/formats/java/legacy_schematic.cpp`.

## Container

Gzip-compressed big-endian NBT [1].

## Structure

| Tag | Meaning |
|---|---|
| `Width`, `Height`, `Length` | Shorts |
| `Materials` | must be `Alpha` (`Classic`/`Pocket` are refused) |
| `Blocks` | byte array of block ids (low 8 bits) |
| `AddBlocks` | optional nibble array: high 4 bits of the id; even index = high nibble of the byte |
| `Data` | byte array of data values (low 4 bits used) |
| `TileEntities`, `Entities` | lists |
| `Add` | an older, conflicting spelling — refused rather than guessed |

Index order `(y * Length + z) * Width + x`.

## Block data

`(id, data)` pairs are flattened to 1.13+ block states with the table from minecraft-data [2] (MIT), pinned by
commit and checksum in `tools/mapgen/fetch_sources.py` and embedded in the mapping blob. Names that changed
after 1.13 are renamed by `tools/mapgen/overrides/legacy_renames.json` (e.g. `grass` → `short_grass`,
`sign` → `oak_sign`; cauldron levels → `water_cauldron`). Pairs missing from the table fall back to the
same id with data `& 7` (drops the "upper half"/"powered" bit), then data 0 (counted in a warning); unknown ids
are reported as unmapped. `WEOffsetX/Y/Z` (WorldEdit) become the paste offset.

## Block entities

Pre-1.11 ids (`Chest`, `Sign`, …) are normalized to namespaced ids. Item stacks
`{id: Short|String, Count: Byte, Damage: Short}` become `{id: String, count: Int}`; numeric ids go through the
legacy item table; string ids with a non-zero damage cannot be flattened without a name table and are kept
with a warning; item `tag` data is reported as not converted. The state that 1.13 moved out of block entities or
across two blocks (door halves, tall plants, bed colours, flower pot contents, skull types and rotation, banner
base colours, note block pitches) is restored as for old Java worlds (`docs/formats/anvil.md`, "Old worlds";
shared code in `src/formats/java/legacy_blocks.cpp`). Banner patterns stay in the block entity.

## Limits we enforce on input

`Blocks` and `Data` lengths must equal the volume, `AddBlocks` must hold at least half of it; volume ≤ `max_volume`.

## Sources

1. minecraft.wiki, "Schematic file format" — <https://minecraft.wiki/w/Schematic_file_format>
2. PrismarineJS minecraft-data, `data/pc/common/legacy.json` — <https://github.com/PrismarineJS/minecraft-data>
   (MIT), commit `dd01127457231e7da629d645b3c6738068018eb0`.
