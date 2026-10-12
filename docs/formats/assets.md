# Resource packs: blockstates, models and textures (viewer input)

The 3D viewer draws blocks with the user's own Minecraft Java assets (no Mojang assets are in the repository or
the release). Reader: `src/render/assets.cpp` (files), `src/render/block_models.cpp` (baking),
`src/render/entity_models.cpp` (blocks the game draws in code), `src/render/mesher.cpp` (meshes).

Sources: Minecraft Wiki, [Tutorials/Models](https://minecraft.wiki/w/Tutorials/Models) and
[Resource pack](https://minecraft.wiki/w/Resource_pack) (checked 2026-10-12); the files of the pinned client jar
(Java 26.3, `%APPDATA%/.minecraft/versions/26.3/26.3.jar`), read locally and never committed. Where the wiki is
silent, the jar's files and in-game comparison decided (noted below).

## Where assets come from

- **Packs**, lowest priority first: the client jar of the pinned version, then the user's resource packs
  (zip or folder). A file is taken from the highest-priority pack that has it (`AssetSource::read`).
- The jar is found at `%APPDATA%/.minecraft/versions/<v>/<v>.jar` (`AssetSource::find_client_jar`), or chosen
  in the app's settings / with `--client-jar`.
- Zip reading uses minizip-ng with the same limits as other untrusted zips (`io::ZipReader`). Every JSON file is
  untrusted input: unknown fields are ignored, bad files make the block fall back to the missing model and are
  listed in `BlockModels::missing()`.

## Blockstates (`assets/minecraft/blockstates/<id>.json`)

- `variants`: keys are comma-separated `property=value` lists (the empty key matches every state). A state
  matches a key when every listed property has that value. The value is one model or a list of weighted
  alternatives (`weight`, default 1); the mesher picks one per position from a hash of the position.
- `multipart`: a list of `{when?, apply}`; every part whose condition holds is added. `when` is an object of
  `property: value` pairs (all must hold), where a value may list alternatives with `|`, or `{"OR": [...]}` /
  `{"AND": [...]}` of such objects.
- Model references: `model`, `x` and `y` (rotation in steps of 90°), `uvlock`.

## Models (`assets/minecraft/models/<path>.json`)

- `parent` chains are followed (depth-limited); `textures` merge child over parent; `elements` come from the
  nearest model that has them.
- Texture variables (`#name`) resolve through the merged `textures` map (depth-limited). **26.x:** a texture
  value may be an object `{"sprite": "block/glass", "force_translucent": true}` instead of a string (seen in the
  26.3 jar; not yet on the wiki). Both forms are read.
- `elements`: `from`/`to` in 1/16 block, optional `rotation {origin, axis, angle, rescale}`, `shade`, and
  `faces` with `uv` (default: from the element's position), `texture`, `cullface`, `rotation` (UV rotation in
  steps of 90°) and `tintindex`.
- Variant rotation: `x` first, then `y`, clockwise looking from +X / from above (x=90 turns up toward north,
  y=90 turns north toward east). `cullface` and occlusion turn with the model.
- `uvlock` is approximated by world-aligned UVs on rotated faces (exact for full cubes, which is where it is
  used).

## Textures (`assets/minecraft/textures/<path>.png`)

- PNG via stb_image. Block textures are 16 × 16; animated strips (taller than wide) use their first frame;
  larger textures (HD packs) are resampled to 16 × 16 for the texture array.
- **Render layer** per block, from its textures' alpha (the game's hard-coded layer lists are code, not
  assets): translucent if `force_translucent` or more than 30% of visible pixels are partly transparent
  (alpha < 250); cutout if any pixel is clear or partly transparent; otherwise solid. The 30% threshold keeps
  beds and chests (a few soft edge pixels) out of the translucent pass; checked in game for glass, stained
  glass, ice, leaves, beds and chests.
- Tints (`tintindex`): grass, foliage and water use fixed plains colours (the game computes them from biome
  data, which the viewer does not model); redstone uses its power level.

## Blocks the game draws in code

In 26.3, beds and signs are ordinary JSON models. Chests (normal, trapped, ender; single and double) and shulker
boxes still have particle-only models, so `entity_models.cpp` builds them from boxes textured with the entity
textures (`textures/entity/chest/*.png`, `textures/entity/shulker/*.png`, 64 × 64 nets). Box sizes and net
offsets were measured from the textures and compared in game:

- Chest: base 14 × 10 × 14 at net (0, 19), lid 14 × 5 × 14 at (0, 0), lock 2 × 4 × 1 at (0, 0). Double
  chests use 15-wide halves (`*_left.png` / `*_right.png`). The net is drawn upside down (the game flips the
  model), so up/down, north/south and west/east are swapped and the sides are mirrored vertically.
- Shulker box: base 16 × 8 × 16 at (0, 28), lid 16 × 12 × 16 at (0, 0) overlapping the base from y 4.

Banners, skulls, decorated pots and conduits are still drawn as their particle texture (listed for a later
phase).

## Fluids

Water and lava (and waterlogged blocks) are meshed by the mesher, not from models: height from the `level`
property (source and falling = full), top face lowered to 14/16, faces hidden against the same fluid and
against occluding neighbours. Textures `block/water_still` and `block/lava_still` (first frame).
