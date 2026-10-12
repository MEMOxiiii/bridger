# Litematica schematic (`.litematic`)

Editions/versions covered: Litematica format versions 2–7 (`Version` tag). Read only.
Code: `src/formats/java/litematic.cpp`.

There is no written specification. The layout below comes from reading Litematica's source for behavior
only [1] (LGPL-3.0; no code was copied), and every detail is exercised by a file written with nbtlib
(`tests/data/generate_test_data.py`, `generate_litematic`).

## Container

Gzip-compressed big-endian NBT.

## Structure

| Tag | Meaning |
|---|---|
| `Version`, `SubVersion` | format version (2–7 accepted; 1 stored block entities differently) |
| `MinecraftDataVersion` | Java DataVersion |
| `Metadata` | `Name`, `Author`, `TimeCreated` (ms), `EnclosingSize`, … — kept verbatim in the structure metadata |
| `Regions` | compound of named regions |

Each region: `Position {x,y,z}`, `Size {x,y,z}`, `BlockStatePalette`, `BlockStates`, `TileEntities`,
`Entities`, `PendingBlockTicks` (ignored).

**Negative sizes** are legal: a region spans from `Position` to `Position + Size − sign(Size)` on each axis,
so the minimum corner is `min(Position, Position + Size ∓ 1)`. A zero size is malformed.

Several regions are merged into one structure covering their bounding box (a warning says how many).

## Block data

Palette: `{Name, Properties}`. `BlockStates` packs `max(2, ceil(log2(palette size)))` bits per index, LSB
first, **and an index may span two longs** (unlike Anvil's 1.16+ packing). Index order
`(y * sizeZ + z) * sizeX + x` within the region (absolute sizes).

## Block entities / entities

Block entity `x/y/z` are relative to the region's minimum corner. Entity `Pos` is relative to the region too;
entities without a valid `id` or a `Pos` list of 3 doubles are skipped with a warning.

## Limits we enforce on input

Volume and palette limits as for other structures; `BlockStates` must hold `volume × bits` bits; every index
must be inside the palette.

## Sources

1. Litematica (sakura-ryoko fork, branch LTS/26.2), `LitematicaSchematic`, `LitematicaBlockStateContainer`,
   `LitematicaBitArray` — <https://github.com/sakura-ryoko/litematica> (LGPL-3.0, consulted for behavior).
