// Java -> Bedrock block entities. Formats and sources: docs/formats/bedrock-world.md.

#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/block_entity_translator.hpp>

#include "item_translator.hpp"
#include "tables.hpp"

#include <algorithm>
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

using detail::text_of;
using detail::write_block_tag;

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

// Block entities Chunker writes with their id only (BedrockBlockEntityResolver, EmptyBlockEntityHandler).
std::optional<std::string_view> empty_block_entity(std::string_view java_id) {
    for (const tables::Pair& e : tables::kEmptyBlockEntities)
        if (e.java == java_id) return e.bedrock;
    return std::nullopt;
}

// A Byte, Short or Int field as a Short (clamped): `name`, else `old`, else `fallback`.
i16 number(NbtCompoundView c, std::string_view name, std::string_view old, i32 fallback) {
    NbtView v = c.find(name);
    if (!v && !old.empty()) v = c.find(old);
    i32 n = fallback;
    if (const auto b = v.as_byte()) n = *b;
    else if (const auto sh = v.as_short()) n = *sh;
    else if (const auto i = v.as_int()) n = *i;
    return static_cast<i16>(std::clamp(n, -32768, 32767));
}

// Java loot table id -> Bedrock path: "minecraft:chests/simple_dungeon" -> "loot_tables/chests/simple_dungeon.json",
// with the tables Bedrock still names the old way (Chunker BedrockRandomizableContainerBlockEntityHandler).
std::optional<std::string> bedrock_loot_table(std::string_view java) {
    if (java.empty() || java == "minecraft:chests/village/village_fisher") return std::nullopt; // no Bedrock table
    std::string id(java);
    for (const tables::Pair& r : tables::kLootTableRenames)
        if (id == r.java) id = std::string(r.bedrock);
    std::string path = id.starts_with("minecraft:") ? "loot_tables/" + id.substr(10) : "loot_tables/" + id;
    if (!path.ends_with(".json")) path += ".json";
    return path;
}

void write_loot_table(NbtWriter& w, NbtCompoundView c, ConversionReport& report, BlockPos pos) {
    const auto table = c.find("LootTable").as_string();
    if (!table) return;
    if (const auto path = bedrock_loot_table(*table)) w.write_string("LootTable", *path);
    else report.add(IssueKind::LossyBlock, "loot table without a Bedrock counterpart: " + std::string(*table), pos);
}

// Java entity id -> Bedrock entity id where they differ (Chunker JavaEntityTypeResolver / BedrockEntityTypeResolver).
std::optional<std::string> bedrock_entity(std::string_view java) {
    // Java-only entities (Chunker has no Bedrock id for them).
    static constexpr std::array<std::string_view, 6> kJavaOnly{"minecraft:giant", "minecraft:illusioner", "minecraft:marker",
                                                              "minecraft:interaction", "minecraft:spectral_arrow", "minecraft:mannequin"};
    for (const tables::Pair& r : tables::kEntityRenames)
        if (java == r.java) return std::string(r.bedrock);
    for (const std::string_view j : kJavaOnly)
        if (java == j || java.ends_with("_display")) return std::nullopt;
    return std::string(java);
}

// Mob spawner (Chunker BedrockSpawnerBlockEntityHandler / JavaSpawnerBlockEntityHandler): timings as Shorts, the
// mob from SpawnData {entity: {id}} (1.18+) or SpawnData {id} (older).
void write_spawner(NbtWriter& w, NbtCompoundView c, ConversionReport& report, BlockPos pos) {
    const auto shorts = [&](std::string_view name, i16 fallback) {
        const NbtView v = c.find(name);
        w.write_short(name, v.as_short().value_or(static_cast<i16>(v.as_int().value_or(fallback))));
    };
    shorts("Delay", 20);
    shorts("SpawnRange", 4);
    shorts("MaxSpawnDelay", 800);
    shorts("MinSpawnDelay", 200);
    shorts("SpawnCount", 4);
    shorts("RequiredPlayerRange", 16);
    shorts("MaxNearbyEntities", 6);
    const NbtCompoundView data = c.find("SpawnData").entries();
    std::optional<std::string_view> mob = data.find("entity").entries().find("id").as_string();
    if (!mob) mob = data.find("id").as_string();
    if (!mob) return; // an empty spawner
    if (const auto id = bedrock_entity(*mob)) w.write_string("EntityIdentifier", *id);
    else report.add(IssueKind::LossyBlock, "spawner mob without a Bedrock counterpart: " + std::string(*mob), pos);
    if (c.find("SpawnPotentials").items().size() > 1) report.add(IssueKind::LossyBlock, "spawner with several mobs: first kept", pos);
}

// Bedrock beacon effect ids (Chunker BedrockEffectIDResolver); Java stores names (1.20.2+) or the same numbers.
i32 beacon_effect(NbtView name, NbtView number) {
    if (const auto s = name.as_string()) {
        const std::string_view n = s->starts_with("minecraft:") ? s->substr(10) : *s;
        for (usize i = 1; i < tables::kBeaconEffects.size(); ++i)
            if (tables::kBeaconEffects[i] == n) return static_cast<i32>(i);
        return 0;
    }
    return number.as_int().value_or(0);
}

// Brewing stand slots: Java bottles 0-2, ingredient 3, fuel 4; Bedrock ingredient 0, bottles 1-3, fuel 4
// (Chunker BedrockContainerBlockEntityHandler.remapSlot).
i8 brewing_slot(i8 java) { return java == 3 ? i8{0} : java <= 2 && java >= 0 ? static_cast<i8>(java + 1) : java; }

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
        detail::write_items(w, "Items", c.find("Items"), m, report, java.pos);
        write_custom_name(w, c, report, java.pos);
        if (out.id == "Chest") write_pairing(w, *state, java.pos);
        write_loot_table(w, c, report, java.pos);
        // Furnaces (Chunker BedrockFurnaceBlockEntityHandler): Java lit_time_remaining / cooking_time_spent /
        // lit_total_time (1.21.4+) or BurnTime / CookTime / CookTimeTotal -> Bedrock BurnTime / CookTime / BurnDuration.
        if (out.id == "Furnace" || out.id == "BlastFurnace" || out.id == "Smoker") {
            w.write_short("BurnTime", number(c, "lit_time_remaining", "BurnTime", 0));
            w.write_short("CookTime", number(c, "cooking_time_spent", "CookTime", 0));
            w.write_short("BurnDuration", number(c, "lit_total_time", "CookTimeTotal", 0));
        }
        // Hopper TransferCooldown and LootTableSeed are transient (Chunker writes neither).
    } else if (java.id == "minecraft:brewing_stand") {
        // Chunker BedrockBrewingStandBlockEntityHandler: FuelAmount, FuelTotal (Java total_fuel from 26.3, else 20),
        // CookTime (Java BrewTime).
        out.id = "BrewingStand";
        w.write_string("id", out.id);
        detail::write_items(w, "Items", c.find("Items"), m, report, java.pos, brewing_slot);
        write_custom_name(w, c, report, java.pos);
        w.write_short("FuelAmount", number(c, "Fuel", {}, 0));
        w.write_short("FuelTotal", number(c, "total_fuel", {}, 20));
        w.write_short("CookTime", number(c, "BrewTime", {}, 0));
    } else if (java.id == "minecraft:chiseled_bookshelf") {
        out.id = "ChiseledBookshelf";
        w.write_string("id", out.id);
        detail::write_items(w, "Items", c.find("Items"), m, report, java.pos);
    } else if (java.id == "minecraft:crafter") {
        // Chunker BedrockCrafterBlockEntityHandler: disabled_slots as a Short bit mask (Java: an int array of slots).
        out.id = "Crafter";
        w.write_string("id", out.id);
        detail::write_items(w, "Items", c.find("Items"), m, report, java.pos);
        write_custom_name(w, c, report, java.pos);
        i32 disabled = 0;
        if (const auto slots = c.find("disabled_slots").as_int_array())
            for (usize i = 0; i < slots->size(); ++i)
                if ((*slots)[i] >= 0 && (*slots)[i] < 9) disabled |= 1 << (*slots)[i];
        w.write_short("disabled_slots", static_cast<i16>(disabled));
        w.write_int("crafting_ticks_remaining", c.find("crafting_ticks_remaining").as_int().value_or(0));
    } else if (java.id == "minecraft:decorated_pot") {
        // Chunker BedrockDecoratedPotBlockEntityHandler: "sherds" = 4 Bedrock item names (back, left, right, front);
        // Java: a list of ids, or {back: {id}, ...} from 26.3; missing sides are bricks.
        out.id = "DecoratedPot";
        w.write_string("id", out.id);
        std::array<std::string, 4> sides{"minecraft:brick", "minecraft:brick", "minecraft:brick", "minecraft:brick"};
        const NbtView sherds = c.find("sherds");
        if (sherds.is(TagType::List)) {
            usize i = 0;
            for (const NbtView v : sherds.items())
                if (i < 4) sides[i++] = std::string(v.as_string().value_or("minecraft:brick"));
        } else if (sherds.is(TagType::Compound)) {
            static constexpr std::array<std::string_view, 4> kSides{"back", "left", "right", "front"};
            for (usize i = 0; i < 4; ++i)
                if (const auto id = sherds.entries().find(kSides[i]).entries().find("id").as_string()) sides[i] = std::string(*id);
        }
        w.begin_list("sherds", TagType::String, 4);
        for (const std::string& side : sides) {
            const auto item = m.find_item(side);
            if (item) w.write_string({}, item->bedrock_name);
            else w.write_string({}, "minecraft:brick");
            if (!item) report.add(IssueKind::LossyBlock, "decorated pot sherd " + side, java.pos);
        }
        w.end_list();
        if (const NbtView item = c.find("item"); item.is(TagType::Compound)) detail::write_item(w, "item", item.entries(), m, report, java.pos);
        write_loot_table(w, c, report, java.pos);
    } else if (java.id == "minecraft:campfire" || java.id == "minecraft:soul_campfire") {
        // Chunker BedrockCampfireBlockEntityHandler: Item1-4 and ItemTime1-4 (Java Items with Slot 0-3, CookingTimes).
        out.id = "Campfire";
        w.write_string("id", out.id);
        const auto times = c.find("CookingTimes").as_int_array();
        for (const NbtView it : c.find("Items").items()) {
            const NbtCompoundView item = it.entries();
            const i8 slot = item.find("Slot").as_byte().value_or(-1);
            if (slot < 0 || slot > 3) continue;
            const std::string n = std::to_string(slot + 1);
            const usize index = static_cast<usize>(slot);
            if (detail::write_item(w, "Item" + n, item, m, report, java.pos))
                w.write_int("ItemTime" + n, times && index < times->size() ? (*times)[index] : 0);
        }
    } else if (java.id == "minecraft:beacon") {
        out.id = "Beacon";
        w.write_string("id", out.id);
        w.write_int("primary", beacon_effect(c.find("primary_effect"), c.find("Primary")));
        w.write_int("secondary", beacon_effect(c.find("secondary_effect"), c.find("Secondary")));
    } else if (java.id == "minecraft:lectern") {
        // Chunker BedrockLecternBlockEntityHandler: book, hasBook, page (Java Page / 2: Bedrock counts open spreads),
        // totalPages.
        out.id = "Lectern";
        w.write_string("id", out.id);
        const NbtView book = c.find("Book");
        bool has = false;
        if (book.is(TagType::Compound)) {
            has = detail::write_item(w, "book", book.entries(), m, report, java.pos);
            if (has) {
                const NbtCompoundView comps = book.entries().find("components").entries();
                NbtView pages = comps.find("minecraft:written_book_content").entries().find("pages");
                if (!pages) pages = comps.find("minecraft:writable_book_content").entries().find("pages");
                if (!pages) pages = book.entries().find("tag").entries().find("pages");
                w.write_int("totalPages", static_cast<i32>(pages.items().size()));
            }
        }
        w.write_byte("hasBook", has ? 1 : 0);
        w.write_int("page", c.find("Page").as_int().value_or(0) / 2);
    } else if (java.id == "minecraft:jukebox") {
        out.id = "Jukebox";
        w.write_string("id", out.id);
        if (const NbtView record = c.find("RecordItem"); record.is(TagType::Compound))
            detail::write_item(w, "RecordItem", record.entries(), m, report, java.pos);
    } else if (java.id == "minecraft:mob_spawner") {
        out.id = "MobSpawner";
        w.write_string("id", out.id);
        write_spawner(w, c, report, java.pos);
    } else if (java.id == "minecraft:command_block") {
        // Chunker BedrockCommandBlockBlockEntityHandler: Command, auto, TrackOutput.
        out.id = "CommandBlock";
        w.write_string("id", out.id);
        const std::string command(c.find("Command").as_string().value_or(""));
        w.write_string("Command", command);
        w.write_byte("auto", c.find("auto").as_byte().value_or(0));
        w.write_byte("TrackOutput", c.find("TrackOutput").as_byte().value_or(1));
        write_custom_name(w, c, report, java.pos);
        // Java and Bedrock commands share most syntax, but not all of it (selectors, NBT, block states).
        if (!command.empty()) report.add(IssueKind::LossyBlock, "command block: check the command in Bedrock", java.pos);
    } else if (java.id == "minecraft:brushable_block") {
        // Chunker BedrockBrushableBlockEntityHandler: type (the block), brush_direction, brush_count, item, LootTable.
        out.id = "BrushableBlock";
        w.write_string("id", out.id);
        w.write_string("type", state->id());
        w.write_byte("brush_direction", c.find("hit_direction").as_byte().value_or(6));
        w.write_int("brush_count", 0);
        if (const NbtView item = c.find("item"); item.is(TagType::Compound)) detail::write_item(w, "item", item.entries(), m, report, java.pos);
        write_loot_table(w, c, report, java.pos);
    } else if (java.id == "minecraft:sculk_sensor") {
        out.id = "SculkSensor";
        w.write_string("id", out.id);
        w.begin_compound("VibrationListener"); // Chunker BedrockSculkSensorBlockEntityHandler
        w.begin_compound("selector");
        w.end_compound();
        w.end_compound();
    } else if (const auto empty = empty_block_entity(java.id)) {
        // Written with their id only, as Chunker does: the data is transient or rebuilt by the game, except what is
        // reported here.
        out.id = std::string(*empty);
        w.write_string("id", out.id);
        if (java.id == "minecraft:beehive" && !c.find("bees").items().empty())
            report.add(IssueKind::LossyBlock, "bees in a beehive not converted (entities)", java.pos, c.find("bees").items().size());
        if (java.id == "minecraft:vault" && !c.find("server_data").entries().find("rewarded_players").items().empty())
            report.add(IssueKind::LossyBlock, "vault: players already rewarded are forgotten", java.pos);
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
