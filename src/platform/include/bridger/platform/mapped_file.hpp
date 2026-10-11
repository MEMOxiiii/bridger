#pragma once

// Read-only memory-mapped file. The mapping lives as long as the object; views
// handed out by data() must not outlive it.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <filesystem>
#include <span>

namespace bridger::platform {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Opens `path` read-only (other processes may still read it). Empty files are
    // valid and yield an empty span.
    [[nodiscard]] static Result<MappedFile> open(const std::filesystem::path& path);

    [[nodiscard]] std::span<const u8> data() const noexcept { return {data_, size_}; }
    [[nodiscard]] usize size() const noexcept { return size_; }

private:
    void reset() noexcept;

    const u8* data_ = nullptr;
    usize size_ = 0;
    void* file_ = nullptr;    // OS file handle
    void* mapping_ = nullptr; // OS mapping handle
};

} // namespace bridger::platform
