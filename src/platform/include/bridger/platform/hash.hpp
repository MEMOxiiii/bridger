#pragma once

// SHA-1 and SHA-256 in portable C++ (FIPS 180-4). POSIX builds hash with these (net.hpp's sha1_hex/sha256_hex);
// Windows uses BCrypt, and the unit tests check both against the NIST example vectors.

#include <bridger/core/types.hpp>

#include <array>
#include <span>
#include <string>

namespace bridger::platform {

[[nodiscard]] std::array<u8, 20> sha1_digest(std::span<const u8> data) noexcept;
[[nodiscard]] std::array<u8, 32> sha256_digest(std::span<const u8> data) noexcept;
// Lowercase hex digits of `bytes`.
[[nodiscard]] std::string to_hex(std::span<const u8> bytes);

} // namespace bridger::platform
