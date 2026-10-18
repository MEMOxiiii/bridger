// Bedrock -> Java block entities and item stacks: the inverse of block_entity_translator.cpp,
// using the same tables (tables.hpp). Formats: docs/formats/bedrock-world.md ("Block
// entities") for the Bedrock side; Java 1.20.5+ item stacks {Slot, id, count} and 1.20+
// sign faces {messages, color, has_glowing_text} (minecraft.wiki, "Block entity format").

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <array>
#include <bridger/translate/block_entity_translator.hpp>

#include "item_translator.hpp"
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

// Bedrock loot table path -> Java id: "loot_tables/chests/shipwrecksupply.json" -> "minecraft:chests/shipwreck_supply"
// (Chunker bedrockLootTableToChunker and its renames).
void write_java_loot_table(NbtWriter& w, NbtCompoundView c) {
    const auto table = c.find("LootTable").as_string();
    if (!table || table->empty()) return;
    std::string id(*table);
    if (id.starts_with("loot_tables/")) id = "minecraft:" + id.substr(12);
    if (id.ends_with(".json")) id.resize(id.size() - 5);
    for (const tables::Pair& r : tables::kLootTableRenames)
        if (id == r.bedrock) id = std::string(r.java);
    w.write_string("LootTable", id);
}

// Bedrock mob id -> Java id (tables::kEntityRenames; everything else has the same id).
std::string java_entity(std::string_view bedrock) {
    for (const tables::Pair& r : tables::kEntityRenames)
        if (bedrock == r.bedrock) return std::string(r.java);
    return std::string(bedrock);
}

i16 short_of(NbtCompoundView c, std::string_view name, i16 fallback) {
    const NbtView v = c.find(name);
    if (const auto sh = v.as_short()) return *sh;
    if (const auto i = v.as_int()) return static_cast<i16>(std::clamp(*i, -32768, 32767));
    if (const auto b = v.as_byte()) return *b;
    return fallback;
}

// Brewing stand slots: Bedrock ingredient 0, bottles 1-3, fuel 4 -> Java bottles 0-2, ingredient 3, fuel 4.
i8 java_brewing_slot(i8 bedrock) { return bedrock == 0 ? i8{3} : bedrock >= 1 && bedrock <= 3 ? static_cast<i8>(bedrock - 1) : bedrock; }

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
        detail::write_java_items(w, "Items", c.find("Items"), m, report, bedrock.pos);
        write_java_custom_name(w, c);
        write_java_loot_table(w, c);
        // Furnaces: Bedrock BurnTime / CookTime / BurnDuration -> Java 26.3 lit_time_remaining / cooking_time_spent /
        // lit_total_time (Int; older targets: java_block_entity_downgrade.cpp). Chunker JavaFurnaceBlockEntityHandler.
        if (id == "Furnace" || id == "BlastFurnace" || id == "Smoker") {
            w.write_int("lit_time_remaining", short_of(c, "BurnTime", 0));
            w.write_int("cooking_time_spent", short_of(c, "CookTime", 0));
            w.write_int("lit_total_time", short_of(c, "BurnDuration", 0));
        }
    } else if (id == "BrewingStand") {
        // Chunker JavaBrewingStandBlockEntityHandler (26.3: Int BrewTime, Fuel, total_fuel).
        out.id = "minecraft:brewing_stand";
        detail::write_java_items(w, "Items", c.find("Items"), m, report, bedrock.pos, java_brewing_slot);
        write_java_custom_name(w, c);
        w.write_int("BrewTime", short_of(c, "CookTime", 0));
        w.write_int("Fuel", short_of(c, "FuelAmount", 0));
        w.write_int("total_fuel", short_of(c, "FuelTotal", 20));
    } else if (id == "ChiseledBookshelf") {
        out.id = "minecraft:chiseled_bookshelf";
        detail::write_java_items(w, "Items", c.find("Items"), m, report, bedrock.pos);
    } else if (id == "Crafter") {
        // disabled_slots: a Short bit mask -> Java's Int array of slots (Chunker JavaCrafterBlockEntityHandler).
        out.id = "minecraft:crafter";
        detail::write_java_items(w, "Items", c.find("Items"), m, report, bedrock.pos);
        write_java_custom_name(w, c);
        std::vector<i32> slots;
        const i32 mask = short_of(c, "disabled_slots", 0);
        for (i32 i = 0; i < 9; ++i)
            if (mask & (1 << i)) slots.push_back(i);
        w.write_int_array("disabled_slots", slots);
        w.write_int("crafting_ticks_remaining", c.find("crafting_ticks_remaining").as_int().value_or(0));
    } else if (id == "DecoratedPot") {
        // Bedrock "sherds" (or "shards" before 1.20): back, left, right, front -> Java 26.3 {back: {id}, ...}
        // (Chunker JavaDecoratedPotBlockEntityHandler; a list before 26.3: java_block_entity_downgrade.cpp).
        out.id = "minecraft:decorated_pot";
        NbtView sherds = c.find("sherds");
        if (!sherds) sherds = c.find("shards");
        static constexpr std::array<std::string_view, 4> kSides{"back", "left", "right", "front"};
        w.begin_compound("sherds");
        usize i = 0;
        for (const NbtView v : sherds.items()) {
            if (i >= 4) break;
            const std::string sherd = utf8_of(v.as_string().value_or("minecraft:brick"));
            const auto java = m.java_item_of_bedrock(sherd, 0);
            if (!java) report.add(IssueKind::LossyBlock, "decorated pot sherd " + sherd, bedrock.pos);
            w.begin_compound(kSides[i++]);
            w.write_string("id", java ? std::string(*java) : std::string("minecraft:brick"));
            w.end_compound();
        }
        w.end_compound();
        if (const NbtView item = c.find("item"); item.is(TagType::Compound)) (void)detail::write_java_item(w, "item", item.entries(), m, report, bedrock.pos);
        write_java_loot_table(w, c);
    } else if (id == "Campfire") {
        // Item1-4 / ItemTime1-4 -> Items (Slot 0-3) and CookingTimes (Chunker JavaCampfireBlockEntityHandler).
        out.id = block == "minecraft:soul_campfire" ? "minecraft:soul_campfire" : "minecraft:campfire";
        NbtWriter items(Endian::Big);
        items.begin_root_compound();
        std::vector<i8> slots;
        std::array<i32, 4> times{};
        for (i8 i = 0; i < 4; ++i) {
            const std::string n = std::to_string(i + 1);
            if (const NbtView item = c.find("Item" + n); item.is(TagType::Compound) &&
                                                         detail::write_java_item(items, std::to_string(slots.size()), item.entries(), m, report, bedrock.pos)) {
                slots.push_back(i);
                times[static_cast<usize>(i)] = c.find("ItemTime" + n).as_int().value_or(0);
            }
        }
        items.end_compound();
        const auto bytes = std::move(items).finish();
        std::optional<io::nbt::NbtDocument> parsed;
        if (bytes)
            if (auto d = io::nbt::NbtDocument::parse(*bytes, Endian::Big)) parsed = std::move(*d);
        const usize n = parsed ? slots.size() : 0;
        w.begin_list("Items", n == 0 ? TagType::End : TagType::Compound, n);
        for (usize k = 0; k < n; ++k) {
            w.begin_compound();
            for (const io::nbt::NbtEntry& en : parsed->root().entries().find(std::to_string(k)).entries()) w.write_view(en.name, en.value);
            w.write_byte("Slot", slots[k]);
            w.end_compound();
        }
        w.end_list();
        w.write_int_array("CookingTimes", times);
    } else if (id == "Beacon") {
        // Bedrock effect ids -> Java names (1.20.2+; ids before: java_block_entity_downgrade.cpp).
        out.id = "minecraft:beacon";
        for (const auto& [bedrock_key, java_key] : {std::pair{"primary", "primary_effect"}, std::pair{"secondary", "secondary_effect"}}) {
            const i32 e = c.find(bedrock_key).as_int().value_or(0);
            if (e > 0 && static_cast<usize>(e) < tables::kBeaconEffects.size())
                w.write_string(java_key, "minecraft:" + std::string(tables::kBeaconEffects[static_cast<usize>(e)]));
            else if (e != 0)
                report.add(IssueKind::LossyBlock, "beacon effect " + std::to_string(e), bedrock.pos);
        }
        write_java_custom_name(w, c);
    } else if (id == "Lectern") {
        // book, page (open spreads) -> Book, Page = page * 2 (Chunker JavaLecternBlockEntityHandler).
        out.id = "minecraft:lectern";
        if (const NbtView book = c.find("book"); book.is(TagType::Compound)) (void)detail::write_java_item(w, "Book", book.entries(), m, report, bedrock.pos);
        w.write_int("Page", c.find("page").as_int().value_or(0) * 2);
    } else if (id == "Jukebox") {
        out.id = "minecraft:jukebox";
        if (const NbtView record = c.find("RecordItem"); record.is(TagType::Compound))
            (void)detail::write_java_item(w, "RecordItem", record.entries(), m, report, bedrock.pos);
    } else if (id == "MobSpawner") {
        // Chunker JavaSpawnerBlockEntityHandler: timings as Shorts, SpawnData {entity: {id}} (1.18+).
        out.id = "minecraft:mob_spawner";
        for (const auto& [name, fallback] : {std::pair{"Delay", i16{20}}, std::pair{"SpawnRange", i16{4}}, std::pair{"MaxSpawnDelay", i16{800}},
                                             std::pair{"MinSpawnDelay", i16{200}}, std::pair{"SpawnCount", i16{4}},
                                             std::pair{"RequiredPlayerRange", i16{16}}, std::pair{"MaxNearbyEntities", i16{6}}})
            w.write_short(name, short_of(c, name, fallback));
        if (const auto mob = c.find("EntityIdentifier").as_string(); mob && !mob->empty() && *mob != "minecraft:") {
            w.begin_compound("SpawnData");
            w.begin_compound("entity");
            w.write_string("id", java_entity(*mob));
            w.end_compound();
            w.end_compound();
        }
    } else if (id == "CommandBlock") {
        out.id = "minecraft:command_block";
        const std::string command(c.find("Command").as_string().value_or(""));
        w.write_string("Command", command);
        w.write_byte("auto", c.find("auto").as_byte().value_or(0));
        w.write_byte("TrackOutput", c.find("TrackOutput").as_byte().value_or(1));
        write_java_custom_name(w, c);
        if (!command.empty()) report.add(IssueKind::LossyBlock, "command block: check the command in Java", bedrock.pos);
    } else if (id == "BrushableBlock") {
        out.id = "minecraft:brushable_block";
        w.write_byte("hit_direction", c.find("brush_direction").as_byte().value_or(6));
        if (const NbtView item = c.find("item"); item.is(TagType::Compound)) (void)detail::write_java_item(w, "item", item.entries(), m, report, bedrock.pos);
        write_java_loot_table(w, c);
    } else if (id == "SculkSensor") {
        out.id = "minecraft:sculk_sensor"; // the vibration state is transient
    } else if (const auto empty = [&]() -> std::optional<std::string_view> {
                   for (const tables::Pair& e : tables::kEmptyBlockEntities)
                       if (e.bedrock == id) return e.java;
                   return std::nullopt;
               }()) {
        out.id = std::string(*empty); // id only, as Chunker (EmptyBlockEntityHandler)
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
