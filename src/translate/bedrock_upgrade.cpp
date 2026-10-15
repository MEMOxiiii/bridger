// Bedrock block state upgrades. Data layout: tools/mapgen/build_bedrock_upgrade.py; rules:
// docs/formats/bedrock-world.md ("Old worlds"), after the schema documentation of
// BedrockBlockUpgradeSchema (README, nbt_upgrade_schema_schema.json).

#include <bridger/translate/bedrock_upgrade.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace bridger::embedded {
extern const unsigned char bedrock_upgrade_nbt[];
extern const std::size_t bedrock_upgrade_nbt_size;
} // namespace bridger::embedded

namespace bridger::translate {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

namespace {

using Props = std::vector<std::pair<std::string, UpgradeValue>>;

struct Flatten {
    std::string prefix, property, suffix;
    NbtValueType type = NbtValueType::String;
    std::unordered_map<std::string, std::string> remaps;
};

struct Remap {
    Props old_state;
    std::optional<std::string> new_name;
    std::optional<Flatten> new_flattened;
    Props new_state;
    std::vector<std::string> copied;
};

struct Schema {
    i32 id = 0;
    i32 version = 0;
    std::unordered_map<std::string, std::string> renamed_ids;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> renamed_properties;
    std::unordered_map<std::string, Props> added_properties;
    std::unordered_map<std::string, std::vector<std::string>> removed_properties;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::pair<UpgradeValue, UpgradeValue>>>> remapped_values;
    std::unordered_map<std::string, Flatten> flattened;
    std::unordered_map<std::string, std::vector<Remap>> remapped_states;
};

std::optional<UpgradeValue> value_of(NbtView v) {
    UpgradeValue out;
    if (const auto b = v.as_byte()) { out.type = NbtValueType::Byte; out.int_value = *b; }
    else if (const auto i = v.as_int()) { out.type = NbtValueType::Int; out.int_value = *i; }
    else if (const auto s = v.as_string()) { out.type = NbtValueType::String; out.str_value = std::string(*s); }
    else return std::nullopt;
    return out;
}

Result<Props> props_of(NbtView compound) {
    Props out;
    for (const io::nbt::NbtEntry& e : compound.entries()) {
        const auto v = value_of(e.value);
        if (!v) return make_error(ErrorCode::Malformed, "bedrock upgrade data: property '" + std::string(e.name) + "' has an unsupported type");
        out.emplace_back(std::string(e.name), *v);
    }
    return out;
}

std::string str(NbtView v) { return std::string(v.as_string().value_or("")); }

Flatten flatten_of(NbtCompoundView c) {
    Flatten f;
    f.prefix = str(c.find("prefix"));
    f.property = str(c.find("property"));
    f.suffix = str(c.find("suffix"));
    const std::string t = str(c.find("type"));
    f.type = t == "int" ? NbtValueType::Int : t == "byte" ? NbtValueType::Byte : NbtValueType::String;
    for (const io::nbt::NbtEntry& e : c.find("remaps").entries()) f.remaps.emplace(std::string(e.name), str(e.value));
    return f;
}

UpgradeValue* find_prop(Props& p, std::string_view key) {
    for (auto& [k, v] : p)
        if (k == key) return &v;
    return nullptr;
}
const UpgradeValue* find_prop(const Props& p, std::string_view key) {
    for (const auto& [k, v] : p)
        if (k == key) return &v;
    return nullptr;
}
void erase_prop(Props& p, std::string_view key) {
    std::erase_if(p, [&](const auto& kv) { return kv.first == key; });
}
void set_prop(Props& p, const std::string& key, const UpgradeValue& v) {
    if (UpgradeValue* old = find_prop(p, key)) *old = v;
    else p.emplace_back(key, v);
}

// The flattened name, or nullopt if the property is absent or of another type (then the rule
// does not apply).
std::optional<std::string> flattened_name(const Flatten& f, const Props& states) {
    const UpgradeValue* v = find_prop(states, f.property);
    if (v == nullptr || v->type != f.type) return std::nullopt;
    const std::string key = v->type == NbtValueType::String ? v->str_value : std::to_string(v->int_value);
    const auto it = f.remaps.find(key);
    return f.prefix + (it != f.remaps.end() ? it->second : key) + f.suffix;
}

} // namespace

std::vector<BedrockProperty> UpgradedState::properties() const {
    std::vector<BedrockProperty> out;
    out.reserve(states.size());
    for (const auto& [k, v] : states) out.push_back({k, v.type, v.int_value, v.str_value});
    return out;
}

struct BedrockUpgrader::Impl {
    // Schemas grouped by version id, in application order.
    std::vector<std::pair<i32, std::vector<Schema>>> groups;
    usize schema_count = 0;
    i32 latest = 0;
    std::unordered_map<std::string, std::vector<std::pair<i32, UpgradedState>>> legacy_states; // id -> (data, 1.12 state)
    std::unordered_map<i32, std::string> legacy_names;                                          // numeric id -> string id

    void apply(const Schema& s, std::string& name, Props& states) const {
        // remappedStates: highest priority; a match replaces the whole state.
        if (const auto it = s.remapped_states.find(name); it != s.remapped_states.end()) {
            for (const Remap& r : it->second) {
                bool match = r.old_state.size() <= states.size();
                for (const auto& [k, v] : r.old_state) {
                    if (!match) break;
                    const UpgradeValue* have = find_prop(states, k);
                    match = have != nullptr && *have == v;
                }
                if (!match) continue;
                std::string new_name;
                if (r.new_name) new_name = *r.new_name;
                else if (const auto f = flattened_name(*r.new_flattened, states)) new_name = *f;
                else new_name = name;
                Props next = r.new_state;
                for (const std::string& k : r.copied)
                    if (const UpgradeValue* v = find_prop(states, k)) set_prop(next, k, *v);
                name = std::move(new_name);
                states = std::move(next);
                return;
            }
        }
        // The other rules are indexed by the old name and old property names, so order is free.
        const std::string old = name;
        if (const auto it = s.renamed_ids.find(old); it != s.renamed_ids.end()) {
            name = it->second;
        } else if (const auto ft = s.flattened.find(old); ft != s.flattened.end()) {
            if (const auto f = flattened_name(ft->second, states)) {
                name = *f;
                erase_prop(states, ft->second.property);
            }
        }
        if (const auto it = s.added_properties.find(old); it != s.added_properties.end())
            for (const auto& [k, v] : it->second)
                if (find_prop(states, k) == nullptr) states.emplace_back(k, v);
        if (const auto it = s.removed_properties.find(old); it != s.removed_properties.end())
            for (const std::string& k : it->second) erase_prop(states, k);

        const auto values = s.remapped_values.find(old);
        auto remap_value = [&](const std::string& prop, const UpgradeValue& v) -> UpgradeValue {
            if (values == s.remapped_values.end()) return v;
            const auto pit = values->second.find(prop);
            if (pit == values->second.end()) return v;
            for (const auto& [from, to] : pit->second)
                if (from == v) return to;
            return v;
        };
        if (const auto it = s.renamed_properties.find(old); it != s.renamed_properties.end()) {
            for (const auto& [from, to] : it->second) {
                const UpgradeValue* v = find_prop(states, from);
                if (v == nullptr) continue;
                const UpgradeValue nv = remap_value(from, *v); // value remaps use the old property name
                erase_prop(states, from);
                set_prop(states, to, nv);
            }
        }
        if (values != s.remapped_values.end())
            for (const auto& [prop, pairs] : values->second)
                if (UpgradeValue* v = find_prop(states, prop)) *v = remap_value(prop, *v);
    }
};

BedrockUpgrader::BedrockUpgrader() : impl_(std::make_unique<Impl>()) {}
BedrockUpgrader::BedrockUpgrader(BedrockUpgrader&&) noexcept = default;
BedrockUpgrader& BedrockUpgrader::operator=(BedrockUpgrader&&) noexcept = default;
BedrockUpgrader::~BedrockUpgrader() = default;

usize BedrockUpgrader::schema_count() const noexcept { return impl_->schema_count; }
i32 BedrockUpgrader::latest_version() const noexcept { return impl_->latest; }

Result<BedrockUpgrader> BedrockUpgrader::load(std::span<const u8> nbt) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Little));
    if (!doc.root().is(TagType::Compound)) return make_error(ErrorCode::Malformed, "bedrock upgrade data: root is not a compound");
    const NbtCompoundView root = doc.root().entries();
    BedrockUpgrader u;
    Impl& m = *u.impl_;

    for (const NbtView sv : root.find("schemas").items()) {
        const NbtCompoundView c = sv.entries();
        Schema s;
        s.id = c.find("id").as_int().value_or(0);
        s.version = c.find("version").as_int().value_or(0);
        for (const io::nbt::NbtEntry& e : c.find("renamed_ids").entries()) s.renamed_ids.emplace(std::string(e.name), str(e.value));
        for (const io::nbt::NbtEntry& e : c.find("renamed_properties").entries()) {
            auto& list = s.renamed_properties[std::string(e.name)];
            for (const io::nbt::NbtEntry& p : e.value.entries()) list.emplace_back(std::string(p.name), str(p.value));
        }
        for (const io::nbt::NbtEntry& e : c.find("added_properties").entries()) {
            BRIDGER_TRY(Props p, props_of(e.value));
            s.added_properties.emplace(std::string(e.name), std::move(p));
        }
        for (const io::nbt::NbtEntry& e : c.find("removed_properties").entries()) {
            auto& list = s.removed_properties[std::string(e.name)];
            for (const NbtView p : e.value.items()) list.push_back(str(p));
        }
        for (const io::nbt::NbtEntry& e : c.find("remapped_values").entries()) {
            auto& block = s.remapped_values[std::string(e.name)];
            for (const io::nbt::NbtEntry& p : e.value.entries()) {
                auto& pairs = block[std::string(p.name)];
                for (const NbtView pair : p.value.items()) {
                    const auto from = value_of(pair.entries().find("old")), to = value_of(pair.entries().find("new"));
                    if (!from || !to) return make_error(ErrorCode::Malformed, "bedrock upgrade data: bad value remap");
                    pairs.emplace_back(*from, *to);
                }
            }
        }
        for (const io::nbt::NbtEntry& e : c.find("flattened").entries()) s.flattened.emplace(std::string(e.name), flatten_of(e.value.entries()));
        for (const io::nbt::NbtEntry& e : c.find("remapped_states").entries()) {
            auto& list = s.remapped_states[std::string(e.name)];
            for (const NbtView rv : e.value.items()) {
                const NbtCompoundView rc = rv.entries();
                Remap r;
                BRIDGER_TRY(r.old_state, props_of(rc.find("old")));
                BRIDGER_TRY(r.new_state, props_of(rc.find("new")));
                if (const auto n = rc.find("new_name").as_string()) r.new_name = std::string(*n);
                else if (const NbtView f = rc.find("new_flattened", TagType::Compound)) r.new_flattened = flatten_of(f.entries());
                else return make_error(ErrorCode::Malformed, "bedrock upgrade data: remap without a new name");
                for (const NbtView k : rc.find("copied").items()) r.copied.push_back(str(k));
                list.push_back(std::move(r));
            }
        }
        if (m.groups.empty() || m.groups.back().first != s.version) {
            if (!m.groups.empty() && m.groups.back().first > s.version)
                return make_error(ErrorCode::Malformed, "bedrock upgrade data: schemas out of order");
            m.groups.emplace_back(s.version, std::vector<Schema>{});
        }
        m.latest = std::max(m.latest, s.version);
        m.groups.back().second.push_back(std::move(s));
        ++m.schema_count;
    }
    if (m.schema_count == 0) return make_error(ErrorCode::Malformed, "bedrock upgrade data: no schemas");

    for (const io::nbt::NbtEntry& e : root.find("legacy_ids").entries())
        if (const auto id = e.value.as_int()) m.legacy_names.emplace(*id, std::string(e.name));
    for (const io::nbt::NbtEntry& e : root.find("legacy_states").entries()) {
        auto& list = m.legacy_states[std::string(e.name)];
        for (const NbtView sv : e.value.items()) {
            const NbtCompoundView c = sv.entries();
            UpgradedState st;
            st.name = str(c.find("name"));
            BRIDGER_TRY(st.states, props_of(c.find("states")));
            st.version = c.find("version").as_int().value_or(0);
            list.emplace_back(c.find("meta").as_int().value_or(0), std::move(st));
        }
    }
    return u;
}

Result<const BedrockUpgrader*> BedrockUpgrader::embedded() {
    static std::once_flag once;
    static std::optional<Result<BedrockUpgrader>> loaded;
    std::call_once(once, [] {
        loaded.emplace(load(std::span<const u8>(embedded::bedrock_upgrade_nbt, embedded::bedrock_upgrade_nbt_size)));
    });
    if (!*loaded) return tl::unexpected((*loaded).error());
    return &**loaded;
}

UpgradedState BedrockUpgrader::upgrade(UpgradedState state) const {
    for (const auto& [version, schemas] : impl_->groups) {
        if (state.version > version || (schemas.size() == 1 && state.version == version)) continue;
        for (const Schema& s : schemas) impl_->apply(s, state.name, state.states);
    }
    state.version = std::max(state.version, impl_->latest);
    return state;
}

std::optional<UpgradedState> BedrockUpgrader::from_legacy(std::string_view id, i32 data) const {
    std::string key(id);
    if (key.find(':') == std::string::npos) key = "minecraft:" + key;
    const auto it = impl_->legacy_states.find(key);
    if (it == impl_->legacy_states.end()) return std::nullopt;
    const UpgradedState* base = nullptr;
    for (const auto& [meta, st] : it->second) {
        if (meta == data) { base = &st; break; }
        if (meta == 0 && base == nullptr) base = &st; // PocketMine-MP's fallback for unknown data values
    }
    if (base == nullptr) return std::nullopt;
    return upgrade(*base);
}

std::optional<UpgradedState> BedrockUpgrader::from_legacy_id(i32 numeric_id, i32 data) const {
    const auto it = impl_->legacy_names.find(numeric_id);
    if (it == impl_->legacy_names.end()) return std::nullopt;
    return from_legacy(it->second, data);
}

std::optional<UpgradedState> BedrockUpgrader::from_entry(NbtCompoundView entry) const {
    const auto val = entry.find("val").as_short();
    if (const auto id = entry.find(kLegacyIdKey).as_int()) return from_legacy_id(*id, val.value_or(0));
    const auto name = entry.find("name").as_string();
    if (!name) return std::nullopt;
    if (val) return from_legacy(*name, *val);
    UpgradedState st;
    st.name = std::string(*name);
    if (st.name.find(':') == std::string::npos) st.name = "minecraft:" + st.name;
    auto props = props_of(entry.find("states"));
    if (!props) return std::nullopt;
    st.states = std::move(*props);
    st.version = entry.find("version").as_int().value_or(0);
    return upgrade(std::move(st));
}

} // namespace bridger::translate
