# bridger_embed_resource(<target> <input-file> <symbol>)
#
# Embeds a binary file (e.g. the compiled .brmap mapping blob or SPIR-V shaders)
# into <target>. Generates a .cpp defining:
#
#   namespace bridger::embedded {
#   extern const unsigned char <symbol>[];
#   extern const std::size_t   <symbol>_size;
#   }
#
# The conversion is done by a small host tool (cmake/bin2cpp.cpp) and reruns
# whenever the input changes.

set(_BRIDGER_BIN2CPP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/bin2cpp.cpp")

function(bridger_embed_resource target input symbol)
    if(NOT TARGET bridger_bin2cpp)
        add_executable(bridger_bin2cpp "${_BRIDGER_BIN2CPP_SOURCE}")
        # A plain host tool: no sanitizers or fuzzer flags.
        set_target_properties(bridger_bin2cpp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
    endif()
    get_filename_component(input_abs "${input}" ABSOLUTE)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/embedded/${symbol}.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/embedded"
        COMMAND bridger_bin2cpp "${input_abs}" "${output}" "${symbol}"
        DEPENDS "${input_abs}" bridger_bin2cpp
        COMMENT "Embedding ${input} as ${symbol}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
