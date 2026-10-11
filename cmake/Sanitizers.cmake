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
    target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG ${BRIDGER_FUZZER_LINK_ARGS})
endfunction()

# CMake links clang-cl targets by calling lld-link/link.exe directly, so the clang
# driver never adds the sanitizer runtimes. Which runtime libraries are needed
# (names, /MD vs /MT variants, -wholearchive) differs between LLVM releases and
# the copy bundled with Visual Studio, so ask this compiler's driver instead of
# hardcoding names: `clang-cl -### ...` prints the link line it would use.
function(_bridger_query_fuzzer_link_args out_var)
    set(probe "${CMAKE_BINARY_DIR}/bridger_fuzz_probe.cpp")
    file(WRITE "${probe}" "int main() { return 0; }\n")
    # Probe with the same CRT the project uses (/MT for the clangcl-fuzz preset).
    if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        set(crt /MD)
    else()
        set(crt /MT)
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo ${crt} /fsanitize=address -fsanitize=fuzzer -fuse-ld=lld "-###" "${probe}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    string(APPEND out "${err}")
    # Quoted arguments that name a clang runtime library, e.g.
    #   "-wholearchive:C:\\...\\clang_rt.fuzzer-x86_64.lib"   "C:\\...\\clang_rt.asan-x86_64.lib"
    string(REGEX MATCHALL "\"[^\"]*clang_rt[^\"]*\\.lib\"" quoted "${out}")
    set(args "")
    foreach(q IN LISTS quoted)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" a "${q}")
        string(REPLACE "\\\\" "/" a "${a}")
        list(APPEND args "${a}")
    endforeach()
    if(NOT args)
        message(FATAL_ERROR "Could not determine libFuzzer/ASan runtime libraries from ${CMAKE_CXX_COMPILER}:\n${out}")
    endif()
    set(${out_var} "${args}" PARENT_SCOPE)
endfunction()

if(BRIDGER_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT DEFINED BRIDGER_FUZZER_LINK_ARGS)
    _bridger_query_fuzzer_link_args(_args)
    set(BRIDGER_FUZZER_LINK_ARGS "${_args}" CACHE INTERNAL "Sanitizer runtime link arguments for fuzz targets")
    message(STATUS "Bridger: fuzzer runtime link args = ${_args}")
endif()

# Instrument a library so fuzz targets also cover its code paths.
function(bridger_enable_fuzzer_coverage target)
    if(BRIDGER_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_definitions(${target} PRIVATE ${_BRIDGER_ASAN_STL_DEFS})
        target_compile_options(${target} PRIVATE /fsanitize=address -fsanitize=fuzzer-no-link)
    endif()
endfunction()
