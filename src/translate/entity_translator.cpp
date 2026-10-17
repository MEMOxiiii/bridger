// Java entities -> Bedrock. Item frames and paintings follow Chunker (HiveGamesOSS/Chunker, MIT, commit 50aaf6d):
// BedrockItemFrameEntityHandler / ItemFrameEntity.toBedrockBlockEntity / BedrockItemFrameBlockEntityHandler (frames
// as blocks), BedrockPaintingEntityHandler (Motif, Direction and the position Relocator), BedrockPaintingMotiveResolver,
// JavaItemFrameEntityHandler, JavaPaintingEntityHandler, JavaHangingEntityHandler and BedrockEntityHandler.
// docs/formats/bedrock-world.md, "Entities".

#include "entity_translator.hpp"

#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include "item_translator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

namespace bridger::translate::detail {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

struct Motive {
    std::string_view java;    // without "minecraft:"
    std::string_view bedrock; // Bedrock Motif value
    i32 width, height;        // blocks
    std::array<i32, 3> since; // first Bedrock version with it
};
constexpr std::array<i32, 3> kOld{0, 0, 0};
constexpr std::array<i32, 3> k1_21{1, 21, 0};
constexpr std::array<Motive, 51> kMotives{{
    {"kebab", "Kebab", 1, 1, kOld},           {"aztec", "Aztec", 1, 1, kOld},
    {"alban", "Alban", 1, 1, kOld},           {"aztec2", "Aztec2", 1, 1, kOld},
    {"bomb", "Bomb", 1, 1, kOld},             {"plant", "Plant", 1, 1, kOld},
    {"wasteland", "Wasteland", 1, 1, kOld},   {"meditative", "meditative", 1, 1, k1_21},
    {"wanderer", "Wanderer", 1, 2, kOld},     {"graham", "Graham", 1, 2, kOld},
    {"prairie_ride", "prairie_ride", 1, 2, k1_21}, {"sea", "Sea", 2, 1, kOld},
    {"creebet", "Creebet", 2, 1, kOld},       {"sunset", "Sunset", 2, 1, kOld},
    {"courbet", "Courbet", 2, 1, kOld},       {"pool", "Pool", 2, 1, kOld},
    {"earth", "Earth", 2, 2, kOld},           {"fire", "Fire", 2, 2, kOld},
    {"water", "Water", 2, 2, kOld},           {"wind", "Wind", 2, 2, kOld},
    {"match", "Match", 2, 2, kOld},           {"bust", "Bust", 2, 2, kOld},
    {"stage", "Stage", 2, 2, kOld},           {"void", "Void", 2, 2, kOld},
    {"skull_and_roses", "SkullAndRoses", 2, 2, kOld}, {"wither", "Wither", 2, 2, kOld},
    {"baroque", "baroque", 2, 2, k1_21},      {"humble", "humble", 2, 2, k1_21},
    {"bouquet", "bouquet", 3, 3, k1_21},      {"cavebird", "cavebird", 3, 3, k1_21},
    {"cotan", "cotan", 3, 3, k1_21},          {"endboss", "endboss", 3, 3, k1_21},
    {"fern", "fern", 3, 3, k1_21},            {"owlemons", "owlemons", 3, 3, k1_21},
    {"sunflowers", "sunflowers", 3, 3, k1_21}, {"tides", "tides", 3, 3, k1_21},
    {"dennis", "dennis", 3, 3, {1, 21, 93}},  {"backyard", "backyard", 3, 4, k1_21},
    {"pond", "pond", 3, 4, k1_21},            {"fighters", "Fighters", 4, 2, kOld},
    {"changing", "changing", 4, 2, k1_21},    {"finding", "finding", 4, 2, k1_21},
    {"lowmist", "lowmist", 4, 2, k1_21},      {"passage", "passage", 4, 2, k1_21},
    {"skeleton", "Skeleton", 4, 3, kOld},     {"donkey_kong", "DonkeyKong", 4, 3, kOld},
    {"pointer", "Pointer", 4, 4, kOld},       {"pigscene", "Pigscene", 4, 4, kOld},
    {"burning_skull", "BurningSkull", 4, 4, kOld}, {"orb", "orb", 4, 4, k1_21},
    {"unpacked", "unpacked", 4, 4, k1_21},
}};

// Hanging entities: the tile is "block_pos" (1.21.5+) or TileX/TileY/TileZ (JavaHangingEntityHandler).
std::optional<BlockPos> tile_of(NbtCompoundView c) {
    if (const auto bp = c.find("block_pos").as_int_array(); bp && bp->size() == 3) return BlockPos{(*bp)[0], (*bp)[1], (*bp)[2]};
    const auto x = c.find("TileX").as_int(), y = c.find("TileY").as_int(), z = c.find("TileZ").as_int();
    if (x && y && z) return BlockPos{*x, *y, *z};
    return std::nullopt;
}

i8 facing_of(NbtCompoundView c) {
    NbtView f = c.find("facing");
    if (!f) f = c.find("Facing");
    return f.as_byte().value_or(0);
}

// The column origin in world coordinates: the entity's absolute "Pos" minus its column-local position.
BlockPos origin_of(const Entity& e, NbtCompoundView c) {
    const auto p = c.find("Pos").items();
    if (p.size() != 3) return {0, 0, 0};
    return {static_cast<i32>(std::lround(p.at(0).as_double().value_or(0) - e.pos.x)),
            static_cast<i32>(std::lround(p.at(1).as_double().value_or(0) - e.pos.y)),
            static_cast<i32>(std::lround(p.at(2).as_double().value_or(0) - e.pos.z))};
}

void frame(const Entity& e, NbtCompoundView c, Structure& out, const MappingData& map, BedrockStateIndex air, BedrockStateIndex water,
           ConversionReport& report) {
    const bool glow = e.id == "minecraft:glow_item_frame";
    const BlockPos origin = origin_of(e, c);
    const auto tile = tile_of(c);
    const BlockPos where{static_cast<i32>(e.pos.x) + origin.x, static_cast<i32>(e.pos.y) + origin.y, static_cast<i32>(e.pos.z) + origin.z};
    if (!tile) {
        report.add(IssueKind::UnmappedEntity, e.id + " (no position)", where);
        return;
    }
    const BlockPos local{tile->x - origin.x, tile->y - origin.y, tile->z - origin.z};
    if (!out.contains(local)) {
        report.add(IssueKind::UnmappedEntity, e.id + " (outside its chunk)", where);
        return;
    }
    // Java Facing (DOWN, UP, NORTH, SOUTH, WEST, EAST = 0..5) is Bedrock facing_direction's order.
    const NbtView item = c.find("Item");
    const bool map_item = item.entries().find("id").as_string() == "minecraft:filled_map";
    const std::array<BedrockProperty, 3> props{{{"facing_direction", NbtValueType::Int, std::clamp<i32>(facing_of(c), 0, 5), {}},
                                                {"item_frame_map_bit", NbtValueType::Byte, map_item ? 1 : 0, {}},
                                                {"item_frame_photo_bit", NbtValueType::Byte, 0, {}}}};
    const auto state = map.find_bedrock(glow ? "minecraft:glow_frame" : "minecraft:frame", props);
    if (!state) {
        report.add(IssueKind::UnmappedEntity, e.id, where);
        return;
    }
    // A frame in water keeps the water in layer 2 (Chunker keeps the frame waterlogged).
    if (out.block(local) == water) {
        (void)out.section(static_cast<u32>(local.x) >> 4, static_cast<u32>(local.y) >> 4, static_cast<u32>(local.z) >> 4).ensure_secondary(air);
        out.set_secondary(local, water);
    }
    out.set_block(local, *state);

    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    BlockEntity be;
    be.pos = local;
    be.id = glow ? "GlowItemFrame" : "ItemFrame";
    w.write_string("id", be.id);
    if (item.is(TagType::Compound)) (void)write_item(w, "Item", item.entries(), map, report, *tile);
    // Java ItemRotation 0..7 (45 degree steps) -> Bedrock degrees (Float, 1.13+).
    w.write_float("ItemRotation", static_cast<f32>(c.find("ItemRotation").as_byte().value_or(0)) * 45.0f);
    w.end_compound();
    if (c.find("Invisible").as_byte().value_or(0) != 0 || c.find("Fixed").as_byte().value_or(0) != 0)
        report.add(IssueKind::LossyBlock, "item frame invisible/fixed flag (no Bedrock counterpart)", *tile);
    if (auto bytes = std::move(w).finish()) {
        be.data = RawNbt{std::move(*bytes), Endian::Little};
        out.block_entities.push_back(std::move(be));
    }
}

void painting(const Entity& e, NbtCompoundView c, Structure& out, ConversionReport& report) {
    const BlockPos origin = origin_of(e, c);
    const BlockPos where{static_cast<i32>(e.pos.x) + origin.x, static_cast<i32>(e.pos.y) + origin.y, static_cast<i32>(e.pos.z) + origin.z};
    // "variant" (1.19+) or "Motive"; a custom (data pack) painting has no Bedrock counterpart.
    std::optional<std::string_view> id = c.find("variant").as_string();
    if (!id) id = c.find("Motive").as_string();
    const std::string_view name = id ? (id->starts_with("minecraft:") ? id->substr(10) : *id) : std::string_view("kebab");
    const Motive* motive = nullptr;
    for (const Motive& m : kMotives)
        if (m.java == name) motive = &m;
    const auto tile = tile_of(c);
    if (!motive || !tile) {
        report.add(IssueKind::UnmappedEntity, "minecraft:painting " + std::string(id.value_or("(custom)")), where);
        return;
    }
    // Direction: 2D byte SOUTH, WEST, NORTH, EAST = 0..3 in both editions. Position as Chunker's Relocator writes it
    // (inverse): tile -= tile offset, position = tile - position offset.
    const i8 dir = static_cast<i8>(((facing_of(c) % 4) + 4) % 4);
    const bool even_w = motive->width % 2 == 0, even_h = motive->height % 2 == 0;
    const BlockPos tile_offset{dir == 0 && even_w ? -1 : 0, even_h ? -1 : 0, dir == 1 && even_w ? -1 : 0};
    const f64 oy = even_h ? 0 : -0.5, side = even_w ? 0 : -0.5;
    const std::array<f64, 3> pos_offset = dir == 2 ? std::array<f64, 3>{side, oy, -0.96875}
                                          : dir == 3 ? std::array<f64, 3>{-0.03125, oy, side}
                                          : dir == 0 ? std::array<f64, 3>{side, oy, -0.03125}
                                                     : std::array<f64, 3>{-0.96875, oy, side};
    const BlockPos t{tile->x - tile_offset.x, tile->y - tile_offset.y, tile->z - tile_offset.z};
    Entity actor;
    actor.id = "minecraft:painting";
    actor.pos = {t.x - pos_offset[0] - origin.x, t.y - pos_offset[1] - origin.y, t.z - pos_offset[2] - origin.z};
    NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_string("identifier", actor.id);
    w.begin_list("Motion", TagType::Float, 3);
    for (int i = 0; i < 3; ++i) w.write_float({}, 0.0f);
    w.end_list();
    const auto rot = c.find("Rotation").items();
    w.begin_list("Rotation", TagType::Float, 2);
    w.write_float({}, rot.size() == 2 ? rot.at(0).as_float().value_or(0) : 0.0f);
    w.write_float({}, rot.size() == 2 ? rot.at(1).as_float().value_or(0) : 0.0f);
    w.end_list();
    w.write_string("Motif", motive->bedrock); // "Motive" before 1.20.70: rewritten for older targets
    w.write_byte("Direction", dir);
    w.end_compound();
    if (auto bytes = std::move(w).finish()) {
        actor.data = RawNbt{std::move(*bytes), Endian::Little};
        out.entities.push_back(std::move(actor));
    }
}

const Motive* motive_by_bedrock(std::string_view bedrock) {
    for (const Motive& m : kMotives)
        if (m.bedrock == bedrock) return &m;
    return nullptr;
}

void write_rotation(NbtWriter& w, f32 yaw, f32 pitch) {
    w.begin_list("Rotation", TagType::Float, 2);
    w.write_float({}, yaw);
    w.write_float({}, pitch);
    w.end_list();
}

void write_tile(NbtWriter& w, BlockPos local) {
    const std::array<i32, 3> v{local.x, local.y, local.z};
    w.write_int_array("block_pos", v);
}

} // namespace

bool is_bedrock_frame(std::string_view name) noexcept { return name == "minecraft:frame" || name == "minecraft:glow_frame"; }

void frame_to_java(const BlockEntity& be, BedrockStateIndex frame_state, Structure& out, const MappingData& map, ConversionReport& report) {
    const BedrockState& st = map.bedrock(frame_state);
    i32 facing = 0;
    for (const BedrockProperty& p : st.properties)
        if (p.key == "facing_direction") facing = std::clamp(p.int_value, 0, 5);
    const auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    const NbtCompoundView c = doc ? doc->root().entries() : NbtView().entries();
    // Java entity position: the tile's centre moved 0.46875 towards the wall it hangs on; rotation from the facing
    // (yaw 0/90/180/270 for south/west/north/east, pitch -90/90 for up/down), as Java's ItemFrame#setDirection.
    static constexpr std::array<std::array<i32, 3>, 6> kVec{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    static constexpr std::array<f32, 6> kYaw{0, 0, 180, 0, 90, 270};
    static constexpr std::array<f32, 6> kPitch{90, -90, 0, 0, 0, 0};
    const auto& v = kVec[static_cast<usize>(facing)];
    Entity e;
    e.id = st.name == "minecraft:glow_frame" ? "minecraft:glow_item_frame" : "minecraft:item_frame";
    e.pos = {be.pos.x + 0.5 - 0.46875 * v[0], be.pos.y + 0.5 - 0.46875 * v[1], be.pos.z + 0.5 - 0.46875 * v[2]};
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.write_byte("Facing", static_cast<i8>(facing));
    write_tile(w, be.pos);
    if (const NbtView item = c.find("Item"); item.is(TagType::Compound)) (void)write_java_item(w, "Item", item.entries(), map, report, be.pos);
    // Bedrock ItemRotation: degrees as a Float (1.13+) or a Byte step (Chunker BedrockItemFrameBlockEntityHandler).
    i32 steps = c.find("ItemRotation").as_byte().value_or(0);
    if (const auto deg = c.find("ItemRotation").as_float()) steps = static_cast<i32>(std::lround(std::fmod(*deg + 360.0f, 360.0f) / 45.0f));
    w.write_byte("ItemRotation", static_cast<i8>(((steps % 8) + 8) % 8));
    w.write_float("ItemDropChance", 1.0f);
    write_rotation(w, kYaw[static_cast<usize>(facing)], kPitch[static_cast<usize>(facing)]);
    w.end_compound();
    if (auto bytes = std::move(w).finish()) {
        e.data = RawNbt{std::move(*bytes), Endian::Big};
        out.entities.push_back(std::move(e));
    }
}

void actor_to_java(const Entity& actor, Structure& out, ConversionReport& report) {
    const BlockPos where{static_cast<i32>(std::floor(actor.pos.x)), static_cast<i32>(std::floor(actor.pos.y)),
                         static_cast<i32>(std::floor(actor.pos.z))};
    if (actor.id != "minecraft:painting") {
        report.add(IssueKind::UnmappedEntity, actor.id, where);
        return;
    }
    const auto doc = io::nbt::NbtDocument::parse(actor.data.bytes, actor.data.endian);
    if (!doc) {
        report.add(IssueKind::UnmappedEntity, actor.id + " (unreadable data)", where);
        return;
    }
    const NbtCompoundView c = doc->root().entries();
    std::optional<std::string_view> name = c.find("Motif").as_string();
    if (!name) name = c.find("Motive").as_string();
    const Motive* motive = motive_by_bedrock(name.value_or("Kebab"));
    if (!motive) {
        report.add(IssueKind::UnmappedEntity, "minecraft:painting " + std::string(name.value_or("?")), where);
        return;
    }
    // Chunker's Relocator reading Bedrock (not inverse): position += position offset; tile = floor(position + tile
    // offset). Offsets as for writing (translate_entity).
    const i8 dir = static_cast<i8>(((c.find("Direction").as_byte().value_or(0) % 4) + 4) % 4);
    const bool even_w = motive->width % 2 == 0, even_h = motive->height % 2 == 0;
    const std::array<f64, 3> tile_offset{dir == 0 && even_w ? -1.0 : 0.0, even_h ? -1.0 : 0.0, dir == 1 && even_w ? -1.0 : 0.0};
    const f64 oy = even_h ? 0 : -0.5, side = even_w ? 0 : -0.5;
    const std::array<f64, 3> pos_offset = dir == 2 ? std::array<f64, 3>{side, oy, -0.96875}
                                          : dir == 3 ? std::array<f64, 3>{-0.03125, oy, side}
                                          : dir == 0 ? std::array<f64, 3>{side, oy, -0.03125}
                                                     : std::array<f64, 3>{-0.96875, oy, side};
    Entity e;
    e.id = "minecraft:painting";
    e.pos = {actor.pos.x + pos_offset[0], actor.pos.y + pos_offset[1], actor.pos.z + pos_offset[2]};
    const BlockPos tile{static_cast<i32>(std::floor(e.pos.x + tile_offset[0])), static_cast<i32>(std::floor(e.pos.y + tile_offset[1])),
                        static_cast<i32>(std::floor(e.pos.z + tile_offset[2]))};
    NbtWriter w(Endian::Big);
    w.begin_root_compound();
    w.write_string("variant", "minecraft:" + std::string(motive->java));
    w.write_byte("facing", dir);
    write_tile(w, tile);
    static constexpr std::array<f32, 4> kYaw{0, 90, 180, 270}; // south, west, north, east
    write_rotation(w, kYaw[static_cast<usize>(dir)], 0);
    w.end_compound();
    if (auto bytes = std::move(w).finish()) {
        e.data = RawNbt{std::move(*bytes), Endian::Big};
        out.entities.push_back(std::move(e));
    }
}

i32 java_painting_since(std::string_view java_variant) noexcept {
    const std::string_view n = java_variant.starts_with("minecraft:") ? java_variant.substr(10) : java_variant;
    if (n == "earth" || n == "fire" || n == "water" || n == "wind") return 3105; // 1.19
    if (n == "dennis") return 4438;                                              // 1.21.7
    for (const Motive& m : kMotives)
        if (m.java == n && m.since == k1_21) return 3953; // 1.21
    return 0;
}

std::optional<std::array<i32, 3>> painting_motive_since(std::string_view bedrock_motive) noexcept {
    for (const Motive& m : kMotives)
        if (m.bedrock == bedrock_motive) return m.since;
    return std::nullopt;
}

void translate_entity(const Entity& e, Structure& out, const MappingData& map, BedrockStateIndex air, BedrockStateIndex water,
                      ConversionReport& report) {
    const BlockPos where{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)};
    if (e.id != "minecraft:item_frame" && e.id != "minecraft:glow_item_frame" && e.id != "minecraft:painting") {
        report.add(IssueKind::UnmappedEntity, e.id, where);
        return;
    }
    const auto doc = io::nbt::NbtDocument::parse(e.data.bytes, e.data.endian);
    if (!doc) {
        report.add(IssueKind::UnmappedEntity, e.id + " (unreadable data)", where);
        return;
    }
    const NbtCompoundView c = doc->root().entries();
    if (e.id == "minecraft:painting") painting(e, c, out, report);
    else frame(e, c, out, map, air, water, report);
}

} // namespace bridger::translate::detail
