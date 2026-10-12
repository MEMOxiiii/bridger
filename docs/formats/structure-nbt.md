# Java structure file (`.nbt`)

Editions/versions covered: files written by structure blocks and shipped in the game's data (Java 1.13+;
tested on Java 26.3 vanilla files). Read only. Code: `src/formats/java/structure_nbt.cpp`.

## Container

Gzip-compressed big-endian NBT [1]; raw (uncompressed) NBT is accepted as well.

## Structure

| Tag | Meaning |
|---|---|
| `DataVersion` | Int |
| `size` | list of 3 Ints (x, y, z) |
| `palette` | list of block states, or |
| `palettes` | list of palettes (random variants, e.g. shipwrecks): the first is used and a warning lists the count |
| `blocks` | list of `{state: Int, pos: [x,y,z], nbt?: Compound}` |
| `entities` | list of `{pos: [3 Doubles], blockPos, nbt: Compound}` |

Positions not listed in `blocks` are **structure void**: they are kept as air in Bridger's structure (the
writer side has no void concept yet; `--air keep` gives the same result when placing).

## Block data

Palette entries are `{Name, Properties}` per [1]. **Mojang's own 26.3 files use `{id, properties}`**
(found when all 1511 vanilla structure files from the 26.3 jar failed to read with the documented spelling);
both are accepted (`nbt_helpers.hpp`, `state_from_compound`). Names without a namespace get `minecraft:`.

## Block entities / entities

`nbt` on a block entry is the block entity (its own `id`; the position comes from `pos`). Entities keep their
`nbt` minus `id`, `Pos` and `UUID`.

## Writing

Code: `src/formats/java/java_writers.cpp`. Gzip-compressed: `DataVersion`, `size`, one `palette` of
`{id, properties}` (the spelling `NbtUtils.writeBlockState` uses in the 26.3 server), `blocks` listing **every**
position (air included, so a structure block reproduces the volume exactly) with `nbt` = block entity (`id` +
data), and an empty `entities` list. Validated by re-reading and comparing every block.

## Limits we enforce on input

Volume ≤ `max_volume`, palette ≤ `max_palette_size`, state indices inside the palette; blocks outside `size`
are skipped with a warning.

## Verification

All 1511 structure files in the Java 26.3 server jar (`data/minecraft/structure/**`) read and convert to
`.mcstructure` (run locally, 2026-10-12; the jar is not redistributed).

## Sources

1. minecraft.wiki, "Structure file" — <https://minecraft.wiki/w/Structure_file>
