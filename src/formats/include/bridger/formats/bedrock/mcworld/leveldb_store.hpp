#pragma once

// Minimal RAII wrapper around leveldb-mcpe (docs/adr/0004). Keeps LevelDB headers
// out of Bridger's public headers. Writes use raw-deflate compression, as Bedrock does
// (compressor id 4; zlib id 2 is registered for reading older worlds).

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::formats::bedrock {

class LevelDbStore {
public:
    // create_new: fail if a database already exists at `dir` (never touch existing worlds).
    [[nodiscard]] static Result<LevelDbStore> open(const std::filesystem::path& dir, bool create_new);
    LevelDbStore(LevelDbStore&&) noexcept;
    LevelDbStore& operator=(LevelDbStore&&) noexcept;
    ~LevelDbStore();

    [[nodiscard]] Status put(std::string_view key, std::span<const u8> value);
    [[nodiscard]] Result<std::optional<std::vector<u8>>> get(std::string_view key) const;
    // Visits every key/value pair in key order.
    [[nodiscard]] Status for_each(const std::function<void(std::string_view key, std::span<const u8> value)>& fn) const;
    // Flushes and compacts so the directory is complete and ready to be zipped.
    [[nodiscard]] Status compact();

private:
    LevelDbStore();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::formats::bedrock
