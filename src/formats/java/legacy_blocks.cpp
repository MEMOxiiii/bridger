// Pre-1.13 block data shared by the legacy .schematic reader and old Java chunks
// (docs/formats/legacy-schematic.md, docs/formats/anvil.md "Old worlds").

#include "legacy_blocks.hpp"
#include "nbt_helpers.hpp"

#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace bridger::formats::detail {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

std::string normalize_block_entity_id(std::string_view id) {
    // minecraft.wiki "Block entity format", history: 16w32a (1.11) renamed them.
    static const std::map<std::string_view, std::string_view> kLegacy{
        {"Chest", "minecraft:chest"},           {"Trap", "minecraft:dispenser"},       {"Dropper", "minecraft:dropper"},
        {"Furnace", "minecraft:furnace"},       {"Hopper", "minecraft:hopper"},        {"Sign", "minecraft:sign"},
        {"Cauldron", "minecraft:brewing_stand"}, {"EnchantTable", "minecraft:enchanting_table"},
        {"EnderChest", "minecraft:ender_chest"}, {"Beacon", "minecraft:beacon"},       {"Skull", "minecraft:skull"},
        {"FlowerPot", "minecraft:flower_pot"},  {"Banner", "minecraft:banner"},        {"Bed", "minecraft:bed"},
        {"MobSpawner", "minecraft:mob_spawner"}, {"Music", "minecraft:noteblock"},     {"RecordPlayer", "minecraft:jukebox"},
        {"Comparator", "minecraft:comparator"}, {"Control", "minecraft:command_block"}, {"Piston", "minecraft:piston"},
        {"Airportal", "minecraft:end_portal"},  {"EndGateway", "minecraft:end_gateway"}, {"Structure", "minecraft:structure_block"},
        {"DLDetector", "minecraft:daylight_detector"}};
    if (const auto it = kLegacy.find(id); it != kLegacy.end()) return std::string(it->second);
    if (id.find(':') == std::string_view::npos) return "minecraft:" + std::string(id);
    return std::string(id);
}

Result<std::optional<BlockEntity>> legacy_block_entity(NbtCompoundView c, BlockPos pos, const java::LegacyTables& tables,
                                                       std::vector<std::string>& warnings) {
    const auto id_raw = c.find("id").as_string();
    if (!id_raw) {
        warnings.push_back("a block entity without an id was not imported");
        return std::optional<BlockEntity>{};
    }
    BlockEntity be;
    be.pos = pos;
    BRIDGER_TRY(const std::string raw_id, detail::utf8(*id_raw));
    be.id = normalize_block_entity_id(raw_id);

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
                if (damage != 0) warnings.push_back("item " + sid + " with damage " + std::to_string(damage) + " kept without its variant");
                name = sid;
            }
            if (!name) {
                warnings.push_back("an item with an unknown legacy id was not imported");
                continue;
            }
            if (ic.find("tag")) warnings.push_back("item data (enchantments, names) of " + *name + " not converted");
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
    return std::optional<BlockEntity>(std::move(be));
}

namespace {

std::string prop(const BlockState& s, std::string_view name) {
    for (const BlockProperty& p : s.properties())
        if (p.name == name) return p.value;
    return {};
}

std::vector<BlockProperty> with(std::vector<BlockProperty> props, std::string_view name, std::string value) {
    for (BlockProperty& p : props)
        if (p.name == name) {
            p.value = std::move(value);
            return props;
        }
    props.push_back({std::string(name), std::move(value)});
    return props;
}

bool is_tall_plant(std::string_view id) {
    return id == "minecraft:sunflower" || id == "minecraft:lilac" || id == "minecraft:tall_grass" || id == "minecraft:large_fern" ||
           id == "minecraft:rose_bush" || id == "minecraft:peony";
}

// Flower pot contents of 1.7-1.12 (block entity Item: String, or Int before 1.8; Data: Int) -> the
// 1.13 potted block (legacy ids: minecraft.wiki "Java Edition data values/Pre-flattening").
std::optional<std::string> potted(std::string item, i32 data) {
    if (item.starts_with("minecraft:")) item = item.substr(10);
    static constexpr std::array<std::string_view, 9> kFlowers{"poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip",
                                                              "orange_tulip", "white_tulip", "pink_tulip", "oxeye_daisy"};
    static constexpr std::array<std::string_view, 6> kSaplings{"oak", "spruce", "birch", "jungle", "acacia", "dark_oak"};
    std::string plant;
    if (item == "red_flower" && data >= 0 && data < 9) plant = std::string(kFlowers[static_cast<usize>(data)]);
    else if (item == "yellow_flower") plant = "dandelion";
    else if (item == "sapling" && data >= 0 && data < 6) plant = std::string(kSaplings[static_cast<usize>(data)]) + "_sapling";
    else if (item == "red_mushroom" || item == "brown_mushroom" || item == "cactus") plant = item;
    else if (item == "deadbush") plant = "dead_bush";
    else if (item == "tallgrass" && data == 2) plant = "fern";
    else if (item.empty() || item == "air") return "minecraft:flower_pot";
    else return std::nullopt;
    return "minecraft:potted_" + plant;
}

std::optional<io::nbt::NbtDocument> parse(const BlockEntity& be) {
    auto d = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    if (!d || !d->root().is(TagType::Compound)) return std::nullopt;
    return std::move(*d);
}

} // namespace

void fix_legacy_blocks(Structure& s, std::vector<std::string>& warnings) {
    BlockStateRegistry& reg = s.block_registry();
    auto at = [&](i32 x, i32 y, i32 z) -> const BlockState* {
        if (!s.contains({x, y, z})) return nullptr;
        return &reg.get(s.block({x, y, z}));
    };

    // Two-block states: doors keep facing/open in the lower half and hinge/powered in the upper.
    // Each state is classified once; sections without an upper door or plant half are skipped.
    enum : u8 { kOther = 0, kUpperDoor = 1, kUpperPlant = 2 };
    std::vector<u8> cls(reg.size(), kOther);
    for (u32 id = 0; id < reg.size(); ++id) {
        const BlockState& st = reg.get(id);
        if (prop(st, "half") != "upper") continue;
        if (st.id().ends_with("_door")) cls[id] = kUpperDoor;
        else if (is_tall_plant(st.id())) cls[id] = kUpperPlant;
    }
    for (u32 sy = 0; sy < s.sections_y(); ++sy)
        for (u32 sz = 0; sz < s.sections_z(); ++sz)
            for (u32 sx = 0; sx < s.sections_x(); ++sx) {
                const auto palette = s.section(sx, sy, sz).blocks().palette();
                if (std::none_of(palette.begin(), palette.end(), [&](u32 v) { return v < cls.size() && cls[v] != kOther; })) continue;
                for (i32 ly = 0; ly < 16; ++ly)
                    for (i32 lz = 0; lz < 16; ++lz)
                        for (i32 lx = 0; lx < 16; ++lx) {
                            const i32 x = static_cast<i32>(sx * 16) + lx, y = static_cast<i32>(sy * 16) + ly, z = static_cast<i32>(sz * 16) + lz;
                            if (y < 1 || !s.contains({x, y, z})) continue;
                            const StateId up_id = s.block({x, y, z});
                            if (up_id >= cls.size() || cls[up_id] == kOther) continue;
                            const BlockState up = reg.get(up_id); // copy: interning below may grow the registry
                            const BlockState* lowp = at(x, y - 1, z);
                            if (lowp == nullptr) continue;
                            const BlockState low = *lowp;
                            const std::string& id = up.id();
                            if (cls[up_id] == kUpperDoor && low.id() == id && prop(low, "half") == "lower") {
                                auto props = with(up.properties(), "facing", prop(low, "facing"));
                                props = with(std::move(props), "open", prop(low, "open"));
                                auto lprops = with(low.properties(), "hinge", prop(up, "hinge"));
                                lprops = with(std::move(lprops), "powered", prop(up, "powered"));
                                s.set_block({x, y, z}, reg.intern(BlockState(id, std::move(props))));
                                s.set_block({x, y - 1, z}, reg.intern(BlockState(id, std::move(lprops))));
                            } else if (cls[up_id] == kUpperPlant && is_tall_plant(low.id()) && prop(low, "half") == "lower" && low.id() != id) {
                                // The upper half's data value did not say which plant it was (1.12: data 8).
                                s.set_block({x, y, z}, reg.intern(BlockState(low.id(), up.properties())));
                            }
                        }
            }

    // States kept in block entities.
    usize unknown_pots = 0;
    std::vector<BlockEntity> kept;
    kept.reserve(s.block_entities.size());
    for (BlockEntity& be : s.block_entities) {
        const BlockState* b = at(be.pos.x, be.pos.y, be.pos.z);
        const auto doc = parse(be);
        if (b == nullptr || !doc) {
            kept.push_back(std::move(be));
            continue;
        }
        const NbtCompoundView c = doc->root().entries();
        const std::string& id = b->id();
        if (be.id == "minecraft:bed" && id.ends_with("_bed")) {
            // 1.12: block entity color (dye index); since 1.13 the colour is part of the block.
            if (const auto color = c.find("color").as_int(); color && *color >= 0 && *color < 16)
                s.set_block(be.pos, reg.intern(BlockState("minecraft:" + std::string(kDyeColors[static_cast<usize>(*color)]) + "_bed", b->properties())));
        } else if (be.id == "minecraft:flower_pot" && id == "minecraft:flower_pot") {
            std::string item;
            if (const auto str = c.find("Item").as_string()) item = std::string(*str);
            else if (const auto num = c.find("Item").as_int()) item = std::to_string(*num);
            const i32 data = c.find("Data").as_int().value_or(0);
            std::optional<std::string> pot;
            if (!item.empty() && std::all_of(item.begin(), item.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                static const std::unordered_map<std::string, std::string> kIds{{"0", ""}, {"6", "sapling"}, {"31", "tallgrass"},
                    {"32", "deadbush"}, {"37", "yellow_flower"}, {"38", "red_flower"}, {"39", "brown_mushroom"},
                    {"40", "red_mushroom"}, {"81", "cactus"}};
                const auto it = kIds.find(item);
                pot = it != kIds.end() ? potted(it->second, data) : std::nullopt;
            } else {
                pot = potted(item, data);
            }
            if (pot) s.set_block(be.pos, reg.intern(BlockState(*pot, {})));
            else ++unknown_pots;
            continue; // 1.13 has no flower pot block entity
        } else if (be.id == "minecraft:noteblock" && id == "minecraft:note_block") {
            if (const auto note = c.find("note").as_byte(); note && *note >= 0 && *note <= 24)
                s.set_block(be.pos, reg.intern(BlockState(id, with(b->properties(), "note", std::to_string(*note)))));
            continue; // 1.13 keeps the pitch in the block
        } else if (be.id == "minecraft:skull" && (id.ends_with("_skull") || id.ends_with("_head"))) {
            // SkullType 0-5; floor skulls (data 1) take the block entity's 16-step Rot.
            static constexpr std::array<std::string_view, 6> kTypes{"skeleton_skull", "wither_skeleton_skull", "zombie_head",
                                                                    "player_head", "creeper_head", "dragon_head"};
            const i32 type = c.find("SkullType").as_byte().value_or(0);
            if (type >= 0 && type < 6) {
                std::string name(kTypes[static_cast<usize>(type)]);
                const bool wall = id.find("_wall_") != std::string::npos;
                if (wall) name.insert(name.rfind('_'), "_wall");
                auto props = b->properties();
                if (!wall) props = with(std::move(props), "rotation", std::to_string(c.find("Rot").as_byte().value_or(0) & 15));
                s.set_block(be.pos, reg.intern(BlockState("minecraft:" + name, std::move(props))));
            }
        } else if (be.id == "minecraft:banner" && id.ends_with("_banner")) {
            // Base: dye damage value (0 = black ... 15 = white), the reverse of the colour order.
            if (const auto base = c.find("Base").as_int(); base && *base >= 0 && *base < 16) {
                const bool wall = id.ends_with("_wall_banner");
                s.set_block(be.pos, reg.intern(BlockState("minecraft:" + std::string(kDyeColors[static_cast<usize>(15 - *base)]) +
                                                              (wall ? "_wall_banner" : "_banner"),
                                                          b->properties())));
            }
        }
        kept.push_back(std::move(be));
    }
    s.block_entities = std::move(kept);
    if (unknown_pots > 0) warnings.push_back(std::to_string(unknown_pots) + " flower pot(s) with unknown contents were left empty");
}

} // namespace bridger::formats::detail
