find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED) # golden files are JSON
include(Catch)

# One test executable for all unit tests; tests are grouped by module with Catch2 tags
# ([core], [io], ...). Run a single test: ctest --preset msvc-debug -R "<test name>"
# or bin/bridger_unit_tests "[io]".
file(GLOB_RECURSE unit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*_test.cpp")
if(NOT BRIDGER_BUILD_RENDER)
    list(FILTER unit_sources EXCLUDE REGEX "/unit/render/")
endif()

add_executable(bridger_unit_tests ${unit_sources})
target_link_libraries(bridger_unit_tests PRIVATE
    bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
    leveldb::mcpe ZLIB::ZLIB
    Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
if(BRIDGER_BUILD_RENDER)
    target_link_libraries(bridger_unit_tests PRIVATE bridger::render)
endif()
target_compile_definitions(bridger_unit_tests PRIVATE
    BRIDGER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    BRIDGER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
bridger_configure_executable(bridger_unit_tests)

catch_discover_tests(bridger_unit_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
