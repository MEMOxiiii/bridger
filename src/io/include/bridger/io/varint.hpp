#pragma once

// LEB128-style unsigned varints (7 bits per byte, low group first, high bit = "more").
// Used by Sponge .schem block data (see docs/formats/ once Phase 2 documents it).
// Decoding is bounds-checked: input is untrusted.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <span>
#include <vector>

namespace bridger::io {

inline constexpr usize kMaxVarint32Bytes = 5;
inline constexpr usize kMaxVarint64Bytes = 10;

struct VarintDecoded32 {
    u32 value = 0;
    usize length = 0; // bytes consumed
};

struct VarintDecoded64 {
    u64 value = 0;
    usize length = 0;
};

// Decodes one varint from the start of `data`.
// Errors: UnexpectedEof if truncated, Malformed if it overflows the type.
[[nodiscard]] Result<VarintDecoded32> decode_varint32(std::span<const u8> data) noexcept;
[[nodiscard]] Result<VarintDecoded64> decode_varint64(std::span<const u8> data) noexcept;

// Appends the encoding of `value` to `out`; returns the number of bytes written.
usize encode_varint32(u32 value, std::vector<u8>& out);
usize encode_varint64(u64 value, std::vector<u8>& out);

[[nodiscard]] constexpr usize varint_size(u64 value) noexcept {
    usize n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

} // namespace bridger::io
