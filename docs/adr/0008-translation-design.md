# 0008. Translation design: Bedrock state ids, two stages, translate depends on io

- Status: accepted
- Date: 2026-10-12

## Decisions

1. **Bedrock IR ids are palette indices.** A structure whose source edition is Bedrock stores, in its block
   layers, indices into the pinned Bedrock palette of the mapping data (`BedrockStateIndex`). This avoids a second
   string-keyed registry for typed Bedrock states, and lets the writer emit exact typed NBT. Biome layers of Bedrock
   structures hold Bedrock numeric biome ids.

2. **The "empty" value of a layer belongs to its domain.** Index 0 is a real Bedrock state (`cyan_terracotta`),
   not air. Code must not assume `kAirStateId` (Java air = 0) means "nothing" outside the Java domain: secondary
   layers in Bedrock structures are allocated with Bedrock air as their fill (`Section::ensure_secondary(fill)`),
   and readers check `has_secondary()`. A unit test found this in Phase 4. Before the fix, the writer filled layer
   2 with terracotta wherever a section had no liquids.

3. **Two stages** (`translate/translator.cpp`):
   - Palette stage: each distinct Java state of a section is translated once (cached per session) and the
     section's palette is remapped; the 4096-entry index array is untouched.
   - Context stage: only sections whose palette contains a state needing more than itself are walked:
     waterlogged states (→ water in layer 2), double chests (pairing) and beds (color). The rules read the
     immutable Java source and write to the output, so results do not depend on order.
   - Block entities are translated with the Java state at their position; chests and beds without Java data
     get a synthesized Bedrock block entity.

4. **Positions are regenerated, never copied.** Translated block entities carry no `x/y/z`. Position-valued fields
   (`pairx`/`pairz`) are structure-local and offset by the world writer.

5. **`translate` may depend on `io`** (the dependency rule was `core` only). Block entities and items are NBT, and
   the NBT library lives in `io`. The graph stays acyclic and free of UI/GPU dependencies:
   `core ← platform ← io ← {formats, translate} ← convert`. Enforced in `cmake/BridgerModule.cmake`.

6. **Placement before translation.** Clipping and shifting work on the source structure (Java, or Bedrock for
   `.mcstructure` input), so their reports use source positions and the source domain's air id.

7. **Bedrock → Java** (`translate/reverse_translator.cpp`, `reverse_block_entities.cpp`, Phase 6) uses the same
   two stages.
   - Palette stage, in order of preference: the exact reverse mapping (the Java state whose forward mapping is this
     Bedrock state; not waterlogged, same block name if possible, closest to the block's default); else the
     **nearest** Java state of the same Bedrock block (most equal properties; Bedrock-only property values),
     reported as lossy; else a stand-in from `tools/mapgen/overrides/reverse_blocks.json` (Bedrock-only blocks
     such as hard glass), reported as lossy; else the fallback block (air, or `minecraft:sponge` as the visible
     marker), reported as unmapped. Counts are in `docs/mappings/`.
   - Context stage. Layer 2: water becomes `waterlogged=true`, or the water block itself where layer 1 is air;
     blocks that are always under water in Java (seagrass, kelp, bubble columns) need nothing; anything else is
     a reported dropped secondary layer. Then the states Java stores but Bedrock derives:
     - **Stair shape** from neighbouring stairs, by Java's own rule (`StairBlock.getStairsShape`, behaviour read
       from the 26.3 server; no code copied).
     - **Fence, pane and iron-bar connections** from neighbours: same family, fence gates across the direction,
       walls for panes, and "sturdy" faces. Java decides sturdiness from collision shapes, which the mapping data
       does not contain, so Bridger uses a **name-based approximation** (partial blocks such as slabs, stairs,
       doors, plants are excluded; leaves, pumpkins, melons, barriers and shulker boxes are Java's exceptions). The
       game recomputes connections on the next block update, so a wrong guess is cosmetic and temporary.
     - Chest `type` from the Bedrock pairing (`pairx`/`pairz`, inverse of the forward rule), bed and banner colors,
       skull type and floor rotation, and potted plants from the Bedrock block entity.
   - Neighbours come from the immutable source plus the palette-stage table. For whole worlds, translated one chunk
     column at a time, neighbours across the column border come from the adjacent column
     (`TranslateOptions::bedrock_outside`); absent chunks are "unknown" and treated as air.
   - Block entities are translated against the final Java state at their position (e.g. a chest becomes
     `minecraft:trapped_chest` when the block is one). Bed colors, skull types and potted plants become part of the
     Java block; a Bedrock flower pot produces no Java block entity, since Java has none for pots.

## Consequences

- Entities are not translated in 0.1.0 (actor storage is a separate format); each one is reported.
- Item components (enchantments, names, damage) are dropped and reported in 0.1.0.
- Round trip Java → Bedrock → Java (`tests/unit/translate/roundtrip_test.cpp`) returns every block state, biome
  and supported block entity of the test structures unchanged, except stair shapes that their neighbours do not
  justify (Bedrock does not store shapes; Java itself would recompute them on the next update).
