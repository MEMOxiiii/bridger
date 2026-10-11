# Bridger

High-performance Java ⇄ Bedrock converter for Minecraft schematics, structures and worlds, with a 3D viewer.

Moving builds between Minecraft Java Edition and Bedrock Edition is painful. Existing tools are slow on large
inputs, lose data silently (block orientations, waterlogging, chest contents), and handle the most common
request badly: **taking a Java schematic and getting a working Bedrock world** (`.schem` → `.mcworld`). Bridger
is a native Windows tool written in C++20. It aims to be faster and more reliable, and every claim is backed by
tests, benchmarks and in-game checks. It never modifies your input files, and it reports anything it could not
convert exactly.

> **Status: early development (Phase 0, foundation).** Bridger cannot convert anything yet. The first public
> release, **0.1.0**, will convert `.schem` → new `.mcworld` from the command line.

## Format support

| Format | Edition | Read | Write |
|---|---|---|---|
| `.schem` (Sponge v1–v3) | Java | planned (0.1.0) | planned (0.3.0) |
| `.schematic` (MCEdit legacy) | Java | planned (0.2.0) | — |
| `.litematic` | Java | planned (0.2.0) | planned (0.3.0) |
| Structure `.nbt` | Java | planned (0.2.0) | planned (0.3.0) |
| Java world (Anvil, 1.18+ layout) | Java | planned (0.2.0) | planned (0.3.0) |
| `.mcstructure` | Bedrock | planned (0.3.0) | planned (0.2.0) |
| `.mcworld` (new world) | Bedrock | planned (0.3.0) | planned (**0.1.0**) |
| `.mcworld` (place into existing world → new output) | Bedrock | — | planned (0.2.0) |

"Supported" will mean: golden tests, a fuzz target, a benchmark, and a recorded in-game check all pass. Run
`bridger formats` to see what your build supports.

**Game versions:** v1 targets exactly **Java Edition 26.3** and **Bedrock Edition 26.52**
([docs/versions.md](docs/versions.md)).

## Screenshots

_Coming with the 3D viewer (0.4.0)._

## Installation

There are no binary releases yet. 0.1.0 will ship as a portable zip.

### Building from source (Windows x64)

Requirements:
- Visual Studio 2022 17.10+ with "Desktop development with C++". The optional component "C++ Clang Compiler
  for Windows" is needed for the clang-cl presets.
- CMake ≥ 3.28 and Ninja (both ship with Visual Studio).
- [vcpkg](https://github.com/microsoft/vcpkg), checked out at tag `2026.07.29`, with `VCPKG_ROOT` set.

```bat
git clone --recursive https://github.com/MEMOxiiii/bridger.git
cd bridger
:: from an "x64 Native Tools Command Prompt for VS 2022":
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The first configure builds all dependencies with vcpkg, which takes a while. Presets: `msvc-debug`,
`msvc-release`, `msvc-asan`, `clangcl-release`, `clangcl-fuzz`.

## Usage

```text
bridger --version
bridger formats                                   # list formats and their support status
bridger info <file>                               # (Phase 2)
bridger convert <input> <output> [options]        # (Phase 4)
    --target-version X   --fallback air|marker   --report report.json   --format <name>
    --into-world <existing.mcworld>   --place-at x,y,z   --air keep|replace
    --clear-entities   --clip | --shift-to-fit
```

The output format is inferred from the file extension. Defaults are chosen for safety: air in the structure
keeps existing blocks, and a structure that exceeds the world's build height is an error unless you pass
`--clip` or `--shift-to-fit` ([docs/safety.md](docs/safety.md)).

The desktop app (`bridger-gui.exe`: drag and drop, 3D preview, convert, report) is planned for 0.4.0.

## Performance

No numbers yet. Bridger publishes only **measured** results, with machine specs and reproduction steps
([docs/performance.md](docs/performance.md)).

## Known limitations

- Nothing converts yet (Phase 0).
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
