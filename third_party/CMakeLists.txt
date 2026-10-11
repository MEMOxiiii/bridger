# Third-party code that is not available through vcpkg.
# Warnings are NOT treated as errors here, and Bridger's warning flags are never applied.

# ---------------------------------------------------------------------------
# leveldb-mcpe — Bedrock's LevelDB fork (Amulet-Team/leveldb-mcpe, BSD-3-Clause).
# The upstream CMakeLists.txt builds a shared library for Amulet's Python wrapper
# and hardcodes a `zlibstatic` target, so we compile the same sources here as a
# static library linked against vcpkg's zlib. The submodule itself is untouched.
# See docs/adr/0004-leveldb-mcpe-fork.md.
# ---------------------------------------------------------------------------
set(LEVELDB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/leveldb-mcpe")
if(NOT EXISTS "${LEVELDB_DIR}/include/leveldb/db.h")
    message(FATAL_ERROR "third_party/leveldb-mcpe is missing. Run: git submodule update --init --recursive")
endif()

add_library(leveldb_mcpe STATIC
    ${LEVELDB_DIR}/db/builder.cc
    ${LEVELDB_DIR}/db/c.cc
    ${LEVELDB_DIR}/db/db_impl.cc
    ${LEVELDB_DIR}/db/db_iter.cc
    ${LEVELDB_DIR}/db/dbformat.cc
    ${LEVELDB_DIR}/db/filename.cc
    ${LEVELDB_DIR}/db/log_reader.cc
    ${LEVELDB_DIR}/db/log_writer.cc
    ${LEVELDB_DIR}/db/memtable.cc
    ${LEVELDB_DIR}/db/repair.cc
    ${LEVELDB_DIR}/db/table_cache.cc
    ${LEVELDB_DIR}/db/version_edit.cc
    ${LEVELDB_DIR}/db/version_set.cc
    ${LEVELDB_DIR}/db/write_batch.cc
    ${LEVELDB_DIR}/db/zlib_compressor.cc
    ${LEVELDB_DIR}/table/block.cc
    ${LEVELDB_DIR}/table/block_builder.cc
    ${LEVELDB_DIR}/table/filter_block.cc
    ${LEVELDB_DIR}/table/format.cc
    ${LEVELDB_DIR}/table/iterator.cc
    ${LEVELDB_DIR}/table/merger.cc
    ${LEVELDB_DIR}/table/table.cc
    ${LEVELDB_DIR}/table/table_builder.cc
    ${LEVELDB_DIR}/table/two_level_iterator.cc
    ${LEVELDB_DIR}/util/arena.cc
    ${LEVELDB_DIR}/util/bloom.cc
    ${LEVELDB_DIR}/util/cache.cc
    ${LEVELDB_DIR}/util/coding.cc
    ${LEVELDB_DIR}/util/comparator.cc
    ${LEVELDB_DIR}/util/crc32c.cc
    ${LEVELDB_DIR}/util/env.cc
    ${LEVELDB_DIR}/util/filter_policy.cc
    ${LEVELDB_DIR}/util/hash.cc
    ${LEVELDB_DIR}/util/histogram.cc
    ${LEVELDB_DIR}/util/logging.cc
    ${LEVELDB_DIR}/util/options.cc
    ${LEVELDB_DIR}/util/status.cc
    ${LEVELDB_DIR}/port/port_posix_sse.cc)

if(WIN32)
    target_sources(leveldb_mcpe PRIVATE
        ${LEVELDB_DIR}/port/port_win.cc
        ${LEVELDB_DIR}/util/env_win.cc
        ${LEVELDB_DIR}/util/win_logger.cc)
    target_compile_definitions(leveldb_mcpe
        PUBLIC  LEVELDB_PLATFORM_WINDOWS
        PRIVATE WIN32 _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN
                _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_sources(leveldb_mcpe PRIVATE
        ${LEVELDB_DIR}/port/port_posix.cc
        ${LEVELDB_DIR}/util/env_posix.cc)
    target_compile_definitions(leveldb_mcpe PUBLIC LEVELDB_PLATFORM_POSIX)
endif()

# DLLX is the fork's export macro; empty for a static build (consumers need it too).
target_compile_definitions(leveldb_mcpe PUBLIC "DLLX=")
target_include_directories(leveldb_mcpe
    SYSTEM PUBLIC "${LEVELDB_DIR}/include"
    PRIVATE "${LEVELDB_DIR}")
target_link_libraries(leveldb_mcpe PRIVATE ZLIB::ZLIB)
set_target_properties(leveldb_mcpe PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(leveldb_mcpe PRIVATE /W0 /utf-8)
endif()
add_library(leveldb::mcpe ALIAS leveldb_mcpe)
