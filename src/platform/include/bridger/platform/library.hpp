#pragma once

// Loading a shared library by path, for a driver that ships next to the program: MoltenVK inside the macOS app bundle
// (docs/adr/0010). The library stays loaded until the process ends.

#include <filesystem>

namespace bridger::platform {

// The address of `symbol` in the library at `path`, or nullptr when the library or the symbol is missing.
[[nodiscard]] void* load_library_symbol(const std::filesystem::path& path, const char* symbol);

} // namespace bridger::platform
