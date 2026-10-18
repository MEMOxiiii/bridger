# Bridger documentation

## Project

| Document | What it covers |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute (start with [AGENTS.md](../AGENTS.md)) |
| [THIRD_PARTY.md](THIRD_PARTY.md) | Every dependency, its license, and whether it ships |
| [release.md](release.md) | Release plan and the checklist for each release |
| [versions.md](versions.md) | The pinned Java and Bedrock versions |

## Engineering

| Document | What it covers |
|---|---|
| [safety.md](safety.md) | How outputs are written without ever corrupting or losing user data |
| [testing.md](testing.md) | Test layers, the in-game checklist and the desktop app checklist, with run logs |
| [performance.md](performance.md) | Benchmarks, baselines and how to measure |
| [app/bridge.md](app/bridge.md) | Messages between the desktop app's interface and its native core |
| [app/updates.md](app/updates.md) | How the desktop app finds, verifies and installs a new version |

## Architecture decisions

| ADR | Decision |
|---|---|
| [0001](adr/0001-record-architecture-decisions.md) | Recording architecture decisions |
| [0002](adr/0002-build-toolchain.md) | Build toolchain |
| [0003](adr/0003-error-handling.md) | Error handling |
| [0004](adr/0004-leveldb-mcpe-fork.md) | LevelDB fork for Bedrock worlds |
| [0005](adr/0005-rendering-and-ui-stack.md) | Rendering stack (Vulkan 1.3) |
| [0006](adr/0006-in-house-nbt-and-libraries.md) | In-house NBT and library choices |
| [0007](adr/0007-mapping-data-sources.md) | Block mapping data sources |
| [0008](adr/0008-translation-design.md) | Translation design |
| [0009](adr/0009-hybrid-desktop-ui.md) | Desktop app: web UI in WebView2 over the native viewer |

## File formats

Byte-level notes with a source for every detail ([formats/README.md](formats/README.md) lists the supported
conversion paths):

- Java: [Sponge `.schem`](formats/sponge-schem.md), [legacy `.schematic`](formats/legacy-schematic.md),
  [`.litematic`](formats/litematic.md), [structure `.nbt`](formats/structure-nbt.md), [Anvil worlds](formats/anvil.md)
- Bedrock: [`.mcstructure`](formats/mcstructure.md), [`.mcworld` / LevelDB](formats/bedrock-world.md)
- Shared: [NBT](formats/nbt.md), [resource packs and block models](formats/assets.md) (viewer input)

## Other folders

- [mappings/](mappings/) — generated coverage reports of the Java ⇄ Bedrock block mapping.
- [assets/](assets/) — the logo and the README screenshots.
