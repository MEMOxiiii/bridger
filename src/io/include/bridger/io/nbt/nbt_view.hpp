#pragma once

// Zero-copy NBT reader (big-endian Java and little-endian Bedrock).
//
// NbtDocument::parse() validates the whole tree once: every length is bounds-checked,
// tag types and nesting depth are verified. After that, views navigate the source
// buffer directly, with no allocation per tag. Views are valid only while the
// source buffer is alive. Strings are raw modified UTF-8 (see mutf8.hpp); keys
// given to find() are compared byte-for-byte (ASCII keys work as expected).

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bridger::io::nbt {

class NbtView;
class NbtListView;
class NbtCompoundView;

namespace detail {
// Advances past a validated payload. Never called on unvalidated data.
const u8* skip_payload(TagType type, const u8* p, Endian endian) noexcept;

template <typename T>
inline T load(const u8* p, Endian e) noexcept {
    return e == Endian::Big ? load_be<T>(p) : load_le<T>(p);
}
} // namespace detail

// Typed view of an Int/Long array payload (elements decoded on access).
template <typename T>
class NbtArrayView {
public:
    NbtArrayView() = default;
    NbtArrayView(const u8* data, usize count, Endian endian) noexcept : data_(data), count_(count), endian_(endian) {}

    [[nodiscard]] usize size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] T operator[](usize i) const noexcept { return detail::load<T>(data_ + i * sizeof(T), endian_); }
    [[nodiscard]] std::vector<T> to_vector() const {
        std::vector<T> out(count_);
        for (usize i = 0; i < count_; ++i) out[i] = (*this)[i];
        return out;
    }
    // Raw on-disk bytes (for verbatim copying).
    [[nodiscard]] std::span<const u8> raw() const noexcept { return {data_, count_ * sizeof(T)}; }

private:
    const u8* data_ = nullptr;
    usize count_ = 0;
    Endian endian_ = Endian::Big;
};

class NbtView {
public:
    NbtView() = default; // type End: "no value"
    NbtView(TagType type, const u8* payload, Endian endian) noexcept
        : payload_(payload), type_(type), endian_(endian) {}

    [[nodiscard]] TagType type() const noexcept { return type_; }
    [[nodiscard]] Endian endian() const noexcept { return endian_; }
    [[nodiscard]] bool is(TagType t) const noexcept { return type_ == t; }
    explicit operator bool() const noexcept { return type_ != TagType::End; }

    // Scalars: nullopt if the tag has a different type (no implicit conversions).
    [[nodiscard]] std::optional<i8> as_byte() const noexcept;
    [[nodiscard]] std::optional<i16> as_short() const noexcept;
    [[nodiscard]] std::optional<i32> as_int() const noexcept;
    [[nodiscard]] std::optional<i64> as_long() const noexcept;
    [[nodiscard]] std::optional<f32> as_float() const noexcept;
    [[nodiscard]] std::optional<f64> as_double() const noexcept;
    // Raw modified UTF-8 bytes.
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;
    [[nodiscard]] std::optional<std::span<const u8>> as_byte_array() const noexcept;
    [[nodiscard]] std::optional<NbtArrayView<i32>> as_int_array() const noexcept;
    [[nodiscard]] std::optional<NbtArrayView<i64>> as_long_array() const noexcept;
    [[nodiscard]] std::optional<NbtListView> as_list() const noexcept;
    [[nodiscard]] std::optional<NbtCompoundView> as_compound() const noexcept;

    // For range-for loops: views returned by value (empty if the type differs).
    // Never write `for (x : *view.as_list())`: in C++20 the temporary optional dies
    // before the loop body runs (dangling reference). Use `for (x : view.items())`.
    [[nodiscard]] NbtListView items() const noexcept;
    [[nodiscard]] NbtCompoundView entries() const noexcept;

    [[nodiscard]] const u8* payload() const noexcept { return payload_; }
    // Bytes of this payload in the source buffer (computed by skipping).
    [[nodiscard]] std::span<const u8> payload_bytes() const noexcept;

private:
    const u8* payload_ = nullptr;
    TagType type_ = TagType::End;
    Endian endian_ = Endian::Big;
};

class NbtListView {
public:
    NbtListView(TagType element_type, usize count, const u8* first, Endian endian) noexcept
        : first_(first), count_(count), element_type_(element_type), endian_(endian) {}

    [[nodiscard]] TagType element_type() const noexcept { return element_type_; }
    [[nodiscard]] usize size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    class iterator {
    public:
        using value_type = NbtView;
        using difference_type = std::ptrdiff_t;
        iterator() = default;
        iterator(const u8* p, usize index, TagType t, Endian e) noexcept : p_(p), index_(index), t_(t), e_(e) {}
        NbtView operator*() const noexcept { return {t_, p_, e_}; }
        iterator& operator++() noexcept {
            p_ = detail::skip_payload(t_, p_, e_);
            ++index_;
            return *this;
        }
        iterator operator++(int) noexcept {
            iterator old = *this;
            ++*this;
            return old;
        }
        bool operator==(const iterator& o) const noexcept { return index_ == o.index_; }

    private:
        const u8* p_ = nullptr;
        usize index_ = 0;
        TagType t_ = TagType::End;
        Endian e_ = Endian::Big;
    };

    [[nodiscard]] iterator begin() const noexcept { return {first_, 0, element_type_, endian_}; }
    [[nodiscard]] iterator end() const noexcept { return {nullptr, count_, element_type_, endian_}; }

    // O(1) for fixed-size elements, O(i) otherwise.
    [[nodiscard]] NbtView at(usize i) const noexcept;

private:
    const u8* first_;
    usize count_;
    TagType element_type_;
    Endian endian_;
};

struct NbtEntry {
    std::string_view name; // raw modified UTF-8
    NbtView value;
};

class NbtCompoundView {
public:
    NbtCompoundView(const u8* first_entry, Endian endian) noexcept : first_(first_entry), endian_(endian) {}

    class iterator {
    public:
        using value_type = NbtEntry;
        using difference_type = std::ptrdiff_t;
        iterator() = default;
        iterator(const u8* p, Endian e) noexcept : p_(p), e_(e) {}
        NbtEntry operator*() const noexcept;
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept {
            iterator old = *this;
            ++*this;
            return old;
        }
        // End is reached when the cursor sits on a TAG_End byte.
        bool operator==(const iterator& o) const noexcept { return at_end() == o.at_end() && (at_end() || p_ == o.p_); }

    private:
        [[nodiscard]] bool at_end() const noexcept { return p_ == nullptr || *p_ == 0; }
        const u8* p_ = nullptr;
        Endian e_ = Endian::Big;
    };

    [[nodiscard]] iterator begin() const noexcept { return {first_, endian_}; }
    [[nodiscard]] iterator end() const noexcept { return {}; }

    // First entry with this name, or a null view. Linear scan.
    [[nodiscard]] NbtView find(std::string_view name) const noexcept;
    // find() restricted to a type: null view if absent or of another type.
    [[nodiscard]] NbtView find(std::string_view name, TagType type) const noexcept;
    [[nodiscard]] usize size() const noexcept;

private:
    const u8* first_;
    Endian endian_;
};

class NbtDocument {
public:
    // Parses one named root tag at the start of `data`. Trailing bytes are allowed
    // and reported by consumed_bytes().
    [[nodiscard]] static Result<NbtDocument> parse(std::span<const u8> data, Endian endian,
                                                   const NbtLimits& limits = {});

    [[nodiscard]] std::string_view root_name() const noexcept { return root_name_; }
    [[nodiscard]] NbtView root() const noexcept { return root_; }
    [[nodiscard]] usize consumed_bytes() const noexcept { return consumed_; }
    [[nodiscard]] Endian endian() const noexcept { return root_.endian(); }

private:
    std::string_view root_name_;
    NbtView root_;
    usize consumed_ = 0;
};

// Validates a bare payload of `type` (no header), e.g. for nameless network NBT.
// Returns the payload size on success.
[[nodiscard]] Result<usize> validate_payload(std::span<const u8> data, TagType type, Endian endian,
                                             const NbtLimits& limits = {});

} // namespace bridger::io::nbt
