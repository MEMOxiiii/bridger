// Java block entities written for an older target version (docs/versions.md, "Target versions"). The reverse
// translator and the Java readers produce the pinned (26.3) forms; older releases store some of them differently:
//   item stacks   1.20.5 (24w09a) moved {id, Count: Byte, tag} to {id, count: Int, components}
//   text          1.21.5 (25w02a) moved JSON strings to NBT text components (a plain String tag = literal text)
//   signs         1.20 (23w12a) moved Text1..Text4 / Color / GlowingText to front_text / back_text
//   banners       1.20.5 moved Patterns [{Pattern: "bo", Color: Int}] to patterns [{pattern, color}]
// Sources: minecraft.wiki "Item format", "Text component format", "Sign", "Banner/Patterns" (history sections,
// checked 2026-10-16); DataVersions from minecraft.wiki "Data version".

#include "java_block_entity_downgrade.hpp"

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace bridger::translate::detail {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

constexpr i32 kItemComponents = 3837; // 1.20.5
constexpr i32 kTextAsNbt = 4325;      // 1.21.5
constexpr i32 kSignFaces = 3463;      // 1.20

constexpr std::array<std::string_view, 16> kDyes{"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                 "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

// Banner pattern ids (1.20.5+) and their codes before it (minecraft.wiki "Banner/Patterns").
constexpr std::array<std::pair<std::string_view, std::string_view>, 41> kPatternCodes{{
    {"minecraft:base", "b"}, {"minecraft:square_bottom_left", "bl"}, {"minecraft:square_bottom_right", "br"},
    {"minecraft:square_top_left", "tl"}, {"minecraft:square_top_right", "tr"}, {"minecraft:stripe_bottom", "bs"},
    {"minecraft:stripe_top", "ts"}, {"minecraft:stripe_left", "ls"}, {"minecraft:stripe_right", "rs"},
    {"minecraft:stripe_center", "cs"}, {"minecraft:stripe_middle", "ms"}, {"minecraft:stripe_downright", "drs"},
    {"minecraft:stripe_downleft", "dls"}, {"minecraft:small_stripes", "ss"}, {"minecraft:cross", "cr"},
    {"minecraft:straight_cross", "sc"}, {"minecraft:triangle_bottom", "bt"}, {"minecraft:triangle_top", "tt"},
    {"minecraft:triangles_bottom", "bts"}, {"minecraft:triangles_top", "tts"}, {"minecraft:diagonal_left", "ld"},
    {"minecraft:diagonal_up_right", "rd"}, {"minecraft:diagonal_up_left", "lud"}, {"minecraft:diagonal_right", "rud"},
    {"minecraft:circle", "mc"}, {"minecraft:rhombus", "mr"}, {"minecraft:half_vertical", "vh"},
    {"minecraft:half_horizontal", "hh"}, {"minecraft:half_vertical_right", "vhr"}, {"minecraft:half_horizontal_bottom", "hhb"},
    {"minecraft:border", "bo"}, {"minecraft:curly_border", "cbo"}, {"minecraft:gradient", "gra"},
    {"minecraft:gradient_up", "gru"}, {"minecraft:bricks", "bri"}, {"minecraft:globe", "glb"}, {"minecraft:creeper", "cre"},
    {"minecraft:skull", "sku"}, {"minecraft:flower", "flo"}, {"minecraft:mojang", "moj"}, {"minecraft:piglin", "pig"},
}};

// Item ids renamed since the target (the old id exists there, the new one does not).
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kItemRenames{{
    {"minecraft:short_grass", "minecraft:grass"},   // 1.20.3 (23w40a)
    {"minecraft:turtle_scute", "minecraft:scute"},  // 1.20.5 (24w03a)
    {"minecraft:iron_chain", "minecraft:chain"},    // 1.21.9
}};

std::string json_quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<int>(c));
        else out += c;
    }
    return out + "\"";
}

// A text value in pinned form (String = literal text, or a component compound) as a JSON string for releases
// before 1.21.5; compounds other than {text} are reduced to their text (reported by the caller).
std::string text_json(NbtView v, bool* lossy) {
    if (const auto s = v.as_string()) return json_quote(io::nbt::mutf8_to_utf8(*s).value_or(""));
    if (v.is(TagType::Compound)) {
        const auto t = v.entries().find("text").as_string();
        if (lossy != nullptr && v.entries().size() > 1) *lossy = true;
        return "{\"text\":" + json_quote(io::nbt::mutf8_to_utf8(t.value_or("")).value_or("")) + "}";
    }
    return "\"\"";
}

void write_text_string(NbtWriter& w, std::string_view name, const std::string& json) {
    w.write_string(name, io::nbt::utf8_to_mutf8(json).value_or("\"\""));
}

struct Ctx {
    Endian endian;
    const JavaTargets& targets;
    const JavaTarget& target;
    std::vector<std::string>& lossy; // short notes for the report
    std::vector<std::string>& dropped;
};

// One item stack compound, written for the target (nullopt: the target has no such item).
bool write_item(NbtWriter& w, std::string_view name, NbtCompoundView item, Ctx& c) {
    std::string id = io::nbt::mutf8_to_utf8(item.find("id").as_string().value_or("")).value_or("");
    if (!c.targets.has_item(id, c.target)) {
        bool renamed = false;
        for (const auto& [now, before] : kItemRenames)
            if (id == now && c.targets.has_item(before, c.target)) {
                id = std::string(before);
                renamed = true;
            }
        if (!renamed) {
            c.dropped.push_back("item " + id);
            return false;
        }
    }
    w.begin_compound(name);
    const bool old_form = c.target.data_version < kItemComponents;
    for (const io::nbt::NbtEntry& e : item) {
        if (e.name == "id") {
            w.write_string("id", io::nbt::utf8_to_mutf8(id).value_or(""));
        } else if (old_form && e.name == "count") {
            w.write_byte("Count", static_cast<i8>(std::clamp(e.value.as_int().value_or(1), 1, 127)));
        } else if (old_form && e.name == "components") {
            c.lossy.push_back("item data (components) before 1.20.5");
        } else {
            w.write_view(e.name, e.value);
        }
    }
    w.end_compound();
    return true;
}

void write_items(NbtWriter& w, std::string_view name, NbtView list, Ctx& c) {
    // Two passes: the list length must be known up front, so encode each item first.
    std::vector<std::vector<u8>> encoded;
    for (const NbtView it : list.items()) {
        if (!it.is(TagType::Compound)) continue;
        NbtWriter one(c.endian);
        one.begin_root_compound();
        if (write_item(one, "item", it.entries(), c)) {
            one.end_compound();
            if (auto bytes = std::move(one).finish()) encoded.push_back(std::move(*bytes));
        }
    }
    w.begin_list(name, encoded.empty() ? TagType::End : TagType::Compound, encoded.size());
    for (const auto& bytes : encoded) {
        const auto doc = io::nbt::NbtDocument::parse(bytes, c.endian);
        if (doc) w.write_view({}, doc->root().entries().find("item"));
    }
    w.end_list();
}

// Sign face {messages, color, has_glowing_text} for 1.20 - 1.21.4 (JSON message strings).
void write_face_json(NbtWriter& w, std::string_view name, NbtView face, Ctx& c) {
    w.begin_compound(name);
    for (const io::nbt::NbtEntry& e : face.entries()) {
        if (e.name == "messages" || e.name == "filtered_messages") {
            w.begin_list(e.name, TagType::String, e.value.items().size());
            bool lossy = false;
            for (const NbtView m : e.value.items()) write_text_string(w, {}, text_json(m, &lossy));
            w.end_list();
            if (lossy) c.lossy.push_back("text formatting before 1.21.5");
        } else {
            w.write_view(e.name, e.value);
        }
    }
    w.end_compound();
}

} // namespace

std::optional<std::vector<u8>> downgrade_java_block_entity(const BlockEntity& be, const JavaTargets& targets, const JavaTarget& target,
                                                          std::vector<std::string>& lossy, std::vector<std::string>& dropped) {
    if (target.pinned) return std::nullopt;
    const auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    if (!doc || !doc->root().is(TagType::Compound)) return std::nullopt;
    const NbtCompoundView root = doc->root().entries();
    Ctx c{be.data.endian, targets, target, lossy, dropped};
    const bool text_json_form = target.data_version < kTextAsNbt;
    NbtWriter w(be.data.endian);
    w.begin_root_compound();
    for (const io::nbt::NbtEntry& e : root) {
        const std::string_view k = e.name;
        if (k == "Items" && e.value.is(TagType::List)) {
            write_items(w, "Items", e.value, c);
        } else if ((k == "item" || k == "RecordItem" || k == "Book") && e.value.is(TagType::Compound)) {
            if (!write_item(w, k, e.value.entries(), c)) continue;
        } else if (k == "CustomName" && text_json_form) {
            bool l = false;
            write_text_string(w, "CustomName", text_json(e.value, &l));
            if (l) lossy.push_back("text formatting before 1.21.5");
        } else if ((k == "front_text" || k == "back_text") && target.data_version < kSignFaces) {
            if (k == "back_text") {
                bool any = false;
                for (const NbtView m : e.value.entries().find("messages").items()) {
                    const auto s = m.as_string();
                    if ((s && !s->empty() && *s != "\"\"") || m.is(TagType::Compound)) any = true;
                }
                if (any) lossy.push_back("sign back text before 1.20");
                continue;
            }
            const NbtCompoundView f = e.value.entries();
            usize line = 1;
            for (const NbtView m : f.find("messages").items()) {
                if (line > 4) break;
                bool l = false;
                write_text_string(w, std::format("Text{}", line++), text_json(m, &l));
                if (l) lossy.push_back("text formatting before 1.21.5");
            }
            w.write_string("Color", f.find("color").as_string().value_or("black"));
            w.write_byte("GlowingText", f.find("has_glowing_text").as_byte().value_or(0));
        } else if ((k == "front_text" || k == "back_text") && text_json_form) {
            write_face_json(w, k, e.value, c);
        } else if (k == "is_waxed" && target.data_version < kSignFaces) {
            continue; // waxed signs are 1.20+
        } else if (k == "patterns" && e.value.is(TagType::List) && target.data_version < kItemComponents) {
            std::vector<std::pair<std::string, i32>> layers;
            for (const NbtView p : e.value.items()) {
                const NbtCompoundView pc = p.entries();
                const std::string id(pc.find("pattern").as_string().value_or(""));
                const std::string color(pc.find("color").as_string().value_or("white"));
                std::string code;
                for (const auto& [pid, cd] : kPatternCodes)
                    if (pid == id) code = std::string(cd);
                i32 dye = 0;
                for (usize d = 0; d < kDyes.size(); ++d)
                    if (kDyes[d] == color) dye = static_cast<i32>(d);
                if (code.empty()) lossy.push_back("banner pattern " + id + " before 1.20.5");
                else layers.emplace_back(code, dye);
            }
            w.begin_list("Patterns", layers.empty() ? TagType::End : TagType::Compound, layers.size());
            for (const auto& [code, dye] : layers) {
                w.begin_compound();
                w.write_string("Pattern", code);
                w.write_int("Color", dye);
                w.end_compound();
            }
            w.end_list();
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
