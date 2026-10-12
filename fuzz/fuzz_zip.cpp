// Fuzz target: zip reading (.mcworld files are untrusted) must fail cleanly, respect
// the size limits, and never accept unsafe entry names.

#include <bridger/io/zip_archive.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    bridger::io::ZipLimits limits;
    limits.max_entries = 64;
    limits.max_total_bytes = 1u << 22;
    const auto entries = bridger::io::read_zip(std::span<const bridger::u8>(data, size), limits);
    if (!entries) return 0;
    bridger::u64 total = 0;
    for (const auto& e : *entries) {
        if (e.name.find("..") != std::string::npos && (e.name == ".." || e.name.starts_with("../") ||
                                                        e.name.find("/../") != std::string::npos))
            std::abort();
        total += e.data.size();
    }
    if (total > limits.max_total_bytes || entries->size() > limits.max_entries) std::abort();
    return 0;
}
