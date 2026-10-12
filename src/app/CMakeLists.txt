add_executable(bridger_gui WIN32
    main.cpp app.cpp app.hpp
    settings.cpp settings.hpp
    ui_bundle.cpp ui_bundle.hpp
    viewer.cpp viewer.hpp
    win32/compositor.cpp win32/compositor.hpp
    win32/shell_win32.cpp win32/shell.hpp
    win32/webview_host.cpp win32/webview_host.hpp)
set_target_properties(bridger_gui PROPERTIES OUTPUT_NAME bridger-gui)
target_link_libraries(bridger_gui PRIVATE
    bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
    bridger::render
    SDL3::SDL3 unofficial::webview2::webview2 WIL::WIL nlohmann_json::nlohmann_json
    shlwapi comctl32 d3d11 dxgi dcomp dwmapi)
bridger_configure_executable(bridger_gui)

# The web user interface (src/app/ui, docs/adr/0009): built with npm (Vite) and embedded as a zip.
# `npm ci` runs when package-lock.json changes (cmake/NpmInstall.cmake); the build reruns when any UI
# source changes.
find_program(BRIDGER_NPM NAMES npm.cmd npm REQUIRED)
set(ui_dir "${CMAKE_CURRENT_SOURCE_DIR}/ui")
set(ui_out "${CMAKE_CURRENT_BINARY_DIR}/ui")
file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS "${ui_dir}/src/*" "${ui_dir}/public/*")
list(APPEND ui_sources "${ui_dir}/index.html" "${ui_dir}/vite.config.ts" "${ui_dir}/tsconfig.json" "${ui_dir}/package.json")
add_custom_command(
    OUTPUT "${ui_out}/npm-ci.stamp"
    COMMAND "${CMAKE_COMMAND}" -DNPM=${BRIDGER_NPM} -DDIR=${ui_dir} -P "${PROJECT_SOURCE_DIR}/cmake/NpmInstall.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ui_out}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${ui_out}/npm-ci.stamp"
    WORKING_DIRECTORY "${ui_dir}"
    DEPENDS "${ui_dir}/package-lock.json"
    COMMENT "Installing the user interface's npm packages"
    VERBATIM)
add_custom_command(
    OUTPUT "${ui_out}/ui_bundle.zip"
    COMMAND "${BRIDGER_NPM}" run build -- --outDir "${ui_out}/dist" --emptyOutDir
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${ui_out}/ui_bundle.zip"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${ui_out}/dist" "${CMAKE_COMMAND}" -E tar cf "${ui_out}/ui_bundle.zip" --format=zip .
    WORKING_DIRECTORY "${ui_dir}"
    DEPENDS ${ui_sources} "${ui_out}/npm-ci.stamp"
    COMMENT "Building the user interface"
    VERBATIM)
add_custom_target(bridger_gui_ui DEPENDS "${ui_out}/ui_bundle.zip")
bridger_embed_resource(bridger_gui "${ui_out}/ui_bundle.zip" ui_bundle_zip)
add_dependencies(bridger_gui bridger_gui_ui)
