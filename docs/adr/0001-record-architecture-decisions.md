# 0001. Record architecture decisions; hub-and-spoke IR

- Status: accepted
- Date: 2026-10-11

## Context

Bridger converts between seven formats across two editions and will add more versions over time. Direct
format-to-format converters would grow N×N. We also need a written record of why decisions were made.

## Options

1. Direct converters for each pair of formats.
2. A universal intermediate representation (IR), with one reader (format → IR) and/or one writer (IR → format)
   per format.

## Decision

- **Option 2: hub-and-spoke around an IR** (`bridger_core`). Block states are interned to `uint32_t` ids. Spatial
  data is stored as 16³ sections with local palettes and explicit primary/secondary block layers (Java
  `waterlogged` ⇄ Bedrock layer 2). Worlds are sparse and lazily loaded. Original NBT is preserved, but reused
  verbatim only under the strict conditions of the build specification §4.1.
- Translation is two-stage: a palette stage, then a context stage over only context-dependent positions. The
  context stage reads from an immutable snapshot, so output is the same for any thread count.
- Significant decisions are recorded as ADRs in `docs/adr/NNNN-title.md` (context, options, decision,
  consequences).

## Consequences

- Adding a format is O(1) work in the number of formats.
- The IR must represent everything both editions can express, or report what it cannot. That makes it larger
  than any single format.
- Module boundaries follow the dependency rule in `AGENTS.md`, enforced in CMake.
