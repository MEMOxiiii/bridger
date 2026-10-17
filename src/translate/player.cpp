// The single-player player. Fields follow Chunker (HiveGamesOSS/Chunker, MIT, commit 50aaf6d): JavaLevelReader.parsePlayer
// and SLOT_TO_EQUIPMENT, JavaLevelWriter.writePlayer, BedrockLevelWriter.writeLocalPlayer and BedrockLevelReader
// (PLAYER_HEIGHT 1.62001: Bedrock stores the eye position). docs/formats/bedrock-world.md, "Player".

#include <bridger/translate/player.hpp>

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include "item_translator.hpp"

#include <array>
#include <optional>
#include <cfloat>
#include <string>

namespace bridger::translate {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

constexpr f32 kPlayerHeight = 1.62001f;

// Java armor/offhand slots and their 1.21.5+ equipment names (Chunker SLOT_TO_EQUIPMENT).
struct Equip {
    i8 slot;
    std::string_view name;
};
constexpr std::array<Equip, 5> kEquipment{{{100, "feet"}, {101, "legs"}, {102, "chest"}, {103, "head"}, {-106, "offhand"}}};

void write_empty_item(NbtWriter& w) {
    // An empty Bedrock slot: an item without a name (as Bedrock saves empty armor and offhand slots).
    w.begin_compound();
    w.write_byte("Count", 0);
    w.write_short("Damage", 0);
    w.write_string("Name", "");
    w.write_byte("WasPickedUp", 0);
    w.end_compound();
}

void write_floats(NbtWriter& w, std::string_view name, std::initializer_list<f32> values) {
    w.begin_list(name, TagType::Float, values.size());
    for (const f32 v : values) w.write_float({}, v);
    w.end_list();
}

void write_doubles(NbtWriter& w, std::string_view name, std::initializer_list<f64> values) {
    w.begin_list(name, TagType::Double, values.size());
    for (const f64 v : values) w.write_double({}, v);
    w.end_list();
}

f64 num(NbtView list, usize i) {
    const NbtView v = list.items().size() > i ? list.items().at(i) : NbtView();
    return v.as_double().value_or(static_cast<f64>(v.as_float().value_or(0.0f)));
}

void report_dropped(NbtCompoundView c, std::initializer_list<std::string_view> fields, ConversionReport& report, BlockPos where) {
    for (const std::string_view f : fields)
        if (const NbtView v = c.find(f); v && !(v.is(TagType::List) && v.items().empty()))
            report.add(IssueKind::LossyBlock, "player " + std::string(f) + " not converted", where);
}

} // namespace

std::optional<std::vector<u8>> java_player_to_bedrock(const RawNbt& java_player, const MappingData& mapping, ConversionReport& report) {
    const auto doc = io::nbt::NbtDocument::parse(java_player.bytes, java_player.endian);
    if (!doc) return std::nullopt;
    const NbtCompoundView c = doc->root().entries();
    const NbtView pos = c.find("Pos");
    const BlockPos where{static_cast<i32>(num(pos, 0)), static_cast<i32>(num(pos, 1)), static_cast<i32>(num(pos, 2))};
    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    write_floats(w, "Pos", {static_cast<f32>(num(pos, 0)), static_cast<f32>(num(pos, 1)) + kPlayerHeight, static_cast<f32>(num(pos, 2))});
    write_floats(w, "Motion", {static_cast<f32>(num(c.find("Motion"), 0)), static_cast<f32>(num(c.find("Motion"), 1)),
                               static_cast<f32>(num(c.find("Motion"), 2))});
    write_floats(w, "Rotation", {static_cast<f32>(num(c.find("Rotation"), 0)), static_cast<f32>(num(c.find("Rotation"), 1))});

    // Main inventory: slots 0..35 keep their numbers; armor (100..103) and offhand (-106) go to their own lists.
    std::array<std::optional<NbtCompoundView>, 5> equipment{};
    std::vector<std::pair<i8, NbtCompoundView>> main;
    for (const NbtView it : c.find("Inventory").items()) {
        const NbtCompoundView item = it.entries();
        const i8 slot = item.find("Slot").as_byte().value_or(0);
        bool equip = false;
        for (usize i = 0; i < kEquipment.size(); ++i)
            if (kEquipment[i].slot == slot) {
                equipment[i] = item;
                equip = true;
            }
        if (!equip && slot >= 0 && slot < 100) main.emplace_back(slot, item);
    }
    for (usize i = 0; i < kEquipment.size(); ++i) // 1.21.5+ "equipment" compound
        if (const NbtView e = c.find("equipment").entries().find(kEquipment[i].name); e.is(TagType::Compound)) equipment[i] = e.entries();
    // The inventory goes through a scratch compound first, so that items without a Bedrock counterpart drop out.
    {
        NbtWriter scratch(Endian::Little);
        scratch.begin_root_compound();
        std::vector<i8> slots;
        for (const auto& [slot, item] : main)
            if (detail::write_item(scratch, std::to_string(slots.size()), item, mapping, report, where)) slots.push_back(slot);
        scratch.end_compound();
        const auto bytes = std::move(scratch).finish();
        std::optional<io::nbt::NbtDocument> items;
        if (bytes)
            if (auto d = io::nbt::NbtDocument::parse(*bytes, Endian::Little)) items = std::move(*d);
        const usize n = items ? slots.size() : 0;
        w.begin_list("Inventory", n == 0 ? TagType::End : TagType::Compound, n);
        for (usize i = 0; i < n; ++i) {
            w.begin_compound();
            for (const io::nbt::NbtEntry& en : items->root().entries().find(std::to_string(i)).entries()) w.write_view(en.name, en.value);
            w.write_byte("Slot", slots[i]);
            w.end_compound();
        }
        w.end_list();
    }
    // Armor: head, chest, legs, feet (Chunker writes slots 103..100); empty slots are written empty.
    w.begin_list("Armor", TagType::Compound, 4);
    for (const usize i : {3u, 2u, 1u, 0u}) {
        if (!equipment[i] || !detail::write_item(w, {}, *equipment[i], mapping, report, where)) write_empty_item(w);
    }
    w.end_list();
    w.begin_list("Offhand", TagType::Compound, 1);
    if (!equipment[4] || !detail::write_item(w, {}, *equipment[4], mapping, report, where)) write_empty_item(w);
    w.end_list();

    // Dimension: "minecraft:the_nether" / -1 -> 1, "minecraft:the_end" / 1 -> 2.
    i32 dim = 0;
    if (const auto s = c.find("Dimension").as_string()) dim = *s == "minecraft:the_nether" ? 1 : *s == "minecraft:the_end" ? 2 : 0;
    else if (const auto n = c.find("Dimension").as_int()) dim = *n == -1 ? 1 : *n == 1 ? 2 : 0;
    w.write_int("DimensionId", dim);
    // Game mode: survival 0, creative 1, adventure 2; spectator (3) becomes spectator (6) on Bedrock 1.19.50+ (Chunker).
    const i32 mode = c.find("playerGameType").as_int().value_or(0);
    w.write_int("PlayerGameMode", mode == 3 ? 6 : mode);
    // Bedrock does not use the player default movement speed without this attribute (Chunker).
    w.begin_list("Attributes", TagType::Compound, 1);
    w.begin_compound();
    w.write_float("Base", 0.1f);
    w.write_float("Current", 0.1f);
    w.write_float("DefaultMax", FLT_MAX);
    w.write_float("DefaultMin", 0.0f);
    w.write_float("Max", FLT_MAX);
    w.write_float("Min", 0.0f);
    w.write_string("Name", "minecraft:movement");
    w.end_compound();
    w.end_list();
    w.end_compound();
    report_dropped(c, {"EnderItems", "XpLevel", "ActiveEffects", "active_effects", "SpawnX", "respawn"}, report, where);
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    return std::move(*bytes);
}

std::optional<RawNbt> bedrock_player_to_java(std::span<const u8> bedrock_player, i32 java_data_version, const MappingData& mapping,
                                             ConversionReport& report) {
    const auto doc = io::nbt::NbtDocument::parse(bedrock_player, Endian::Little);
    if (!doc) return std::nullopt;
    const NbtCompoundView c = doc->root().entries();
    const NbtView pos = c.find("Pos");
    const BlockPos where{static_cast<i32>(num(pos, 0)), static_cast<i32>(num(pos, 1)), static_cast<i32>(num(pos, 2))};
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.write_int("DataVersion", java_data_version);
    write_doubles(w, "Pos", {num(pos, 0), num(pos, 1) - static_cast<f64>(kPlayerHeight), num(pos, 2)});
    write_doubles(w, "Motion", {num(c.find("Motion"), 0), num(c.find("Motion"), 1), num(c.find("Motion"), 2)});
    write_floats(w, "Rotation", {static_cast<f32>(num(c.find("Rotation"), 0)), static_cast<f32>(num(c.find("Rotation"), 1))});
    const i32 dim = c.find("DimensionId").as_int().value_or(0);
    w.write_string("Dimension", dim == 1 ? "minecraft:the_nether" : dim == 2 ? "minecraft:the_end" : "minecraft:overworld");
    const i32 mode = c.find("PlayerGameMode").as_int().value_or(0);
    w.write_int("playerGameType", mode > 3 ? 3 : mode); // 5 (default) and 6 (spectator) -> spectator, as Chunker

    // Inventory, armor (Bedrock head..feet -> Java 103..100) and offhand (-106).
    const bool split = java_data_version >= 4325; // 1.21.5: armor and offhand in "equipment"
    std::vector<std::pair<i8, NbtCompoundView>> main;
    for (const NbtView it : c.find("Inventory").items()) main.emplace_back(it.entries().find("Slot").as_byte().value_or(0), it.entries());
    std::vector<std::pair<i8, NbtCompoundView>> equip; // Java slot, item
    usize a = 0;
    for (const NbtView it : c.find("Armor").items()) {
        if (a < 4) equip.emplace_back(static_cast<i8>(103 - a), it.entries());
        ++a;
    }
    if (const auto offhand = c.find("Offhand").items(); !offhand.empty()) equip.emplace_back(static_cast<i8>(-106), offhand.at(0).entries());
    const auto write_list = [&](std::string_view name, const std::vector<std::pair<i8, NbtCompoundView>>& items) {
        NbtWriter scratch(Endian::Big);
        scratch.begin_root_compound();
        std::vector<i8> slots;
        for (const auto& [slot, item] : items)
            if (detail::write_java_item(scratch, std::to_string(slots.size()), item, mapping, report, where)) slots.push_back(slot);
        scratch.end_compound();
        const auto bytes = std::move(scratch).finish();
        std::optional<io::nbt::NbtDocument> parsed;
        if (bytes)
            if (auto d = io::nbt::NbtDocument::parse(*bytes, Endian::Big)) parsed = std::move(*d);
        w.begin_list(name, slots.empty() || !parsed ? TagType::End : TagType::Compound, parsed ? slots.size() : 0);
        if (parsed)
            for (usize i = 0; i < slots.size(); ++i) {
                w.begin_compound();
                for (const io::nbt::NbtEntry& en : parsed->root().entries().find(std::to_string(i)).entries()) w.write_view(en.name, en.value);
                w.write_byte("Slot", slots[i]);
                w.end_compound();
            }
        w.end_list();
    };
    if (split) {
        write_list("Inventory", main);
        w.begin_compound("equipment");
        for (const auto& [slot, item] : equip) {
            std::string_view key;
            for (const Equip& e : kEquipment)
                if (e.slot == slot) key = e.name;
            (void)detail::write_java_item(w, key, item, mapping, report, where);
        }
        w.end_compound();
    } else {
        std::vector<std::pair<i8, NbtCompoundView>> all = main;
        all.insert(all.end(), equip.begin(), equip.end());
        write_list("Inventory", all);
    }
    w.end_compound();
    report_dropped(c, {"EnderChestInventory", "PlayerLevel", "ActiveEffects", "SpawnX"}, report, where);
    auto bytes = std::move(w).finish();
    if (!bytes) return std::nullopt;
    return RawNbt{std::move(*bytes), Endian::Big};
}

} // namespace bridger::translate
