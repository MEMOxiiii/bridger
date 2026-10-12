// Java -> Bedrock block entities. Formats and sources: docs/formats/bedrock-world.md.

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>

#include "tables.hpp"

#include <array>
#include <string>

namespace bridger::translate {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

using tables::kDyeColors;
using tables::dye_index;

std::optional<i8> bed_color(std::string_view block_id) {
    // "minecraft:<color>_bed"
    constexpr std::string_view ns = "minecraft:";
    if (!block_id.starts_with(ns) || !block_id.ends_with("_bed")) return std::nullopt;
    const std::string_view color = block_id.substr(ns.size(), block_id.size() - ns.size() - 4);
    for (usize i = 0; i < kDyeColors.size(); ++i)
        if (kDyeColors[i] == color) return static_cast<i8>(i);
    return std::nullopt;
}

bool is_chest(std::string_view id) { return id == "minecraft:chest" || id == "minecraft:trapped_chest"; }

// Block entities written with their id only (Bedrock ids as in Chunker's handler list).
std::optional<std::string_view> simple_marker(std::string_view java_id) {
    if (java_id == "minecraft:soul_campfire") return "Campfire";
    for (const tables::Pair& m : tables::kMarkers)
        if (m.java == java_id) return m.bedrock;
    return std::nullopt;
}

// "minecraft:<color>_banner" / "minecraft:<color>_wall_banner" -> dye index.
std::optional<i32> banner_color(std::string_view id) {
    constexpr std::string_view ns = "minecraft:";
    if (!id.starts_with(ns)) return std::nullopt;
    std::string_view rest = id.substr(ns.size());
    if (rest.ends_with("_wall_banner")) rest.remove_suffix(12);
    else if (rest.ends_with("_banner")) rest.remove_suffix(7);
    else return std::nullopt;
    return dye_index(rest);
}

// Bedrock SkullType by Java block (tables::kSkulls).
std::optional<i8> skull_type(std::string_view id) {
    for (const tables::Skull& e : tables::kSkulls)
        if (id == e.floor || id == e.wall) return e.type;
    return std::nullopt;
}

// Java banner pattern id -> Bedrock short code (tables::kBannerPatterns).
std::optional<std::string_view> pattern_code(std::string_view java) {
    if (java.starts_with("minecraft:")) java.remove_prefix(10);
    for (const tables::Pair& p : tables::kBannerPatterns)
        if (p.java == java) return p.bedrock;
    return std::nullopt;
}

// "minecraft:potted_<plant>" -> the plant block shown in a Bedrock flower pot.
std::optional<std::string> potted_plant(std::string_view id) {
    constexpr std::string_view prefix = "minecraft:potted_";
    if (!id.starts_with(prefix)) return std::nullopt;
    const std::string_view plant = id.substr(prefix.size());
    if (plant == "azalea_bush") return std::string("minecraft:azalea");
    if (plant == "flowering_azalea_bush") return std::string("minecraft:flowering_azalea");
    return "minecraft:" + std::string(plant);
}

// Containers whose slot layout is the same in both editions (tables::kContainers).
std::optional<std::string_view> simple_container(std::string_view java_id) {
    for (const tables::Pair& e : tables::kContainers)
        if (e.java == java_id) return e.bedrock;
    if (java_id == "minecraft:trapped_chest") return "Chest"; // Bedrock stores trapped chests as "Chest"
    return std::nullopt;
}

// Bedrock pairing of a Java double chest (tables::chest_partner).
std::optional<BlockPos> chest_partner(const BlockState& s, BlockPos pos) {
    const auto type = s.property("type");
    const auto facing = s.property("facing");
    if (!type || !facing) return std::nullopt;
    return tables::chest_partner(*type, *facing, pos);
}

// Plain text of a Java text component: a plain string, a JSON string (pre-1.21.5
// files), or an NBT compound/list (1.21.5+). Formatting is dropped; `lossy` is set
// when anything besides text was present.
std::string text_of(NbtView v, bool& lossy);

std::string json_text(std::string_view json, bool& lossy) {
    // Minimal scan for "text":"..." values in order; enough for names and sign lines.
    std::string out;
    bool any = false;
    usize i = 0;
    while ((i = json.find("\"text\"", i)) != std::string_view::npos) {
        i = json.find(':', i + 6);
        if (i == std::string_view::npos) break;
        i = json.find('"', i + 1);
        if (i == std::string_view::npos) break;
        ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                const char e = json[i + 1];
                out += e == 'n' ? '\n' : e;
                i += 2;
            } else {
                out += json[i++];
            }
        }
        any = true;
    }
    if (!any) {
        // A bare JSON string: "abc"
        if (json.size() >= 2 && json.front() == '"' && json.back() == '"') return std::string(json.substr(1, json.size() - 2));
        if (!json.empty()) lossy = true; // e.g. a translatable component
        return {};
    }
    for (std::string_view key : {"\"color\"", "\"bold\"", "\"italic\"", "\"translate\"", "\"underlined\"",
                                 "\"strikethrough\"", "\"obfuscated\"", "\"clickEvent\"", "\"hoverEvent\""})
        if (json.find(key) != std::string_view::npos) lossy = true;
    return out;
}

std::string text_of(NbtView v, bool& lossy) {
    if (const auto s = v.as_string()) {
        auto utf8 = io::nbt::mutf8_to_utf8(*s);
        const std::string text = utf8 ? *utf8 : std::string(*s);
        if (!text.empty() && (text.front() == '{' || text.front() == '[' || text.front() == '"'))
            return json_text(text, lossy);
        return text;
    }
    if (v.is(TagType::Compound)) {
        std::string out;
        for (const io::nbt::NbtEntry& e : v.entries()) {
            if (e.name == "text") out += text_of(e.value, lossy);
            else if (e.name == "extra") for (const NbtView x : e.value.items()) out += text_of(x, lossy);
            else lossy = true;
        }
        return out;
    }
    if (v.is(TagType::List)) {
        std::string out;
        for (const NbtView x : v.items()) out += text_of(x, lossy);
        return out;
    }
    lossy = true;
    return {};
}

// Writes the Bedrock "Block" compound {name, states, version} for an item that is a block.
void write_block_tag(NbtWriter& w, std::string_view name, const MappingData& m, BedrockStateIndex idx) {
    const BedrockState& s = m.bedrock(idx);
    w.begin_compound(name);
    w.write_string("name", s.name);
    w.begin_compound("states");
    for (const BedrockProperty& p : s.properties) {
        switch (p.type) {
        case NbtValueType::Byte: w.write_byte(p.key, static_cast<i8>(p.int_value)); break;
        case NbtValueType::Int: w.write_int(p.key, p.int_value); break;
        case NbtValueType::String: w.write_string(p.key, p.str_value); break;
        }
    }
    w.end_compound();
    w.write_int("version", static_cast<i32>(m.bedrock_block_version()));
    w.end_compound();
}

// Java "Items" list -> Bedrock "Items" list. Unknown items and dropped item data are reported.
void write_items(NbtWriter& w, NbtView java_items, const MappingData& m, ConversionReport& report, BlockPos pos) {
    struct Item {
        std::string bedrock;
        i16 damage;
        i8 count;
        i8 slot;
        std::optional<BedrockStateIndex> block;
    };
    std::vector<Item> items;
    for (const NbtView it : java_items.items()) {
        const NbtCompoundView c = it.entries();
        const auto id = c.find("id").as_string();
        if (!id) continue;
        const std::string java_id(*id);
        // 1.20.5+: "count" (Int); older: "Count" (Byte).
        i32 count = 1;
        if (const auto n = c.find("count").as_int()) count = *n;
        else if (const auto b = c.find("Count").as_byte()) count = *b;
        const i8 slot = c.find("Slot").as_byte().value_or(0);

        const auto mapping = m.find_item(java_id);
        if (!mapping) {
            report.add(IssueKind::UnmappedBlockEntity, "item " + java_id, pos, static_cast<u64>(count));
            continue;
        }
        if (c.find("components") || c.find("tag"))
            report.add(IssueKind::LossyBlock, "item data dropped: " + java_id, pos);
        if (count < 1 || count > 127) {
            report.add(IssueKind::LossyBlock, "item count clamped: " + java_id, pos);
            count = count < 1 ? 1 : 127;
        }
        Item out{std::string(mapping->bedrock_name), mapping->damage, static_cast<i8>(count), slot, std::nullopt};
        if (mapping->java_block_state) {
            const JavaStateMapping jm = m.map(*mapping->java_block_state);
            if (jm.bedrock != UINT32_MAX) out.block = jm.bedrock;
        }
        items.push_back(std::move(out));
    }
    w.begin_list("Items", items.empty() ? TagType::End : TagType::Compound, items.size());
    for (const Item& item : items) {
        w.begin_compound();
        w.write_string("Name", item.bedrock);
        w.write_byte("Count", item.count);
        w.write_short("Damage", item.damage);
        w.write_byte("Slot", item.slot);
        w.write_byte("WasPickedUp", 0);
        if (item.block) write_block_tag(w, "Block", m, *item.block);
        w.end_compound();
    }
    w.end_list();
}

void write_custom_name(NbtWriter& w, NbtCompoundView c, ConversionReport& report, BlockPos pos) {
    const NbtView name = c.find("CustomName");
    if (!name) return;
    bool lossy = false;
    const std::string text = text_of(name, lossy);
    if (lossy) report.add(IssueKind::LossyBlock, "custom name formatting dropped", pos);
    if (auto m = io::nbt::utf8_to_mutf8(text)) w.write_string("CustomName", *m);
}

void write_pairing(NbtWriter& w, const BlockState& state, BlockPos pos) {
    if (const auto partner = chest_partner(state, pos)) {
        w.write_int(kLocalPairX, partner->x);
        w.write_int(kLocalPairZ, partner->z);
        if (state.property("type") == "right") w.write_byte("pairlead", 1);
    }
}

void write_sign_face(NbtWriter& w, std::string_view name, NbtView java_face, std::optional<std::array<NbtView, 4>> legacy,
                     ConversionReport& report, BlockPos pos) {
    bool lossy = false;
    std::string text;
    bool glowing = false;
    if (java_face) {
        const NbtCompoundView f = java_face.entries();
        usize line = 0;
        for (const NbtView msg : f.find("messages").items()) {
            if (line++ > 0) text += '\n';
            text += text_of(msg, lossy);
        }
        glowing = f.find("has_glowing_text").as_byte().value_or(0) != 0;
        if (const auto color = f.find("color").as_string(); color && *color != "black") lossy = true;
    } else if (legacy) {
        for (usize i = 0; i < 4; ++i) {
            if (i > 0) text += '\n';
            if ((*legacy)[i]) text += text_of((*legacy)[i], lossy);
        }
    }
    while (text.ends_with('\n')) text.pop_back();
    if (lossy) report.add(IssueKind::LossyBlock, "sign text formatting dropped", pos);
    w.begin_compound(name);
    if (auto m = io::nbt::utf8_to_mutf8(text)) w.write_string("Text", *m);
    w.write_int("SignTextColor", static_cast<i32>(0xFF000000u)); // black
    w.write_byte("IgnoreLighting", glowing ? 1 : 0);
    w.write_byte("TextIgnoreLegacyBugResolved", 1);
    w.end_compound();
}

std::optional<RawNbt> finish(NbtWriter&& w) {
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    return RawNbt{std::move(*bytes), Endian::Little};
}

void write_banner(NbtWriter& w, const BlockState& state, NbtView java_patterns, ConversionReport& report, BlockPos pos) {
    // Bedrock stores dye colors reversed (15 - index), Chunker ChunkerDyeColor::getReversedID.
    w.write_int("Base", 15 - banner_color(state.id()).value_or(0));
    struct Layer {
        std::string code;
        i32 color;
    };
    std::vector<Layer> layers;
    for (const NbtView p : java_patterns.items()) {
        const auto c = p.entries();
        std::optional<std::string_view> pid = c.find("pattern").as_string();
        const auto color = c.find("color").as_string();
        const auto code = pid ? pattern_code(*pid) : std::nullopt;
        const auto dye = color ? dye_index(*color) : std::nullopt;
        if (!code || !dye) {
            report.add(IssueKind::LossyBlock, "banner pattern not converted", pos);
            continue;
        }
        layers.push_back({std::string(*code), 15 - *dye});
    }
    w.begin_list("Patterns", layers.empty() ? TagType::End : TagType::Compound, layers.size());
    for (const Layer& l : layers) {
        w.begin_compound();
        w.write_int("Color", l.color);
        w.write_string("Pattern", l.code);
        w.end_compound();
    }
    w.end_list();
}

void write_skull(NbtWriter& w, const BlockState& state) {
    w.write_byte("SkullType", skull_type(state.id()).value_or(0));
    const auto rot = state.property("rotation"); // floor skulls only; wall skulls face via the block state
    w.write_float("Rotation", rot ? static_cast<f32>(std::stoi(std::string(*rot))) * 22.5f : 0.0f);
    w.write_byte("DoingAnimation", state.property("powered") == "true" ? 1 : 0);
}

bool write_flower_pot(NbtWriter& w, const BlockState& state, const MappingData& m) {
    if (const auto plant = potted_plant(state.id())) {
        const auto jid = m.find_java_state(BlockState(*plant, {}));
        if (!jid) return false;
        const JavaStateMapping jm = m.map(*jid);
        if (jm.bedrock == UINT32_MAX) return false;
        write_block_tag(w, "PlantBlock", m, jm.bedrock);
    }
    return true;
}

} // namespace

bool is_context_dependent(std::string_view id) noexcept {
    return is_chest(id) || bed_color(id).has_value() || banner_color(id).has_value() || skull_type(id).has_value() ||
           id == "minecraft:flower_pot" || id.starts_with("minecraft:potted_");
}

std::optional<BlockEntity> translate_block_entity(const BlockEntity& java, JavaStateId java_state, const MappingData& m,
                                                  ConversionReport& report) {
    auto state = BlockState::parse(m.java_state_string(java_state));
    if (!state) return std::nullopt;

    // Source NBT (may be empty).
    std::optional<io::nbt::NbtDocument> doc;
    if (!java.data.empty()) {
        auto parsed = io::nbt::NbtDocument::parse(java.data.bytes, java.data.endian);
        if (!parsed) {
            report.add(IssueKind::UnmappedBlockEntity, java.id + " (unreadable data)", java.pos);
            return std::nullopt;
        }
        doc = std::move(*parsed);
    }
    const NbtCompoundView c = doc ? doc->root().entries() : NbtView().entries();

    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    BlockEntity out;
    out.pos = java.pos;

    if (const auto container = simple_container(java.id)) {
        out.id = std::string(*container);
        w.write_string("id", out.id);
        write_items(w, c.find("Items"), m, report, java.pos);
        write_custom_name(w, c, report, java.pos);
        if (out.id == "Chest") write_pairing(w, *state, java.pos);
        if (c.find("LootTable")) report.add(IssueKind::LossyBlock, "loot table dropped", java.pos);
        if (c.find("CookTime") || c.find("cooking_time_spent") || c.find("TransferCooldown"))
            report.add(IssueKind::LossyBlock, "container progress state dropped", java.pos);
    } else if (java.id == "minecraft:ender_chest" || java.id == "minecraft:enchanting_table") {
        out.id = java.id == "minecraft:ender_chest" ? "EnderChest" : "EnchantTable";
        w.write_string("id", out.id);
        write_custom_name(w, c, report, java.pos);
    } else if (const auto marker = simple_marker(java.id)) {
        // Block entities whose block works without their (transient) data in Bedrock.
        out.id = std::string(*marker);
        w.write_string("id", out.id);
        if (java.id == "minecraft:comparator")
            if (const auto signal = c.find("OutputSignal").as_int()) w.write_int("OutputSignal", *signal);
        for (const io::nbt::NbtEntry& e : c)
            if (e.name != "OutputSignal" && e.name != "components") {
                report.add(IssueKind::LossyBlock, java.id + " data dropped", java.pos);
                break;
            }
    } else if (java.id == "minecraft:bed") {
        out.id = "Bed";
        w.write_string("id", out.id);
        w.write_byte("color", bed_color(state->id()).value_or(14)); // red if unknown
    } else if (java.id == "minecraft:banner") {
        out.id = "Banner";
        w.write_string("id", out.id);
        write_banner(w, *state, c.find("patterns"), report, java.pos);
        write_custom_name(w, c, report, java.pos);
    } else if (java.id == "minecraft:skull") {
        out.id = "Skull";
        w.write_string("id", out.id);
        write_skull(w, *state);
        if (c.find("profile")) report.add(IssueKind::LossyBlock, "player head skin (profile) dropped", java.pos);
    } else if (java.id == "minecraft:sign" || java.id == "minecraft:hanging_sign") {
        out.id = java.id == "minecraft:sign" ? "Sign" : "HangingSign";
        w.write_string("id", out.id);
        std::optional<std::array<NbtView, 4>> legacy;
        if (c.find("Text1")) legacy = std::array<NbtView, 4>{c.find("Text1"), c.find("Text2"), c.find("Text3"), c.find("Text4")};
        write_sign_face(w, "FrontText", c.find("front_text"), legacy, report, java.pos);
        write_sign_face(w, "BackText", c.find("back_text"), std::nullopt, report, java.pos);
        w.write_byte("IsWaxed", c.find("is_waxed").as_byte().value_or(0));
    } else {
        report.add(IssueKind::UnmappedBlockEntity, java.id, java.pos);
        return std::nullopt;
    }
    w.end_compound();
    auto data = finish(std::move(w));
    if (!data) return std::nullopt;
    out.data = std::move(*data);
    return out;
}

std::optional<BlockEntity> synthesize_block_entity(BlockPos pos, JavaStateId java_state, const MappingData& m) {
    auto state = BlockState::parse(m.java_state_string(java_state));
    if (!state) return std::nullopt;
    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    BlockEntity out;
    out.pos = pos;
    if (const auto color = bed_color(state->id())) {
        out.id = "Bed";
        w.write_string("id", out.id);
        w.write_byte("color", *color);
    } else if (is_chest(state->id())) {
        out.id = "Chest";
        w.write_string("id", out.id);
        w.begin_list("Items", TagType::End, 0);
        w.end_list();
        write_pairing(w, *state, pos);
    } else if (banner_color(state->id())) {
        out.id = "Banner";
        w.write_string("id", out.id);
        ConversionReport ignored;
        write_banner(w, *state, NbtView(), ignored, pos);
    } else if (skull_type(state->id())) {
        out.id = "Skull";
        w.write_string("id", out.id);
        write_skull(w, *state);
    } else if (state->id() == "minecraft:flower_pot" || state->id().starts_with("minecraft:potted_")) {
        out.id = "FlowerPot";
        w.write_string("id", out.id);
        if (!write_flower_pot(w, *state, m)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    w.end_compound();
    auto data = finish(std::move(w));
    if (!data) return std::nullopt;
    out.data = std::move(*data);
    return out;
}

} // namespace bridger::translate
