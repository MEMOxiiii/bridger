// Bedrock block entities written for an older target version (docs/versions.md, "Target versions"):
//   signs   before 1.19.80 the front face's fields (Text, SignTextColor, IgnoreLighting) sit at the root, with
//           TextIgnoreLegacyBugResolved = 1, and there is no back face or wax flag (HiveGamesOSS/Chunker
//           BedrockSignBlockEntityHandler, consulted; MIT)
//   items   item ids and data values renamed since the target (BedrockItemUpgradeSchema, applied backwards), and
//           the Block tag of block items in the target's spelling (bedrock-targets.nbt)

#include "bedrock_block_entity_downgrade.hpp"

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <array>
#include <string>

namespace bridger::translate::detail {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

constexpr std::array<i32, 3> kSignFaces{1, 19, 80};
// Decorated pots: "shards" before 1.20.0, an item only from 1.20.50 (Chunker BedrockDecoratedPotBlockEntityHandler).
constexpr std::array<i32, 3> kPotSherds{1, 20, 0};
constexpr std::array<i32, 3> kPotItem{1, 20, 50};

bool before(const BedrockTarget& t, const std::array<i32, 3>& v) {
    return std::array<i32, 3>{t.version[0], t.version[1], t.version[2]} < v;
}

// The Block tag {name, states, version} of an item, respelled for the target (dropped if the target lacks it).
void write_block_tag(NbtWriter& w, NbtCompoundView block, const BedrockTargets& targets, const BedrockTarget& target,
                     const MappingData& mapping, std::vector<std::string>& lossy) {
    std::vector<BedrockProperty> props;
    for (const io::nbt::NbtEntry& e : block.find("states").entries()) {
        BedrockProperty p;
        p.key = e.name;
        if (const auto b = e.value.as_byte()) { p.type = NbtValueType::Byte; p.int_value = *b; }
        else if (const auto i = e.value.as_int()) { p.type = NbtValueType::Int; p.int_value = *i; }
        else if (const auto s = e.value.as_string()) { p.type = NbtValueType::String; p.str_value = *s; }
        props.push_back(p);
    }
    const auto idx = mapping.find_bedrock(block.find("name").as_string().value_or(""), props);
    if (!idx) return; // not a pinned state: leave the tag out (the game resolves the item by name)
    BedrockStateIndex state = *idx;
    const auto how = targets.resolve(target, state);
    if (how.kind == BedrockTargets::Written::Kind::Missing) {
        lossy.push_back("item block " + std::string(block.find("name").as_string().value_or("")) + " (not in this version)");
        return;
    }
    if (how.kind == BedrockTargets::Written::Kind::Nearest) state = how.nearest;
    const auto bytes = targets.state_nbt(target, state, mapping);
    const auto doc = io::nbt::NbtDocument::parse(bytes, Endian::Little);
    if (doc) w.write_view("Block", doc->root());
}

void write_items(NbtWriter& w, std::string_view name, NbtView list, const BedrockTargets& targets, const BedrockTarget& target,
                 const MappingData& mapping, std::vector<std::string>& lossy) {
    w.begin_list(name, list.items().empty() ? TagType::End : TagType::Compound, list.items().size());
    for (const NbtView it : list.items()) {
        w.begin_compound();
        const NbtCompoundView c = it.entries();
        const auto [id, damage] = targets.item_for(target, c.find("Name").as_string().value_or(""), c.find("Damage").as_short().value_or(0));
        for (const io::nbt::NbtEntry& e : c) {
            if (e.name == "Name") w.write_string("Name", id);
            else if (e.name == "Damage") w.write_short("Damage", damage);
            else if (e.name == "Block" && e.value.is(TagType::Compound)) write_block_tag(w, e.value.entries(), targets, target, mapping, lossy);
            else w.write_view(e.name, e.value);
        }
        w.end_compound();
    }
    w.end_list();
}

} // namespace

std::optional<std::vector<u8>> downgrade_bedrock_block_entity(const BlockEntity& be, const BedrockTargets& targets, const BedrockTarget& target,
                                                             const MappingData& mapping, std::vector<std::string>& lossy) {
    if (target.pinned) return std::nullopt;
    const auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    if (!doc || !doc->root().is(TagType::Compound)) return std::nullopt;
    const NbtCompoundView root = doc->root().entries();
    const bool sign = be.id == "Sign" || be.id == "HangingSign";
    const bool flat_sign = sign && before(target, kSignFaces);
    NbtWriter w(be.data.endian);
    w.begin_root_compound();
    for (const io::nbt::NbtEntry& e : root) {
        if (e.name == "Items" && e.value.is(TagType::List)) {
            write_items(w, "Items", e.value, targets, target, mapping, lossy);
        } else if (flat_sign && e.name == "FrontText") {
            for (const io::nbt::NbtEntry& f : e.value.entries())
                if (f.name == "Text" || f.name == "SignTextColor" || f.name == "IgnoreLighting") w.write_view(f.name, f.value);
            w.write_byte("TextIgnoreLegacyBugResolved", 1);
        } else if (flat_sign && e.name == "BackText") {
            if (const auto t = e.value.entries().find("Text").as_string(); t && !t->empty()) lossy.push_back("sign back text (before 1.19.80)");
        } else if (flat_sign && e.name == "IsWaxed") {
            continue;
        } else if (be.id == "DecoratedPot" && e.name == "sherds" && before(target, kPotSherds)) {
            w.write_view("shards", e.value);
        } else if (be.id == "DecoratedPot" && e.name == "item" && before(target, kPotItem)) {
            lossy.push_back("decorated pot item (before 1.20.50)");
        } else {
            w.write_view(e.name, e.value);
        }
    }
    w.end_compound();
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    return std::move(*bytes);
}

} // namespace bridger::translate::detail
