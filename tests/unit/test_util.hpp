#pragma once

// Shared helpers for unit tests.

#include <bridger/core/types.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace bridger::test {

inline std::filesystem::path data_path(const std::string& relative) {
    return std::filesystem::path(BRIDGER_TEST_DATA_DIR) / relative;
}

inline std::filesystem::path golden_path(const std::string& relative) {
    return std::filesystem::path(BRIDGER_GOLDEN_DIR) / relative;
}

inline std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open test file: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A fresh directory under %TEMP%, removed on destruction.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("bridger-test-" + std::to_string(stamp) + "-" + std::to_string(++counter));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // namespace bridger::test
