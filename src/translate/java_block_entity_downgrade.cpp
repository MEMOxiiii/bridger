// Java block entities written for an older target version (docs/versions.md, "Target versions"). The reverse
// translator and the Java readers produce the pinned (26.3) forms; older releases store some of them differently:
//   item stacks   1.20.5 (24w09a) moved {id, Count: Byte, tag} to {id, count: Int, components}
//   text          1.21.5 (25w02a) moved JSON strings to NBT text components (a plain String tag = literal text)
//   signs         1.20 (23w12a) moved Text1..Text4 / Color / GlowingText to front_text / back_text
//   banners       1.20.5 moved Patterns [{Pattern: "bo", Color: Int}] to patterns [{pattern, color}]
// Sources: minecraft.wiki "Item format", "Text component format", "Sign", "Banner/Patterns" (history sections,
// checked 2026-10-16); DataVersions from minecraft.wiki "Data version".

#include "java_block_entity_downgrade.hpp"

#include "tables.hpp"

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
// Block entity fields by version (Chunker's Java handlers): beacon effect names from 1.20.2, the decorated pot's item
// from 1.20.3, furnace lit_* names from 1.21.4, Int brewing/furnace fields, total_fuel and the sherd map from 26.3.
constexpr i32 kBeaconNames = 3578;  // 1.20.2
constexpr i32 kPotItem = 3698;      // 1.20.3
constexpr i32 kFurnaceLit = 4189;   // 1.21.4
constexpr i32 kJava26_3 = 5023;     // 26.3

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

bool write_item(NbtWriter& w, std::string_view name, NbtCompoundView item, Ctx& c, int depth = 0);

constexpr int kMaxItemDepth = 3; // a shulker box in a chest holds a bundle at most

// Components that exist in 1.20.5 - 1.21.4 (minecraft.wiki "Data component format", history); others would make the
// item unreadable there and are reported instead.
constexpr std::array<std::string_view, 26> kComponents1_20_5{
    "minecraft:damage", "minecraft:repair_cost", "minecraft:enchantments", "minecraft:stored_enchantments",
    "minecraft:custom_name", "minecraft:item_name", "minecraft:lore", "minecraft:dyed_color", "minecraft:potion_contents",
    "minecraft:written_book_content", "minecraft:writable_book_content", "minecraft:container", "minecraft:bundle_contents",
    "minecraft:fireworks", "minecraft:firework_explosion", "minecraft:trim", "minecraft:instrument", "minecraft:banner_patterns",
    "minecraft:base_color", "minecraft:suspicious_stew_effects", "minecraft:map_id", "minecraft:custom_data",
    "minecraft:unbreakable", "minecraft:rarity", "minecraft:hide_additional_tooltip", "minecraft:ominous_bottle_amplifier"};

// A text value as JSON for releases before 1.21.5.
void write_json_text(NbtWriter& w, std::string_view name, NbtView v, Ctx& c) {
    bool l = false;
    write_text_string(w, name, text_json(v, &l));
    if (l) c.lossy.push_back("text formatting before 1.21.5");
}

// Writes a nested item list. `component`: {slot, item} entries (1.20.5+ container); else items with "Slot".
void write_nested_items(NbtWriter& w, std::string_view name, NbtView list, bool component, bool old_form, Ctx& c, int depth) {
    std::vector<std::vector<u8>> encoded;
    for (const NbtView it : list.items()) {
        const NbtCompoundView e = it.entries();
        NbtWriter one(c.endian);
        one.begin_root_compound();
        const NbtCompoundView item = component ? e.find("item").entries() : e;
        const i32 slot = component ? e.find("slot").as_int().value_or(0) : e.find("Slot").as_byte().value_or(0);
        bool ok;
        if (component && !old_form) {
            one.write_int("slot", slot);
            ok = write_item(one, "item", item, c, depth + 1);
        } else {
            ok = write_item(one, "item", item, c, depth + 1);
        }
        one.end_compound();
        if (!ok) continue;
        auto bytes = std::move(one).finish();
        if (!bytes) continue;
        if (old_form) { // {Slot: Byte, id, Count, tag}: the item's fields with the slot
            NbtWriter flat(c.endian);
            flat.begin_root_compound();
            flat.begin_compound("item");
            flat.write_byte("Slot", static_cast<i8>(slot));
            const auto doc = io::nbt::NbtDocument::parse(*bytes, c.endian);
            if (doc)
                for (const io::nbt::NbtEntry& en : doc->root().entries().find("item").entries()) flat.write_view(en.name, en.value);
            flat.end_compound();
            flat.end_compound();
            bytes = std::move(flat).finish();
            if (!bytes) continue;
        }
        encoded.push_back(std::move(*bytes));
    }
    w.begin_list(name, encoded.empty() ? TagType::End : TagType::Compound, encoded.size());
    for (const auto& bytes : encoded) {
        const auto doc = io::nbt::NbtDocument::parse(bytes, c.endian);
        if (!doc) continue;
        if (component && !old_form) {
            w.begin_compound();
            for (const io::nbt::NbtEntry& en : doc->root().entries()) w.write_view(en.name, en.value);
            w.end_compound();
        } else {
            w.write_view({}, doc->root().entries().find("item"));
        }
    }
    w.end_list();
}

// 1.20.5 - 1.21.4: the same components in their older shapes (enchantments wrapped in "levels", dyed_color as
// {rgb}, text as JSON strings); components that did not exist yet are reported.
void write_components_1_20_5(NbtWriter& w, NbtCompoundView comps, Ctx& c, int depth) {
    w.begin_compound("components");
    for (const io::nbt::NbtEntry& e : comps) {
        const std::string_view n = e.name;
        if (std::find(kComponents1_20_5.begin(), kComponents1_20_5.end(), n) == kComponents1_20_5.end()) {
            c.lossy.push_back("item component " + std::string(n) + " (not in " + c.target.name + ")");
            continue;
        }
        if ((n == "minecraft:enchantments" || n == "minecraft:stored_enchantments") && !e.value.entries().find("levels")) {
            w.begin_compound(n);
            w.write_view("levels", e.value);
            w.end_compound();
        } else if (n == "minecraft:dyed_color" && e.value.as_int()) {
            w.begin_compound(n);
            w.write_int("rgb", *e.value.as_int());
            w.end_compound();
        } else if (n == "minecraft:custom_name" || n == "minecraft:item_name") {
            write_json_text(w, n, e.value, c);
        } else if (n == "minecraft:lore") {
            w.begin_list(n, TagType::String, e.value.items().size());
            for (const NbtView l : e.value.items()) write_json_text(w, {}, l, c);
            w.end_list();
        } else if (n == "minecraft:written_book_content") {
            w.begin_compound(n);
            for (const io::nbt::NbtEntry& b : e.value.entries()) {
                if (b.name != "pages") {
                    w.write_view(b.name, b.value);
                    continue;
                }
                w.begin_list("pages", b.value.items().empty() ? TagType::End : TagType::Compound, b.value.items().size());
                for (const NbtView pg : b.value.items()) {
                    w.begin_compound();
                    write_json_text(w, "raw", pg.is(TagType::Compound) ? pg.entries().find("raw") : pg, c);
                    w.end_compound();
                }
                w.end_list();
            }
            w.end_compound();
        } else if (n == "minecraft:container" && depth < kMaxItemDepth) {
            write_nested_items(w, n, e.value, true, false, c, depth);
        } else if (n == "minecraft:bundle_contents" && depth < kMaxItemDepth) {
            write_nested_items(w, n, e.value, false, false, c, depth);
        } else {
            w.write_view(n, e.value);
        }
    }
    w.end_compound();
}

// Before 1.20.5: components -> the item "tag" (minecraft:wiki "Item format/Before 1.20.5"; Chunker JavaItemStackResolver).
// Written into separate bytes so that an item without data gets no empty "tag".
std::optional<std::vector<u8>> legacy_tag(NbtCompoundView comps, Ctx& c, int depth) {
    NbtWriter w(c.endian);
    w.begin_root_compound();
    usize written = 0;
    NbtView name, lore;
    std::optional<i32> color;
    for (const io::nbt::NbtEntry& e : comps) {
        const std::string_view n = e.name;
        if (n == "minecraft:damage") {
            w.write_int("Damage", e.value.as_int().value_or(0));
        } else if (n == "minecraft:repair_cost") {
            w.write_int("RepairCost", e.value.as_int().value_or(0));
        } else if (n == "minecraft:enchantments" || n == "minecraft:stored_enchantments") {
            NbtView levels = e.value.entries().find("levels");
            if (!levels) levels = e.value;
            usize count = 0;
            for (const io::nbt::NbtEntry& l : levels.entries()) count += l.value.as_int() ? 1 : 0;
            w.begin_list(n == "minecraft:enchantments" ? "Enchantments" : "StoredEnchantments", count ? TagType::Compound : TagType::End, count);
            for (const io::nbt::NbtEntry& l : levels.entries()) {
                if (!l.value.as_int()) continue;
                w.begin_compound();
                w.write_string("id", l.name);
                w.write_short("lvl", static_cast<i16>(std::clamp(*l.value.as_int(), 0, 32767)));
                w.end_compound();
            }
            w.end_list();
        } else if (n == "minecraft:custom_name") {
            name = e.value;
            continue;
        } else if (n == "minecraft:lore") {
            lore = e.value;
            continue;
        } else if (n == "minecraft:dyed_color") {
            color = e.value.as_int() ? e.value.as_int() : e.value.entries().find("rgb").as_int();
            continue;
        } else if (n == "minecraft:potion_contents") {
            const auto potion = e.value.as_string() ? e.value.as_string() : e.value.entries().find("potion").as_string();
            if (!potion) continue;
            w.write_string("Potion", *potion);
        } else if (n == "minecraft:written_book_content" || n == "minecraft:writable_book_content") {
            const bool written_book = n == "minecraft:written_book_content";
            const NbtCompoundView b = e.value.entries();
            const NbtView pages = b.find("pages");
            w.begin_list("pages", TagType::String, pages.items().size());
            for (const NbtView pg : pages.items()) {
                const NbtView raw = pg.is(TagType::Compound) ? pg.entries().find("raw") : pg;
                if (written_book) write_json_text(w, {}, raw, c);
                else w.write_string({}, raw.as_string().value_or(""));
            }
            w.end_list();
            if (written_book) {
                const NbtView title = b.find("title");
                w.write_string("title", (title.is(TagType::Compound) ? title.entries().find("raw") : title).as_string().value_or(""));
                w.write_string("author", b.find("author").as_string().value_or(""));
            }
        } else if (n == "minecraft:container" && depth < kMaxItemDepth) {
            w.begin_compound("BlockEntityTag");
            write_nested_items(w, "Items", e.value, true, true, c, depth);
            w.end_compound();
        } else if (n == "minecraft:fireworks" || n == "minecraft:firework_explosion") {
            static constexpr std::array<std::string_view, 5> kShapes{"small_ball", "large_ball", "star", "creeper", "burst"};
            const auto explosion = [&](std::string_view key, NbtCompoundView ex) {
                w.begin_compound(key);
                i8 type = 0;
                for (usize i = 0; i < kShapes.size(); ++i)
                    if (ex.find("shape").as_string() == kShapes[i]) type = static_cast<i8>(i);
                w.write_byte("Type", type);
                for (const auto& [from, to] : {std::pair{"colors", "Colors"}, std::pair{"fade_colors", "FadeColors"}}) {
                    std::vector<i32> v;
                    if (const auto a = ex.find(from).as_int_array())
                        for (usize i = 0; i < a->size(); ++i) v.push_back((*a)[i]);
                    w.write_int_array(to, v);
                }
                w.write_byte("Trail", ex.find("has_trail").as_byte().value_or(0));
                w.write_byte("Flicker", ex.find("has_twinkle").as_byte().value_or(0));
                w.end_compound();
            };
            if (n == "minecraft:firework_explosion") {
                explosion("Explosion", e.value.entries());
            } else {
                const NbtCompoundView f = e.value.entries();
                w.begin_compound("Fireworks");
                w.write_byte("Flight", f.find("flight_duration").as_byte().value_or(1));
                const NbtView list = f.find("explosions");
                w.begin_list("Explosions", list.items().empty() ? TagType::End : TagType::Compound, list.items().size());
                for (const NbtView ex : list.items()) explosion({}, ex.entries());
                w.end_list();
                w.end_compound();
            }
        } else if (n == "minecraft:trim") {
            w.begin_compound("Trim");
            w.write_string("material", e.value.entries().find("material").as_string().value_or(""));
            w.write_string("pattern", e.value.entries().find("pattern").as_string().value_or(""));
            w.end_compound();
        } else if (n == "minecraft:instrument" && e.value.as_string()) {
            w.write_string("instrument", *e.value.as_string());
        } else if (n == "minecraft:banner_patterns") {
            std::vector<std::pair<std::string, i32>> layers;
            for (const NbtView pt : e.value.items()) {
                const std::string pid(pt.entries().find("pattern").as_string().value_or(""));
                const std::string col(pt.entries().find("color").as_string().value_or("white"));
                std::string code;
                for (const auto& [id, cd] : kPatternCodes)
                    if (id == pid) code = std::string(cd);
                i32 dye = 0;
                for (usize d = 0; d < kDyes.size(); ++d)
                    if (kDyes[d] == col) dye = static_cast<i32>(d);
                if (!code.empty()) layers.emplace_back(code, dye);
            }
            w.begin_compound("BlockEntityTag");
            w.begin_list("Patterns", layers.empty() ? TagType::End : TagType::Compound, layers.size());
            for (const auto& [code, dye] : layers) {
                w.begin_compound();
                w.write_string("Pattern", code);
                w.write_int("Color", dye);
                w.end_compound();
            }
            w.end_list();
            w.end_compound();
        } else if (n == "minecraft:suspicious_stew_effects" && c.target.data_version >= 3578) { // 1.20.2: {id, duration}
            w.write_view("effects", e.value);
        } else {
            c.lossy.push_back("item component " + std::string(n) + " before 1.20.5");
            continue;
        }
        ++written;
    }
    if (name || lore || color) {
        w.begin_compound("display");
        if (name) write_json_text(w, "Name", name, c);
        if (lore) {
            w.begin_list("Lore", TagType::String, lore.items().size());
            for (const NbtView l : lore.items()) write_json_text(w, {}, l, c);
            w.end_list();
        }
        if (color) w.write_int("color", *color);
        w.end_compound();
        ++written;
    }
    w.end_compound();
    if (written == 0) return std::nullopt;
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    return std::move(*bytes);
}

// One item stack compound, written for the target (nullopt: the target has no such item).
bool write_item(NbtWriter& w, std::string_view name, NbtCompoundView item, Ctx& c, int depth) {
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
            if (const auto tag = legacy_tag(e.value.entries(), c, depth))
                if (const auto doc = io::nbt::NbtDocument::parse(*tag, c.endian)) w.write_view("tag", doc->root()); // a view of *tag
        } else if (e.name == "components" && c.target.data_version < kTextAsNbt) {
            write_components_1_20_5(w, e.value.entries(), c, depth);
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
    const bool furnace = be.id == "minecraft:furnace" || be.id == "minecraft:blast_furnace" || be.id == "minecraft:smoker";
    const auto short_of = [](NbtView v) {
        if (const auto i = v.as_int()) return static_cast<i16>(std::clamp(*i, -32768, 32767));
        return v.as_short().value_or(static_cast<i16>(v.as_byte().value_or(0)));
    };
    for (const io::nbt::NbtEntry& e : root) {
        const std::string_view k = e.name;
        if (furnace && (k == "lit_time_remaining" || k == "cooking_time_spent" || k == "lit_total_time") && target.data_version < kJava26_3) {
            const bool old_names = target.data_version < kFurnaceLit;
            const std::string_view name = !old_names ? k : k == "lit_time_remaining" ? "BurnTime" : k == "cooking_time_spent" ? "CookTime" : "CookTimeTotal";
            w.write_short(name, short_of(e.value));
        } else if (be.id == "minecraft:brewing_stand" && target.data_version < kJava26_3 && (k == "BrewTime" || k == "Fuel" || k == "total_fuel")) {
            if (k == "BrewTime") w.write_short("BrewTime", short_of(e.value));
            else if (k == "Fuel") w.write_byte("Fuel", static_cast<i8>(std::clamp<i32>(short_of(e.value), 0, 127)));
        } else if (be.id == "minecraft:decorated_pot" && k == "sherds" && e.value.is(TagType::Compound) && target.data_version < kJava26_3) {
            static constexpr std::array<std::string_view, 4> kSides{"back", "left", "right", "front"};
            w.begin_list(target.data_version < kSignFaces ? "shards" : "sherds", TagType::String, 4);
            for (const std::string_view side : kSides)
                w.write_string({}, e.value.entries().find(side).entries().find("id").as_string().value_or("minecraft:brick"));
            w.end_list();
        } else if (be.id == "minecraft:decorated_pot" && k == "item" && target.data_version < kPotItem) {
            lossy.push_back("decorated pot item before 1.20.3");
        } else if (be.id == "minecraft:beacon" && (k == "primary_effect" || k == "secondary_effect") && target.data_version < kBeaconNames) {
            const std::string_view n = e.value.as_string().value_or("");
            const std::string_view effect = n.starts_with("minecraft:") ? n.substr(10) : n;
            for (usize i = 1; i < tables::kBeaconEffects.size(); ++i)
                if (tables::kBeaconEffects[i] == effect) w.write_int(k == "primary_effect" ? "Primary" : "Secondary", static_cast<i32>(i));
        } else if (k == "Items" && e.value.is(TagType::List)) {
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
