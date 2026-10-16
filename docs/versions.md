# Pinned game versions

The mapping data (`.brmap`) translates between **one pinned Java version and one pinned Bedrock version**: the latest
stable release of each when Phase 0 started (2026-10-11). Files from older versions are read and upgraded to the
pinned versions, and output can be written for older versions ("Target versions" below).

| Edition | Version | Released | How it was confirmed |
|---|---|---|---|
| Java Edition | **26.3** ("Wilderness Bound" game drop) | 2026-09-15 | Mojang's launcher manifest `piston-meta.mojang.com/mc/game/version_manifest_v2.json`: `latest.release = "26.3"` (checked 2026-10-11) |
| Bedrock Edition | **26.52** (hotfix of the 26.50 "Wilderness Bound" release) | 2026-09-25 | minecraft.wiki "Bedrock Edition version history" lists 26.52 as the latest release (checked 2026-10-11) |

## Numbers still to record

These identifiers are part of the formats Bridger reads and writes. Fill them in from authoritative sources
**before** the phase that needs them. Do not guess them.

| Value | Needed in | Source to use | Value |
|---|---|---|---|
| Java `DataVersion` for 26.3 | `.schem`, mapping data | `version.json` (`world_version`) inside the official 26.3 server jar | **5023** |
| Java block state count | mapping data | Mojang data generator `--reports` (`blocks.json`) | **35,723** (1,286 blocks) |
| Bedrock block-state `version` (palette NBT) | world writer | Bedrock 26.50 block palette (GeyserMC, `docs/adr/0007`) | **18168865** |
| Bedrock `StorageVersion` / level.dat header | world writer | minecraft.wiki; Chunker | **10** |
| Bedrock `NetworkVersion` | level.dat | Chunker `BedrockDataVersion.V1_26_50` | **2192** |
| `lastOpenedWithVersion` / `MinimumCompatibleClientVersion` | level.dat | Chunker | **[1, 26, 50, 0, 0]** |
| Bedrock chunk `Version` key (`0x2C`) | world writer | Chunker writer chain for 26.50 | **42** |

Values marked ⚠ in `docs/formats/bedrock-world.md` are confirmed by the in-game checklist.

## Version names (what saved a file)

The convert page and `bridger info` show which game version saved the input ("Saved by"), from what the file records
(`convert/source_version.cpp`):

| Input | Recorded | Shown |
|---|---|---|
| Java world | `level.dat` `Version.Name` (since 1.9), else `DataVersion`, else `version` (19132 McRegion, 19133 Anvil) | the name, e.g. "Java 1.20.1" |
| `.schem` v2/v3, `.nbt`, `.litematic` | `DataVersion` / `MinecraftDataVersion` | the release with that DataVersion, or "a snapshot between X and Y" |
| `.schem` v1 | nothing | "version not recorded (1.13 or newer)" |
| `.schematic` | numeric ids | "before 1.13" |
| Bedrock world (`.mcworld` or folder) | `level.dat` `lastOpenedWithVersion` | e.g. "Bedrock 1.21.50" (26.x versions as "26.50") |
| `.mcstructure` | the highest palette `version` | "Bedrock 1.21.60 or newer" (block data versions change less often than releases) |

DataVersion → release name: Mojang's version manifest joined with PrismarineJS minecraft-data
`pc/common/protocolVersions.json` (MIT), every release since 1.9 (`core/game_versions.cpp`).

## Target versions (what a conversion writes)

Every conversion can be written for an older version than the pinned one: `bridger convert --target-version <v>`,
`bridger versions` for the lists, and the "Game version" list on the app's convert page. The translators produce
pinned states; a final pass (`translate/targets.cpp`) rewrites them for the target and reports what the target
cannot hold. Nothing is dropped silently.

| Edition | Targets | Block data | Checked |
|---|---|---|---|
| Java | every release 1.18 – 26.3 (32) | `blocks.json` and `registries.json` (items) of every release from **Mojang's own data generator** run on each official server jar (`tools/mapgen/build_java_targets.py` → `src/translate/data/java-targets.nbt`) | 26.3 in game; the others against their block data |
| Bedrock | every release 1.18.30 – 26.50 (37) | each version's block palette from **BedrockBlockPaletteArchive** (CC0), every state upgraded with Bridger's own upgrader to get an exact map pinned → target (`build_bedrock_targets.py`, `bridger_targetgen` → `bedrock-targets.nbt`); item renames from **BedrockItemUpgradeSchema** (CC0) applied backwards | 26.50 in game; the others against their block data (the Bedrock game only runs its newest version) |

Checks built into the data: every state of every archived Bedrock palette resolves (0 unresolved), and the 26.50
target reproduces the pinned palette exactly (0 missing, 0 respelled).

**Blocks.** A state is written as the target spells it (renamed blocks such as `short_grass` → `grass` before
1.20.3, `iron_chain` → `chain` before 1.21.9; Bedrock respellings such as `red_wool` → `wool[color=red]` before
1.19.70). Properties the target lacks are dropped and properties it has take its default; that is reported as lossy
unless the dropped value was the default. On Bedrock, a missing state of a block the target has becomes the same
block's closest state (e.g. fence connections and the stairs' corner shape, which 26.50 started saving and older
versions compute; those are not reported).

**Blocks newer than the target** (the maintainer's choice, 2026-10-16), always reported:

- *Bedrock* keeps them: the state is written with its own name and the pinned block version, with its block entity.
  The older game shows its "unknown" placeholder block there and keeps the saved data, so the block comes back when
  the world is opened in a newer version (minecraft.wiki "Unknown"; ⚠ checked in game, `docs/testing.md`).
- *Java* has no such placeholder (an unknown block is lost on load), so the block becomes the closest block the
  target has, from a reviewed table (`substitute_once` in `translate/targets.cpp`): e.g. cherry and pale oak →
  birch, mangrove → jungle, mud bricks → bricks, tuff bricks → stone bricks, crafter → dropper, chiseled bookshelf →
  bookshelf, copper bulb → redstone lamp; a few with no sensible stand-in (hanging signs, petals, leaf litter, heavy
  core) become air. Up to four steps are followed (e.g. a 26.x block → a 1.21 block → a 1.20 block). Block entities
  follow where the stand-in has the same kind (crafter → dropper, copper chest → chest, sculk shrieker → sculk
  sensor); others are removed with the block. Only a block with no entry becomes the fallback block.

**Same edition.** A world or file can also be converted to another version of its own edition, up or down: a
`.mcworld` (or Bedrock world folder) to a `.mcworld`, a Java world to a Java world folder, and every structure format
to any other within an edition (`.schem` → `.schem` for 1.18.2, `.mcstructure` → `.mcstructure`, ...). The input is
read and upgraded to the pinned version as usual, then written for the target. Whole worlds convert the Overworld
only and do not write entities yet (both are warned about, as for cross-edition worlds). The output may never be the
input or lie inside it (`docs/safety.md`).

**Experimental blocks (Java).** Mojang's reports list blocks of experimental features in the releases where they were
still experimental (`update_1_20` in 1.19.3 – 1.19.4, `update_1_21` in 1.20.3 – 1.20.6, `winter_drop` in 1.21.2 –
1.21.3; minecraft.wiki "Experiments"). Such blocks are written; a world gets the experiment switched on in its
`level.dat` (data packs and `enabled_features`, as Chunker does), a structure gets a warning. Bedrock palettes also
contain experimental states without marking them; for Bedrock targets this is a known limit (the experiment is not
switched on).

**Biomes** newer than the target become the closest older one, reported: deep dark → dripstone caves and mangrove
swamp → swamp (before Java 1.19 / Bedrock 1.19.0), cherry grove → meadow (before 1.20 / 1.20.0), pale garden →
dark forest (before Java 1.21.4 / Bedrock 1.21.50).

**Block entities.** Java: item stacks `{id, Count: Byte}` without components before 1.20.5, items the target lacks
dropped (reported), text as JSON strings before 1.21.5, signs as `Text1`–`Text4`/`Color`/`GlowingText` before 1.20,
banner patterns as `{Pattern: code, Color: dye}` before 1.20.5; block entities of substituted blocks as above. Bedrock: signs with the front face at the root before 1.19.80; item ids, data values and the items' `Block`
tag in the target's spelling.

**World files.** Java: before 26.1 the Overworld is in `region/` and the generation settings (`WorldGenSettings`
with the three vanilla dimensions) in `level.dat`; before 1.21.9 the spawn is `SpawnX/Y/Z`; `DataVersion`,
`Version.Name` and every chunk's `DataVersion` are the target's. Bedrock: `level.dat` `lastOpenedWithVersion`,
`MinimumCompatibleClientVersion`, `NetworkVersion` and `StorageVersion`, the chunks' `Version` byte (40 from 1.18.30,
41 from 1.21.40, 42 from 1.21.120) and the palette entries' `version` are the target's
(`tools/mapgen/overrides/bedrock_targets.json` cites each value). Placing into an existing world always writes for
that world's game and refuses another target.

## Watch list

- Bedrock **26.60** is planned for 2026-10-27, Java **26.4** for Q4 2026.
- **Decision (maintainer, 2026-10-11): stay on Bedrock 26.52** (and Java 26.3) for v1. Moving the pin is a
  compatibility change and needs the maintainer's approval (see `AGENTS.md`).
