# Sanitizer and fuzzer flags for Bridger's own targets.
#
# bridger_enable_sanitizers(<target>)  — applies ASan when BRIDGER_ENABLE_ASAN is ON.
# bridger_enable_fuzzer(<target>)      — libFuzzer + ASan; clang-cl only.
#
# Third-party code (vcpkg) is not instrumented. MSVC's STL container-overflow
# annotations must match across every object in a link, so they are disabled for
# instrumented targets (LNK2038 'annotate_string'/'annotate_vector' otherwise).
# Heap/stack/use-after-free detection is unaffected; see docs/testing.md.
set(_BRIDGER_ASAN_STL_DEFS _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)

function(bridger_enable_sanitizers target)
    if(NOT BRIDGER_ENABLE_ASAN)
        return()
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE ${_BRIDGER_ASAN_STL_DEFS})
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        # ASan is incompatible with incremental linking and /RTC (the preset uses RelWithDebInfo, which has no /RTC).
        target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
    else()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

function(bridger_enable_fuzzer target)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "Fuzz targets need clang-cl (use the clangcl-fuzz preset)")
    endif()
    target_compile_definitions(${target} PRIVATE ${_BRIDGER_ASAN_STL_DEFS})
    target_compile_options(${target} PRIVATE /fsanitize=address -fsanitize=fuzzer /Zi)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
    # CMake links with link.exe, which doesn't know clang's runtime directory or
    # the fuzzer runtime; add both explicitly.
    _bridger_clang_runtime_dir(runtime_dir)
    target_link_directories(${target} PRIVATE "${runtime_dir}")
    target_link_libraries(${target} PRIVATE clang_rt.fuzzer_MD-x86_64)
endfunction()

# <clang resource dir>/lib/windows — where clang_rt.* libraries live.
function(_bridger_clang_runtime_dir out_var)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT EXISTS "${resource_dir}/lib/windows")
        message(FATAL_ERROR "Cannot locate clang runtime libraries (resource dir: '${resource_dir}')")
    endif()
    set(${out_var} "${resource_dir}/lib/windows" PARENT_SCOPE)
endfunction()

# Instrument a library so fuzz targets also cover its code paths.
function(bridger_enable_fuzzer_coverage target)
    if(BRIDGER_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_definitions(${target} PRIVATE ${_BRIDGER_ASAN_STL_DEFS})
        target_compile_options(${target} PRIVATE /fsanitize=address -fsanitize=fuzzer-no-link)
    endif()
endfunction()
