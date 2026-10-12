# Bedrock world (`.mcworld`)

Version covered: **Bedrock 26.50 / 26.52** (writing new worlds). Implementation: `src/formats/bedrock/mcworld/`.
Values that only the game can confirm are marked **⚠ verify in-game** and are covered by the in-game checklist in
`docs/testing.md`.

## Container

A `.mcworld` is a zip of the world folder: `level.dat`, `levelname.txt` and `db/` (a LevelDB database) at the zip
root [1][3]. Bridger zips with deflate and fixed timestamps (1980-01-01), so the output is reproducible.

## LevelDB

Mojang's LevelDB fork adds zlib compression [1]. Bridger writes with **raw deflate** (compressor id 4) and registers
zlib (id 2) for reading, the same setup as Mojang's `mcpe_sample_setup.cpp` in leveldb-mcpe [4]
(`docs/adr/0004-leveldb-mcpe-fork.md`).

## LevelDB keys [1]

`x` (i32 LE, chunk coordinate), `z` (i32 LE), dimension (i32 LE; **omitted for the Overworld**, 1 = Nether,
2 = End), tag (u8), and for `SubChunkPrefix` only, the subchunk y index (i8, block y >> 4).

| Tag | Name | Bridger writes |
|---|---|---|
| `0x2B` | Data3D | heightmap + 3D biomes (below) |
| `0x2C` | Version | 1 byte: **42** for 26.50, from Chunker's writer chain (`v1_21_120` override, inherited through `v1_26_50`) [3] |
| `0x2F` | SubChunkPrefix | one per non-empty subchunk |
| `0x31` | BlockEntity | concatenated little-endian NBT roots [1] |
| `0x36` | FinalizedState | i32 LE **2** ⚠ verify in-game (Chunker does not write it; the value 2, "done", keeps the game from populating our chunks) |

Entities (`actorprefix`/`digp` storage [1]) are **not written yet**: every entity is reported.

## Subchunks (`SubChunkPrefix`)

Version **9** [1][3]:

```
u8 version (9) | u8 layer count | i8 subchunk y
per layer:
  u8  bits_per_index << 1 | flag      flag 0 = persistent NBT palette (stored worlds) [3]
  u32 LE words, ceil(4096 / floor(32 / bits)) of them; indices packed from the lowest bits,
      floor(32/bits) per word, the rest padding [1]
  i32 LE palette count
  palette entries: little-endian NBT roots {name: String, states: Compound, version: Int}
```

- Allowed bit widths: **1, 2, 3, 4, 5, 6, 8, 16**. The smallest width with `count <= 2^bits` is used; a single
  entry uses 1 bit [3].
- Index order is **XZY with y fastest**: `i = x*256 + z*16 + y` [1][3]. Bridger's IR uses `y*256 + z*16 + x`;
  the codec converts between them.
- **Layer 0** holds blocks, **layer 1** holds the liquid at the same position (waterlogging). A second layer is
  written only when a subchunk has liquids [3].
- `states` value types must match the game's palette exactly (Byte for `*_bit` properties, Int, String). Bridger
  takes names, typed states and the block `version` (**18168865** for 26.50) from the Bedrock block palette of the
  mapping data (`docs/adr/0007-mapping-data-sources.md`).

## Data3D

```
256 x i16 LE heightmap, z-major (index z*16 + x)
24 biome storages, one per subchunk from y -64 up (Overworld) [3]:
  u8 bits << 1 | 1 (runtime palette), packed words as for blocks, i32 LE palette count, i32 LE biome ids
```

- The biome storage layout, the runtime flag and the 24 storages follow Chunker's writer [3]. Biome ids are Bedrock
  numeric ids (GeyserMC `biomes.json`: plains = 1) [5].
- **Heightmap value:** Bridger stores `highest non-air y - (-64) + 1` (0 for an empty column). The sources disagree
  on whether this is relative to the world bottom (Chunker writes absolute values [3]). The game rebuilds lighting
  data, so a wrong value is unlikely to matter. ⚠ verify in-game against a world created by the game itself.

## Block entities

Stored per chunk under tag `0x31` as concatenated little-endian NBT roots [1]. Each one gets `x`, `y`, `z`
(absolute Int) and `isMovable` (Byte 1) from the writer [3]. Translated content:

| Java id | Bedrock id [3] | Fields |
|---|---|---|
| `chest`, `trapped_chest` | `Chest` (Bedrock has no separate trapped-chest id; Chunker refuses to write `TrappedChest` [3]) | `Items`, `CustomName`, pairing |
| `barrel`, `furnace`, `blast_furnace`, `smoker`, `hopper`, `dispenser`, `dropper`, `shulker_box` | `Barrel`, `Furnace`, `BlastFurnace`, `Smoker`, `Hopper`, `Dispenser`, `Dropper`, `ShulkerBox` | `Items`, `CustomName` (slot layouts are identical) |
| `ender_chest`, `enchanting_table` | `EnderChest`, `EnchantTable` | `CustomName` |
| `bed` | `Bed` | `color` (Byte, dye order white=0 … black=15) [3] |
| `sign`, `hanging_sign` | `Sign`, `HangingSign` | `FrontText`/`BackText` {`Text`, `SignTextColor`, `IgnoreLighting`, `TextIgnoreLegacyBugResolved`}, `IsWaxed` [3] |

Anything else is reported as an unmapped block entity (the block itself is still placed).

- **Double chests:** Bedrock pairs chests through `pairx`/`pairz` (absolute) and `pairlead` (Byte 1 on one half).
  The partner position comes from the Java `facing` and `type`: facing east → left half's partner at z+1, west →
  z−1, south → x−1, north → x+1 (the right half the opposite) [6]. Bridger sets `pairlead` on the **right** half.
  ⚠ verify in-game that both halves open as one chest.
- **Items** (`Items` list): `Name` (Bedrock id from GeyserMC `items.json`), `Count` (Byte), `Damage` (Short, from
  the mapping's `bedrock_data`), `Slot` (Byte), `WasPickedUp` (Byte 0); block items also get `Block` {name, states,
  version} [3][5]. Item components (enchantments, custom names, damage, …) are **dropped and reported** in 0.1.0.
- Java text components (custom names, sign lines) are reduced to plain text; formatting is dropped and reported.

## level.dat

8-byte header: i32 LE storage version (**10**) and i32 LE payload length, then little-endian NBT [1][3]. The field
list and types follow a `level.dat` written by Bedrock itself (Chunker's test resource `bedrock_level.dat`,
1.20.30) [3], updated with:

| Field | Value | Source |
|---|---|---|
| `StorageVersion` | 10 | [1][3] |
| `NetworkVersion` | 2192 | Chunker `BedrockDataVersion.V1_26_50` [3] |
| `lastOpenedWithVersion`, `MinimumCompatibleClientVersion` | [1, 26, 50, 0, 0] | Chunker writes the version this way [3] |
| `Generator` | 2 (flat) | [1] |
| `FlatWorldLayers` | `{"biome_id":1,"block_layers":[bedrock×1, dirt×2, grass_block×1],"encoding_version":6,"structure_options":null,"world_version":"version.post_1_18"}` | format as written by the game [3]; `grass_block` is the current name of the old `grass` |
| `GameType` | 1 (creative) | Bridger's choice: converted worlds are for exploring the build |
| `spawnMobs` | 0 | Bridger's choice: keeps the build undisturbed |
| `SpawnX/Y/Z` | next to the structure, outside its footprint | `docs/safety.md` §5 |

## New-world layout

- Superflat layers bedrock (y −64), dirt (−63, −62), grass block (−61): **surface at y = −61**, first free block at
  y = −60. ⚠ verify in-game.
- Only chunk columns the structure touches are written. Their flat layers are written too, so the structure stands
  on ground. Every other chunk is generated by the game's flat generator, so the edges must line up seamlessly.
  ⚠ verify in-game.
- Default placement: the structure's lowest layer at y = −60, centered on x = 0, z = 0 (`--place-at` overrides).

## Open questions (verified by the in-game checklist)

1. Heightmap convention (relative vs absolute), see Data3D.
2. Whether `FinalizedState = 2` is needed or harmful (Chunker omits it).
3. Seamless edges between written chunks and game-generated flat chunks.
4. Double-chest pairing (`pairlead` side).

Results are recorded in `docs/testing.md` and folded back into this document.

## Sources

1. minecraft.wiki, "Bedrock Edition level format": https://minecraft.wiki/w/Bedrock_Edition_level_format (checked 2026-10-11)
2. Microsoft Learn, Minecraft Bedrock "actorstorage" (entity storage), referenced by [1]
3. HiveGamesOSS/Chunker (MIT), commit `50aaf6d8` (2026-10-02). Behavior consulted, no code copied:
   `bedrock/util/PaletteUtil.java`, `bedrock/base/writer/BedrockChunkWriter.java`,
   `bedrock/v1_17_30/writer/ColumnWriter.java` (Data3D), `v1_18_30` / `v1_21_120` `ColumnWriter.java` (chunk
   version), `base/writer/BedrockLevelWriter.java`, `BedrockDataVersion.java`, block entity handlers;
   `cli/src/test/resources/nbt/bedrock_level.dat`. https://github.com/HiveGamesOSS/Chunker
4. Amulet-Team/leveldb-mcpe (BSD-3-Clause), `mcpe_sample_setup.cpp`: https://github.com/Amulet-Team/leveldb-mcpe
5. GeyserMC/mappings (MIT), branch `feature/26.3` commit `01ec65e3`: `items.json`, `biomes.json`, `blocks.nbt`
6. GeyserMC/Geyser (MIT), commit `50a464d7`: `DoubleChestBlockEntityTranslator.java`, `BlockRegistryPopulator.java`
