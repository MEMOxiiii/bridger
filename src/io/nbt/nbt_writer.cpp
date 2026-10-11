#include <bridger/io/nbt/nbt_writer.hpp>

#include <bit>
#include <limits>
#include <vector>

namespace bridger::io::nbt {

void NbtWriter::fail(const char* message) {
    if (error_ == nullptr) error_ = message;
}

bool NbtWriter::begin_tag(std::string_view name, TagType type) {
    if (error_ != nullptr) return false;
    if (stack_.empty()) {
        if (root_written_) {
            fail("more than one root tag");
            return false;
        }
        root_written_ = true;
    } else if (stack_.back().is_list) {
        Frame& f = stack_.back();
        if (type != f.element_type) {
            fail("list element has the wrong type");
            return false;
        }
        if (f.remaining == 0) {
            fail("more list elements than declared");
            return false;
        }
        --f.remaining;
        return true; // list elements have no header
    }
    if (name.size() > std::numeric_limits<u16>::max()) {
        fail("tag name longer than 65535 bytes");
        return false;
    }
    out_.write(static_cast<u8>(type));
    out_.write(static_cast<u16>(name.size()));
    out_.write_bytes(name);
    return true;
}

void NbtWriter::begin_root_compound(std::string_view name) { begin_compound(name); }

void NbtWriter::begin_compound(std::string_view name) {
    if (!begin_tag(name, TagType::Compound)) return;
    stack_.push_back({false, TagType::End, 0});
}

void NbtWriter::end_compound() {
    if (error_ != nullptr) return;
    if (stack_.empty() || stack_.back().is_list) return fail("end_compound without matching begin_compound");
    out_.write(static_cast<u8>(TagType::End));
    stack_.pop_back();
}

void NbtWriter::begin_list(std::string_view name, TagType element_type, usize count) {
    if (count > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("list too long");
    if (count > 0 && element_type == TagType::End) return fail("non-empty list of TAG_End");
    if (!begin_tag(name, TagType::List)) return;
    out_.write(static_cast<u8>(element_type));
    out_.write(static_cast<i32>(count));
    stack_.push_back({true, element_type, count});
}

void NbtWriter::end_list() {
    if (error_ != nullptr) return;
    if (stack_.empty() || !stack_.back().is_list) return fail("end_list without matching begin_list");
    if (stack_.back().remaining != 0) return fail("fewer list elements than declared");
    stack_.pop_back();
}

void NbtWriter::write_byte(std::string_view name, i8 v) {
    if (begin_tag(name, TagType::Byte)) out_.write(v);
}
void NbtWriter::write_short(std::string_view name, i16 v) {
    if (begin_tag(name, TagType::Short)) out_.write(v);
}
void NbtWriter::write_int(std::string_view name, i32 v) {
    if (begin_tag(name, TagType::Int)) out_.write(v);
}
void NbtWriter::write_long(std::string_view name, i64 v) {
    if (begin_tag(name, TagType::Long)) out_.write(v);
}
void NbtWriter::write_float(std::string_view name, f32 v) {
    if (begin_tag(name, TagType::Float)) out_.write(std::bit_cast<u32>(v));
}
void NbtWriter::write_double(std::string_view name, f64 v) {
    if (begin_tag(name, TagType::Double)) out_.write(std::bit_cast<u64>(v));
}

void NbtWriter::write_string_payload(std::string_view raw) {
    if (raw.size() > std::numeric_limits<u16>::max()) return fail("string longer than 65535 bytes");
    out_.write(static_cast<u16>(raw.size()));
    out_.write_bytes(raw);
}

void NbtWriter::write_string(std::string_view name, std::string_view raw_mutf8) {
    if (begin_tag(name, TagType::String)) write_string_payload(raw_mutf8);
}

void NbtWriter::write_byte_array(std::string_view name, std::span<const u8> bytes) {
    if (bytes.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
    if (!begin_tag(name, TagType::ByteArray)) return;
    out_.write(static_cast<i32>(bytes.size()));
    out_.write_bytes(bytes);
}

void NbtWriter::write_int_array(std::string_view name, std::span<const i32> values) {
    if (values.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
    if (!begin_tag(name, TagType::IntArray)) return;
    out_.write(static_cast<i32>(values.size()));
    for (const i32 v : values) out_.write(v);
}

void NbtWriter::write_long_array(std::string_view name, std::span<const i64> values) {
    if (values.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
    if (!begin_tag(name, TagType::LongArray)) return;
    out_.write(static_cast<i32>(values.size()));
    for (const i64 v : values) out_.write(v);
}

// Iterative so hostile trees at the maximum depth cannot overflow the stack.
void NbtWriter::write_payload(const NbtValue& root) {
    auto leaf = [this](const NbtValue& value) {
        const auto& s = value.storage();
        switch (value.type()) {
        case TagType::Byte: return out_.write(std::get<i8>(s));
        case TagType::Short: return out_.write(std::get<i16>(s));
        case TagType::Int: return out_.write(std::get<i32>(s));
        case TagType::Long: return out_.write(std::get<i64>(s));
        case TagType::Float: return out_.write(std::bit_cast<u32>(std::get<f32>(s)));
        case TagType::Double: return out_.write(std::bit_cast<u64>(std::get<f64>(s)));
        case TagType::ByteArray: {
            const auto& a = std::get<std::vector<i8>>(s);
            if (a.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
            out_.write(static_cast<i32>(a.size()));
            out_.write_bytes(std::span<const u8>(reinterpret_cast<const u8*>(a.data()), a.size()));
            return;
        }
        case TagType::String: return write_string_payload(std::get<std::string>(s));
        case TagType::IntArray: {
            const auto& a = std::get<std::vector<i32>>(s);
            if (a.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
            out_.write(static_cast<i32>(a.size()));
            for (const i32 v : a) out_.write(v);
            return;
        }
        case TagType::LongArray: {
            const auto& a = std::get<std::vector<i64>>(s);
            if (a.size() > static_cast<usize>(std::numeric_limits<i32>::max())) return fail("array too long");
            out_.write(static_cast<i32>(a.size()));
            for (const i64 v : a) out_.write(v);
            return;
        }
        case TagType::End: return fail("cannot write a TAG_End value");
        case TagType::List:
        case TagType::Compound: return; // handled by the loop below
        }
    };

    struct Frame {
        const NbtValue* value;
        usize next; // index of the next child to write
    };
    std::vector<Frame> stack;

    // Writes a list header (on entering a list) and pushes containers.
    auto enter = [&](const NbtValue& v) -> bool {
        if (const auto* list = v.get_if<NbtList>()) {
            if (list->items.size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
                fail("list too long");
                return false;
            }
            if (!list->items.empty() && list->element_type == TagType::End) {
                fail("non-empty list of TAG_End");
                return false;
            }
            out_.write(static_cast<u8>(list->element_type));
            out_.write(static_cast<i32>(list->items.size()));
        }
        if (v.type() == TagType::List || v.type() == TagType::Compound) {
            if (stack.size() >= kMaxNbtDepth) {
                fail("NBT nested too deeply");
                return false;
            }
            stack.push_back({&v, 0});
        } else {
            leaf(v);
        }
        return true;
    };

    if (!enter(root)) return;
    while (!stack.empty() && error_ == nullptr) {
        Frame& f = stack.back();
        if (const auto* list = f.value->get_if<NbtList>()) {
            if (f.next == list->items.size()) {
                stack.pop_back();
                continue;
            }
            const NbtValue& item = list->items[f.next++];
            if (item.type() != list->element_type) return fail("list element has the wrong type");
            if (!enter(item)) return;
        } else {
            const auto& entries = f.value->get_if<NbtCompound>()->entries;
            if (f.next == entries.size()) {
                out_.write(static_cast<u8>(TagType::End));
                stack.pop_back();
                continue;
            }
            const NbtNamed& e = entries[f.next++];
            if (e.value.type() == TagType::End) return fail("cannot write a TAG_End value");
            if (e.name.size() > std::numeric_limits<u16>::max()) return fail("tag name longer than 65535 bytes");
            out_.write(static_cast<u8>(e.value.type()));
            out_.write(static_cast<u16>(e.name.size()));
            out_.write_bytes(e.name);
            if (!enter(e.value)) return;
        }
    }
}

void NbtWriter::write_value(std::string_view name, const NbtValue& value) {
    if (begin_tag(name, value.type())) write_payload(value);
}

void NbtWriter::write_view_payload(NbtView v) {
    // Same byte order: the payload can be copied as-is.
    if (v.endian() == out_.endian()) {
        out_.write_bytes(v.payload_bytes());
        return;
    }
    write_payload(materialize(v));
}

void NbtWriter::write_view(std::string_view name, NbtView view) {
    if (view.type() == TagType::End) return fail("cannot write a TAG_End value");
    if (begin_tag(name, view.type())) write_view_payload(view);
}

Result<std::vector<u8>> NbtWriter::finish() && {
    if (error_ == nullptr && !root_written_) error_ = "no root tag written";
    if (error_ == nullptr && !stack_.empty()) error_ = "unterminated compound or list";
    if (error_ != nullptr) return make_error(ErrorCode::InvalidArgument, std::string("NBT writer: ") + error_);
    return std::move(out_).take();
}

Result<std::vector<u8>> write_nbt(const NbtRoot& root, Endian endian) {
    NbtWriter w(endian);
    w.write_value(root.name, root.value);
    return std::move(w).finish();
}

} // namespace bridger::io::nbt
