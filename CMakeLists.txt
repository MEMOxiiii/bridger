cmake_minimum_required(VERSION 3.28)

project(bridger
    VERSION 1.0.0
    DESCRIPTION "High-performance Minecraft format converter and 3D viewer"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset msvc-debug")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BRIDGER_BUILD_TESTS      "Build unit tests (Catch2)"                 ON)
option(BRIDGER_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)"       ON)
option(BRIDGER_BUILD_FUZZERS    "Build libFuzzer targets (clang-cl only)"   OFF)
option(BRIDGER_BUILD_TOOLS      "Build developer tools (tools/mapgen)"      ON)
option(BRIDGER_BUILD_CLI        "Build the command-line tool (bridger.exe)" ON)
option(BRIDGER_BUILD_APP        "Build the desktop app (bridger-gui.exe)"   ON)
option(BRIDGER_BUILD_RENDER     "Build the Vulkan renderer module"          ON)
option(BRIDGER_ENABLE_ASAN      "Build project code with AddressSanitizer"  OFF)
option(BRIDGER_ENABLE_TRACY     "Enable Tracy profiler instrumentation"     OFF)

if(BRIDGER_BUILD_APP AND NOT BRIDGER_BUILD_RENDER)
    message(FATAL_ERROR "BRIDGER_BUILD_APP requires BRIDGER_BUILD_RENDER")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static vcpkg triplet with the dynamic CRT (see docs/adr/0002-build-toolchain.md).
# The clangcl-fuzz preset overrides this with the static CRT, which libFuzzer needs.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(BridgerModule)
include(EmbedResource)
include(Shaders)

# ---------------------------------------------------------------------------
# Third-party dependencies (vcpkg manifest mode)
# ---------------------------------------------------------------------------
find_package(tl-expected CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(libdeflate CONFIG REQUIRED)
find_package(minizip-ng CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
if(BRIDGER_BUILD_CLI)
    find_package(CLI11 CONFIG REQUIRED)
endif()

if(BRIDGER_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

if(BRIDGER_BUILD_RENDER)
    find_package(volk CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(glm CONFIG REQUIRED)
    find_package(Stb REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    bridger_find_glslang()
endif()

if(BRIDGER_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(unofficial-webview2 CONFIG REQUIRED)
    find_package(wil CONFIG REQUIRED)
endif()

# leveldb-mcpe (git submodule, built by our own CMake file — see third_party/CMakeLists.txt)
add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Modules — dependency rule:
#   core <- platform <- io <- formats <- convert (+ translate) <- cli
#   render <- core + formats ; app <- everything
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/io)
add_subdirectory(src/formats)
add_subdirectory(src/translate)
add_subdirectory(src/convert)
if(BRIDGER_BUILD_CLI)
    add_subdirectory(src/cli)
endif()
if(BRIDGER_BUILD_RENDER)
    add_subdirectory(src/render)
endif()
if(BRIDGER_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(BRIDGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BRIDGER_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(BRIDGER_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

if(BRIDGER_BUILD_TOOLS)
    add_subdirectory(tools/mapgen)
endif()
