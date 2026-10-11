#pragma once

// Whole-buffer (de)compression via libdeflate. Decompression output is capped
// (zip/deflate bomb guard): exceeding the cap is LimitExceeded, never an OOM.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace bridger::io {

enum class Compression : u8 {
    None,
    Gzip,       // RFC 1952 (Java .schem/.nbt/level.dat, .litematic)
    Zlib,       // RFC 1950 (Java region chunks)
    RawDeflate, // RFC 1951 (Bedrock LevelDB blocks, zip entries)
};

[[nodiscard]] std::string_view to_string(Compression c) noexcept;

// Recognizes gzip by its magic bytes and zlib by its header checksum. Raw deflate
// has no signature and is never detected.
[[nodiscard]] Compression detect_compression(std::span<const u8> data) noexcept;

struct DecompressLimits {
    usize max_output_bytes = usize{1} << 30; // 1 GiB
};

// Gzip input may contain several concatenated members; their outputs are joined.
[[nodiscard]] Result<std::vector<u8>> decompress(std::span<const u8> data, Compression format,
                                                 const DecompressLimits& limits = {});

// level: 0 (store) .. 12 (libdeflate max); 6 is zlib's default.
[[nodiscard]] Result<std::vector<u8>> compress(std::span<const u8> data, Compression format, int level = 6);

} // namespace bridger::io
