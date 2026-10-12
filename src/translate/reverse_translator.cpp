// Bedrock -> Java translation of a structure (docs/adr/0008-translation-design.md,
// "Bedrock -> Java"). Same two stages as translator.cpp:
//
// 1. Palette stage: each distinct Bedrock state of a section becomes a Java state once
//    (exact reverse mapping, else the nearest state of the same block, reported as lossy,
//    else the fallback block, reported as unmapped); the palette is remapped.
// 2. Context stage, only at positions that need it:
//    - Bedrock layer 2: water becomes waterlogged=true (or the block itself where layer 1
//      is air); anything Java cannot hold is reported as a dropped secondary layer.
//    - Java stores what Bedrock derives from neighbours or block entities: fence, pane and
//      iron-bar connections and stair shapes (from neighbours), chest type (from the
//      Bedrock pairing), bed and banner colors, skull type and rotation, potted plants
//      (from the block entity).
// Neighbours are read from the immutable source plus the palette-stage table, never from
// partially written output, so the result does not depend on order or thread count.

#include <bridger/translate/block_entity_translator.hpp>
#include <bridger/translate/translator.hpp>

#include <bridger/io/nbt/nbt_view.hpp>

#include "tables.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>

namespace bridger::translate {
namespace {

constexpr JavaStateId kNone = UINT32_MAX;

enum class Rule : u8 { None, Fence, NetherFence, Pane, Stairs, Chest, Bed, Banner, Skull, Pot };

bool ends_with(std::string_view s, std::string_view suffix) { return s.ends_with(suffix); }

Rule rule_of(std::string_view name) {
    if (name == "minecraft:nether_brick_fence") return Rule::NetherFence;
    if (ends_with(name, "_fence")) return Rule::Fence;
    if (ends_with(name, "_pane") || name == "minecraft:iron_bars") return Rule::Pane;
    if (ends_with(name, "_stairs")) return Rule::Stairs;
    if (name == "minecraft:chest" || name == "minecraft:trapped_chest") return Rule::Chest;
    if (ends_with(name, "_bed")) return Rule::Bed;
    if (ends_with(name, "_banner")) return Rule::Banner;
    for (const tables::Skull& s : tables::kSkulls)
        if (name == s.floor || name == s.wall) return Rule::Skull;
    if (name == "minecraft:flower_pot") return Rule::Pot;
    return Rule::None;
}

// Java blocks that are always under water without a waterlogged property.
bool implies_water(std::string_view name) {
    return name == "minecraft:seagrass" || name == "minecraft:tall_seagrass" || name == "minecraft:kelp" ||
           name == "minecraft:kelp_plant" || name == "minecraft:bubble_column" || name == "minecraft:water";
}

// Whether a neighbour presents a full, sturdy face (what fences and panes attach to). Java
// decides this from collision shapes, which the mapping data does not carry; this is a
// name-based approximation (docs/adr/0008): blocks known to be partial are excluded.
bool looks_sturdy(std::string_view n) {
    if (n == "minecraft:air" || n == "minecraft:cave_air" || n == "minecraft:void_air") return false;
    if (n == "minecraft:snow_block" || n == "minecraft:mushroom_stem" || ends_with(n, "_mushroom_block")) return true;
    static constexpr std::array<std::string_view, 64> kPartial{
        "water", "lava", "_slab", "_stairs", "_fence", "_wall", "_pane", "iron_bars", "door", "sign", "banner",
        "torch", "lantern", "button", "pressure_plate", "rail", "carpet", "flower", "sapling", "short_grass",
        "tall_grass", "fern", "bush", "vine", "ladder", "_head", "_skull", "chain", "lever", "pot", "candle", "_bed",
        "chest", "cake", "cactus", "bamboo", "scaffolding", "snow", "anvil", "bell", "hopper", "cauldron", "brewing",
        "enchanting", "end_rod", "lightning_rod", "campfire", "conduit", "coral", "kelp", "seagrass", "sugar_cane",
        "mushroom", "dripleaf", "sea_pickle", "egg", "lichen", "sculk_vein", "cluster", "_bud", "dripstone", "web",
        "repeater", "comparator"};
    for (const std::string_view p : kPartial)
        if (n.find(p) != std::string_view::npos) return false;
    for (const std::string_view p : {"daylight_detector", "tripwire", "redstone_wire", "farmland", "dirt_path",
                                     "grindstone", "lectern", "stonecutter", "frogspawn", "petals", "roots", "spore"})
        if (n.find(p) != std::string_view::npos) return false;
    return true;
}

// Java: blocks a fence or pane never attaches to even though they are full (FenceBlock /
// IronBarsBlock isExceptionForConnection).
bool connection_exception(std::string_view n) {
    return ends_with(n, "_leaves") || n == "minecraft:barrier" || n == "minecraft:carved_pumpkin" ||
           n == "minecraft:jack_o_lantern" || n == "minecraft:melon" || n == "minecraft:pumpkin" ||
           ends_with(n, "shulker_box");
}

struct Dir {
    std::string_view name;
    i32 dx, dz;
};
constexpr std::array<Dir, 4> kDirs{{{"north", 0, -1}, {"east", 1, 0}, {"south", 0, 1}, {"west", -1, 0}}};

std::optional<Dir> dir_named(std::string_view n) {
    for (const Dir& d : kDirs)
        if (d.name == n) return d;
    return std::nullopt;
}
Dir opposite(const Dir& d) { return *dir_named(d.name == "north" ? "south" : d.name == "south" ? "north" : d.name == "east" ? "west" : "east"); }
Dir counter_clockwise(const Dir& d) {
    return *dir_named(d.name == "north" ? "west" : d.name == "west" ? "south" : d.name == "south" ? "east" : "north");
}
bool same_axis(const Dir& a, const Dir& b) { return (a.dx != 0) == (b.dx != 0); }

struct Rev {
    JavaStateId java = kNone; // kNone: unmapped (the fallback is used)
    bool lossy = false;
    Rule rule = Rule::None;
};

// What the neighbour rules need to know about a Java state (computed once per state).
struct JInfo {
    bool ready = false;
    Rule rule = Rule::None;
    i8 facing = -1; // index into kDirs
    i8 half = -1;   // 0 bottom, 1 top
    bool gate = false, wall = false, sturdy = false, exception = false;
};

i8 dir_index(std::string_view n) {
    for (usize i = 0; i < kDirs.size(); ++i)
        if (kDirs[i].name == n) return static_cast<i8>(i);
    return -1;
}

} // namespace

Result<Structure> translate_to_java(const Structure& bedrock, const MappingData& m, const TranslateOptions& options,
                                    ConversionReport& report) {
    if (bedrock.source().edition != Edition::Bedrock)
        return make_error(ErrorCode::InvalidArgument, "translate_to_java needs a Bedrock structure");
    const auto air_b = m.find_bedrock("minecraft:air", {});
    const auto air_j = m.find_java_state(BlockState("minecraft:air", {}));
    const auto marker_j = m.find_java_state(BlockState("minecraft:sponge", {}));
    if (!air_b || !air_j || !marker_j) return make_error(ErrorCode::Malformed, "mapping data lacks air or the marker block");
    const JavaStateId fallback = options.fallback == Fallback::Air ? *air_j : *marker_j;

    BRIDGER_TRY(Structure out, Structure::create(std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                                 bedrock.size(), {Edition::Java, m.java_data_version()}));
    out.offset = bedrock.offset;
    out.metadata = bedrock.metadata;
    BlockStateRegistry& registry = out.block_registry();

    // --- Palette stage table: Bedrock state -> Java state.
    std::vector<std::optional<Rev>> rev_cache(m.bedrock_state_count());
    auto rev = [&](u32 b) -> Rev {
        if (b >= rev_cache.size()) return Rev{};
        if (rev_cache[b]) return *rev_cache[b];
        Rev r;
        if (const auto j = m.java_of_bedrock(b)) r.java = *j;
        else if (const auto n = m.nearest_java_of_bedrock(b)) {
            r.java = *n;
            r.lossy = true;
        }
        if (r.java != kNone) r.rule = rule_of(m.java_block_name(r.java));
        rev_cache[b] = r;
        return r;
    };
    // Java state -> id in the output registry, and back.
    std::unordered_map<JavaStateId, StateId> out_id;
    std::vector<JavaStateId> java_of_out;
    auto intern = [&](JavaStateId j) -> StateId {
        if (const auto it = out_id.find(j); it != out_id.end()) return it->second;
        auto state = BlockState::parse(m.java_state_string(j));
        const StateId id = state ? registry.intern(*state) : kAirStateId;
        out_id.emplace(j, id);
        if (java_of_out.size() <= id) java_of_out.resize(static_cast<usize>(id) + 1, *air_j);
        java_of_out[id] = j;
        return id;
    };
    (void)intern(*air_j); // kAirStateId <-> Java air
    auto bedrock_desc = [&](u32 b) {
        std::string s(m.bedrock(b).name);
        if (!m.bedrock(b).properties.empty()) {
            s += '[';
            for (usize i = 0; i < m.bedrock(b).properties.size(); ++i) {
                const BedrockProperty& p = m.bedrock(b).properties[i];
                s += (i ? "," : "") + std::string(p.key) + "=" +
                     (p.type == NbtValueType::String ? std::string(p.str_value) : std::to_string(p.int_value));
            }
            s += ']';
        }
        return s;
    };

    std::vector<JInfo> jinfo(m.java_state_count());
    auto info = [&](JavaStateId j) -> const JInfo& {
        static const JInfo kEmpty{true};
        if (j >= jinfo.size()) return kEmpty;
        JInfo& i = jinfo[j];
        if (!i.ready) {
            const std::string_view n = m.java_block_name(j);
            i.ready = true;
            i.rule = rule_of(n);
            if (const auto f = m.property(j, "facing")) i.facing = dir_index(*f);
            if (const auto h = m.property(j, "half")) i.half = *h == "top" ? 1 : *h == "bottom" ? 0 : -1;
            i.gate = ends_with(n, "_fence_gate");
            i.wall = ends_with(n, "_wall");
            i.sturdy = looks_sturdy(n);
            i.exception = connection_exception(n);
        }
        return i;
    };
    // with_property() results, keyed by (state, property literal, value literal).
    struct PropKey {
        JavaStateId j;
        const char* k;
        const char* v;
        bool operator==(const PropKey&) const = default;
    };
    struct PropKeyHash {
        usize operator()(const PropKey& p) const noexcept {
            const usize h = std::hash<const char*>{}(p.k) * 31 + std::hash<const char*>{}(p.v);
            return h * 1000003u ^ p.j;
        }
    };
    std::unordered_map<PropKey, JavaStateId, PropKeyHash> prop_cache;

    // Java state at a source position after the palette stage (immutable snapshot): kNone outside.
    auto java_at = [&](BlockPos p) -> JavaStateId {
        u32 b = 0;
        if (bedrock.contains(p)) b = bedrock.block(p);
        else if (!options.bedrock_outside) return kNone;
        else if (const auto o = options.bedrock_outside(p)) b = *o;
        else return kNone;
        const Rev r = rev(b);
        return r.java == kNone ? fallback : r.java;
    };

    std::unordered_map<u64, const BlockEntity*> be_at;
    auto key = [](BlockPos p) {
        return (static_cast<u64>(static_cast<u32>(p.x) & 0x1FFFFF) << 42) | (static_cast<u64>(static_cast<u32>(p.y) & 0x1FFFFF) << 21) |
               (static_cast<u64>(static_cast<u32>(p.z) & 0x1FFFFF));
    };
    for (const BlockEntity& be : bedrock.block_entities) be_at.emplace(key(be.pos), &be);
    auto be_doc = [&](BlockPos p) -> std::optional<io::nbt::NbtDocument> {
        const auto it = be_at.find(key(p));
        if (it == be_at.end() || it->second->data.empty()) return std::nullopt;
        auto d = io::nbt::NbtDocument::parse(it->second->data.bytes, it->second->data.endian);
        if (!d) return std::nullopt;
        return std::move(*d);
    };
    auto renamed = [&](JavaStateId j, std::string_view new_name) -> std::optional<JavaStateId> {
        auto st = BlockState::parse(m.java_state_string(j));
        if (!st) return std::nullopt;
        return m.find_java_state(BlockState(std::string(new_name), st->properties()));
    };
    auto set_prop = [&](JavaStateId j, std::string_view k, std::string_view v) {
        // k and v are always string literals here (see the rotation rule), so their addresses identify them.
        const PropKey key{j, k.data(), v.data()};
        if (const auto it = prop_cache.find(key); it != prop_cache.end()) return it->second;
        const JavaStateId r = m.with_property(j, k, v).value_or(j);
        prop_cache.emplace(key, r);
        return r;
    };

    // --- Context rules (read java_at / block entities only).
    auto apply_rule = [&](JavaStateId j, Rule rule, BlockPos pos) -> JavaStateId {
        switch (rule) {
        case Rule::Fence:
        case Rule::NetherFence:
        case Rule::Pane:
            for (const Dir& d : kDirs) {
                const JavaStateId nj = java_at({pos.x + d.dx, pos.y, pos.z + d.dz});
                bool connect = false;
                if (nj != kNone) {
                    const JInfo& n = info(nj);
                    if (rule == Rule::Pane) {
                        connect = n.rule == Rule::Pane || n.wall || (n.sturdy && !n.exception);
                    } else {
                        connect = n.rule == rule;
                        if (!connect && n.gate && n.facing >= 0)
                            connect = !same_axis(kDirs[static_cast<usize>(n.facing)], d); // gate spans across the direction
                        if (!connect) connect = n.sturdy && !n.exception;
                    }
                }
                j = set_prop(j, d.name, connect ? "true" : "false");
            }
            return j;
        case Rule::Stairs: {
            // Java StairBlock.getStairsShape (behavior read from the 26.3 server; docs/adr/0008).
            const JInfo& self = info(j);
            if (self.facing < 0 || self.half < 0) return j;
            const Dir dir = kDirs[static_cast<usize>(self.facing)];
            const i8 half = self.half;
            auto stair = [&](BlockPos p) -> std::optional<std::pair<Dir, i8>> {
                const JavaStateId n = java_at(p);
                if (n == kNone) return std::nullopt;
                const JInfo& ni = info(n);
                if (ni.rule != Rule::Stairs || ni.facing < 0 || ni.half < 0) return std::nullopt;
                return std::pair{kDirs[static_cast<usize>(ni.facing)], ni.half};
            };
            auto can_take_shape = [&](const Dir& face) {
                const auto n = stair({pos.x + face.dx, pos.y, pos.z + face.dz});
                return !n || n->first.name != dir.name || n->second != half;
            };
            if (const auto front = stair({pos.x + dir.dx, pos.y, pos.z + dir.dz}); front && front->second == half) {
                const Dir d1 = front->first;
                if (!same_axis(d1, dir) && can_take_shape(opposite(d1)))
                    return set_prop(j, "shape", d1.name == counter_clockwise(dir).name ? "outer_left" : "outer_right");
            }
            const Dir back_dir = opposite(dir);
            if (const auto back = stair({pos.x + back_dir.dx, pos.y, pos.z + back_dir.dz}); back && back->second == half) {
                const Dir d2 = back->first;
                if (!same_axis(d2, dir) && can_take_shape(d2))
                    return set_prop(j, "shape", d2.name == counter_clockwise(dir).name ? "inner_left" : "inner_right");
            }
            return set_prop(j, "shape", "straight");
        }
        case Rule::Chest: {
            // Bedrock pairs chests through pairx/pairz (structure-local); Java stores left/right.
            const auto doc = be_doc(pos);
            const auto facing = m.property(j, "facing");
            if (!doc || !facing) return set_prop(j, "type", "single");
            const auto c = doc->root().entries();
            const auto px = c.find("pairx").as_int(), pz = c.find("pairz").as_int();
            if (!px || !pz) return set_prop(j, "type", "single");
            for (const std::string_view type : {"left", "right"})
                if (const auto p = tables::chest_partner(type, *facing, pos); p && p->x == *px && p->z == *pz)
                    return set_prop(j, "type", type);
            return set_prop(j, "type", "single");
        }
        case Rule::Bed: {
            const auto doc = be_doc(pos);
            const auto color = doc ? doc->root().entries().find("color").as_byte() : std::nullopt;
            if (color && *color >= 0 && *color < 16)
                if (const auto r = renamed(j, "minecraft:" + std::string(tables::kDyeColors[static_cast<usize>(*color)]) + "_bed"))
                    return *r;
            return j;
        }
        case Rule::Banner: {
            const auto doc = be_doc(pos);
            const auto base = doc ? doc->root().entries().find("Base").as_int() : std::nullopt;
            if (base && *base >= 0 && *base < 16) {
                const bool wall = ends_with(m.java_block_name(j), "_wall_banner");
                const std::string name = "minecraft:" + std::string(tables::kDyeColors[static_cast<usize>(15 - *base)]) +
                                         (wall ? "_wall_banner" : "_banner");
                if (const auto r = renamed(j, name)) return *r;
            }
            return j;
        }
        case Rule::Skull: {
            const auto doc = be_doc(pos);
            if (!doc) return j;
            const auto c = doc->root().entries();
            const std::string_view name = m.java_block_name(j);
            bool wall = false;
            for (const tables::Skull& s : tables::kSkulls) wall = wall || name == s.wall;
            if (const auto type = c.find("SkullType").as_byte(); type && *type >= 0 && *type < 7) {
                const tables::Skull& s = tables::kSkulls[static_cast<usize>(*type)];
                if (const auto r = renamed(j, wall ? s.wall : s.floor)) j = *r;
            }
            if (!wall)
                if (const auto rot = c.find("Rotation").as_float()) {
                    const long r = std::lround(static_cast<double>(*rot) / 22.5);
                    j = m.with_property(j, "rotation", std::to_string(((r % 16) + 16) % 16)).value_or(j); // not cached: the value is not a literal
                }
            return j;
        }
        case Rule::Pot: {
            const auto doc = be_doc(pos);
            if (!doc) return j;
            const auto plant = doc->root().entries().find("PlantBlock").entries().find("name").as_string();
            if (!plant || plant->empty()) return j;
            std::string p(*plant);
            if (p.starts_with("minecraft:")) p.erase(0, 10);
            if (p == "azalea") p = "azalea_bush";
            else if (p == "flowering_azalea") p = "flowering_azalea_bush";
            if (const auto r = m.find_java_state(BlockState("minecraft:potted_" + p, {}))) return *r;
            report.add(IssueKind::LossyBlock, "flower pot plant " + std::string(*plant) + " not converted", pos);
            return j;
        }
        case Rule::None: return j;
        }
        return j;
    };

    // --- Biomes: Bedrock numeric id -> Java biome name, once per id.
    std::unordered_map<u32, BiomeId> biome_cache;
    auto translate_biome = [&](u32 b, BlockPos where) -> BiomeId {
        if (const auto it = biome_cache.find(b); it != biome_cache.end()) return it->second;
        std::string name = "minecraft:plains";
        if (const auto j = m.java_biome_of_bedrock(static_cast<i32>(b))) name = std::string(*j);
        else report.add(IssueKind::UnmappedBiome, "bedrock biome " + std::to_string(b), where);
        const BiomeId id = out.biome_registry().intern(name);
        biome_cache.emplace(b, id);
        return id;
    };

    const Extent size = bedrock.size();
    for (u32 cy = 0; cy < bedrock.sections_y(); ++cy)
        for (u32 cz = 0; cz < bedrock.sections_z(); ++cz)
            for (u32 cx = 0; cx < bedrock.sections_x(); ++cx) {
                const Section& bs = bedrock.section(cx, cy, cz);
                Section& os = out.section(cx, cy, cz);

                PaletteLayer layer = bs.blocks();
                std::vector<u32> palette;
                palette.reserve(layer.palette().size());
                bool needs_positions = false;
                for (const u32 b : layer.palette()) {
                    const Rev r = rev(b);
                    palette.push_back(intern(r.java == kNone ? fallback : r.java));
                    needs_positions |= r.java == kNone || r.lossy || r.rule != Rule::None;
                }
                layer.remap(palette);
                os.blocks() = std::move(layer);

                const PaletteLayer* liquids = bs.secondary_layer();
                if (liquids)
                    for (const u32 b : liquids->palette()) needs_positions |= b != *air_b;

                if (needs_positions)
                    for (u32 y = 0; y < 16 && cy * 16 + y < size.y; ++y)
                        for (u32 z = 0; z < 16 && cz * 16 + z < size.z; ++z)
                            for (u32 x = 0; x < 16 && cx * 16 + x < size.x; ++x) {
                                const i32 ix = static_cast<i32>(x), iy = static_cast<i32>(y), iz = static_cast<i32>(z);
                                const BlockPos pos{static_cast<i32>(cx * 16 + x), static_cast<i32>(cy * 16 + y),
                                                   static_cast<i32>(cz * 16 + z)};
                                const u32 b = bs.block(ix, iy, iz);
                                const Rev r = rev(b);
                                const u32 l2 = liquids ? liquids->get(section_index(ix, iy, iz)) : *air_b;
                                if (r.java != kNone && !r.lossy && r.rule == Rule::None && l2 == *air_b) continue;

                                JavaStateId j = r.java == kNone ? fallback : r.java;
                                if (r.java == kNone) report.add(IssueKind::UnmappedBlock, bedrock_desc(b), pos);
                                else if (r.lossy) report.add(IssueKind::LossyBlock, "bedrock-only state " + bedrock_desc(b), pos);
                                if (r.rule != Rule::None) j = apply_rule(j, r.rule, pos);

                                if (l2 != *air_b) {
                                    const std::string_view liquid = m.bedrock(l2).name;
                                    const bool water = liquid == "minecraft:water" || liquid == "minecraft:flowing_water";
                                    const std::string_view name = m.java_block_name(j);
                                    if (name == "minecraft:air" || name == "minecraft:cave_air") {
                                        const Rev lr = rev(l2);
                                        if (lr.java != kNone) j = lr.java;
                                        else report.add(IssueKind::DroppedSecondaryLayer, bedrock_desc(l2), pos);
                                    } else if (water && implies_water(name)) {
                                        // already under water in Java
                                    } else if (const JavaStateId wl = water ? set_prop(j, "waterlogged", "true") : j; wl != j) {
                                        j = wl;
                                    } else {
                                        report.add(IssueKind::DroppedSecondaryLayer,
                                                   std::string(liquid) + " in " + std::string(name), pos);
                                    }
                                }
                                if (const StateId id = intern(j); id != os.block(ix, iy, iz)) os.set_block(ix, iy, iz, id);
                            }

                if (const PaletteLayer* bb = bs.biome_layer()) {
                    PaletteLayer biomes = *bb;
                    std::vector<u32> ids;
                    const BlockPos where{static_cast<i32>(cx * 16), static_cast<i32>(cy * 16), static_cast<i32>(cz * 16)};
                    for (const u32 b : biomes.palette()) ids.push_back(translate_biome(b, where));
                    biomes.remap(ids);
                    os.ensure_biomes(ids.empty() ? 0 : ids[0]) = std::move(biomes);
                }
            }

    // --- Block entities, against the final Java state at their position.
    for (const BlockEntity& be : bedrock.block_entities) {
        if (!out.contains(be.pos)) continue;
        const StateId id = out.block(be.pos);
        const JavaStateId j = id < java_of_out.size() ? java_of_out[id] : *air_j;
        if (auto translated = translate_block_entity_to_java(be, j, m, report)) out.block_entities.push_back(std::move(*translated));
    }
    for (const Entity& e : bedrock.entities)
        report.add(IssueKind::UnmappedEntity, e.id, BlockPos{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)});
    return out;
}

} // namespace bridger::translate
