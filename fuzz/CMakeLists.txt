# libFuzzer targets — clangcl-fuzz preset only.
# Each fuzz_<name>.cpp becomes bridger_fuzz_<name>.exe.
# Run: build/clangcl-fuzz/bin/bridger_fuzz_varint.exe -max_total_time=60

file(GLOB fuzz_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_*.cpp")
foreach(src IN LISTS fuzz_sources)
    get_filename_component(name "${src}" NAME_WE)
    set(target bridger_${name})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE
        bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert)
    bridger_set_warnings(${target})
    bridger_enable_fuzzer(${target})
endforeach()
