add_executable(bridger_gui main.cpp app.cpp app.hpp)
set_target_properties(bridger_gui PROPERTIES OUTPUT_NAME bridger-gui)
target_link_libraries(bridger_gui PRIVATE
    bridger::core bridger::platform bridger::io bridger::formats bridger::translate bridger::convert
    bridger::render
    SDL3::SDL3 imgui::imgui)
bridger_configure_executable(bridger_gui)
