#include <bridger/platform/library.hpp>

#include <Windows.h>

namespace bridger::platform {

void* load_library_symbol(const std::filesystem::path& path, const char* symbol) {
    HMODULE lib = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return lib != nullptr ? reinterpret_cast<void*>(GetProcAddress(lib, symbol)) : nullptr;
}

} // namespace bridger::platform
