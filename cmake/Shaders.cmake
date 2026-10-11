# GLSL -> SPIR-V at build time with glslang (from vcpkg, feature "tools").
#
# bridger_find_glslang()                         — locates glslangValidator / glslang.
# bridger_compile_shaders(<target> <shaders...>) — compiles each shader to
#     ${CMAKE_CURRENT_BINARY_DIR}/shaders/<name>.spv and embeds it in <target> as
#     bridger::embedded::shader_<name>_<stage> (via EmbedResource.cmake).

function(bridger_find_glslang)
    find_program(BRIDGER_GLSLANG
        NAMES glslang glslangValidator
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/glslang"
        REQUIRED)
    message(STATUS "Bridger: glslang = ${BRIDGER_GLSLANG}")
endfunction()

function(bridger_compile_shaders target)
    foreach(shader IN LISTS ARGN)
        get_filename_component(shader_abs "${shader}" ABSOLUTE)
        get_filename_component(name "${shader}" NAME_WE)
        get_filename_component(ext "${shader}" LAST_EXT)
        string(SUBSTRING "${ext}" 1 -1 stage)
        set(spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.${stage}.spv")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${BRIDGER_GLSLANG}" --target-env vulkan1.3 -o "${spv}" "${shader_abs}"
            DEPENDS "${shader_abs}"
            COMMENT "Compiling shader ${name}.${stage}"
            VERBATIM)
        bridger_embed_resource(${target} "${spv}" "shader_${name}_${stage}")
    endforeach()
endfunction()
