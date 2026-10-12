# Bridger

High-performance Java ⇄ Bedrock converter for Minecraft schematics, structures and worlds, with a 3D viewer.

Moving builds between Minecraft Java Edition and Bedrock Edition is painful. Existing tools are slow on large
inputs, lose data silently (block orientations, waterlogging, chest contents), and handle the most common
request badly: **taking a Java schematic and getting a working Bedrock world** (`.schem` → `.mcworld`). Bridger
is a native Windows tool written in C++20. It aims to be faster and more reliable, and every claim is backed by
tests, benchmarks and in-game checks. It never modifies your input files, and it reports anything it could not
convert exactly.

> **Status: 0.3.0.** `bridger convert` works in both directions: Java schematics (`.schem`, `.litematic`,
> structure `.nbt`, legacy `.schematic`) and whole Java worlds become Bedrock worlds (`.mcworld`) or `.mcstructure`
> files, a structure can be placed into a copy of an existing Bedrock world, and `.mcstructure` files and Bedrock
> worlds become Java schematics and Java worlds. Every format passed the automated tests, fuzzing, the independent
> checks and the in-game checklist in Java 26.3 and Bedrock 26.52. The 3D viewer and desktop app come next (0.4.0).

## Format support

| Format | Edition | Read | Write |
|---|---|---|---|
| `.schem` (Sponge v1–v3; writes v3) | Java | supported | supported |
| `.schematic` (MCEdit legacy) | Java | supported | — |
| `.litematic` | Java | supported | supported |
| Structure `.nbt` | Java | supported | supported |
| Java world (Anvil, 1.18+ layout, Overworld) | Java | supported | supported |
| `.mcstructure` | Bedrock | supported | supported |
| `.mcworld` (new world; reading: Overworld) | Bedrock | supported | supported |
| `.mcworld` (place into existing world → new output) | Bedrock | — | supported |

"Supported" means: golden tests, a fuzz target, a benchmark, and a recorded in-game check all pass. Run
`bridger formats` to see what your build supports.

**Game versions:** v1 targets exactly **Java Edition 26.3** and **Bedrock Edition 26.52**
([docs/versions.md](docs/versions.md)).

## Screenshots

_Coming with the 3D viewer (0.4.0)._

## Installation

Download `bridger-<version>-windows-x64.zip` from the [releases page](https://github.com/MEMOxiiii/bridger/releases)
and unzip it anywhere. Double-click `bridger-gui.exe` for the desktop app, or run `bridger.exe` from a terminal.
No installer; the Visual C++ runtime DLLs are included. The app needs a GPU with Vulkan 1.3 and the Microsoft
Edge WebView2 Runtime (part of Windows 11 and current Windows 10).

### Building from source (Windows x64)

Requirements:
- Visual Studio 2022 17.10+ with "Desktop development with C++". The optional component "C++ Clang Compiler
  for Windows" is needed for the clang-cl presets.
- CMake ≥ 3.28 and Ninja (both ship with Visual Studio).
- [vcpkg](https://github.com/microsoft/vcpkg), checked out at tag `2026.07.29`, with `VCPKG_ROOT` set.
- [Node.js](https://nodejs.org) 22 or newer (npm builds the desktop app's interface).

```bat
git clone --recursive https://github.com/MEMOxiiii/bridger.git
cd bridger
:: from an "x64 Native Tools Command Prompt for VS 2022".
:: Set VCPKG_ROOT *here*: the prompt points it at Visual Studio's bundled vcpkg, which is too old.
set VCPKG_ROOT=%USERPROFILE%\vcpkg
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The first configure builds all dependencies with vcpkg, which takes a while. Presets: `msvc-debug`,
`msvc-release`, `msvc-asan`, `clangcl-release`, `clangcl-fuzz`.

## Usage

### Desktop app

`bridger-gui.exe` opens a file or world you drop on its window (or pass on the command line) in a 3D view drawn
with your own Minecraft Java textures, shows what is in it and what would not convert exactly, and converts it:

- **View:** orbit, pan and zoom; **fly** through it like a player (double-click or F, then WASD, Space/Shift,
  Ctrl to sprint, Esc to stop); a top-down **map** (M); a **slice** to look inside; worlds stream around the
  camera.
- **Convert:** choose the format (Bedrock or Java), a name, where to put it (a new world, or a copy of one of
  your worlds at a position), and where to save it. Progress can be cancelled; nothing is written then.
- **Report:** everything that was not converted exactly, grouped and counted. Clicking an entry outlines those
  blocks in the 3D view and flies the camera there.

The textures come from `%APPDATA%/.minecraft/versions/26.3/26.3.jar` (install that version once with the
launcher) or a client jar you choose in Settings, plus any resource packs you add. Converting works without them.

### Command line

```text
bridger --version
bridger formats                                   # list formats and their support status
bridger info <file or world folder>               # describe any supported structure file or world
bridger convert <input> <output> [options]        # any supported pair; see "bridger formats"
bridger render <file or world> out.png [--size WxH --map --slice Y --pack p.zip]   # screenshot of the 3D view
    --target-version X   --fallback air|marker   --report report.json   --format <name>
    --into-world <existing.mcworld>   --place-at x,y,z   --air keep|replace
    --clear-entities   --clip | --shift-to-fit   --threads N   --overwrite
```

Example:

```text
bridger convert castle.schem castle.mcworld --report castle-report.json
bridger convert castle.litematic castle.mcstructure
bridger convert "C:\saves\My Java World" my-world.mcworld
bridger convert tower.nbt out.mcworld --into-world survival.mcworld --place-at 120,64,-40
bridger convert house.mcstructure house.schem
bridger convert my-bedrock-world.mcworld "C:\saves\Converted World"     # no extension: a Java world folder
```

A structure converted to a new world is placed on a superflat world, centered on x = 0, z = 0, and the world
opens in creative mode. A Java world keeps its seed, spawn and biomes; chunks it never generated are generated by
Bedrock. `--into-world` never modifies the existing world: it writes a new world in which only the chunks the
structure touches differ. The conversion report lists everything that was not converted exactly: entities (not
written yet), item enchantments and names, text formatting on signs, and so on.

The output format is inferred from the file extension. Defaults are chosen for safety: air in the structure
keeps existing blocks, and a structure that exceeds the world's build height is an error unless you pass
`--clip` or `--shift-to-fit` ([docs/safety.md](docs/safety.md)).

## Performance

Bridger publishes only **measured** results, with machine specs and reproduction steps. The full table is in
[docs/performance.md](docs/performance.md). Early baselines on an Intel i7-13620H laptop (`msvc-release`):

| Operation | Result |
|---|---|
| NBT validation (zero-copy) | 1.37 GiB/s |
| Reading a gzip `.schem` into the IR (256 × 128 × 256) | 155 M blocks/s (54.7 ms) |
| Translating Java → Bedrock (8.4 M blocks) | 34.4 ms |
| A real 405 × 140 × 405 WorldEdit schematic → `.mcworld` (incl. validation) | 2.7 s |
| A synthetic 4096-chunk Java world → `.mcworld` (16 threads, incl. validation) | 9.2 s |
| 3D viewer: a real Java world streamed in at render distance 20 (1257 chunks, 7.6 M quads; RTX 4060 Laptop) | 2.3–3.0 s, then 60 fps (vsync) |

These measure Bridger alone. No comparison with other tools has been measured yet.

## Known limitations

- Worlds: only the Overworld, in both directions; Java worlds saved by Java 1.18 or newer.
- A Java world folder is never overwritten: the destination folder must not exist.
- Bedrock-only blocks (Education Edition chemistry, item frames as blocks, …) have no Java equivalent: they are
  replaced and reported. Fence and pane connections rebuilt for Java use an approximation of which blocks are
  solid; the game corrects any wrong one on the next block update.
- Entities (armor stands, item frames, mobs) are not written yet; they are listed in the report.
- Item components (enchantments, custom names, durability) and text formatting are dropped and reported.
- Only whole-world conversion uses several threads; structures are converted on one thread.
- Windows x64 only for now. The code keeps OS-specific parts in one module so other platforms can be added
  later.
- One Java and one Bedrock version at a time (see above).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md).

## License

[Mozilla Public License 2.0](LICENSE). Third-party components and their licenses:
[THIRD_PARTY.md](THIRD_PARTY.md).

## Disclaimer

Bridger is not an official Minecraft product. It is not approved by or associated with Mojang or Microsoft.
Bridger does not include or distribute any Minecraft assets. Textures and models are loaded at runtime from your
own Minecraft installation or a resource pack you provide.
