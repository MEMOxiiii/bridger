# Changelog

All notable changes to Bridger. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow the release plan in `docs/release.md`.

## [0.4.1] - 2026-10-14

### Fixed

- `--report` can no longer overwrite the input, the output, a file inside an output world folder or the world given
  to `--into-world`; the path is checked before converting, and an existing report is replaced only with
  `--overwrite`.
- The report's `"exact"` is `true` only when nothing at all was lost: warnings (skipped dimensions, entities,
  partially generated chunks) now count. The new `"blocks_exact"` keeps the blocks-only answer. The desktop app
  and the CLI summary no longer call such a result "exact": they say the translation recorded no problems but
  other warnings describe skipped or dropped data.
- Cancelling is checked one last time after the output is validated and right before it is moved into place, on
  every output path (files and Java world folders), through a gate the atomic writer calls exactly once. A
  cancelled conversion never leaves output behind and an overwritten file is kept; the move itself is the point
  of no return, and a cancel after it can no longer undo a finished replacement (`docs/safety.md`).
- Water and lava in the 3D preview: the surface is continuous and slopes like the game's (corner heights averaged
  from the neighbouring blocks), without holes between different levels, and stacked water faces no longer
  darken each other.

### Changed

- The desktop app works without a Vulkan 1.3 GPU: opening and converting files work, and the 3D preview says it is
  unavailable instead of the app refusing to start.

## [0.4.0] - 2026-10-14

### Added

- **Desktop app `bridger-gui.exe`.** It opens on the Bridger wordmark, then a home screen to drop or choose a
  file. Each opened file gets a page with its facts, what could not be read exactly, and the conversion options;
  the 3D preview is optional, one click away, and returns to the same file page. In detail:
  - a convert drawer with every supported target format, a world name, placement (a new flat world, or a copy of
    one of your worlds at x, y, z), advanced placement options, a Save dialog, progress and Cancel;
  - the report of everything not converted exactly, grouped and counted; clicking an entry outlines those blocks
    in the 3D view and flies the camera there;
  - orbit, pan and zoom; Minecraft-style fly mode (WASD, Space/Shift, Ctrl, mouse look, any keyboard layout);
    a top-down map; a slice to look inside; worlds stream around the camera;
  - settings for the textures' client jar and resource packs; recent files.
  The interface is React + GSAP + Lenis in WebView2, composited with real transparency over the native Vulkan
  viewer (`docs/adr/0009-hybrid-desktop-ui.md`); it follows the system's reduced-motion setting.
- **3D viewer** (Vulkan 1.3): block models from your own Minecraft Java installation and resource packs
  (blockstate variants and multipart, parent models, rotations, UV lock, tints), chests and shulker boxes built
  from their entity textures, water and lava, smooth lighting, translucent sorting, fog, frustum culling, and
  world streaming around the camera (`docs/formats/assets.md`).
- `bridger render <file or world> out.png` for screenshots (`--size`, `--yaw`, `--pitch`, `--zoom`, `--map`,
  `--slice`, `--radius`, `--pack`, `--client-jar`).
- Conversions report progress and can be cancelled (library API: `ConvertOptions::progress`, `cancel`); a
  cancelled conversion leaves no output behind.

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

[0.4.1]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.4.1
[0.4.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.4.0
[0.3.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.3.0
