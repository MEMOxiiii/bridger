add_executable(bridger_cli main.cpp commands/commands.hpp commands/info.cpp commands/convert.cpp commands/render.cpp)
set_target_properties(bridger_cli PROPERTIES OUTPUT_NAME bridger)
target_link_libraries(bridger_cli PRIVATE
    bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert bridger::render
    CLI11::CLI11)
bridger_configure_executable(bridger_cli)
