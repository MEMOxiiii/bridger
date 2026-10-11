#pragma once

// NBT tag types (docs/formats/nbt.md). The numeric values are the on-disk type ids.

#include <bridger/core/types.hpp>

#include <string_view>

namespace bridger::io::nbt {

enum class TagType : u8 {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

inline constexpr u8 kMaxTagType = 12;

[[nodiscard]] constexpr bool is_valid_tag_type(u8 id) noexcept { return id <= kMaxTagType; }

// Payload size of fixed-size tags; 0 for variable-size tags (and End).
[[nodiscard]] constexpr usize fixed_payload_size(TagType t) noexcept {
    switch (t) {
    case TagType::Byte: return 1;
    case TagType::Short: return 2;
    case TagType::Int: return 4;
    case TagType::Long: return 8;
    case TagType::Float: return 4;
    case TagType::Double: return 8;
    default: return 0;
    }
}

[[nodiscard]] std::string_view to_string(TagType t) noexcept;

// Hard cap on nesting (minecraft.wiki: "may not be nested beyond a depth of 512").
// Tree walks use fixed-size or heap stacks sized for this, never C++ recursion.
inline constexpr u32 kMaxNbtDepth = 512;

// Limits applied while parsing untrusted NBT.
struct NbtLimits {
    u32 max_depth = kMaxNbtDepth;   // nesting of compounds/lists; values above kMaxNbtDepth are clamped
    u64 max_tags = u64{1} << 28;    // total tag count, bounds parse time on adversarial input
};

} // namespace bridger::io::nbt
