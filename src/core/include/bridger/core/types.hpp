#pragma once

// Fixed-width aliases and basic coordinate types shared by every module.

#include <compare>
#include <cstddef>
#include <cstdint>

namespace bridger {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "IEEE-754 float sizes required");

// Edge length of a section (16x16x16 blocks), shared by both editions.
inline constexpr i32 kSectionSize = 16;

// Floor division by 16 that is correct for negative coordinates.
[[nodiscard]] constexpr i32 floor_div16(i32 v) noexcept { return v >> 4; }
// Position within a section, in [0, 15], correct for negative coordinates.
[[nodiscard]] constexpr i32 floor_mod16(i32 v) noexcept { return v & 15; }

// Absolute block position in a world or structure.
struct BlockPos {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    friend constexpr auto operator<=>(const BlockPos&, const BlockPos&) = default;
};

// Chunk column position (16x16 blocks in x/z).
struct ChunkPos {
    i32 x = 0;
    i32 z = 0;
    friend constexpr auto operator<=>(const ChunkPos&, const ChunkPos&) = default;

    // Packs both coordinates into one 64-bit key for hash maps.
    [[nodiscard]] constexpr u64 packed() const noexcept {
        return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(z));
    }
};

// Section position (16x16x16 blocks). y is the section index, not the block y.
struct SectionPos {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    friend constexpr auto operator<=>(const SectionPos&, const SectionPos&) = default;

    [[nodiscard]] constexpr ChunkPos chunk() const noexcept { return {x, z}; }
};

[[nodiscard]] constexpr ChunkPos chunk_of(BlockPos p) noexcept {
    return {floor_div16(p.x), floor_div16(p.z)};
}

[[nodiscard]] constexpr SectionPos section_of(BlockPos p) noexcept {
    return {floor_div16(p.x), floor_div16(p.y), floor_div16(p.z)};
}

} // namespace bridger
