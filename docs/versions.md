# Pinned game versions

Bridger v1 supports **exactly one Java version and one Bedrock version**: the latest stable release of each when
Phase 0 started (2026-10-11). Other versions are added later, one at a time (Phase 9). The mapping data
(`.brmap`) records the same versions.

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

## Watch list

- Bedrock **26.60** is planned for 2026-10-27, Java **26.4** for Q4 2026.
- **Decision (maintainer, 2026-10-11): stay on Bedrock 26.52** (and Java 26.3) for v1. Moving the pin is a
  compatibility change and needs the maintainer's approval (see `AGENTS.md`).
