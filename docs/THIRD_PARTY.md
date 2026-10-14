# Third-party components

Every dependency is listed here with its license and the reason it is used. Licenses were taken from each
package's vcpkg port metadata (baseline `2026.07.29`) and the upstream repository. Check them again whenever a
dependency is added or upgraded.

"Shipped" means the component's code ends up in a Bridger release binary. Build/test-only tools are not
distributed. Release zips include the license texts of all shipped components (`docs/release.md`).

## Libraries (vcpkg)

| Component | Version | License | Shipped | Used for |
|---|---|---|---|---|
| [libdeflate](https://github.com/ebiggers/libdeflate) | 1.25 | MIT | yes | Fast whole-buffer deflate/zlib/gzip for chunks and NBT |
| [minizip-ng](https://github.com/zlib-ng/minizip-ng) | 4.1.0 | Zlib | yes | Reading/writing `.mcworld` zips and Minecraft `.jar` files. Built with the `zlib` feature only (no bzip2/lzma/zstd/crypto: not needed for these formats) |
| [zlib](https://www.zlib.net/) | 1.3.2 | Zlib | yes | Required by leveldb-mcpe (Bedrock's raw-deflate block compression) and minizip-ng |
| [oneTBB](https://github.com/oneapi-src/oneTBB) | 2023.1.0 | Apache-2.0 | yes | Parallel pipeline: task arena, `parallel_for`, concurrent containers |
| [tl-expected](https://github.com/TartanLlama/expected) | 1.3.1 | CC0-1.0 | yes | `bridger::Result<T>` |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.3 | BSD-2-Clause | yes | Registry interning, cache keys |
| [spdlog](https://github.com/gabime/spdlog) | 1.17.0 | MIT | yes | Logging |
| [{fmt}](https://github.com/fmtlib/fmt) | (spdlog dependency) | MIT | yes | Formatting backend of spdlog |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.6.2 | BSD-3-Clause | yes | CLI argument parsing |
| [SDL3](https://www.libsdl.org) (feature `vulkan`) | 3.4.12 | Zlib AND MIT AND Apache-2.0 | yes (app) | Windowing, input, file dialogs, drag and drop |
| [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) | 1.0.3800.47 | BSD-3-Clause | yes (app; the static loader) | Hosts the HTML user interface (`docs/adr/0009`). The WebView2 *Runtime* is part of Windows and is not shipped |
| [WIL](https://github.com/microsoft/wil) | 1.0.260126.7 | MIT | yes (app, header-only) | COM smart pointers for WebView2 and DirectComposition |
| [volk](https://github.com/zeux/volk) | 1.4.350.1 | MIT | yes (app) | Vulkan function loader |
| [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) | 1.4.350 | MIT | yes (app) | Vulkan instance/device setup |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.4.0 | MIT | yes (app) | GPU memory allocation |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.350 | Apache-2.0 OR MIT | yes (headers) | Vulkan API declarations |
| [GLM](https://glm.g-truc.net) | 1.0.3 | MIT | yes | Math |
| [stb](https://github.com/nothings/stb) (`stb_image`, `stb_image_write`) | 2024-07-29 | MIT OR Public Domain | yes | Loading textures from the user's resource packs, writing screenshots |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | yes | Resource-pack blockstate/model JSON (render), mapping generator. Never in conversion hot paths |
| [glslang](https://github.com/KhronosGroup/glslang) | 16.4.0 | BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 (mixed; see upstream `LICENSE.txt`) | no (build tool) | GLSL → SPIR-V at build time |
| [Catch2](https://github.com/catchorg/Catch2) | 3.15.3 | BSL-1.0 | no (tests) | Unit tests |
| [Google Benchmark](https://github.com/google/benchmark) | 1.9.5 | Apache-2.0 | no (bench) | Benchmarks |
| [Tracy](https://github.com/wolfpld/tracy) | 0.13.1 | BSD-3-Clause | only with `BRIDGER_ENABLE_TRACY=ON` (never in releases) | Profiling |

`vulkan-1.dll` is never linked or shipped: volk loads the GPU driver's loader at runtime.

## Web user interface (npm, embedded in `bridger-gui.exe`)

Built from `src/app/ui` with the versions pinned in `src/app/ui/package-lock.json`, bundled by Vite and
embedded into the executable (`src/app/ui_bundle.cpp`).

| Component | Version | License | Shipped | Used for |
|---|---|---|---|---|
| [React](https://react.dev) + react-dom (+ scheduler) | 19.3.0 (scheduler 0.28.0) | MIT | yes (app) | UI components |
| [GSAP](https://gsap.com) + [@gsap/react](https://github.com/greensock/react) | 3.15.0 / 2.1.2 | GSAP Standard "no charge" license (<https://gsap.com/standard-license>) | yes (app) | Animation (timelines, Flip). The license allows free use in any project except tools that compete with Webflow's visual animation builder; Bridger is not one |
| [Lenis](https://github.com/darkroomengineering/lenis) | 1.3.26 | MIT | yes (app) | Smooth wheel scrolling in panels |
| [Inter](https://rsms.me/inter/) via @fontsource-variable/inter | 5.3.0 | OFL-1.1 | yes (app) | UI typeface |
| [JetBrains Mono](https://www.jetbrains.com/lp/mono/) via @fontsource/jetbrains-mono | 5.3.0 | OFL-1.1 | yes (app) | Numbers, ids, paths |
| [Vite](https://vite.dev) + @vitejs/plugin-react | 8.3.4 / 6.1.2 | MIT | no (build tool) | Bundling the UI |
| [TypeScript](https://www.typescriptlang.org) | 7.0.2 | Apache-2.0 | no (build tool) | Type checking the UI |

## Git submodules

| Component | Commit | License | Shipped | Used for |
|---|---|---|---|---|
| [leveldb-mcpe (Amulet-Team fork)](https://github.com/Amulet-Team/leveldb-mcpe) | `4846fc7` (2024-11-22) | BSD-3-Clause (Google LevelDB authors; Mojang modifications) | yes | Reading/writing Bedrock world databases. Mojang's original `Mojang/leveldb-mcpe` repository is no longer available (HTTP 404, checked 2026-10-11); see `docs/adr/0004-leveldb-mcpe-fork.md` |

## Mapping data (compiled into `src/translate/data/*.brmap`, shipped)

| Component | Version | License | Used for |
|---|---|---|---|
| [GeyserMC/mappings](https://github.com/GeyserMC/mappings) (git submodule `tools/mapgen/sources/geyser-mappings`) | `feature/26.3`, commit `01ec65e3` | MIT | Java → Bedrock block, item and biome mappings. GeyserMC notes it "does not relicense Minecraft, its data, or any other material owned by Mojang or Microsoft" |
| [GeyserMC/Geyser](https://github.com/GeyserMC/Geyser) `block_palette.26_50.nbt` (fetched, SHA-256 pinned) | commit `50a464d7` | MIT (repository) | Bedrock block palette: names, typed states, block version |
| Minecraft Java 26.3 data generator output (`blocks.json`, `registries.json`, `version.json`) | 26.3 | Mojang; generated locally from the official server jar, never committed or shipped | Java block state list and ids, item and biome registries |
| [PrismarineJS/minecraft-data](https://github.com/PrismarineJS/minecraft-data) `data/pc/common/legacy.json` (fetched, SHA-256 pinned) | commit `dd011274` | MIT | Pre-1.13 numeric block/item ids → 1.13 names, for legacy `.schematic` files. Post-1.13 renames are Bridger's own (`tools/mapgen/overrides/legacy_renames.json`) |

The `.brmap` blob contains block, item and biome **names and state values** (facts about the game, as GeyserMC
publishes them). It contains no Mojang code, textures, models or sounds.

## Tools (not shipped, not linked)

| Component | Version | License | Used for |
|---|---|---|---|
| [nbtlib](https://github.com/vberlier/nbtlib) | 2.0.4 | MIT | `tests/data/generate_test_data.py` writes the committed test files with it, so the NBT reader is tested against an independent implementation |
| [amulet-leveldb](https://pypi.org/project/amulet-leveldb/) | 1.0.7 | Amulet's license | **Test oracle only** in `tests/integration/validate_mcworld.py` (independent reading of written worlds). No code copied, not committed |
| [HiveGamesOSS/Chunker](https://github.com/HiveGamesOSS/Chunker) | commit `50aaf6d8` | MIT | **Consulted only** for Bedrock format behavior (`docs/formats/bedrock-world.md` cites the files). No code copied |
| [Litematica](https://github.com/sakura-ryoko/litematica) (branch LTS/26.2) | — | LGPL-3.0 | **Consulted only** for the `.litematic` layout (`docs/formats/litematic.md`). No code copied |
| [minizip-ng](https://github.com/zlib-ng/minizip-ng) | (see above) | Zlib | also used to write `.mcworld` zips |

## Data

Third-party data is limited to the mapping sources above, compiled into the `.brmap` blob.

## Minecraft assets

Bridger does **not** include or distribute any Mojang assets (textures, models, sounds, block data files). The
viewer loads them at runtime from the user's own Minecraft installation or a resource pack the user provides. If
Minecraft Java is not installed, the user can ask the app to download the client jar from Mojang's own servers, as
the official launcher does; it is stored on the user's computer only (`docs/safety.md`, "Network access").
