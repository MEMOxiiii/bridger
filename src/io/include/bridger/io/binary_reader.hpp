#pragma once

// Bounds-checked sequential reader over an untrusted byte span. Every read either
// succeeds or returns UnexpectedEof with the offset; it never reads past the end.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/endian.hpp>

#include <span>
#include <string>
#include <string_view>

namespace bridger::io {

class BinaryReader {
public:
    BinaryReader(std::span<const u8> data, Endian endian) noexcept : data_(data), endian_(endian) {}

    [[nodiscard]] usize position() const noexcept { return pos_; }
    [[nodiscard]] usize remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }
    [[nodiscard]] Endian endian() const noexcept { return endian_; }

    template <Loadable T>
    [[nodiscard]] Result<T> read() noexcept {
        if (remaining() < sizeof(T)) return eof(sizeof(T));
        const u8* p = data_.data() + pos_;
        pos_ += sizeof(T);
        return endian_ == Endian::Big ? load_be<T>(p) : load_le<T>(p);
    }

    // Zero-copy view of the next `n` bytes.
    [[nodiscard]] Result<std::span<const u8>> read_bytes(usize n) noexcept {
        if (remaining() < n) return eof(n);
        auto view = data_.subspan(pos_, n);
        pos_ += n;
        return view;
    }

    [[nodiscard]] Status skip(usize n) noexcept {
        if (remaining() < n) return eof(n);
        pos_ += n;
        return {};
    }

    [[nodiscard]] Status seek(usize position) noexcept {
        if (position > data_.size())
            return make_error(ErrorCode::UnexpectedEof, "seek past end", ErrorContext::at_offset(position));
        pos_ = position;
        return {};
    }

private:
    [[nodiscard]] tl::unexpected<Error> eof(usize wanted) const {
        return make_error(ErrorCode::UnexpectedEof,
                          "need " + std::to_string(wanted) + " bytes, " + std::to_string(remaining()) + " left",
                          ErrorContext::at_offset(pos_));
    }

    std::span<const u8> data_;
    usize pos_ = 0;
    Endian endian_;
};

} // namespace bridger::io
