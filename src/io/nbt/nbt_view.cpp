#include <bridger/io/nbt/nbt_view.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace bridger::io::nbt {
namespace {

// Smallest possible payload per type; lets list validation reject absurd counts
// up front instead of looping billions of times on a short buffer.
constexpr usize min_payload_size(TagType t) noexcept {
    switch (t) {
    case TagType::End: return 0;
    case TagType::Byte: return 1;
    case TagType::Short: return 2;
    case TagType::Int: return 4;
    case TagType::Long: return 8;
    case TagType::Float: return 4;
    case TagType::Double: return 8;
    case TagType::ByteArray: return 4;
    case TagType::String: return 2;
    case TagType::List: return 5;
    case TagType::Compound: return 1;
    case TagType::IntArray: return 4;
    case TagType::LongArray: return 4;
    }
    return 1;
}

// Iterative validator: nesting is tracked on a heap stack, so hostile input at the
// maximum depth cannot overflow the C++ call stack.
class Validator {
public:
    Validator(std::span<const u8> data, Endian endian, const NbtLimits& limits) noexcept
        : data_(data), endian_(endian), limits_(limits) {}

    // Validates a payload of `type` starting at `pos`; returns the position after it.
    Result<usize> payload(TagType type, usize pos) {
        usize p = pos;
        BRIDGER_TRY_STATUS(begin_value(type, p));
        while (!stack_.empty()) {
            Frame& top = stack_.back();
            if (top.is_list) {
                if (top.remaining == 0) {
                    stack_.pop_back();
                    continue;
                }
                --top.remaining;
                BRIDGER_TRY_STATUS(begin_value(top.element, p)); // may push (invalidates `top`)
                continue;
            }
            // Compound: next entry header or TAG_End.
            BRIDGER_TRY(const usize after_type, need(p, 1));
            const u8 id = data_[p];
            if (id == 0) {
                p = after_type;
                stack_.pop_back();
                continue;
            }
            if (!is_valid_tag_type(id)) return error(ErrorCode::Malformed, "invalid tag type", p);
            BRIDGER_TRY(const usize after_len, need(after_type, 2));
            const u16 name_len = detail::load<u16>(data_.data() + after_type, endian_);
            BRIDGER_TRY(p, need(after_len, name_len));
            BRIDGER_TRY_STATUS(begin_value(static_cast<TagType>(id), p));
        }
        return p;
    }

private:
    struct Frame {
        bool is_list;
        TagType element; // lists only
        u32 remaining;   // lists only: elements still to validate
    };

    Result<usize> need(usize pos, u64 n) const {
        if (pos > data_.size() || n > data_.size() - pos) return error(ErrorCode::UnexpectedEof, "truncated NBT", pos);
        return pos + static_cast<usize>(n);
    }

    Status push(Frame f, usize pos) {
        if (stack_.size() + 1 > std::min(limits_.max_depth, kMaxNbtDepth)) return error(ErrorCode::LimitExceeded, "NBT nested too deeply", pos);
        stack_.push_back(f);
        return {};
    }

    // Consumes a scalar/array/string payload at p, or validates a list/compound
    // header and pushes a frame for its contents.
    Status begin_value(TagType type, usize& p) {
        if (++tags_ > limits_.max_tags) return error(ErrorCode::LimitExceeded, "too many tags", p);
        switch (type) {
        case TagType::End: return error(ErrorCode::Malformed, "unexpected TAG_End", p);
        case TagType::Byte:
        case TagType::Short:
        case TagType::Int:
        case TagType::Long:
        case TagType::Float:
        case TagType::Double: {
            BRIDGER_TRY(p, need(p, fixed_payload_size(type)));
            return {};
        }
        case TagType::ByteArray: return array(p, 1);
        case TagType::IntArray: return array(p, 4);
        case TagType::LongArray: return array(p, 8);
        case TagType::String: {
            BRIDGER_TRY(const usize after_len, need(p, 2));
            BRIDGER_TRY(p, need(after_len, detail::load<u16>(data_.data() + p, endian_)));
            return {};
        }
        case TagType::List: {
            const usize start = p;
            BRIDGER_TRY(const usize after_header, need(p, 5));
            const u8 elem_id = data_[p];
            if (!is_valid_tag_type(elem_id)) return error(ErrorCode::Malformed, "invalid list element type", p);
            const auto elem = static_cast<TagType>(elem_id);
            const i32 count = detail::load<i32>(data_.data() + p + 1, endian_);
            if (count < 0) return error(ErrorCode::Malformed, "negative list length", p);
            p = after_header;
            if (count == 0) return {};
            if (elem == TagType::End) return error(ErrorCode::Malformed, "non-empty list of TAG_End", start);
            // Reject counts that cannot fit before walking them.
            BRIDGER_TRY_STATUS(need(p, static_cast<u64>(count) * min_payload_size(elem)));
            if (const usize fixed = fixed_payload_size(elem); fixed != 0) {
                tags_ += static_cast<u64>(count);
                if (tags_ > limits_.max_tags) return error(ErrorCode::LimitExceeded, "too many tags", start);
                BRIDGER_TRY(p, need(p, static_cast<u64>(count) * fixed));
                return {};
            }
            return push(Frame{true, elem, static_cast<u32>(count)}, start);
        }
        case TagType::Compound: return push(Frame{false, TagType::End, 0}, p);
        }
        return error(ErrorCode::Malformed, "invalid tag type", p);
    }

    Status array(usize& p, usize elem_size) const {
        BRIDGER_TRY(const usize after_len, need(p, 4));
        const i32 count = detail::load<i32>(data_.data() + p, endian_);
        if (count < 0) return error(ErrorCode::Malformed, "negative array length", p);
        BRIDGER_TRY(p, need(after_len, static_cast<u64>(count) * elem_size));
        return {};
    }

    static tl::unexpected<Error> error(ErrorCode code, const char* what, usize pos) {
        return make_error(code, what, ErrorContext::at_offset(pos));
    }

    std::span<const u8> data_;
    Endian endian_;
    NbtLimits limits_;
    u64 tags_ = 0;
    std::vector<Frame> stack_;
};

} // namespace

std::string_view to_string(TagType t) noexcept {
    switch (t) {
    case TagType::End: return "End";
    case TagType::Byte: return "Byte";
    case TagType::Short: return "Short";
    case TagType::Int: return "Int";
    case TagType::Long: return "Long";
    case TagType::Float: return "Float";
    case TagType::Double: return "Double";
    case TagType::ByteArray: return "ByteArray";
    case TagType::String: return "String";
    case TagType::List: return "List";
    case TagType::Compound: return "Compound";
    case TagType::IntArray: return "IntArray";
    case TagType::LongArray: return "LongArray";
    }
    return "?";
}

namespace detail {

namespace {

// Skips a payload that has no children (scalars, arrays, strings).
const u8* skip_leaf(TagType type, const u8* p, Endian e) noexcept {
    switch (type) {
    case TagType::ByteArray: return p + 4 + static_cast<usize>(load<i32>(p, e));
    case TagType::IntArray: return p + 4 + static_cast<usize>(load<i32>(p, e)) * 4;
    case TagType::LongArray: return p + 4 + static_cast<usize>(load<i32>(p, e)) * 8;
    case TagType::String: return p + 2 + load<u16>(p, e);
    default: return p + fixed_payload_size(type);
    }
}

} // namespace

// Iterative: validated data is at most kMaxNbtDepth deep, so a fixed-size stack suffices.
const u8* skip_payload(TagType type, const u8* p, Endian e) noexcept {
    if (type != TagType::List && type != TagType::Compound) return skip_leaf(type, p, e);

    struct Frame {
        TagType element; // End = compound
        u32 remaining;   // lists only
    };
    std::array<Frame, kMaxNbtDepth> stack; // NOLINT(cppcoreguidelines-pro-type-member-init)
    usize depth = 0;

    // Enters a list/compound payload at p (pushing a frame when it has children to walk).
    auto enter = [&](TagType t) {
        if (t == TagType::Compound) {
            stack[depth++] = {TagType::End, 0};
            return;
        }
        const auto elem = static_cast<TagType>(p[0]);
        const auto count = static_cast<u32>(load<i32>(p + 1, e));
        p += 5;
        if (count == 0) return;
        if (const usize fixed = fixed_payload_size(elem); fixed != 0) {
            p += static_cast<usize>(count) * fixed;
            return;
        }
        if (elem != TagType::List && elem != TagType::Compound) {
            for (u32 i = 0; i < count; ++i) p = skip_leaf(elem, p, e);
            return;
        }
        stack[depth++] = {elem, count};
    };

    enter(type);
    while (depth > 0) {
        Frame& f = stack[depth - 1];
        TagType child;
        if (f.element == TagType::End) { // compound
            if (*p == 0) {
                ++p;
                --depth;
                continue;
            }
            child = static_cast<TagType>(*p);
            p += 3 + load<u16>(p + 1, e);
        } else { // list of lists/compounds
            if (f.remaining == 0) {
                --depth;
                continue;
            }
            --f.remaining;
            child = f.element;
        }
        if (child == TagType::List || child == TagType::Compound)
            enter(child);
        else
            p = skip_leaf(child, p, e);
    }
    return p;
}

} // namespace detail

// ---------------------------------------------------------------------------
// NbtView
// ---------------------------------------------------------------------------

std::optional<i8> NbtView::as_byte() const noexcept {
    if (type_ != TagType::Byte) return std::nullopt;
    return static_cast<i8>(*payload_);
}
std::optional<i16> NbtView::as_short() const noexcept {
    if (type_ != TagType::Short) return std::nullopt;
    return detail::load<i16>(payload_, endian_);
}
std::optional<i32> NbtView::as_int() const noexcept {
    if (type_ != TagType::Int) return std::nullopt;
    return detail::load<i32>(payload_, endian_);
}
std::optional<i64> NbtView::as_long() const noexcept {
    if (type_ != TagType::Long) return std::nullopt;
    return detail::load<i64>(payload_, endian_);
}
std::optional<f32> NbtView::as_float() const noexcept {
    if (type_ != TagType::Float) return std::nullopt;
    return detail::load<f32>(payload_, endian_);
}
std::optional<f64> NbtView::as_double() const noexcept {
    if (type_ != TagType::Double) return std::nullopt;
    return detail::load<f64>(payload_, endian_);
}
std::optional<std::string_view> NbtView::as_string() const noexcept {
    if (type_ != TagType::String) return std::nullopt;
    return std::string_view(reinterpret_cast<const char*>(payload_ + 2), detail::load<u16>(payload_, endian_));
}
std::optional<std::span<const u8>> NbtView::as_byte_array() const noexcept {
    if (type_ != TagType::ByteArray) return std::nullopt;
    return std::span<const u8>(payload_ + 4, static_cast<usize>(detail::load<i32>(payload_, endian_)));
}
std::optional<NbtArrayView<i32>> NbtView::as_int_array() const noexcept {
    if (type_ != TagType::IntArray) return std::nullopt;
    return NbtArrayView<i32>(payload_ + 4, static_cast<usize>(detail::load<i32>(payload_, endian_)), endian_);
}
std::optional<NbtArrayView<i64>> NbtView::as_long_array() const noexcept {
    if (type_ != TagType::LongArray) return std::nullopt;
    return NbtArrayView<i64>(payload_ + 4, static_cast<usize>(detail::load<i32>(payload_, endian_)), endian_);
}
std::optional<NbtListView> NbtView::as_list() const noexcept {
    if (type_ != TagType::List) return std::nullopt;
    return NbtListView(static_cast<TagType>(payload_[0]), static_cast<usize>(detail::load<i32>(payload_ + 1, endian_)),
                       payload_ + 5, endian_);
}
std::optional<NbtCompoundView> NbtView::as_compound() const noexcept {
    if (type_ != TagType::Compound) return std::nullopt;
    return NbtCompoundView(payload_, endian_);
}
NbtListView NbtView::items() const noexcept {
    if (const auto list = as_list()) return *list;
    return NbtListView(TagType::End, 0, nullptr, endian_);
}

NbtCompoundView NbtView::entries() const noexcept {
    static constexpr u8 kEmptyCompound = 0; // a lone TAG_End
    if (type_ == TagType::Compound) return NbtCompoundView(payload_, endian_);
    return NbtCompoundView(&kEmptyCompound, endian_);
}

std::span<const u8> NbtView::payload_bytes() const noexcept {
    if (payload_ == nullptr) return {};
    const u8* end = detail::skip_payload(type_, payload_, endian_);
    return {payload_, static_cast<usize>(end - payload_)};
}

// ---------------------------------------------------------------------------
// Lists and compounds
// ---------------------------------------------------------------------------

NbtView NbtListView::at(usize i) const noexcept {
    if (i >= count_) return {};
    if (const usize fixed = fixed_payload_size(element_type_); fixed != 0)
        return {element_type_, first_ + i * fixed, endian_};
    const u8* p = first_;
    for (usize k = 0; k < i; ++k) p = detail::skip_payload(element_type_, p, endian_);
    return {element_type_, p, endian_};
}

NbtEntry NbtCompoundView::iterator::operator*() const noexcept {
    const auto type = static_cast<TagType>(*p_);
    const u16 len = detail::load<u16>(p_ + 1, e_);
    return {std::string_view(reinterpret_cast<const char*>(p_ + 3), len), NbtView(type, p_ + 3 + len, e_)};
}

NbtCompoundView::iterator& NbtCompoundView::iterator::operator++() noexcept {
    const auto type = static_cast<TagType>(*p_);
    const u8* payload = p_ + 3 + detail::load<u16>(p_ + 1, e_);
    p_ = detail::skip_payload(type, payload, e_);
    return *this;
}

NbtView NbtCompoundView::find(std::string_view name) const noexcept {
    for (const NbtEntry& entry : *this)
        if (entry.name == name) return entry.value;
    return {};
}

NbtView NbtCompoundView::find(std::string_view name, TagType type) const noexcept {
    const NbtView v = find(name);
    return v.type() == type ? v : NbtView{};
}

usize NbtCompoundView::size() const noexcept {
    usize n = 0;
    for (auto it = begin(); it != end(); ++it) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

Result<NbtDocument> NbtDocument::parse(std::span<const u8> data, Endian endian, const NbtLimits& limits) {
    if (data.size() < 3) return make_error(ErrorCode::UnexpectedEof, "NBT too short", ErrorContext::at_offset(0));
    const u8 id = data[0];
    if (id == 0 || !is_valid_tag_type(id))
        return make_error(ErrorCode::Malformed, "invalid NBT root tag type " + std::to_string(id),
                          ErrorContext::at_offset(0));
    const u16 name_len = detail::load<u16>(data.data() + 1, endian);
    if (3u + name_len > data.size())
        return make_error(ErrorCode::UnexpectedEof, "truncated NBT root name", ErrorContext::at_offset(1));

    Validator v(data, endian, limits);
    const auto type = static_cast<TagType>(id);
    BRIDGER_TRY(const usize end, v.payload(type, 3u + name_len));

    NbtDocument doc;
    doc.root_name_ = std::string_view(reinterpret_cast<const char*>(data.data() + 3), name_len);
    doc.root_ = NbtView(type, data.data() + 3 + name_len, endian);
    doc.consumed_ = end;
    return doc;
}

Result<usize> validate_payload(std::span<const u8> data, TagType type, Endian endian, const NbtLimits& limits) {
    Validator v(data, endian, limits);
    return v.payload(type, 0);
}

} // namespace bridger::io::nbt
