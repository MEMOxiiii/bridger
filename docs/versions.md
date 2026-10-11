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
| Java `DataVersion` for 26.3 | Phase 2 (`.schem` `DataVersion`), Phase 3 | `version.json` inside the official 26.3 client/server jar (read locally, never committed) | _TBD_ |
| Bedrock world `StorageVersion` / `lastOpenedWithVersion` | Phase 0.5 | A world created by Bedrock 26.52 itself (`level.dat`) | _TBD_ |
| Bedrock block-state `version` integer (palette NBT) | Phase 0.5 | Same world: sub-chunk palette entries | _TBD_ |
| Bedrock chunk `Version` key byte | Phase 0.5 | Same world: LevelDB key tag `0x2C` (verify) | _TBD_ |

## Watch list

- Bedrock **26.60** is planned for 2026-10-27, Java **26.4** for Q4 2026.
- **Decision (maintainer, 2026-10-11): stay on Bedrock 26.52** (and Java 26.3) for v1. Moving the pin is a
  compatibility change and needs the maintainer's approval (see `AGENTS.md`).
