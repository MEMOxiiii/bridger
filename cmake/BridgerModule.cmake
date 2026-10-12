# bridger_add_module(<name>
#     [DEPS <bridger modules...>]        # other Bridger modules, by short name (core, io, ...)
#     [PUBLIC_LIBS <targets...>]         # third-party targets exposed in public headers
#     [PRIVATE_LIBS <targets...>])       # third-party targets used only in .cpp files
#
# Creates the static library `bridger_<name>` (alias `bridger::<name>`) from
# src/<name>/**/*.cpp with public headers in src/<name>/include/bridger/<name>/.
#
# The dependency rule (AGENTS.md) is enforced here: a module may only depend on
# modules listed in BRIDGER_ALLOWED_DEPS_<name>.

set(BRIDGER_ALLOWED_DEPS_core      "")
set(BRIDGER_ALLOWED_DEPS_platform  "core")
set(BRIDGER_ALLOWED_DEPS_io        "core;platform")
set(BRIDGER_ALLOWED_DEPS_formats   "core;platform;io")
set(BRIDGER_ALLOWED_DEPS_translate "core;platform;io") # io: NBT for block entities (ADR 0008)
set(BRIDGER_ALLOWED_DEPS_convert   "core;platform;io;formats;translate")
set(BRIDGER_ALLOWED_DEPS_render    "core;platform;io;formats")

function(bridger_add_module name)
    cmake_parse_arguments(ARG "" "" "DEPS;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})

    if(NOT DEFINED BRIDGER_ALLOWED_DEPS_${name})
        message(FATAL_ERROR "Unknown Bridger module '${name}'. Add it to BridgerModule.cmake and AGENTS.md first.")
    endif()
    foreach(dep IN LISTS ARG_DEPS)
        if(NOT dep IN_LIST BRIDGER_ALLOWED_DEPS_${name})
            message(FATAL_ERROR "Dependency rule violated: bridger_${name} may not depend on bridger_${dep}")
        endif()
    endforeach()

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
    # OS-specific sources: only the platform module has them, and only the matching OS is compiled.
    if(WIN32)
        list(FILTER sources EXCLUDE REGEX "/posix/")
    else()
        list(FILTER sources EXCLUDE REGEX "/win32/")
    endif()

    set(target bridger_${name})
    add_library(${target} STATIC ${sources} ${headers})
    add_library(bridger::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)

    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(${target} PUBLIC bridger::${dep})
    endforeach()
    target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_LIBS} PRIVATE ${ARG_PRIVATE_LIBS})

    bridger_set_warnings(${target})
    bridger_enable_sanitizers(${target})
    bridger_enable_fuzzer_coverage(${target})
    bridger_enable_tracy(${target})
endfunction()

# Executables (cli, app, tests, benches) use this for consistent flags.
function(bridger_configure_executable target)
    bridger_set_warnings(${target})
    bridger_enable_sanitizers(${target})
    bridger_enable_tracy(${target})
endfunction()

function(bridger_enable_tracy target)
    if(BRIDGER_ENABLE_TRACY)
        target_link_libraries(${target} PUBLIC Tracy::TracyClient)
        target_compile_definitions(${target} PUBLIC BRIDGER_TRACY=1)
    endif()
endfunction()
