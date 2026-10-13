# 0007. Mapping data sources and the `.brmap` blob

- Status: accepted
- Date: 2026-10-12

## Context

Translating Java blocks to Bedrock needs, for the pinned versions (Java 26.3, Bedrock 26.50/26.52):
1. every Java block state with its numeric id,
2. the Bedrock state each one maps to,
3. Bedrock's block palette with exact NBT value types and the block `version` integer,
4. item and biome mappings for block entities and Data3D.

All of it must come from openly licensed or authoritative sources (`AGENTS.md`), and the coverage must be
measured.

## Decision

| Input | Source | How it enters the repo |
|---|---|---|
| Java states (names, properties, ids, defaults) | Mojang's official data generator (`--reports` → `blocks.json`) run on the 26.3 server jar; DataVersion from the jar's `version.json` (5023) | `tools/mapgen/fetch_sources.py` downloads the jar (SHA-1 from Mojang's manifest) into a gitignored cache. **The jar and its reports are never committed.** |
| Java → Bedrock blocks, items, biomes | GeyserMC/mappings (MIT), branch `feature/26.3` ("generated for Java 26.3 and Bedrock 1.26.50"), commit `01ec65e3` | git submodule `tools/mapgen/sources/geyser-mappings` |
| Bedrock palette (names, typed states, version 18168865) | GeyserMC/Geyser (MIT) `core/src/main/resources/bedrock/block_palette.26_50.nbt`, commit `50a464d7` | fetched by `fetch_sources.py`, SHA-256 verified |
| Bridger's own fixes | `tools/mapgen/overrides/blocks.json` (each with a comment and a test) | committed |

`tools/mapgen` (C++, using Bridger's own NBT and compression code) combines these into
`src/translate/data/java-26.3_bedrock-26.50.brmap` (format in `mapping_data.hpp`). The blob is committed and embedded
into the binary with `cmake/bin2cpp.cpp`. It also writes the coverage report `docs/mappings/coverage-*.md`.

Checks mapgen enforces: the Geyser entry count must equal the Java state count; every mapping target must exist in
the Bedrock palette (it is looked up by name + states); state ids must be contiguous; overrides need a comment and
must target an existing palette state.

**Measured coverage (Java 26.3 → Bedrock 26.50):** all 35,723 Java states map to a Bedrock state, and none are
unmapped. 13,325 waterlogged states are handled by the IR's secondary layer. 15,953 of 22,091 Bedrock states are
reachable from Java. That the mapping exists does not prove it is exact: Geyser maps for live network play, so
the in-game checklist remains the judge (`docs/testing.md`).

Java states are stored explicitly (one value index per property per state), not derived by formula: 12 blocks
(chests, `moving_piston`, …) do not enumerate their states in simple mixed-radix order.

## Consequences

- CI does not run mapgen (it needs Mojang's jar); it tests the committed blob.
- Updating to a new version follows "How to update mapping data" in `AGENTS.md`.
- The blob contains block, item and biome names, which are facts about the game, as GeyserMC's published files do.
  No Mojang code or assets are included (`docs/THIRD_PARTY.md`).
