find_package(benchmark CONFIG REQUIRED)

# One executable per benchmark file: bench_<name>.cpp -> bridger_bench_<name>.
file(GLOB bench_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp")
foreach(src IN LISTS bench_sources)
    get_filename_component(name "${src}" NAME_WE)
    set(target bridger_${name})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE
        bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
        benchmark::benchmark benchmark::benchmark_main)
    bridger_configure_executable(${target})
endforeach()
