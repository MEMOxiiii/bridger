# Changelog

All notable changes to Bridger. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow the release plan in `docs/release.md`.

## [Unreleased]

### Added

- The desktop app on Linux and macOS (`docs/adr/0010`): the same interface and 3D preview as on Windows, in a GTK 4 +
  WebKitGTK window on Linux and an AppKit + WKWebView window on macOS (`Bridger.app`, MoltenVK inside). Fly mode uses
  pointer lock there. Updates are announced with the release page (no self-install). Texture download and the update
  check use the system's libcurl.
- `bridger-gui --smoke-test[=seconds] [file]`: starts the app, opens the file in the 3D preview and exits with 0 when
  everything came up; CI runs it on Linux and macOS.
- `bridger render` on Linux and macOS.

### Fixed

- 3D preview: a section replaced before its upload reached the GPU had its buffer freed while a copy into it was
  still queued, and a removed section's buffer could be freed while the previous frame still drew from it (found by
  the Vulkan validation layers; a crash on Mesa's software driver). The shaders' `discard` now has the Vulkan 1.3
  feature it needs enabled, and offscreen rendering no longer enables the swapchain extension.

## [1.2.0] - 2026-10-19

### Added

- The command line (`bridger info`, `bridger convert`) runs on Linux and macOS, with every format, world and option
  of the Windows version except `bridger render`. Releases include `bridger-<version>-linux-x64.tar.gz` (glibc 2.38
  or newer: Ubuntu 24.04, Debian 13, Fedora 39 and later) and `bridger-<version>-macos-arm64.tar.gz` (Apple Silicon,
  macOS 13.3 or newer, not signed), each with a `.sha256` file. Output is committed the same way: a temp file or folder
  next to the destination, flushed, validated, then renamed without replacing (`renameat2(RENAME_NOREPLACE)` on
  Linux, `renamex_np(RENAME_EXCL)` on macOS) and the folder synced (`docs/safety.md`). CI builds and tests Ubuntu
  24.04 (release and ASan + UBSan) and macOS 15.

### Known limitations

- Linux and macOS: the command line only (no desktop app, no `bridger render`). The macOS binary is not signed or
  notarized: after downloading, run `xattr -d com.apple.quarantine bridger` once, or macOS refuses to start it. No
  Intel Mac build.

## [1.1.0] - 2026-10-19

### Added

- Item data in containers converts to Bedrock: enchantments (also on books), damage, names and lore, repair cost,
  potions and tipped arrows, written and writable books, shulker box and bundle contents, fireworks, armor trims and
  dye colours, goat horns, ominous bottles, suspicious stews and banner patterns. Whatever an item carries that
  Bedrock cannot hold is reported by name; the item itself is kept.
- Block entities: unopened loot chests and suspicious sand/gravel keep their loot table; spawners keep their mob and
  timings; brewing stands, chiseled bookshelves, crafters, decorated pots (sherds and item), campfires, lecterns and
  jukeboxes keep what they hold; furnaces their progress; beacons their effects; command blocks their command.
  Sculk blocks, trial spawners, vaults, beehives, creaking hearts and end portals are written as Chunker writes
  them; bees in hives and a vault's rewarded players are reported.

- The Nether and the End convert in both directions and within an edition (Java `DIM-1`/`DIM1` or
  `dimensions/minecraft/the_nether`/`the_end`; Bedrock dimensions 1 and 2). Blocks on a Java Nether roof (y 128 and
  up) have no place in Bedrock's Nether (y 0..127): they are dropped and reported. A server's separate
  `world_nether`/`world_the_end` folders are reported, not read (as in Chunker). `bridger info` counts the chunks of
  every dimension.

- Item frames (with their item and rotation) and paintings convert in both directions, also in old worlds whose
  entities are kept in the chunks; Bedrock stores frames as blocks and paintings as actors. Every other entity is
  reported by type instead of one warning; Bedrock → Bedrock copies all actors.
- The single-player player converts in both directions: position, rotation, dimension, game mode, inventory, armor
  and offhand. Ender chest, experience and effects are reported, as in Chunker.

- Bedrock → Java now keeps item data too (enchantments, damage, names, potions, tipped arrows, books, shulker box and
  bundle contents, fireworks, trims, goat horns, banner patterns) and the block entities added for Java → Bedrock
  (loot tables, spawners, brewing stands, chiseled bookshelves, crafters, decorated pots, campfires, beacons,
  lecterns, jukeboxes, command blocks, suspicious sand, furnace progress, sculk blocks, vaults, trial spawners).
- Older Java targets get item data in their own shape: the 1.20.5 – 1.21.4 components, or the old `tag` before 1.20.5
  (previously dropped), and the older forms of furnaces, brewing stands, decorated pots and beacons.
- The desktop app updates itself: once a day it asks GitHub for the latest release (Settings → Updates turns this off;
  "Check now" asks at once), offers it with its notes, and only after the user agrees downloads it, checks its
  SHA-256, swaps the program's files with rollback and restarts. Releases now publish a `.sha256` file next to the zip.

### Fixed

- Bedrock → Java: potted dead bushes (Bedrock names the plant `deadbush`) and living sea pickles in water (reported as
  losing their water although the Java state is waterlogged).
- Items with names from before 1.21.9 (`chain`) or older (`scute`, `grass`, ...) in old worlds were dropped.

### Known limitations

- Entities: item frames and paintings convert; mobs, villagers, armor stands and minecarts are not written yet (listed
  in the report by type). A server's `playerdata` and its separate `world_nether` / `world_the_end` folders are not
  read (as in Chunker); Bridger says so.
- Bedrock's Nether ends at y 127: blocks on a Java Nether roof are dropped and reported.
- The self-updater needs a writable program folder; 1.0.0 has no updater, so 1.1.0 is installed by hand once.
- The Nether/End, item frames, paintings and the player still await their in-game check (`docs/testing.md`, rows 19
  and 20). Windows x64 only; portable zip only.

## [1.0.0] - 2026-10-16

### Added

- Old Bedrock worlds: every LevelDB world from Pocket Edition 0.9 to 26.52 is read, including the pre-1.0
  `LegacyTerrain` column, numeric-id subchunks (1.0 – 1.2.x), 1.2.13 palettes and 2D biomes. Old block states
  are upgraded to 26.50 with the BedrockBlockUpgradeSchema data (CC0), so old palettes no longer read as air.
- Bedrock world folders (from `minecraftWorlds`) open and convert like `.mcworld` files; only a copy of the
  database is opened.
- Old Java worlds: every layout since McRegion (Beta 1.3) is read: 1.13 – 1.17 palettes (both packings), 1.2 – 1.12
  numeric ids (`Blocks`/`Data`/`Add`) and McRegion columns, with numeric biome ids. Renamed blocks and biomes are
  upgraded to 26.3 (in `.schem`, `.nbt` and `.litematic` files too), and the states 1.13 moved out of block
  entities (doors, tall plants, beds, flower pots, skulls, banners, note blocks) are restored. Alpha worlds get a
  message explaining how to convert them first.

- Distant terrain in the 3D preview: simplified land beyond the render distance (default 32 chunks; Settings,
  "Distant terrain"; `bridger render --distant N`), cached on disk so reopening a world shows it without decoding
  those chunks again.

- The game version each file was saved by is shown on the convert page and by `bridger info` ("Saved by Java 1.20.1",
  "Bedrock 1.21.50", "before 1.13", ...), for every format.
- Target versions: conversions can be written for any Java release from 1.18 to 26.3 and any Bedrock version from
  1.18.30 to 26.50 ("Game version" list on the convert page, `bridger convert --target-version`, `bridger versions`).
  Blocks, biomes, block entities, item names and the world files follow the target; whatever the target cannot hold is
  reported. Block data from Mojang's data generator (Java) and BedrockBlockPaletteArchive (Bedrock, CC0).
- Same-edition conversion: a `.mcworld` or Bedrock world folder to a `.mcworld`, a Java world to a Java world folder,
  and any structure format to another of its edition, for any target version, up or down.
- Blocks newer than the target: Bedrock keeps them (the older game shows its "unknown" block and the block returns in
  a newer version); Java gets the closest block it has (cherry → birch, crafter → dropper, ...). Both are reported.

### Changed

- A conversion is refused when its output is the input, lies inside an input world folder or is a folder containing
  the input (also with `--overwrite`).
- Legacy `.schematic` files: flower pot contents, skull types and banner colours are converted now, and door
  halves are combined.

### Known limitations

- Entities (mobs, armor stands, item frames, paintings) are not converted; they are listed in the report.
- Worlds: the Overworld only, in both directions and within an edition.
- Older target versions are checked against each version's exact block data; the in-game checks cover the pinned
  versions and the releases listed in `docs/testing.md`. Experimental Bedrock blocks are not switched on in an older
  Bedrock world.
- Portable zip only (no installer); Windows x64 only.

## [0.4.2] - 2026-10-15

### Added

- Without Minecraft Java installed, the desktop app offers to download the 26.3 client jar (about 40 MB) for the 3D
  preview from Mojang's official servers, only when the user presses the button: https to Mojang's hosts only,
  checked against Mojang's published SHA-1 and size, saved in Bridger's own folder, never executed, and
  cancellable (`docs/safety.md`, "Network access").

### Fixed

- The desktop app's file page can open a Java world folder again: "Open another" offers a file or a Java world
  folder (Ctrl+O / Ctrl+Shift+O). Choosing a folder that is not a world says so (no `level.dat`).

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

[1.1.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v1.1.0
[1.0.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v1.0.0
[0.4.2]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.4.2
[0.4.1]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.4.1
[0.4.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.4.0
[0.3.0]: https://github.com/MEMOxiiii/bridger/releases/tag/v0.3.0
