# Changelog

All notable changes to Bridger. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow the release plan in `docs/release.md`.

## [0.3.0] - 2026-10-12

First public release. The 0.1.0 (`.schem` → `.mcworld`) and 0.2.0 (more Java formats, placement) milestones were
completed on the way and ship here together with the Bedrock → Java direction. Pinned game versions: **Java
Edition 26.3** (DataVersion 5023) and **Bedrock Edition 26.52** (palette 26.50).

### Added

- `bridger convert <input> <output>` between every supported pair:
  - Java structures (`.schem` v1–v3, `.litematic`, structure `.nbt`, legacy MCEdit `.schematic`) → new Bedrock
    world (`.mcworld`, superflat, structure centred on 0, 0) or `.mcstructure`.
  - Java worlds (1.18+ chunks, both the classic and the 26.1+ `dimensions/` folder layout; Overworld) → `.mcworld`,
    keeping the seed, spawn and biomes. Parallel across all cores (`--threads`).
  - Placing a structure into a **copy** of an existing Bedrock world (`--into-world`, `--place-at`, `--air`,
    `--clear-entities`); everything the structure does not touch stays byte-for-byte identical.
  - `.mcstructure` → `.schem` (v3), `.litematic`, structure `.nbt`, or `.mcworld`.
  - Bedrock worlds → Java world folder (26.1+ layout), in parallel.
  - Java structures → other Java structure formats without translation.
- `bridger info` for every format, including `.mcworld` files and Java world folders. `bridger formats` lists
  the support status.
- Translation in both directions from GeyserMC's mappings plus Bridger's own fixes: every Java 26.3 block state is
  mapped. Waterlogging ⇄ Bedrock layer 2; chests (single, double, trapped), beds, banners (patterns), skulls,
  flower pots, signs (both sides, glowing text), containers with their items; biomes. Bedrock → Java rebuilds stair
  shapes (Java's own rule) and fence/pane connections; Bedrock-only blocks get stand-ins or the fallback.
- A conversion report for everything not converted exactly (`--report report.json`), with counts and example
  positions; `--fallback air|marker`.
- Safe output: temp file next to the destination, re-read and validated, then moved into place atomically; an
  existing output is replaced only with `--overwrite`; Java world folders are never replaced.

### Known limitations

- Entities (mobs, armor stands, item frames, paintings) are not converted; they are listed in the report.
- Item components (enchantments, custom names, durability) and text formatting are dropped and reported.
- Worlds: Overworld only, in both directions; Java worlds saved before 1.18 are refused.
- Fence and pane connections rebuilt for Java use an approximation of which blocks are solid; the game corrects
  any wrong one on the next block update.
- One Java and one Bedrock version at a time.
- Windows x64 only.

[0.3.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.3.0
