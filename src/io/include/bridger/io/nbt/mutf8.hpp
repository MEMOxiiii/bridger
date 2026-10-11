#pragma once

// Java "modified UTF-8" (java.io.DataInput) <-> standard UTF-8.
// Differences from UTF-8: U+0000 is encoded as C0 80, and characters above U+FFFF
// are encoded as a surrogate pair, each surrogate as a 3-byte sequence.
// NBT strings are kept in their raw on-disk form inside Bridger (byte-exact
// round-trips); convert only at the edges.

#include <bridger/core/result.hpp>

#include <string>
#include <string_view>

namespace bridger::io::nbt {

// True if `bytes` is the same in UTF-8 and modified UTF-8 (all ASCII, no NUL):
// the common case, which needs no conversion.
[[nodiscard]] bool is_plain_ascii(std::string_view bytes) noexcept;

// Decodes modified UTF-8. Unpaired surrogates (legal in Java strings, not in UTF-8)
// are replaced with U+FFFD; `lossy` is set when that happens.
[[nodiscard]] Result<std::string> mutf8_to_utf8(std::string_view mutf8, bool* lossy = nullptr);

// Encodes valid UTF-8 as modified UTF-8. Fails on invalid UTF-8.
[[nodiscard]] Result<std::string> utf8_to_mutf8(std::string_view utf8);

} // namespace bridger::io::nbt
