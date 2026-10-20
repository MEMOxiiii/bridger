#include <bridger/platform/library.hpp>

#include <dlfcn.h>

namespace bridger::platform {

void* load_library_symbol(const std::filesystem::path& path, const char* symbol) {
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    return lib != nullptr ? dlsym(lib, symbol) : nullptr;
}

} // namespace bridger::platform
