#include "bedrock_entities.hpp"

#include <bridger/core/log.hpp>
#include <bridger/io/nbt/nbt_view.hpp>

#include <mutex>
#include <unordered_map>

namespace bridger::embedded {
extern const unsigned char bedrock_entities_nbt[];
extern const std::size_t bedrock_entities_nbt_size;
} // namespace bridger::embedded

namespace bridger::translate::detail {
namespace {

using io::nbt::NbtCompoundView;

struct Outcome {
    std::vector<std::string> groups;
    i32 weight = 0;
    i32 variant = 0, mark_variant = 0, color = 0, color2 = 0, skin_id = 0; // absent in the data: 0, as Bedrock defaults
    bool baby = false, tamed = false, saddled = false, chested = false, sheared = false;
};

struct EntityData {
    std::vector<Outcome> outcomes;
    std::vector<BedrockProperty> properties;
    i32 health = 0;
};

using Table = std::unordered_map<std::string, EntityData>;

Table load() {
    Table t;
    const auto doc = io::nbt::NbtDocument::parse(std::span<const u8>(embedded::bedrock_entities_nbt, embedded::bedrock_entities_nbt_size),
                                                 Endian::Little);
    if (!doc) {
        log::error("bedrock-entities.nbt: {}", doc.error().describe());
        return t;
    }
    for (const auto& entry : doc->root().entries().find("entities").entries()) {
        EntityData d;
        const NbtCompoundView e = entry.value.entries();
        std::vector<std::string> names; // each outcome's groups are indices into these
        for (const auto& n : e.find("names").items()) names.emplace_back(n.as_string().value_or(""));
        for (const auto& o : e.find("outcomes").items()) {
            const NbtCompoundView c = o.entries();
            Outcome out;
            if (const auto idx = c.find("groups").as_byte_array())
                for (const u8 k : *idx)
                    if (k < names.size()) out.groups.push_back(names[k]);
            out.weight = c.find("p").as_int().value_or(0);
            out.variant = c.find("variant").as_int().value_or(0);
            out.mark_variant = c.find("mark_variant").as_int().value_or(0);
            out.color = c.find("color").as_int().value_or(0);
            out.color2 = c.find("color2").as_int().value_or(0);
            out.skin_id = c.find("skin_id").as_int().value_or(0);
            out.baby = c.find("baby").as_int().value_or(0) != 0;
            out.tamed = c.find("tamed").as_int().value_or(0) != 0;
            out.saddled = c.find("saddled").as_int().value_or(0) != 0;
            out.chested = c.find("chested").as_int().value_or(0) != 0;
            out.sheared = c.find("sheared").as_int().value_or(0) != 0;
            d.outcomes.push_back(std::move(out));
        }
        for (const auto& p : e.find("properties").items()) {
            const NbtCompoundView c = p.entries();
            BedrockProperty prop;
            prop.name = std::string(c.find("name").as_string().value_or(""));
            if (const auto s = c.find("enum").as_string()) {
                prop.kind = BedrockProperty::Kind::Enum;
                prop.text = std::string(*s);
            } else if (const auto bv = c.find("bool").as_byte()) {
                prop.kind = BedrockProperty::Kind::Bool;
                prop.number = *bv;
            } else {
                prop.kind = BedrockProperty::Kind::Int;
                prop.number = c.find("int").as_int().value_or(0);
            }
            d.properties.push_back(std::move(prop));
        }
        d.health = e.find("health").as_int().value_or(0);
        t.emplace(std::string(entry.name), std::move(d));
    }
    return t;
}

const Table& table() {
    static std::once_flag once;
    static Table t;
    std::call_once(once, [] { t = load(); });
    return t;
}

const EntityData* find(std::string_view identifier) {
    const auto it = table().find(std::string(identifier));
    return it == table().end() ? nullptr : &it->second;
}

} // namespace

std::vector<std::string> bedrock_definitions(std::string_view identifier, const MobState& s) {
    std::vector<std::string> out{"+" + std::string(identifier)};
    const EntityData* d = find(identifier);
    if (d == nullptr || d->outcomes.empty()) return out;
    // The same rule as tools/mapgen/build_bedrock_entities.py choose(): the baby/adult state must match; then the most
    // matching fields win, the likeliest outcome on a tie.
    const Outcome* best = nullptr;
    std::pair<int, i32> best_key{-1, -1};
    for (const bool strict : {true, false}) {
        for (const Outcome& o : d->outcomes) {
            if (strict && o.baby != s.baby) continue;
            int score = 0;
            const auto eq = [&score](const auto& want, auto have) {
                if (want && *want == have) ++score;
            };
            eq(s.variant, o.variant);
            eq(s.mark_variant, o.mark_variant);
            eq(s.color, o.color);
            eq(s.color2, o.color2);
            eq(s.skin_id, o.skin_id);
            eq(s.tamed, o.tamed);
            eq(s.saddled, o.saddled);
            eq(s.chested, o.chested);
            eq(s.sheared, o.sheared);
            const std::pair<int, i32> key{score, o.weight};
            if (best == nullptr || key > best_key) {
                best = &o;
                best_key = key;
            }
        }
        if (best != nullptr) break;
    }
    for (const std::string& g : best->groups) out.push_back("+" + g);
    return out;
}

std::span<const BedrockProperty> bedrock_properties(std::string_view identifier) {
    const EntityData* d = find(identifier);
    return d ? std::span<const BedrockProperty>(d->properties) : std::span<const BedrockProperty>();
}

i32 bedrock_max_health(std::string_view identifier) {
    const EntityData* d = find(identifier);
    return d ? d->health : 0;
}

} // namespace bridger::translate::detail
