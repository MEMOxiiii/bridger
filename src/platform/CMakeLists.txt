# The ONLY module allowed to include OS headers (Windows.h, unistd.h, ...).
# OS-specific sources live in win32/ and posix/; BridgerModule.cmake
# compiles only the folder matching the current OS.
bridger_add_module(platform
    DEPS core)
if(WIN32)
    # SHGetKnownFolderPath (roaming_app_data_dir) and CoTaskMemFree; WinHTTP and BCrypt (net.hpp).
    target_link_libraries(bridger_platform PRIVATE shell32 ole32 winhttp bcrypt)
else()
    # dlopen: libcurl is loaded at run time (posix/net_posix.cpp), MoltenVK on macOS (library.hpp).
    target_link_libraries(bridger_platform PRIVATE ${CMAKE_DL_LIBS})
endif()
