add_executable(bridger_cli main.cpp commands/commands.hpp commands/info.cpp commands/convert.cpp)
set_target_properties(bridger_cli PROPERTIES OUTPUT_NAME bridger)
target_link_libraries(bridger_cli PRIVATE
    bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
    CLI11::CLI11)
# `bridger render` needs the Vulkan renderer (not built on Linux and macOS yet).
if(BRIDGER_BUILD_RENDER)
    target_sources(bridger_cli PRIVATE commands/render.cpp)
    target_link_libraries(bridger_cli PRIVATE bridger::render)
    target_compile_definitions(bridger_cli PRIVATE BRIDGER_CLI_RENDER=1)
endif()
bridger_configure_executable(bridger_cli)
