// Java -> Bedrock block entities. Formats and sources: docs/formats/bedrock-world.md.

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>

#include <array>
#include <string>

namespace bridger::translate {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

// Dye colors in Bedrock's numeric order (bed "color" byte).
constexpr std::array<std::string_view, 16> kDyeColors{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

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

// Containers whose slot layout is the same in both editions: Java id -> Bedrock id
// (Bedrock ids as used by Chunker's block entity handlers, docs/formats/bedrock-world.md).
// The brewing stand is not listed: its slot order differs between editions.
std::optional<std::string_view> simple_container(std::string_view java_id) {
    struct Entry {
        std::string_view java, bedrock;
    };
    static constexpr std::array<Entry, 9> kContainers{{{"minecraft:barrel", "Barrel"},
                                                        {"minecraft:furnace", "Furnace"},
                                                        {"minecraft:blast_furnace", "BlastFurnace"},
                                                        {"minecraft:smoker", "Smoker"},
                                                        {"minecraft:hopper", "Hopper"},
                                                        {"minecraft:dispenser", "Dispenser"},
                                                        {"minecraft:dropper", "Dropper"},
                                                        {"minecraft:shulker_box", "ShulkerBox"},
                                                        {"minecraft:chest", "Chest"}}};
    for (const Entry& e : kContainers)
        if (e.java == java_id) return e.bedrock;
    if (java_id == "minecraft:trapped_chest") return "Chest"; // Bedrock stores trapped chests as "Chest"
    return std::nullopt;
}

// Bedrock pairing of a Java double chest (GeyserMC DoubleChestBlockEntityTranslator):
// the partner is beside the chest, its side given by facing and left/right.
std::optional<BlockPos> chest_partner(const BlockState& s, BlockPos pos) {
    const auto type = s.property("type");
    const auto facing = s.property("facing");
    if (!type || !facing || *type == "single") return std::nullopt;
    const bool left = *type == "left";
    BlockPos p = pos;
    if (*facing == "east") p.z += left ? 1 : -1;
    else if (*facing == "west") p.z += left ? -1 : 1;
    else if (*facing == "south") p.x += left ? -1 : 1;
    else if (*facing == "north") p.x += left ? 1 : -1;
    else return std::nullopt;
    return p;
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

} // namespace

bool is_context_dependent(std::string_view id) noexcept { return is_chest(id) || bed_color(id).has_value(); }

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
    } else if (java.id == "minecraft:bed") {
        out.id = "Bed";
        w.write_string("id", out.id);
        w.write_byte("color", bed_color(state->id()).value_or(14)); // red if unknown
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
