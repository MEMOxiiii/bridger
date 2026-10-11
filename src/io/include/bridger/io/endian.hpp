#pragma once

// Endian-aware loads/stores from raw bytes. Java NBT is big-endian, Bedrock NBT is
// little-endian. These never require alignment and compile to single moves (+bswap).

#include <bridger/core/types.hpp>

#include <bit>
#include <concepts>
#include <cstdlib> // _byteswap_* (MSVC)
#include <cstring>
#include <type_traits>

namespace bridger::io {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

template <typename T>
concept Loadable = std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                                                       sizeof(T) == 8);

namespace detail {
[[nodiscard]] inline u16 bswap(u16 v) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}
[[nodiscard]] inline u32 bswap(u32 v) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
[[nodiscard]] inline u64 bswap(u64 v) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}
[[nodiscard]] inline u8 bswap(u8 v) noexcept { return v; }
} // namespace detail

// C++20 stand-in for std::byteswap (C++23); compiles to a single bswap instruction.
template <std::integral T>
[[nodiscard]] inline T byteswap(T value) noexcept {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(detail::bswap(static_cast<U>(value)));
}

namespace detail {
template <usize N> struct UintOfSize;
template <> struct UintOfSize<1> { using type = u8; };
template <> struct UintOfSize<2> { using type = u16; };
template <> struct UintOfSize<4> { using type = u32; };
template <> struct UintOfSize<8> { using type = u64; };
} // namespace detail

// Loads a T stored with byte order `order` from `src` (no alignment needed).
template <Loadable T, std::endian order>
[[nodiscard]] inline T load(const void* src) noexcept {
    using U = typename detail::UintOfSize<sizeof(T)>::type;
    U raw;
    std::memcpy(&raw, src, sizeof(U));
    if constexpr (order != std::endian::native && sizeof(U) > 1) raw = detail::bswap(raw);
    return std::bit_cast<T>(raw);
}

// Stores `value` into `dst` with byte order `order` (no alignment needed).
template <Loadable T, std::endian order>
inline void store(void* dst, T value) noexcept {
    using U = typename detail::UintOfSize<sizeof(T)>::type;
    U raw = std::bit_cast<U>(value);
    if constexpr (order != std::endian::native && sizeof(U) > 1) raw = detail::bswap(raw);
    std::memcpy(dst, &raw, sizeof(U));
}

template <Loadable T> [[nodiscard]] inline T load_be(const void* src) noexcept { return load<T, std::endian::big>(src); }
template <Loadable T> [[nodiscard]] inline T load_le(const void* src) noexcept { return load<T, std::endian::little>(src); }
template <Loadable T> inline void store_be(void* dst, T v) noexcept { store<T, std::endian::big>(dst, v); }
template <Loadable T> inline void store_le(void* dst, T v) noexcept { store<T, std::endian::little>(dst, v); }

} // namespace bridger::io
