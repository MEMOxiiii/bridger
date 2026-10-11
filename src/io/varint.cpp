#include <bridger/io/varint.hpp>

namespace bridger::io {
namespace {

template <typename T, usize MaxBytes, typename Out>
Result<Out> decode_varint(std::span<const u8> data) noexcept {
    constexpr unsigned kBits = sizeof(T) * 8;
    T value = 0;
    for (usize i = 0; i < MaxBytes; ++i) {
        if (i >= data.size()) return make_error(ErrorCode::UnexpectedEof, "truncated varint", {.offset = i});
        const u8 byte = data[i];
        const unsigned shift = static_cast<unsigned>(i) * 7;
        const T group = static_cast<T>(byte & 0x7F);
        // The last allowed byte may only carry the bits that still fit in T.
        if (i == MaxBytes - 1) {
            const unsigned remaining = kBits - shift;
            if ((byte & 0x80) != 0 || (group >> remaining) != 0)
                return make_error(ErrorCode::Malformed, "varint overflows its type", {.offset = i});
        }
        value |= static_cast<T>(group << shift);
        if ((byte & 0x80) == 0) return Out{value, i + 1};
    }
    // Unreachable: the last iteration either returns or errors.
    return make_error(ErrorCode::Malformed, "varint too long");
}

template <typename T>
usize encode_varint(T value, std::vector<u8>& out) {
    usize n = 0;
    while (value >= 0x80) {
        out.push_back(static_cast<u8>((value & 0x7F) | 0x80));
        value >>= 7;
        ++n;
    }
    out.push_back(static_cast<u8>(value));
    return n + 1;
}

} // namespace

Result<VarintDecoded32> decode_varint32(std::span<const u8> data) noexcept {
    return decode_varint<u32, kMaxVarint32Bytes, VarintDecoded32>(data);
}

Result<VarintDecoded64> decode_varint64(std::span<const u8> data) noexcept {
    return decode_varint<u64, kMaxVarint64Bytes, VarintDecoded64>(data);
}

usize encode_varint32(u32 value, std::vector<u8>& out) { return encode_varint(value, out); }
usize encode_varint64(u64 value, std::vector<u8>& out) { return encode_varint(value, out); }

} // namespace bridger::io
