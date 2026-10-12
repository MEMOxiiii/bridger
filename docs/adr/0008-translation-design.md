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

6. **Placement before translation.** Clipping and shifting work on the Java structure, so their reports use
   source positions and the Java air id.

## Consequences

- Entities are not translated in 0.1.0 (actor storage is a separate format); each one is reported.
- Item components (enchantments, names, damage) are dropped and reported in 0.1.0.
