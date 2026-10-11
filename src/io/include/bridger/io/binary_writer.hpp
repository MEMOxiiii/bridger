#pragma once

// Growable output buffer with endian-aware appends.

#include <bridger/core/types.hpp>
#include <bridger/io/endian.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace bridger::io {

class BinaryWriter {
public:
    explicit BinaryWriter(Endian endian) noexcept : endian_(endian) {}

    template <Loadable T>
    void write(T value) {
        const usize at = buffer_.size();
        buffer_.resize(at + sizeof(T));
        if (endian_ == Endian::Big)
            store_be<T>(buffer_.data() + at, value);
        else
            store_le<T>(buffer_.data() + at, value);
    }

    void write_bytes(std::span<const u8> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }
    void write_bytes(std::string_view bytes) {
        const auto* p = reinterpret_cast<const u8*>(bytes.data());
        buffer_.insert(buffer_.end(), p, p + bytes.size());
    }

    void reserve(usize n) { buffer_.reserve(n); }
    [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
    [[nodiscard]] Endian endian() const noexcept { return endian_; }
    [[nodiscard]] std::span<const u8> view() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<u8> take() && noexcept { return std::move(buffer_); }

private:
    std::vector<u8> buffer_;
    Endian endian_;
};

} // namespace bridger::io
