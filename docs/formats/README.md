# Format documentation

One file per format. **Every binary detail cites its source**: minecraft.wiki technical pages, Mojang
documentation or sample data, or a well-established open-source implementation (named, with a link and its
license). If the sources do not settle a detail, it is verified by experiment in the game, and the experiment is
described here.

Template for each `<format>.md`:

```
# <Format name>
Editions/versions covered: ...
## Container        (compression, magic bytes, endianness)
## Structure        (NBT schema / binary layout, field by field)
## Block data       (palette, packing, layers)
## Block entities / entities / biomes
## Limits we enforce on input
## Open questions   (verified by experiment? date, game version, result)
## Sources          (numbered list, cited inline as [1], [2], ...)
```

| Format | Doc | Phase |
|---|---|---|
| Bedrock world (`level.dat`, LevelDB keys, sub-chunks) | `bedrock-world.md` | 0.5 |
| NBT (Java BE / Bedrock LE) | `nbt.md` | 1 |
| Sponge `.schem` | `sponge-schem.md` | 2 |
| `.mcstructure` | `mcstructure.md` | 5 |
| Structure `.nbt` | `structure-nbt.md` | 5 |
| `.litematic` | `litematic.md` | 5 |
| Legacy `.schematic` | `legacy-schematic.md` | 5 |
| Java Anvil | `anvil.md` | 5 |
