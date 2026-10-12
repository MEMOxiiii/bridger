# Bedrock structure file (`.mcstructure`)

Editions/versions covered: `format_version` 1, as written by Bedrock structure blocks. Written for the pinned
Bedrock 26.50 palette; read with the same palette. Code: `src/formats/bedrock/mcstructure.cpp`.

## Container

**Uncompressed little-endian** NBT with an empty root name [1][2].

## Structure

| Tag | Meaning |
|---|---|
| `format_version` | Int, 1 |
| `size` | list of 3 Ints |
| `structure_world_origin` | list of 3 Ints: where the structure was saved; entity positions are absolute and are made relative with it |
| `structure.block_indices` | list of **two** Int lists (layer 1 blocks, layer 2 = waterlogging liquid), one index per block, `-1` = structure void |
| `structure.entities` | list of entity compounds |
| `structure.palette.default.block_palette` | list of `{name, states, version}` |
| `structure.palette.default.block_position_data` | compound keyed by the block's index (decimal string) → `{block_entity_data: {...}}` |

Block index order is **z fastest, then y, then x**: `index = (x * sizeY + y) * sizeZ + z` [1].

## Writing

- Layer 2 is written as `-1` everywhere except where the structure has a liquid on the secondary layer.
- `version` on every palette entry is the pinned palette's block version.
- Block entities are written with structure-local `x/y/z`; the game replaces positions on load [1].
  `pairx`/`pairz` (double chests) are written structure-local the same way.
- Entities are not written yet; they are reported by translation.

## Reading

Block entity positions are dropped (the block's index gives its position); `pairx`/`pairz`, which the game saves in
world coordinates like `x/y/z`, are made structure-local by subtracting `structure_world_origin`. Palette entries are matched against the pinned palette; unknown entries become air and each distinct name is
reported. Structure-void positions are read as air and counted. More than two layers: the extra layers are
ignored with a warning.

## Limits we enforce on input

Volume ≤ `max_volume`; layer lengths must equal the volume; indices must be inside the palette or `-1`.

## Open questions

- In-game check of written files in Bedrock 26.52 (load with a structure block): pending.

## Sources

1. Bedrock Wiki, "Structure files" — <https://wiki.bedrock.dev/nbt/mcstructure>
2. minecraft.wiki, "Structure file" (Bedrock Edition section) — <https://minecraft.wiki/w/Structure_file>
