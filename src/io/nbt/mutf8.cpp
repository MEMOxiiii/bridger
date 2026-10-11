#include <bridger/io/nbt/mutf8.hpp>

namespace bridger::io::nbt {
namespace {

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void append_mutf8_unit(std::string& out, u32 unit) { // one UTF-16 code unit, Java-style
    if (unit != 0 && unit < 0x80) {
        out += static_cast<char>(unit);
    } else if (unit < 0x800) { // includes U+0000 -> C0 80
        out += static_cast<char>(0xC0 | (unit >> 6));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (unit >> 12));
        out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    }
}

bool is_cont(u8 b) { return (b & 0xC0) == 0x80; }

} // namespace

bool is_plain_ascii(std::string_view bytes) noexcept {
    for (const char c : bytes) {
        const auto b = static_cast<u8>(c);
        if (b == 0 || b >= 0x80) return false;
    }
    return true;
}

Result<std::string> mutf8_to_utf8(std::string_view mutf8, bool* lossy) {
    if (lossy != nullptr) *lossy = false;
    if (is_plain_ascii(mutf8)) return std::string(mutf8);

    // First decode to UTF-16 code units, then pair surrogates.
    std::string out;
    out.reserve(mutf8.size());
    const auto* p = reinterpret_cast<const u8*>(mutf8.data());
    const usize n = mutf8.size();
    u32 pending_high = 0; // a high surrogate waiting for its low half
    auto flush_pending = [&] {
        if (pending_high != 0) {
            append_utf8(out, 0xFFFD);
            if (lossy != nullptr) *lossy = true;
            pending_high = 0;
        }
    };

    for (usize i = 0; i < n;) {
        u32 unit = 0;
        const u8 b0 = p[i];
        // Decoding is as lenient as java.io.DataInputStream.readUTF: a raw NUL byte and
        // overlong forms are accepted, since the game accepts them.
        if (b0 < 0x80) {
            unit = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= n || !is_cont(p[i + 1]))
                return make_error(ErrorCode::Malformed, "truncated modified UTF-8", ErrorContext::at_offset(i));
            unit = ((b0 & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= n || !is_cont(p[i + 1]) || !is_cont(p[i + 2]))
                return make_error(ErrorCode::Malformed, "truncated modified UTF-8", ErrorContext::at_offset(i));
            unit = ((b0 & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
            i += 3;
        } else {
            return make_error(ErrorCode::Malformed, "invalid modified UTF-8 lead byte", ErrorContext::at_offset(i));
        }

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            flush_pending();
            pending_high = unit;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (pending_high != 0) {
                append_utf8(out, 0x10000 + ((pending_high - 0xD800) << 10) + (unit - 0xDC00));
                pending_high = 0;
            } else {
                append_utf8(out, 0xFFFD);
                if (lossy != nullptr) *lossy = true;
            }
        } else {
            flush_pending();
            append_utf8(out, unit);
        }
    }
    flush_pending();
    return out;
}

Result<std::string> utf8_to_mutf8(std::string_view utf8) {
    if (is_plain_ascii(utf8)) return std::string(utf8);
    std::string out;
    out.reserve(utf8.size() + 8);
    const auto* p = reinterpret_cast<const u8*>(utf8.data());
    const usize n = utf8.size();
    for (usize i = 0; i < n;) {
        const u8 b0 = p[i];
        u32 cp = 0;
        usize len = 0;
        if (b0 < 0x80) {
            cp = b0;
            len = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1Fu;
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0Fu;
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07u;
            len = 4;
        } else {
            return make_error(ErrorCode::InvalidArgument, "invalid UTF-8", ErrorContext::at_offset(i));
        }
        if (i + len > n) return make_error(ErrorCode::InvalidArgument, "truncated UTF-8", ErrorContext::at_offset(i));
        for (usize k = 1; k < len; ++k) {
            if (!is_cont(p[i + k])) return make_error(ErrorCode::InvalidArgument, "invalid UTF-8", ErrorContext::at_offset(i));
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        constexpr u32 kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return make_error(ErrorCode::InvalidArgument, "invalid UTF-8 code point", ErrorContext::at_offset(i));
        if (cp >= 0x10000) {
            const u32 v = cp - 0x10000;
            append_mutf8_unit(out, 0xD800 + (v >> 10));
            append_mutf8_unit(out, 0xDC00 + (v & 0x3FF));
        } else {
            append_mutf8_unit(out, cp);
        }
        i += len;
    }
    return out;
}

} // namespace bridger::io::nbt
