find_package(benchmark CONFIG REQUIRED)

# One executable per benchmark file: bench_<name>.cpp -> bridger_bench_<name>.
file(GLOB bench_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp")
if(NOT BRIDGER_BUILD_RENDER)
    list(FILTER bench_sources EXCLUDE REGEX "bench_render")
endif()
foreach(src IN LISTS bench_sources)
    get_filename_component(name "${src}" NAME_WE)
    set(target bridger_${name})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE
        bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
        benchmark::benchmark benchmark::benchmark_main)
    if(name STREQUAL "bench_render")
        target_link_libraries(${target} PRIVATE bridger::render)
    endif()
    bridger_configure_executable(${target})
endforeach()
