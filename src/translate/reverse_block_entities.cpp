// Bedrock -> Java block entities and item stacks: the inverse of block_entity_translator.cpp,
// using the same tables (tables.hpp). Formats: docs/formats/bedrock-world.md ("Block
// entities") for the Bedrock side; Java 1.20.5+ item stacks {Slot, id, count} and 1.20+
// sign faces {messages, color, has_glowing_text} (minecraft.wiki, "Block entity format").

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>

#include "tables.hpp"

#include <string>
#include <vector>

namespace bridger::translate {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

std::string utf8_of(std::string_view mutf8) {
    auto u = io::nbt::mutf8_to_utf8(mutf8);
    return u ? *u : std::string(mutf8);
}

// Bedrock "Items" -> Java "Items" (1.20.5+ form). Unknown items and dropped data are reported.
void write_java_items(NbtWriter& w, NbtView items, const MappingData& m, ConversionReport& report, BlockPos pos) {
    struct Item {
        std::string id;
        i32 count;
        i8 slot;
    };
    std::vector<Item> out;
    for (const NbtView it : items.items()) {
        const NbtCompoundView c = it.entries();
        const auto name = c.find("Name").as_string();
        if (!name || name->empty()) continue; // empty slot
        const std::string bedrock = utf8_of(*name);
        const i16 damage = c.find("Damage").as_short().value_or(0);
        const i32 count = c.find("Count").as_byte().value_or(1);
        const auto java = m.java_item_of_bedrock(bedrock, damage);
        if (!java) {
            report.add(IssueKind::UnmappedBlockEntity, "item " + bedrock, pos, static_cast<u64>(count < 1 ? 1 : count));
            continue;
        }
        if (c.find("tag")) report.add(IssueKind::LossyBlock, "item data dropped: " + bedrock, pos);
        out.push_back({std::string(*java), count < 1 ? 1 : count, c.find("Slot").as_byte().value_or(0)});
    }
    w.begin_list("Items", out.empty() ? TagType::End : TagType::Compound, out.size());
    for (const Item& i : out) {
        w.begin_compound();
        w.write_byte("Slot", i.slot);
        w.write_string("id", i.id);
        w.write_int("count", i.count);
        w.end_compound();
    }
    w.end_list();
}

void write_java_custom_name(NbtWriter& w, NbtCompoundView c) {
    // A plain string is a valid text component in Java 1.21.5+ NBT.
    if (const auto n = c.find("CustomName").as_string(); n && !n->empty()) w.write_string("CustomName", *n);
}

// Bedrock sign face {Text, SignTextColor, IgnoreLighting} -> Java {messages[4], color, has_glowing_text}.
void write_java_sign_face(NbtWriter& w, std::string_view name, NbtView face, ConversionReport& report, BlockPos pos) {
    const NbtCompoundView f = face.entries();
    std::string text = face ? utf8_of(f.find("Text").as_string().value_or("")) : std::string();
    std::vector<std::string> lines;
    for (usize start = 0;;) {
        const usize nl = text.find('\n', start);
        lines.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (lines.size() > 4) report.add(IssueKind::LossyBlock, "sign text beyond 4 lines dropped", pos);
    lines.resize(4);
    if (const auto color = f.find("SignTextColor").as_int(); color && static_cast<u32>(*color) != 0xFF000000u)
        report.add(IssueKind::LossyBlock, "sign text color dropped", pos);
    w.begin_compound(name);
    w.begin_list("messages", TagType::String, 4);
    for (const std::string& l : lines)
        if (auto m = io::nbt::utf8_to_mutf8(l)) w.write_string({}, *m);
        else w.write_string({}, "");
    w.end_list();
    w.write_string("color", "black");
    w.write_byte("has_glowing_text", f.find("IgnoreLighting").as_byte().value_or(0) != 0 ? 1 : 0);
    w.end_compound();
}

void write_java_patterns(NbtWriter& w, NbtView patterns, ConversionReport& report, BlockPos pos) {
    struct Layer {
        std::string pattern;
        std::string_view color;
    };
    std::vector<Layer> layers;
    for (const NbtView p : patterns.items()) {
        const NbtCompoundView c = p.entries();
        const auto code = c.find("Pattern").as_string();
        const auto color = c.find("Color").as_int();
        std::optional<std::string_view> java;
        if (code)
            for (const tables::Pair& t : tables::kBannerPatterns)
                if (t.bedrock == *code) java = t.java;
        if (!java || !color || *color < 0 || *color > 15) {
            report.add(IssueKind::LossyBlock, "banner pattern not converted", pos);
            continue;
        }
        layers.push_back({"minecraft:" + std::string(*java), tables::kDyeColors[static_cast<usize>(15 - *color)]});
    }
    w.begin_list("patterns", layers.empty() ? TagType::End : TagType::Compound, layers.size());
    for (const Layer& l : layers) {
        w.begin_compound();
        w.write_string("pattern", l.pattern);
        w.write_string("color", l.color);
        w.end_compound();
    }
    w.end_list();
}

} // namespace

std::optional<BlockEntity> translate_block_entity_to_java(const BlockEntity& bedrock, JavaStateId java_state,
                                                          const MappingData& m, ConversionReport& report) {
    const std::string_view block = m.java_block_name(java_state);
    std::optional<io::nbt::NbtDocument> doc;
    if (!bedrock.data.empty()) {
        auto parsed = io::nbt::NbtDocument::parse(bedrock.data.bytes, bedrock.data.endian);
        if (!parsed) {
            report.add(IssueKind::UnmappedBlockEntity, bedrock.id + " (unreadable data)", bedrock.pos);
            return std::nullopt;
        }
        doc = std::move(*parsed);
    }
    const NbtCompoundView c = doc ? doc->root().entries() : NbtView().entries();
    const std::string_view id = bedrock.id;

    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    BlockEntity out;
    out.pos = bedrock.pos;

    std::optional<std::string_view> container;
    for (const tables::Pair& e : tables::kContainers)
        if (e.bedrock == id) container = e.java;
    std::optional<std::string_view> marker;
    for (const tables::Pair& e : tables::kMarkers)
        if (e.bedrock == id) marker = e.java;

    if (container) {
        // Chest covers trapped chests; the Java block decides. Shulker boxes of every color use one id.
        out.id = id == "Chest" && block == "minecraft:trapped_chest" ? "minecraft:trapped_chest" : std::string(*container);
        write_java_items(w, c.find("Items"), m, report, bedrock.pos);
        write_java_custom_name(w, c);
        if (c.find("LootTable")) report.add(IssueKind::LossyBlock, "loot table dropped", bedrock.pos);
        if (c.find("CookTime") || c.find("TransferCooldown"))
            report.add(IssueKind::LossyBlock, "container progress state dropped", bedrock.pos);
    } else if (id == "EnderChest") {
        out.id = "minecraft:ender_chest";
        write_java_custom_name(w, c);
    } else if (marker) {
        out.id = std::string(*marker);
        if (id == "EnchantTable") write_java_custom_name(w, c);
        if (id == "Comparator")
            if (const auto signal = c.find("OutputSignal").as_int()) w.write_int("OutputSignal", *signal);
    } else if (id == "Bed") {
        out.id = "minecraft:bed"; // the color is the block itself in Java (context stage)
    } else if (id == "Banner") {
        out.id = "minecraft:banner";
        write_java_patterns(w, c.find("Patterns"), report, bedrock.pos);
        write_java_custom_name(w, c);
    } else if (id == "Skull") {
        out.id = "minecraft:skull"; // type and rotation are the block itself in Java (context stage)
    } else if (id == "Sign" || id == "HangingSign") {
        out.id = id == "Sign" ? "minecraft:sign" : "minecraft:hanging_sign";
        write_java_sign_face(w, "front_text", c.find("FrontText"), report, bedrock.pos);
        write_java_sign_face(w, "back_text", c.find("BackText"), report, bedrock.pos);
        w.write_byte("is_waxed", c.find("IsWaxed").as_byte().value_or(0));
    } else if (id == "FlowerPot") {
        return std::nullopt; // Java has no flower pot block entity: the plant is the block (context stage)
    } else {
        report.add(IssueKind::UnmappedBlockEntity, bedrock.id, bedrock.pos);
        return std::nullopt;
    }
    w.end_compound();
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    out.data = RawNbt{std::move(*bytes), Endian::Big};
    return out;
}

} // namespace bridger::translate
