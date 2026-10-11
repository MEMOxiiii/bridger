#include <bridger/io/nbt/nbt_value.hpp>

#include <algorithm>
#include <bit>

namespace bridger::io::nbt {

NbtValue* NbtCompound::find(std::string_view name) noexcept {
    for (auto& e : entries)
        if (e.name == name) return &e.value;
    return nullptr;
}

const NbtValue* NbtCompound::find(std::string_view name) const noexcept {
    for (const auto& e : entries)
        if (e.name == name) return &e.value;
    return nullptr;
}

void NbtCompound::set(std::string name, NbtValue value) {
    if (NbtValue* existing = find(name)) {
        *existing = std::move(value);
        return;
    }
    entries.push_back(NbtNamed{std::move(name), std::move(value)});
}

usize NbtCompound::erase(std::string_view name) {
    const auto before = entries.size();
    std::erase_if(entries, [name](const NbtNamed& e) { return e.name == name; });
    return before - entries.size();
}

namespace {

bool is_container(TagType t) noexcept { return t == TagType::List || t == TagType::Compound; }

// Moves the direct children of `v` into `out` (leaving `v` shallow).
void take_children(NbtValue& v, std::vector<NbtValue>& out) {
    if (auto* list = v.get_if<NbtList>()) {
        for (auto& item : list->items)
            if (is_container(item.type())) out.push_back(std::move(item));
        list->items.clear();
    } else if (auto* comp = v.get_if<NbtCompound>()) {
        for (auto& e : comp->entries)
            if (is_container(e.value.type())) out.push_back(std::move(e.value));
        comp->entries.clear();
    }
}

bool leaf_equal(const NbtValue& a, const NbtValue& b) noexcept {
    const auto& x = a.storage();
    const auto& y = b.storage();
    switch (a.type()) {
    case TagType::End: return true;
    case TagType::Byte: return std::get<i8>(x) == std::get<i8>(y);
    case TagType::Short: return std::get<i16>(x) == std::get<i16>(y);
    case TagType::Int: return std::get<i32>(x) == std::get<i32>(y);
    case TagType::Long: return std::get<i64>(x) == std::get<i64>(y);
    case TagType::Float: return std::bit_cast<u32>(std::get<f32>(x)) == std::bit_cast<u32>(std::get<f32>(y));
    case TagType::Double: return std::bit_cast<u64>(std::get<f64>(x)) == std::bit_cast<u64>(std::get<f64>(y));
    case TagType::ByteArray: return std::get<std::vector<i8>>(x) == std::get<std::vector<i8>>(y);
    case TagType::String: return std::get<std::string>(x) == std::get<std::string>(y);
    case TagType::IntArray: return std::get<std::vector<i32>>(x) == std::get<std::vector<i32>>(y);
    case TagType::LongArray: return std::get<std::vector<i64>>(x) == std::get<std::vector<i64>>(y);
    case TagType::List:
    case TagType::Compound: break; // containers are handled by the caller
    }
    return false;
}

// A value with no children: leaves converted fully, containers created empty.
NbtValue shallow(NbtView v) {
    switch (v.type()) {
    case TagType::End: return {};
    case TagType::Byte: return *v.as_byte();
    case TagType::Short: return *v.as_short();
    case TagType::Int: return *v.as_int();
    case TagType::Long: return *v.as_long();
    case TagType::Float: return *v.as_float();
    case TagType::Double: return *v.as_double();
    case TagType::ByteArray: {
        const auto bytes = *v.as_byte_array();
        std::vector<i8> out(bytes.size());
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<u8*>(out.data()));
        return out;
    }
    case TagType::String: return std::string(*v.as_string());
    case TagType::List: {
        const NbtListView list = v.items();
        NbtList out;
        out.element_type = list.element_type();
        out.items.reserve(list.size());
        return out;
    }
    case TagType::Compound: return NbtCompound{};
    case TagType::IntArray: return v.as_int_array()->to_vector();
    case TagType::LongArray: return v.as_long_array()->to_vector();
    }
    return {};
}

} // namespace

NbtValue::~NbtValue() {
    if (!is_container(type())) return;
    // Flatten the tree onto the heap so destruction never recurses deeply.
    std::vector<NbtValue> pending;
    take_children(*this, pending);
    while (!pending.empty()) {
        NbtValue v = std::move(pending.back());
        pending.pop_back();
        take_children(v, pending);
    } // `v` is shallow when destroyed here
}

bool operator==(const NbtValue& a, const NbtValue& b) noexcept {
    std::vector<std::pair<const NbtValue*, const NbtValue*>> stack{{&a, &b}};
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (x->type() != y->type()) return false;
        if (const auto* lx = x->get_if<NbtList>()) {
            const auto* ly = y->get_if<NbtList>();
            if (lx->element_type != ly->element_type || lx->items.size() != ly->items.size()) return false;
            for (usize i = 0; i < lx->items.size(); ++i) stack.emplace_back(&lx->items[i], &ly->items[i]);
        } else if (const auto* cx = x->get_if<NbtCompound>()) {
            const auto* cy = y->get_if<NbtCompound>();
            if (cx->entries.size() != cy->entries.size()) return false;
            for (usize i = 0; i < cx->entries.size(); ++i) {
                if (cx->entries[i].name != cy->entries[i].name) return false;
                stack.emplace_back(&cx->entries[i].value, &cy->entries[i].value);
            }
        } else if (!leaf_equal(*x, *y)) {
            return false;
        }
    }
    return true;
}

NbtValue materialize(NbtView root) {
    NbtValue result = shallow(root);
    if (!is_container(root.type())) return result;

    // Depth-first with an explicit stack. A frame's target lives inside its parent's
    // vector, which is not modified again until the frame is popped.
    struct Frame {
        NbtValue* target;
        NbtListView::iterator list_it;
        NbtListView::iterator list_end;
        NbtCompoundView::iterator comp_it;
    };
    auto make_frame = [](NbtValue* target, NbtView source) {
        Frame f{target, {}, {}, {}};
        if (source.is(TagType::List)) {
            const NbtListView list = source.items();
            f.list_it = list.begin();
            f.list_end = list.end();
        } else {
            f.comp_it = source.entries().begin();
        }
        return f;
    };
    std::vector<Frame> stack;
    stack.push_back(make_frame(&result, root));
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (auto* list = f.target->get_if<NbtList>()) {
            if (f.list_it == f.list_end) {
                stack.pop_back();
                continue;
            }
            const NbtView item = *f.list_it;
            ++f.list_it;
            list->items.push_back(shallow(item));
            if (is_container(item.type())) stack.push_back(make_frame(&list->items.back(), item));
        } else {
            auto* comp = f.target->get_if<NbtCompound>();
            if (f.comp_it == NbtCompoundView::iterator{}) {
                stack.pop_back();
                continue;
            }
            const NbtEntry entry = *f.comp_it;
            ++f.comp_it;
            comp->entries.push_back(NbtNamed{std::string(entry.name), shallow(entry.value)});
            if (is_container(entry.value.type())) stack.push_back(make_frame(&comp->entries.back().value, entry.value));
        }
    }
    return result;
}

NbtRoot materialize(const NbtDocument& doc) { return NbtRoot{std::string(doc.root_name()), materialize(doc.root())}; }

Result<NbtRoot> read_nbt(std::span<const u8> data, Endian endian, const NbtLimits& limits) {
    BRIDGER_TRY(const NbtDocument doc, NbtDocument::parse(data, endian, limits));
    return materialize(doc);
}

} // namespace bridger::io::nbt
