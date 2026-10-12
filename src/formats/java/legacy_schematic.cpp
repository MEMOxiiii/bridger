// Legacy .schematic reader. Format and sources: docs/formats/legacy-schematic.md.

#include "nbt_helpers.hpp"

#include <bridger/formats/java/legacy_schematic.hpp>

#include <array>
#include <map>
#include <string>
#include <unordered_map>

namespace bridger::formats::java {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

// Pre-1.11 block entity ids -> namespaced ids (minecraft.wiki "Block entity format", history:
// 16w32a renamed them). Files from 1.11-1.12 already use the namespaced form.
std::string normalize_block_entity_id(std::string_view id) {
    static const std::map<std::string_view, std::string_view> kLegacy{
        {"Chest", "minecraft:chest"},           {"Trap", "minecraft:dispenser"},       {"Dropper", "minecraft:dropper"},
        {"Furnace", "minecraft:furnace"},       {"Hopper", "minecraft:hopper"},        {"Sign", "minecraft:sign"},
        {"Cauldron", "minecraft:brewing_stand"}, {"EnchantTable", "minecraft:enchanting_table"},
        {"EnderChest", "minecraft:ender_chest"}, {"Beacon", "minecraft:beacon"},       {"Skull", "minecraft:skull"},
        {"FlowerPot", "minecraft:flower_pot"},  {"Banner", "minecraft:banner"},        {"Bed", "minecraft:bed"},
        {"MobSpawner", "minecraft:mob_spawner"}, {"Music", "minecraft:noteblock"},     {"RecordPlayer", "minecraft:jukebox"},
        {"Comparator", "minecraft:comparator"}, {"Control", "minecraft:command_block"}, {"Piston", "minecraft:piston"},
        {"Airportal", "minecraft:end_portal"},  {"EndGateway", "minecraft:end_gateway"}, {"Structure", "minecraft:structure_block"}};
    if (const auto it = kLegacy.find(id); it != kLegacy.end()) return std::string(it->second);
    if (id.find(':') == std::string_view::npos) return "minecraft:" + std::string(id);
    return std::string(id);
}

constexpr std::array<std::string_view, 16> kDyeColors{"white", "orange", "magenta", "light_blue", "yellow", "lime",
                                                      "pink", "gray", "light_gray", "cyan", "purple", "blue",
                                                      "brown", "green", "red", "black"};

} // namespace

Result<StructureFile> read_legacy_schematic(std::span<const u8> file_bytes, std::shared_ptr<BlockStateRegistry> blocks,
                                            std::shared_ptr<BiomeRegistry> biomes, const LegacyTables& tables,
                                            const StructureReadOptions& opt) {
    auto bad = [](const std::string& why) { return make_error(ErrorCode::Malformed, "schematic: " + why); };
    BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(file_bytes, opt.decompress, opt.nbt, "schematic"));
    const NbtCompoundView root = nbt.doc.root().entries();

    StructureFile out{FormatId::LegacySchematic, std::nullopt, {}, {}};
    const auto materials = root.find("Materials").as_string();
    if (materials && *materials != "Alpha")
        return make_error(ErrorCode::UnsupportedFormat, "schematic: materials '" + std::string(*materials) +
                                                            "' are not supported (only Alpha)");
    if (root.find("Add")) return make_error(ErrorCode::UnsupportedFormat, "schematic: the deprecated 'Add' tag is not supported");

    const auto w = root.find("Width").as_short(), h = root.find("Height").as_short(), l = root.find("Length").as_short();
    if (!w || !h || !l) return bad("missing Width/Height/Length");
    const Extent extent{static_cast<u16>(*w), static_cast<u16>(*h), static_cast<u16>(*l)};
    if (extent.volume() > opt.max_volume) return make_error(ErrorCode::LimitExceeded, "schematic: volume exceeds the limit");
    const auto ids = root.find("Blocks").as_byte_array();
    const auto data = root.find("Data").as_byte_array();
    if (!ids || !data) return bad("missing Blocks/Data");
    const u64 volume = extent.volume();
    if (ids->size() != volume || data->size() != volume) return bad("Blocks/Data length differs from Width*Height*Length");
    const auto add = root.find("AddBlocks").as_byte_array();
    if (add && add->size() < (volume + 1) / 2) return bad("AddBlocks too short");

    BRIDGER_TRY(out.structure, Structure::create(blocks, std::move(biomes), extent, {Edition::Java, 0}));
    Structure& s = out.structure;
    if (const auto ox = root.find("WEOffsetX").as_int(), oy = root.find("WEOffsetY").as_int(), oz = root.find("WEOffsetZ").as_int();
        ox && oy && oz)
        s.offset = {*ox, *oy, *oz};

    // (id, data) -> state, resolved once per pair.
    std::unordered_map<u32, StateId> cache;
    u64 approximated = 0, unknown = 0;
    auto resolve = [&](u16 id, u8 dv) -> StateId {
        const u32 key = (u32{id} << 4) | dv;
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
        std::optional<std::string> text = tables.block(id, dv);
        if (!text && (dv & 8)) { text = tables.block(id, static_cast<u8>(dv & 7)); approximated += text ? 1 : 0; }
        if (!text && dv != 0) { text = tables.block(id, 0); approximated += text ? 1 : 0; }
        StateId sid;
        if (text) {
            auto parsed = BlockState::parse(*text);
            sid = parsed ? blocks->intern(*parsed) : blocks->intern(BlockState("bridger:legacy_" + std::to_string(id) + "_" + std::to_string(dv), {}));
        } else {
            ++unknown;
            sid = blocks->intern(BlockState("bridger:legacy_" + std::to_string(id) + "_" + std::to_string(dv), {}));
        }
        cache.emplace(key, sid);
        return sid;
    };

    // Index order: (y * Length + z) * Width + x.
    for (u32 y = 0; y < extent.y; ++y)
        for (u32 z = 0; z < extent.z; ++z)
            for (u32 x = 0; x < extent.x; ++x) {
                const usize i = (static_cast<usize>(y) * extent.z + z) * extent.x + x;
                u16 id = (*ids)[i];
                if (add) {
                    const u8 b = (*add)[i >> 1];
                    id |= static_cast<u16>(((i & 1) == 0 ? (b >> 4) & 0xF : b & 0xF) << 8); // even index: high nibble
                }
                s.set_block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)},
                            resolve(id, static_cast<u8>((*data)[i] & 0xF)));
            }
    if (approximated > 0)
        out.warnings.push_back(std::to_string(approximated) + " legacy id/data pair(s) were not in the table and used the "
                                                              "nearest data value");
    if (unknown > 0)
        out.warnings.push_back(std::to_string(unknown) + " legacy block id(s) are unknown; they are reported as unmapped");

    // Block entities, rewritten into the modern Java form the translator expects.
    for (const NbtView entry : root.find("TileEntities").items()) {
        const NbtCompoundView c = entry.entries();
        const auto p = detail::xyz(c);
        const auto id_raw = c.find("id").as_string();
        if (!p || !id_raw) { out.warnings.push_back("a block entity without id/x/y/z was not imported"); continue; }
        BlockEntity be;
        be.pos = {(*p)[0], (*p)[1], (*p)[2]};
        if (!s.contains(be.pos)) { out.warnings.push_back("a block entity outside the schematic was not imported"); continue; }
        BRIDGER_TRY(const std::string raw_id, detail::utf8(*id_raw));
        be.id = normalize_block_entity_id(raw_id);

        // Bed color lived in the block entity (1.12); since 1.13 it is the block itself.
        if (be.id == "minecraft:bed") {
            if (const auto color = c.find("color").as_int(); color && *color >= 0 && *color < 16) {
                const BlockState& old = blocks->get(s.block(be.pos));
                if (old.id().ends_with("_bed"))
                    s.set_block(be.pos, blocks->intern(BlockState("minecraft:" + std::string(kDyeColors[static_cast<usize>(*color)]) + "_bed",
                                                                  old.properties())));
            }
        }
        if (be.id == "minecraft:flower_pot" || be.id == "minecraft:skull" || be.id == "minecraft:banner")
            out.warnings.push_back("legacy " + be.id + " data (contents/type/patterns) at " + std::to_string(be.pos.x) + "," +
                                   std::to_string(be.pos.y) + "," + std::to_string(be.pos.z) + " is not converted");

        NbtWriter wr(Endian::Big);
        wr.begin_root_compound();
        for (const io::nbt::NbtEntry& e : c) {
            if (e.name == "id" || e.name == "x" || e.name == "y" || e.name == "z" || e.name == "Items") continue;
            wr.write_view(e.name, e.value);
        }
        // Items: {id: Short|String, Count: Byte, Damage: Short, Slot: Byte} -> {id: String, count: Int, Slot: Byte}.
        if (const NbtView items = c.find("Items", TagType::List)) {
            std::vector<std::tuple<i8, std::string, i32>> converted;
            for (const NbtView item : items.items()) {
                const NbtCompoundView ic = item.entries();
                const i8 slot = ic.find("Slot").as_byte().value_or(0);
                const i32 count = ic.find("Count").as_byte().value_or(1);
                const u16 damage = static_cast<u16>(ic.find("Damage").as_short().value_or(0));
                std::optional<std::string> name;
                if (const auto num = ic.find("id").as_short()) name = tables.item(static_cast<u16>(*num), damage);
                else if (const auto str = ic.find("id").as_string()) {
                    BRIDGER_TRY(std::string sid, detail::utf8(*str));
                    if (sid.find(':') == std::string::npos) sid = "minecraft:" + sid;
                    // String ids (1.8-1.12) with a damage value cannot be flattened without a name table.
                    if (damage != 0) out.warnings.push_back("item " + sid + " with damage " + std::to_string(damage) +
                                                            " kept without its variant");
                    name = sid;
                }
                if (!name) { out.warnings.push_back("an item with an unknown legacy id was not imported"); continue; }
                if (ic.find("tag")) out.warnings.push_back("item data (enchantments, names) of " + *name + " not converted");
                converted.emplace_back(slot, *name, count);
            }
            wr.begin_list("Items", converted.empty() ? TagType::End : TagType::Compound, converted.size());
            for (const auto& [slot, name, count] : converted) {
                wr.begin_compound();
                wr.write_byte("Slot", slot);
                wr.write_string("id", name);
                wr.write_int("count", count);
                wr.end_compound();
            }
            wr.end_list();
        }
        wr.end_compound();
        BRIDGER_TRY(std::vector<u8> bytes, std::move(wr).finish());
        be.data = RawNbt{std::move(bytes), Endian::Big};
        s.block_entities.push_back(std::move(be));
    }

    for (const NbtView entry : root.find("Entities").items()) {
        const NbtCompoundView c = entry.entries();
        const NbtView pos = c.find("Pos", TagType::List);
        const auto id = c.find("id").as_string();
        if (!pos || pos.items().size() != 3 || pos.items().element_type() != TagType::Double || !id) {
            out.warnings.push_back("an entity without a valid id/Pos was not imported");
            continue;
        }
        Entity e;
        e.pos = {*pos.items().at(0).as_double(), *pos.items().at(1).as_double(), *pos.items().at(2).as_double()};
        BRIDGER_TRY(e.id, detail::utf8(*id));
        s.entities.push_back(std::move(e));
    }
    return out;
}

} // namespace bridger::formats::java
