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
one biome storage per subchunk of the dimension, from its bottom up (24 Overworld, 8 Nether, 16 End) [3]:
  u8 bits << 1 | 1 (runtime palette), packed words as for blocks, i32 LE palette count, i32 LE biome ids
```

- The biome storage layout, the runtime flag and the 24 storages follow Chunker's writer [3]. Biome ids are Bedrock
  numeric ids (GeyserMC `biomes.json`: plains = 1) [5].
- **Heightmap value:** Bridger stores `highest non-air y - bottom + 1` (0 for an empty column; bottom -64 in the
  Overworld, 0 in the Nether and the End). The sources disagree
  on whether this is relative to the world bottom (Chunker writes absolute values [3]). The game rebuilds lighting
  data, so a wrong value is unlikely to matter. ⚠ verify in-game against a world created by the game itself.

## Dimensions

| Dimension | Key field (i32 LE) | Height | Subchunks | Data3D biome storages | Fallback biome |
|---|---|---|---|---|---|
| Overworld | omitted | y -64..319 | -4..19 | 24 | plains (1) |
| Nether | 1 | y 0..127 | 0..7 | 8 | hell / nether_wastes (8) |
| End | 2 | y 0..255 | 0..15 | 16 | the_end (9) |

Sources: Chunker `Dimension` (Bedrock ids 0/1/2, `biomeHeight` 24/8/16, fallback biomes) and its Data3D writer, which
writes one storage per subchunk of the dimension [3]; `BedrockBiomeIDResolver` for the biome ids [3]; minecraft.wiki
"Bedrock Edition level format" for the key layout [1]. A Java Nether chunk has sections up to y 255; whatever is above
y 127 is dropped and counted ("blocks of the Nether outside the Bedrock build height"). The Caves & Cliffs
experiment's shifted subchunk keys (chunk versions 0x16–0x1D) apply to the Overworld only.

## Entities

Bedrock keeps item frames as **blocks** and paintings (and every other entity) as **actors**; Java keeps both as
entities. Translation: `translate/entity_translator.cpp`, following Chunker [3] (`BedrockItemFrameEntityHandler`,
`ItemFrameEntity.toBedrockBlockEntity`, `BedrockItemFrameBlockEntityHandler`, `BedrockPaintingEntityHandler` and its
`Relocator`, `BedrockPaintingMotiveResolver`, `JavaItemFrameEntityHandler`, `JavaPaintingEntityHandler`,
`JavaHangingEntityHandler`, `BedrockEntityHandler`).

| Java entity | Bedrock |
|---|---|
| `item_frame`, `glow_item_frame`: `Facing` (0..5 = down, up, north, south, west, east), tile `block_pos` (1.21.5+) or `TileX/Y/Z`, `Item`, `ItemRotation` (0..7) | block `minecraft:frame` / `glow_frame` at the tile: `facing_direction` = Java `Facing` (same order), `item_frame_map_bit` (holds a filled map), `item_frame_photo_bit` 0; water there moves to layer 2; block entity `ItemFrame` / `GlowItemFrame` with `Item` and `ItemRotation` = steps × 45 (Float degrees, 1.13+) |
| `painting`: `variant` (1.19+) or `Motive`, `facing` / `Facing` (2D: south, west, north, east = 0..3), tile | actor `minecraft:painting`: `Motif` (after 1.20.60; `Motive` before), `Direction` = Java facing, `Pos`, `Rotation`, `Motion` |

Motive names: the old ones in CamelCase (`Kebab`, `SkullAndRoses`, `DonkeyKong`), the 1.21 ones in lower case
(`backyard`, `prairie_ride`, …), `dennis` from 1.21.93; for older targets newer motives are removed and reported.
Painting position (Chunker's Relocator, writing): tile −= tile offset (x −1 facing south and z −1 facing west for an even
width, y −1 for an even height); Pos = tile − position offset, where the offset is (side, y, −0.96875) north,
(−0.03125, y, side) east, (side, y, −0.03125) south, (−0.96875, y, side) west, side = 0 for an even width else −0.5,
y = 0 for an even height else −0.5. Reading Bedrock runs it forward (Pos += offset, tile = floor(Pos + tile offset)).

**Actor storage** (1.18.30+): `actorprefix` + 8-byte storage key → the actor NBT; `digp` + x, z (+ dimension) → the
storage keys of the actors in that chunk (an actor is listed in the chunk its position is in). Bridger assigns
`UniqueID` = (world start count 0, flipped) << 32 | counter and the storage key 0x100000000 + (UniqueID ^
0xFFFFFFFF00000000), big-endian (Chunker `generateStorageKeyForEntity`) [3][1]. Worlds before 1.18.30 keep a chunk's
entities as concatenated NBT under its Entity key (0x32); both are read. Every other entity is reported
("unmapped entity"); within an edition (Bedrock → Bedrock) actors are copied as they are, as Chunker does.

## Player

The single-player player: Java `level.dat` `Data.Player` ↔ Bedrock `~local_player` (`translate/player.cpp`; Chunker
`JavaLevelReader.parsePlayer`, `JavaLevelWriter.writePlayer`, `BedrockLevelWriter.writeLocalPlayer`,
`BedrockLevelReader`) [3]:

| Java | Bedrock |
|---|---|
| `Pos` (feet) | `Pos` Floats, y + 1.62001 (the eye position Bedrock saves) |
| `Motion`, `Rotation` | same, as Floats |
| `Dimension` (`minecraft:the_nether`, …; older −1/0/1) | `DimensionId` 0, 1, 2 |
| `playerGameType` 0..3 | `PlayerGameMode`; spectator 3 ↔ 6 |
| `Inventory` slots 0..35 | `Inventory` with the same `Slot` |
| armor: `equipment` {`head`, `chest`, `legs`, `feet`} (1.21.5+) or slots 103..100 | `Armor` list head, chest, legs, feet (empty slots as items with `Name` "") |
| offhand: `equipment.offhand` or slot −106 | `Offhand` list |

Bedrock also gets the `minecraft:movement` attribute (base 0.1), without which it uses a wrong default speed. Ender
chest, experience, effects and respawn point are reported, not converted. Server players (`playerdata/`) are not read,
as in Chunker.

## Block entities

Stored per chunk under tag `0x31` as concatenated little-endian NBT roots [1]. Each one gets `x`, `y`, `z`
(absolute Int) and `isMovable` (Byte 1) from the writer [3]. Translated content:

| Java id | Bedrock id [3] | Fields |
|---|---|---|
| `chest`, `trapped_chest` | `Chest` (Bedrock has no separate trapped-chest id; Chunker refuses to write `TrappedChest` [3]) | `Items`, `CustomName`, pairing |
| `barrel`, `furnace`, `blast_furnace`, `smoker`, `hopper`, `dispenser`, `dropper`, `shulker_box` | `Barrel`, `Furnace`, `BlastFurnace`, `Smoker`, `Hopper`, `Dispenser`, `Dropper`, `ShulkerBox` | `Items`, `CustomName` (slot layouts are identical); furnaces `BurnTime`, `CookTime`, `BurnDuration` (Short) from Java `lit_time_remaining`, `cooking_time_spent`, `lit_total_time` (older `BurnTime`, `CookTime`, `CookTimeTotal`) |
| any container with `LootTable` (also `decorated_pot`, `brushable_block`) | | `LootTable`: `loot_tables/<path>.json`; Bedrock keeps five old names (`chests/buriedtreasure`, `dispenser_trap`, `shipwreck`, `shipwrecksupply`, `shipwrecktreasure`); `village_fisher` has no Bedrock table (reported). `LootTableSeed` and a hopper's `TransferCooldown` are transient and not written [3] |
| `brewing_stand` | `BrewingStand` | `Items` with slots remapped (Java bottles 0 – 2, ingredient 3, fuel 4 → Bedrock ingredient 0, bottles 1 – 3, fuel 4), `FuelAmount`, `FuelTotal` (Java `total_fuel` from 26.3, else 20), `CookTime` (Java `BrewTime`) [3] |
| `chiseled_bookshelf` | `ChiseledBookshelf` | `Items` [3] |
| `crafter` | `Crafter` | `Items`, `disabled_slots` (Short bit mask; Java: Int array of slots), `crafting_ticks_remaining` [3] |
| `decorated_pot` | `DecoratedPot` | `sherds`: four Bedrock item names in the order back, left, right, front (Java: a list, or `{back: {id}, …}` from 26.3; a missing side is `brick`); `item`. Before Bedrock 1.20.0 the list is `shards`; the item exists from 1.20.50 [3] |
| `campfire`, `soul_campfire` | `Campfire` | `Item1` – `Item4`, `ItemTime1` – `ItemTime4` (Java `Items` slots 0 – 3, `CookingTimes`) [3] |
| `beacon` | `Beacon` | `primary`, `secondary`: Bedrock effect ids (speed 1, haste 3, strength 5, jump boost 8, regeneration 10, resistance 11) from Java `primary_effect` names (older: the same numbers) [3] |
| `lectern` | `Lectern` | `book` (item), `hasBook`, `totalPages`, `page` = Java `Page` / 2 (Bedrock counts open spreads) [3] |
| `jukebox` | `Jukebox` | `RecordItem` [3] |
| `mob_spawner` | `MobSpawner` | `EntityIdentifier` (Java `SpawnData.entity.id`, older `SpawnData.id`; 17 mobs are named differently, e.g. `zombified_piglin` → `zombie_pigman`, `villager` → `villager_v2`), `Delay`, `SpawnRange`, `MinSpawnDelay`, `MaxSpawnDelay`, `SpawnCount`, `RequiredPlayerRange`, `MaxNearbyEntities` (Short) [3] |
| `command_block` | `CommandBlock` | `Command`, `auto`, `TrackOutput`, `CustomName` [3]; reported, because command syntax differs between the editions |
| `brushable_block` | `BrushableBlock` | `type` (the block), `brush_direction` (Java `hit_direction`, default 6), `brush_count` 0, `item`, `LootTable` [3] |
| `sculk_sensor` | `SculkSensor` | `VibrationListener` {`selector` {}} [3] |
| `sculk_catalyst`, `sculk_shrieker`, `calibrated_sculk_sensor`, `trial_spawner`, `vault`, `creaking_heart`, `end_portal`, `beehive`, `conduit` | `SculkCatalyst`, `SculkShrieker`, `CalibratedSculkSensor`, `TrialSpawner`, `Vault`, `CreakingHeart`, `EndPortal`, `Beehive`, `Conduit` | id only, as Chunker writes them [3]; bees in a hive (entities) and a vault's list of rewarded players are reported |
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
  version} [3][5]. The item's data goes into its `tag` compound (next section).
- Java text components (custom names, sign lines) are reduced to plain text; formatting is dropped and reported.

### Items

Item data inside containers (`translate/item_translator.cpp`). Java reads both the 1.20.5+ `components` (with the
1.20.5 – 1.21.4 `{levels: …}` wrapper of enchantments, and the 1.21.5+ form without it) and the older `tag`. Bedrock
fields, ids and data values follow Chunker's `BedrockItemStackResolver`, `BedrockItemIdentifierResolver`,
`BedrockEnchantmentIDResolver`, `BedrockPotionIDResolver`, `BedrockTrim{Material,Pattern}Resolver` and
`ChunkerDyeColor` [3]; the Java side follows its `JavaComponentItemStackResolver` and `JavaItemStackResolver` [3].

| Java (component / old `tag`) | Bedrock |
|---|---|
| `damage` / `Damage` | `tag.Damage` (Int) |
| `enchantments`, `stored_enchantments` / `Enchantments`, `StoredEnchantments` | `tag.ench` [{`id` Short, `lvl` Short}]; ids 0 – 41 (protection 0 … lunge 41). Sweeping Edge has no Bedrock id: reported |
| `repair_cost` / `RepairCost` | `tag.RepairCost` (Int) |
| `custom_name`, `lore` / `display.Name`, `display.Lore` | `tag.display` {`Name`, `Lore` [String]}; plain text, formatting reported |
| `dyed_color` / `display.color` | `tag.customColor` (Int, ARGB with alpha FF) |
| `potion_contents.potion` / `Potion` on potions | the item's data value (`Damage`): water 0, mundane 1, … infested 46; Luck has none: reported |
| the same on `tipped_arrow` | `minecraft:arrow` with data value potion + 1 |
| `written_book_content`, `writable_book_content` / `pages`, `title`, `author` | `tag.pages` [{`photoname` "", `text`}], `tag.title`, `tag.author` |
| `container` / `BlockEntityTag.Items` | `tag.Items` (items with `Slot`), up to three levels deep |
| `bundle_contents` | `tag.storage_item_component_content` |
| `fireworks`, `firework_explosion` / `Fireworks`, `Explosion` | `tag.Fireworks` {`Flight`, `Explosions`}, `tag.FireworksItem`: `FireworkType`, `FireworkColor`/`FireworkFade` as reversed dye indexes (closest dye to the Java RGB), `FireworkTrail`, `FireworkFlicker` |
| `trim` / `Trim` | `tag.Trim` {`Material`, `Pattern`} without namespace |
| `instrument` (goat horn) | data value 0 – 7 (ponder, sing, seek, feel, admire, call, yearn, dream) |
| `ominous_bottle_amplifier` | data value 0 – 4 |
| `suspicious_stew_effects` | data value 0 – 10 by effect and duration |
| `banner_patterns` / `BlockEntityTag.Patterns` | `tag.Patterns` as on the banner block |

Old item ids are renamed first (e.g. `chain` → `iron_chain` before 1.21.9, `scute` → `turtle_scute` before 1.20.5).
Anything else on an item (e.g. `custom_data`, `map_id`, `entity_data`, `block_entity_data`) is reported as "item data
not converted: <name>"; the item itself is kept.

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

## Placing into an existing world (`--into-world`)

Implementation: `place_into_world.cpp`. The source `.mcworld` is unzipped into a scratch directory and only that
copy is opened; the original file is never written (`docs/safety.md` §1).

- For every subchunk the structure overlaps, the existing subchunk is decoded **without resolving its palette**
  (`decode_subchunk_raw`: palette entries stay as their original NBT bytes), so states unknown to Bridger's pinned
  palette survive untouched. Structure blocks are added to the palette; `--air keep` leaves positions where the
  structure has air unchanged. Missing subchunks are created; a liquid on the structure's secondary layer creates
  or updates layer 2.
- `BlockEntity` lists of touched chunks are rewritten: entries at positions a structure block overwrites are
  removed and **reported**; the structure's block entities are added.
- `--clear-entities`: entities in the structure's box are removed from both storages, the modern `digp` +
  `actorprefix` keys and the legacy per-chunk `Entity` key [1], and every removal is reported.
- Columns that did not exist get `Data3D`, `Version` and `FinalizedState` like a new world (no flat layers: there
  is no generator setting to match), and are counted in the summary.
- `level.dat` is copied byte-for-byte.
- Validation before the output is moved into place (`validate_placed_world`): `level.dat` is unchanged, every key
  outside the touched set is byte-identical to the source, every touched subchunk decodes.

## Converted Java worlds

`WorldBuilder` (`bedrock_world.hpp`) writes columns as they arrive, without flat layers. `level.dat` uses
`Generator` 1 (infinite) and the Java world's seed, name and spawn; chunks the Java world did not have are
generated by the game. Details and the parallel pipeline: `docs/formats/anvil.md`.

## Reading worlds (Bedrock → Java)

Implementation: `bedrock_world_reader.cpp`. The `.mcworld` is unzipped into a scratch directory (worlds exported
inside one top-level folder are accepted: the shortest path ending in `level.dat` decides the root) and only that
copy of `db/` is opened.

- **Chunks:** every key of 9 bytes ending in `Version` (`0x2C`) or the pre-1.16.100 `LegacyVersion` (`0x76`) [1] is
  an Overworld chunk; 13-byte keys belong to other dimensions (reported, not converted). `actorprefix` keys and
  legacy `Entity` keys are counted and reported (entities are not converted yet).
- **Subchunks** (every version, see "Old worlds") are decoded with their palette entries kept as raw NBT; each
  distinct entry is upgraded if it is old and resolved once against the pinned palette. Entries the palette does not know are read as air and reported as
  unmapped blocks with their names. Layer 2 is kept as the secondary layer.
- **Data3D biomes:** after the 512-byte heightmap, one storage per subchunk from the bottom. Header byte
  `bits << 1 | runtime`; **bits 0** means no index words and no palette count, a single id; **bits 127** means no
  data for that subchunk (both as read by Chunker's `PaletteUtil.readChunkPalette` [3]). For "no data", Bridger
  keeps the biomes of the subchunk below (its own choice, not a format fact).
- **Block entities:** concatenated NBT roots with absolute `x/y/z`; they become column-local, and so do
  `pairx`/`pairz`. `isMovable` is dropped.
- **level.dat:** `LevelName`, `SpawnX/Y/Z`, `RandomSeed`, `Generator` are read (8-byte header skipped).

## Old worlds (reading)

Bridger reads Overworld terrain from every LevelDB world the game has written, from Pocket Edition 0.9 (2014) to
26.52, as a `.mcworld` or as a world folder from `minecraftWorlds` (its `db/` is copied to a scratch folder first,
because LevelDB writes lock and log files even when only reading). Pocket Edition before 0.9 (`chunks.dat`) is
not LevelDB and is not read. Implementation: `bedrock_world_reader.cpp`, `subchunk_codec.cpp`,
`translate/bedrock_upgrade.cpp`.

**Chunk version** (the byte under `Version` 0x2C, or `LegacyVersion` 0x76 before 1.16.100) [1]; values from
PocketMine-MP `ChunkVersion.php` [8]:

| Chunk version | Game | Terrain stored as |
|---|---|---|
| 0–2 | 0.9.0 – 0.9.5 (to 0.17) | `LegacyTerrain` (0x30): one 16 × 128 × 16 column |
| 3–7 | 1.0 – 1.2.x | `SubChunkPrefix` version 0 (numeric ids) |
| 8–22 | 1.2.13 – 1.16.210 | `SubChunkPrefix` version 1 or 8 (palettes) |
| 23–26 | 1.16.220/230 betas with experimental Caves & Cliffs | as above, but subchunk y is stored at key index **y + 4** [8] |
| 27–42 | 1.17.30 – 26.x | `SubChunkPrefix` version 8 or 9 |

**`LegacyTerrain`** (0x30), 83 200 bytes [1][8]: 32 768 block ids, 16 384 bytes of data nibbles, 32 768 bytes
of sky and block light, a 256-byte heightmap, then 256 big-endian u32 biome entries (biome id in the top byte,
RGB colour below). Index `x*2048 + z*128 + y`. Read as subchunks 0–7.

**`SubChunkPrefix` versions 0 and 2–7** (1.0 – 1.2.x): 4096 block ids, then 2048 bytes of data nibbles, index
`x*256 + z*16 + y`; light may follow and is ignored. Versions 2–7 were written by a third-party converter that
put the subchunk height in the version byte; the game reads them like 0 [7][8]. **Version 1** (1.2.13): one
palette storage with no layer count. **Bits 0** storages (one palette entry, no words, no count) are written by
other tools, not the game; one with a count of 1 is accepted too [8].

**Nibble arrays**: the data value of an even index is in the low four bits of its byte (PocketMine-MP
ext-chunkutils2 `SubChunkConverter.h` [8]).

**`LegacyBlockExtraData`** (0x34, before 1.2.13): a second block layer (e.g. snow layered over grass) as i32
count, then (i32 key, u16 value) pairs with `value = id | data << 8`; the key packs x, z, y in 4/4/8 bits (4/4/7
before chunk version 3) [8]. Read as layer 2 of subchunks stored as versions 0 or 1, or `LegacyTerrain`.

**2D biomes**: `Data2D` (0x2D, before 1.18): 512-byte heightmap, then 256 biome ids of one byte (two bytes LE
when the value is 1024 bytes long, 1.21.40+ "Old" worlds) [1]; `Data2DLegacy` (0x2E, before 1.0): heightmap, then
256 × 4 bytes with the id first [1]; `LegacyTerrain` as above. Index `z*16 + x`; the id fills the whole column.

**Block state upgrades.** Palette entries keep the form of the version that wrote them, and the game upgrades
them only when a chunk is loaded and saved again, so an old world mixes forms [7]:

| Form | Written by | Upgrade |
|---|---|---|
| numeric id + data | before 1.2.13 (no palette) | id → string id (`block_legacy_id_map.json`), then as the next row |
| `{name, val}` | 1.2.13 – 1.12 | (id, data) → 1.12 state (`id_meta_to_nbt/1.12.0.bin`; unknown data values use data 0, as PocketMine-MP does), then as the next row |
| `{name, states, version}` | 1.13 – 26.x | every upgrade schema newer than `version`, in order |

The data is BedrockBlockUpgradeSchema [7] (CC0), converted by `tools/mapgen/build_bedrock_upgrade.py` into
`src/translate/data/bedrock-upgrade.nbt` (little-endian NBT, read with Bridger's own parser). Schema semantics
follow its README and `nbt_upgrade_schema_schema.json`: a `remappedStates` match replaces the whole state and
stops; otherwise `renamedIds` or `flattenedProperties`, `addedProperties`, `removedProperties`,
`renamedProperties` and `remappedPropertyValues` apply, all indexed by the old name and old property names. A
schema version shared by several schemas is applied again to entries of that version, because Mojang changed
states without bumping it (PocketMine-MP `BlockStateUpgrader.php` [8]); the four newest schemas (1.21.60 to
26.50) all carry 1.21.60.33. Entries at the current block version are first looked up as they are and upgraded
only if that fails (e.g. fences from 26.40, which lack the connection states 26.50 added). Numeric ids are passed
from the reader to the lookup as `{"bridger:legacy_id": Int, "val": Short}`, a Bridger-only form.

Checked: every pre-1.13 (id, data) pair of the table upgrades to a state of the pinned 26.50 palette (unit test);
a Bedrock 1.12 world written by Chunker [3] (`BEDROCK_R12`) from a 26.3 Java world converts with no unknown
blocks.

## Open questions (verified by the in-game checklist)

1. Heightmap convention (relative vs absolute), see Data3D.
2. Whether `FinalizedState = 2` is needed or harmful (Chunker omits it).
3. Seamless edges between written chunks and game-generated flat chunks.
4. Double-chest pairing (`pairlead` side).

Results are recorded in `docs/testing.md` and folded back into this document.

**2026-10-12 (release 0.3.0):** the in-game run passed in Bedrock 26.52 with the values described above (relative
heightmap, `FinalizedState` 2, `pairlead` on the right half): the test worlds open, flat edges meet game-generated
chunks without seams, and double chests open as one.

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
7. opencollab-incubator/BedrockBlockUpgradeSchema (CC0-1.0, formerly pmmp), commit `4e874a16` (2026-09-18):
   README, `nbt_upgrade_schema_schema.json`, `nbt_upgrade_schema/*.json`, `0351_1.26.40_to_1.26.50.json`,
   `block_legacy_id_map.json`, `id_meta_to_nbt/1.12.0.bin`. https://github.com/opencollab-incubator/BedrockBlockUpgradeSchema
8. pmmp/PocketMine-MP (LGPL-3.0), branch `stable` (checked 2026-10-15), consulted only, no code copied:
   `src/world/format/io/leveldb/{LevelDB,ChunkVersion,SubChunkVersion,ChunkDataKey}.php`,
   `src/data/bedrock/block/upgrade/*.php`; pmmp/ext-chunkutils2 `lib/SubChunkConverter.h` (index and nibble order).
   Tomcc's "Beta 1.2.13 format" gist (by a Mojang developer, linked from [1]) for subchunk versions 0, 1 and 8.
