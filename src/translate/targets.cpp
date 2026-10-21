// Target game versions; data layouts in tools/mapgen/build_java_targets.py and targetgen.cpp, rules in
// docs/versions.md ("Target versions").

#include <bridger/core/game_versions.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/translate/targets.hpp>

#include "entity_translator.hpp"
#include "entity_types.hpp"
#include "bedrock_block_entity_downgrade.hpp"
#include "java_block_entity_downgrade.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace bridger::embedded {
extern const unsigned char java_targets_nbt[];
extern const std::size_t java_targets_nbt_size;
extern const unsigned char bedrock_targets_nbt[];
extern const std::size_t bedrock_targets_nbt_size;
} // namespace bridger::embedded

namespace bridger::translate {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

namespace {

std::string str(NbtView v) { return std::string(v.as_string().value_or("")); }

const std::string* find_prop(const std::vector<BlockProperty>& props, std::string_view name) {
    for (const BlockProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

// Every block's positions per state, for report counts and examples (only sections that hold one).
template <class Pred>
void count_states(const Structure& s, Pred affected, std::unordered_map<u32, std::pair<u64, BlockPos>>& out, bool secondary) {
    for (u32 sy = 0; sy < s.sections_y(); ++sy)
        for (u32 sz = 0; sz < s.sections_z(); ++sz)
            for (u32 sx = 0; sx < s.sections_x(); ++sx) {
                const Section& sec = s.section(sx, sy, sz);
                const PaletteLayer* layer = secondary ? sec.secondary_layer() : &sec.blocks();
                if (layer == nullptr) continue;
                const auto pal = layer->palette();
                if (std::none_of(pal.begin(), pal.end(), affected)) continue;
                for (i32 y = 0; y < 16; ++y)
                    for (i32 z = 0; z < 16; ++z)
                        for (i32 x = 0; x < 16; ++x) {
                            const BlockPos p{static_cast<i32>(sx * 16) + x, static_cast<i32>(sy * 16) + y, static_cast<i32>(sz * 16) + z};
                            if (!s.contains(p)) continue;
                            const u32 v = layer->get(section_index(x, y, z));
                            if (!affected(v)) continue;
                            auto [it, fresh] = out.try_emplace(v, 0, p);
                            ++it->second.first;
                        }
            }
}

template <class Fn>
void remap_layers(Structure& s, Fn map, bool secondary) {
    for (u32 sy = 0; sy < s.sections_y(); ++sy)
        for (u32 sz = 0; sz < s.sections_z(); ++sz)
            for (u32 sx = 0; sx < s.sections_x(); ++sx) {
                Section& sec = s.section(sx, sy, sz);
                PaletteLayer* layer = secondary ? sec.secondary_layer() : &sec.blocks();
                if (layer == nullptr) continue;
                std::vector<u32> next(layer->palette().begin(), layer->palette().end());
                bool changed = false;
                for (u32& v : next) {
                    const u32 n = map(v);
                    changed |= n != v;
                    v = n;
                }
                if (changed) layer->remap(next);
            }
}

// Biomes added since the oldest target, and what an older version gets instead (Bridger's choice: the
// closest biome that version has; reported). Introduced in: minecraft.wiki "Java Edition 1.19/1.20/1.21.4",
// "Bedrock Edition 1.19.0/1.20.0/1.21.50"; Bedrock ids from GeyserMC/mappings biomes.json.
struct NewBiome {
    std::string_view java;
    i32 bedrock;
    i32 java_data_version;              // first Java release with it
    std::array<i32, 3> bedrock_version; // first Bedrock release with it
    std::string_view java_fallback;
    i32 bedrock_fallback;
};
constexpr std::array<NewBiome, 4> kNewBiomes{{
    {"minecraft:deep_dark", 190, 3105, {1, 19, 0}, "minecraft:dripstone_caves", 188},
    {"minecraft:mangrove_swamp", 191, 3105, {1, 19, 0}, "minecraft:swamp", 6},
    {"minecraft:cherry_grove", 192, 3463, {1, 20, 0}, "minecraft:meadow", 186},
    {"minecraft:pale_garden", 193, 4189, {1, 21, 50}, "minecraft:dark_forest", 29},
}};

// Experimental features that added blocks before their release (minecraft.wiki "Experiments", checked 2026-10-16):
// releases from `first` (DataVersion) up to, not including, `official` have the blocks only with the feature on.
struct Experiment {
    std::string_view name; // data pack and feature flag id ("minecraft:" + name)
    i32 first;
    i32 official;
};
constexpr std::array<Experiment, 3> kExperiments{{
    {"update_1_20", 3218, 3463}, // 1.19.3 - 1.19.4; released in 1.20
    {"update_1_21", 3698, 3953}, // 1.20.3 - 1.20.6; released in 1.21
    {"winter_drop", 4080, 4189}, // 1.21.2 - 1.21.3; released in 1.21.4
}};

// Rewrites the biome layers of every section with `map` (old value -> new value), reporting changed ones.
template <class Map, class Name>
void remap_biomes(Structure& s, Map map, Name name, ConversionReport& report) {
    std::unordered_map<u32, std::pair<u64, BlockPos>> counts;
    for (u32 sy = 0; sy < s.sections_y(); ++sy)
        for (u32 sz = 0; sz < s.sections_z(); ++sz)
            for (u32 sx = 0; sx < s.sections_x(); ++sx) {
                Section& sec = s.section(sx, sy, sz);
                if (!sec.has_biomes()) continue;
                PaletteLayer& layer = *sec.biome_layer();
                std::vector<u32> next(layer.palette().begin(), layer.palette().end());
                bool changed = false;
                for (u32& v : next) {
                    const u32 n = map(v);
                    if (n == v) continue;
                    auto [it, fresh] = counts.try_emplace(v, 0, BlockPos{static_cast<i32>(sx * 16), static_cast<i32>(sy * 16), static_cast<i32>(sz * 16)});
                    ++it->second.first;
                    v = n;
                    changed = true;
                }
                if (changed) layer.remap(next);
            }
    for (const auto& [v, cp] : counts) report.add(IssueKind::UnmappedBiome, name(v), cp.second, cp.first);
}

} // namespace

// ---- Java substitutes ------------------------------------------------------------------------------------------
// Blocks newer than the target are written as the closest block it has (Bridger's choice, reviewed by hand; Java has
// no placeholder that would keep them). Rules are tried in order until the target has the result; properties are then
// matched as for any state. Everything substituted is reported.

std::string substitute_once(std::string_view id) {
    std::string n(id.substr(id.starts_with("minecraft:") ? 10 : 0));
    static const std::unordered_map<std::string_view, std::string_view> kExact{
        // plants and small things
        {"bush", "fern"}, {"firefly_bush", "fern"}, {"short_dry_grass", "dead_bush"}, {"tall_dry_grass", "dead_bush"},
        {"red_shrub", "dead_bush"}, {"cactus_flower", "air"}, {"leaf_litter", "air"}, {"wildflowers", "air"}, {"pink_petals", "air"},
        {"torchflower", "poppy"}, {"potted_torchflower", "potted_poppy"}, {"torchflower_crop", "air"}, {"pitcher_plant", "peony"},
        {"pitcher_crop", "air"}, {"open_eyeblossom", "poppy"}, {"closed_eyeblossom", "poppy"}, {"potted_open_eyeblossom", "potted_poppy"},
        {"potted_closed_eyeblossom", "potted_poppy"}, {"golden_dandelion", "dandelion"}, {"potted_golden_dandelion", "potted_dandelion"},
        {"shelf_mushroom", "brown_mushroom"}, {"orange_poplar_leaves", "oak_leaves"}, {"red_poplar_leaves", "oak_leaves"},
        {"yellow_poplar_leaves", "oak_leaves"}, {"pale_moss_block", "moss_block"}, {"pale_moss_carpet", "moss_carpet"},
        {"pale_hanging_moss", "air"}, {"creaking_heart", "birch_log"}, {"mangrove_propagule", "jungle_sapling"},
        {"potted_mangrove_propagule", "potted_jungle_sapling"}, {"mangrove_roots", "jungle_wood"}, {"muddy_mangrove_roots", "coarse_dirt"},
        {"sniffer_egg", "turtle_egg"}, {"frogspawn", "air"}, {"dried_ghast", "air"}, {"straw_bed", "hay_block"},
        {"ochre_froglight", "shroomlight"}, {"verdant_froglight", "shroomlight"}, {"pearlescent_froglight", "shroomlight"},
        // stone-like
        {"mud", "dirt"}, {"packed_mud", "coarse_dirt"}, {"mud_bricks", "bricks"}, {"mud_brick_slab", "brick_slab"},
        {"mud_brick_stairs", "brick_stairs"}, {"mud_brick_wall", "brick_wall"}, {"tuff_bricks", "stone_bricks"},
        {"tuff_brick_slab", "stone_brick_slab"}, {"tuff_brick_stairs", "stone_brick_stairs"}, {"tuff_brick_wall", "stone_brick_wall"},
        {"polished_tuff", "polished_andesite"}, {"polished_tuff_slab", "polished_andesite_slab"},
        {"polished_tuff_stairs", "polished_andesite_stairs"}, {"polished_tuff_wall", "andesite_wall"}, {"tuff_slab", "andesite_slab"},
        {"tuff_stairs", "andesite_stairs"}, {"tuff_wall", "andesite_wall"}, {"chiseled_tuff", "chiseled_stone_bricks"},
        {"chiseled_tuff_bricks", "chiseled_stone_bricks"}, {"resin_block", "honeycomb_block"}, {"resin_bricks", "orange_terracotta"},
        {"chiseled_resin_bricks", "chiseled_red_sandstone"}, {"resin_brick_slab", "brick_slab"}, {"resin_brick_stairs", "brick_stairs"},
        {"resin_brick_wall", "brick_wall"}, {"resin_clump", "air"}, {"reinforced_deepslate", "deepslate"},
        {"sulfur", "yellow_terracotta"}, {"potent_sulfur", "yellow_terracotta"}, {"polished_sulfur", "yellow_terracotta"},
        {"sulfur_bricks", "yellow_terracotta"}, {"chiseled_sulfur", "yellow_terracotta"}, {"sulfur_spike", "air"},
        {"cinnabar", "red_terracotta"}, {"polished_cinnabar", "red_terracotta"}, {"cinnabar_bricks", "red_terracotta"},
        {"chiseled_cinnabar", "red_terracotta"},
        // sculk
        {"sculk", "deepslate"}, {"sculk_catalyst", "deepslate"}, {"sculk_vein", "air"}, {"sculk_shrieker", "sculk_sensor"},
        {"calibrated_sculk_sensor", "sculk_sensor"},
        // blocks with a function
        {"chiseled_bookshelf", "bookshelf"}, {"decorated_pot", "flower_pot"}, {"piglin_head", "zombie_head"},
        {"piglin_wall_head", "zombie_wall_head"}, {"suspicious_sand", "sand"}, {"suspicious_gravel", "gravel"}, {"crafter", "dropper"},
        /* a spawner would spawn pigs: */ {"trial_spawner", "iron_bars"}, {"vault", "iron_block"}, {"heavy_core", "air"}, {"test_block", "air"}, {"test_instance_block", "air"},
        {"copper_torch", "torch"}, {"copper_wall_torch", "wall_torch"}, {"bamboo_block", "oak_log"}, {"stripped_bamboo_block", "stripped_oak_log"},
        {"bamboo_mosaic", "oak_planks"}, {"bamboo_mosaic_slab", "oak_slab"}, {"bamboo_mosaic_stairs", "oak_stairs"},
    };
    if (const auto it = kExact.find(n); it != kExact.end()) return "minecraft:" + std::string(it->second);

    // Sulfur and cinnabar slabs, stairs, walls (26.x).
    for (const auto& [stone, family] : {std::pair{"sulfur", "sandstone"}, std::pair{"cinnabar", "red_sandstone"}})
        for (const char* shape : {"_slab", "_stairs", "_wall"})
            if (n.find(stone) != std::string::npos && n.ends_with(shape)) return "minecraft:" + std::string(family) + shape;

    // Coloured wool and concrete slabs and stairs (26.x): the full block in that colour.
    for (const char* mat : {"_wool", "_concrete"})
        for (const char* shape : {"_slab", "_stairs"})
            if (n.ends_with(std::string(mat) + shape)) return "minecraft:" + n.substr(0, n.size() - std::string_view(shape).size());

    // Copper (1.20.3+, 1.21.9). Only cut copper keeps "waxed_" and the oxidation stage.
    {
        std::string base = n, prefix;
        if (base.starts_with("waxed_")) {
            prefix += "waxed_";
            base = base.substr(6);
        }
        for (const char* ox : {"exposed_", "weathered_", "oxidized_"})
            if (base.starts_with(ox)) {
                prefix += ox;
                base = base.substr(std::string_view(ox).size());
            }
        static const std::unordered_map<std::string_view, std::string_view> kCopper{
            {"chiseled_copper", "cut_copper"}, {"copper_grate", "cut_copper"}, {"copper_bulb", "redstone_lamp"}, {"copper_door", "iron_door"},
            {"copper_trapdoor", "iron_trapdoor"}, {"copper_bars", "iron_bars"}, {"copper_chain", "chain"}, {"copper_chest", "chest"},
            {"copper_golem_statue", "air"}, {"copper_lantern", "lantern"}, {"lightning_rod", "lightning_rod"}};
        if (const auto it = kCopper.find(base); it != kCopper.end() && (base != "lightning_rod" || !prefix.empty()))
            return "minecraft:" + (it->second == "cut_copper" ? prefix : std::string()) + std::string(it->second);
    }

    // Hanging signs and shelves (1.20, 26.x).
    if (n.ends_with("_hanging_sign")) return "minecraft:air";
    if (n.ends_with("_shelf")) return "minecraft:bookshelf";

    // Wood types the target does not have: the closest older wood.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kWood{
        {{"pale_oak", "birch"}, {"cherry", "birch"}, {"mangrove", "jungle"}, {"bamboo", "oak"}, {"poplar", "oak"}}};
    for (const auto& [wood, older] : kWood) {
        const auto at = n.find(wood);
        if (at == std::string::npos) continue;
        if (at > 0 && n[at - 1] != '_') continue;
        const usize end = at + wood.size();
        if (end < n.size() && n[end] != '_') continue;
        return "minecraft:" + n.substr(0, at) + std::string(older) + n.substr(end);
    }
    return {};
}

// ---- Java -------------------------------------------------------------------------------------------

struct JavaTargets::Impl {
    struct Def {
        u16 from = 0, to = 0; // version indices (oldest first), inclusive
        std::vector<std::pair<std::string, std::vector<std::string>>> props;
        std::vector<std::pair<std::string, std::string>> defaults;
    };
    std::vector<JavaTarget> newest_first;
    std::unordered_map<std::string, std::vector<Def>> blocks;
    std::unordered_map<std::string, std::vector<std::pair<u16, u16>>> items; // id -> version ranges
    usize pinned_index = 0; // in file order (oldest first)

    const Def* def(std::string_view id, usize version) const {
        const auto it = blocks.find(std::string(id));
        if (it == blocks.end()) return nullptr;
        for (const Def& d : it->second)
            if (version >= d.from && version <= d.to) return &d;
        return nullptr;
    }
};

JavaTargets::JavaTargets() : impl_(std::make_unique<Impl>()) {}
JavaTargets::JavaTargets(JavaTargets&&) noexcept = default;
JavaTargets& JavaTargets::operator=(JavaTargets&&) noexcept = default;
JavaTargets::~JavaTargets() = default;

Result<JavaTargets> JavaTargets::load(std::span<const u8> nbt) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Little));
    const NbtCompoundView root = doc.root().entries();
    JavaTargets t;
    Impl& m = *t.impl_;
    std::vector<JavaTarget> oldest_first;
    for (const NbtView v : root.find("versions").items()) {
        JavaTarget jt;
        jt.name = str(v.entries().find("name"));
        jt.data_version = v.entries().find("data_version").as_int().value_or(0);
        jt.index = oldest_first.size();
        oldest_first.push_back(std::move(jt));
    }
    if (oldest_first.empty()) return make_error(ErrorCode::Malformed, "java targets: no versions");
    oldest_first.back().pinned = true;
    m.pinned_index = oldest_first.size() - 1;
    for (const io::nbt::NbtEntry& b : root.find("blocks").entries()) {
        auto& defs = m.blocks[std::string(b.name)];
        for (const NbtView dv : b.value.items()) {
            const NbtCompoundView c = dv.entries();
            Impl::Def d;
            d.from = static_cast<u16>(c.find("from").as_int().value_or(0));
            d.to = static_cast<u16>(c.find("to").as_int().value_or(0));
            for (const io::nbt::NbtEntry& p : c.find("props").entries()) {
                std::vector<std::string> values;
                for (const NbtView v : p.value.items()) values.push_back(str(v));
                d.props.emplace_back(std::string(p.name), std::move(values));
            }
            for (const io::nbt::NbtEntry& p : c.find("default").entries()) d.defaults.emplace_back(std::string(p.name), str(p.value));
            defs.push_back(std::move(d));
        }
    }
    for (const io::nbt::NbtEntry& it : root.find("items").entries()) {
        auto& ranges = m.items[std::string(it.name)];
        if (const auto a = it.value.as_int_array())
            for (usize i = 0; i + 1 < a->size(); i += 2) ranges.emplace_back(static_cast<u16>((*a)[i]), static_cast<u16>((*a)[i + 1]));
    }
    m.newest_first.assign(oldest_first.rbegin(), oldest_first.rend());
    return t;
}

Result<const JavaTargets*> JavaTargets::embedded() {
    static std::once_flag once;
    static std::optional<Result<JavaTargets>> loaded;
    std::call_once(once, [] { loaded.emplace(load(std::span<const u8>(embedded::java_targets_nbt, embedded::java_targets_nbt_size))); });
    if (!*loaded) return tl::unexpected((*loaded).error());
    return &**loaded;
}

std::span<const JavaTarget> JavaTargets::targets() const noexcept { return impl_->newest_first; }
const JavaTarget& JavaTargets::pinned() const noexcept { return impl_->newest_first.front(); }

const JavaTarget* JavaTargets::find(std::string_view name) const noexcept {
    for (const JavaTarget& t : impl_->newest_first)
        if (t.name == name) return &t;
    return nullptr;
}

bool JavaTargets::has_block(std::string_view id, const JavaTarget& target) const { return impl_->def(id, target.index) != nullptr; }

std::string_view JavaTargets::experimental_feature(std::string_view id, const JavaTarget& target) const {
    const auto it = impl_->blocks.find(std::string(id));
    if (it == impl_->blocks.end() || it->second.empty()) return {};
    // The first version that lists the block (oldest first) tells which drop added it.
    usize first = it->second.front().from;
    for (const auto& d : it->second) first = std::min<usize>(first, d.from);
    const i32 first_dv = impl_->newest_first[impl_->newest_first.size() - 1 - first].data_version;
    for (const Experiment& e : kExperiments)
        if (first_dv >= e.first && first_dv < e.official && target.data_version < e.official) return e.name;
    return {};
}

bool JavaTargets::has_item(std::string_view id, const JavaTarget& target) const {
    if (target.pinned) return true;
    const auto it = impl_->items.find(std::string(id));
    if (it == impl_->items.end()) return impl_->items.empty(); // no item data: assume yes
    return std::any_of(it->second.begin(), it->second.end(), [&](const auto& r) { return target.index >= r.first && target.index <= r.second; });
}

JavaTargets::Fit JavaTargets::downgrade(const BlockState& state, const JavaTarget& target, BlockState& out) const {
    if (target.pinned) {
        out = state;
        return Fit::Exact;
    }
    std::string id = state.id();
    std::vector<BlockProperty> props = state.properties();
    const Impl::Def* d = impl_->def(id, target.index);
    // Blocks renamed since the target (the inverse of formats/java/java_upgrade.cpp).
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kRenamed{{
        {"minecraft:iron_chain", "minecraft:chain"},   // 1.21.9
        {"minecraft:short_grass", "minecraft:grass"},  // 1.20.3 (23w40a)
    }};
    if (d == nullptr)
        for (const auto& [now, before] : kRenamed)
            if (id == now)
                if (const Impl::Def* old = impl_->def(before, target.index)) {
                    id = std::string(before);
                    d = old;
                }
    bool approximate = false;
    // Blocks the target does not have: the closest block it has (rules above), at most a few steps.
    for (int step = 0; d == nullptr && step < 4; ++step) {
        const std::string next = substitute_once(id);
        if (next.empty()) break;
        id = next;
        approximate = true;
        if (id == "minecraft:air") {
            out = BlockState(id, {});
            return Fit::Approximate;
        }
        d = impl_->def(id, target.index);
    }
    if (d == nullptr) return Fit::Missing;
    // creaking_heart: creaking_heart_state (1.21.5+) was "active" (1.21.4) and "creaking" (1.21.2 - 1.21.3).
    if (const std::string* st = find_prop(props, "creaking_heart_state")) {
        const auto has = [&](std::string_view p) { return std::any_of(d->props.begin(), d->props.end(), [&](const auto& kv) { return kv.first == p; }); };
        std::string v = *st;
        if (has("active")) props.push_back({"active", v == "awake" ? "true" : "false"});
        else if (has("creaking")) props.push_back({"creaking", v == "awake" ? "active" : v == "dormant" ? "dormant" : "disabled"});
    }
    const Impl::Def* pinned_def = impl_->def(state.id(), impl_->pinned_index);
    std::vector<BlockProperty> next;
    for (const auto& [name, values] : d->props) {
        std::string value;
        for (const auto& [dn, dv] : d->defaults)
            if (dn == name) value = dv;
        if (const std::string* have = find_prop(props, name)) {
            if (std::find(values.begin(), values.end(), *have) != values.end()) value = *have;
            else approximate = true; // a value the target does not have: its default
        }
        next.push_back({name, value});
    }
    // Properties the target does not have are dropped; that loses something unless the value was the default.
    for (const BlockProperty& p : state.properties()) {
        if (p.name == "creaking_heart_state" && (find_prop(next, "active") || find_prop(next, "creaking"))) continue;
        if (find_prop(next, p.name) != nullptr) continue;
        std::string def_value;
        if (pinned_def != nullptr)
            for (const auto& [dn, dv] : pinned_def->defaults)
                if (dn == p.name) def_value = dv;
        if (p.value != def_value) approximate = true;
    }
    out = BlockState(std::move(id), std::move(next));
    return approximate ? Fit::Approximate : Fit::Exact;
}

Result<JavaRewrite> rewrite_for_java_target(Structure& java, const JavaTargets& targets, const JavaTarget& target, const BlockState& fallback,
                                            ConversionReport& report) {
    JavaRewrite result;
    if (target.pinned) return result;
    // Entities (item frames, paintings): Chunker JavaHangingEntityHandler / JavaPaintingEntityHandler /
    // JavaPaintingMotiveResolver: block_pos from 1.21.5 (TileX/TileY/TileZ before), "variant"/"facing" from 1.19
    // ("Motive"/"Facing" before); painting variants newer than the target are removed and reported.
    const i32 dv = target.data_version;
    // Mobs and objects: the type must exist in the target (its name there may be an older one); the NBT in its form.
    const detail::GameVersion3 java_v = detail::java_version3(target.name);
    std::erase_if(java.entities, [&](Entity& e) {
        if (e.id == "minecraft:painting" || e.id == "minecraft:item_frame" || e.id == "minecraft:glow_item_frame") return false;
        const BlockPos where{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)};
        if (const detail::EntityTypeRow* row = detail::entity_type_by_java(e.id)) {
            bool known = false;
            for (const detail::IdSince& k : row->java) known = known || (k.id == e.id && k.since <= java_v);
            const auto id = known ? std::optional<std::string_view>(e.id) : detail::java_entity_id(*row, java_v);
            if (!id) {
                report.add(IssueKind::UnmappedEntity, e.id + " (not in Java " + target.name + ")", where);
                return true;
            }
            e.id = std::string(*id);
        }
        std::vector<std::string> lossy, dropped;
        if (auto bytes = detail::downgrade_java_mob(e.id, e.data, targets, target, lossy, dropped)) {
            if (bytes->empty()) {
                report.add(IssueKind::UnmappedEntity, e.id + " (its item is not in Java " + target.name + ")", where);
                return true;
            }
            e.data.bytes = std::move(*bytes);
        }
        for (const std::string& l : lossy) report.add(IssueKind::LossyEntity, e.id + ": " + l + " (Java " + target.name + ")", where);
        for (const std::string& d : dropped) report.add(IssueKind::LossyEntity, e.id + ": " + d + " (not in Java " + target.name + ")", where);
        return false;
    });
    std::erase_if(java.entities, [&](Entity& e) {
        if (e.id != "minecraft:painting" && e.id != "minecraft:item_frame" && e.id != "minecraft:glow_item_frame") return false;
        const auto doc = io::nbt::NbtDocument::parse(e.data.bytes, e.data.endian);
        if (!doc) return false;
        const io::nbt::NbtCompoundView c = doc->root().entries();
        const BlockPos where{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)};
        if (const auto variant = c.find("variant").as_string(); variant && detail::java_painting_since(*variant) > dv) {
            report.add(IssueKind::UnmappedEntity, "minecraft:painting " + std::string(*variant) + " (not in Java " + target.name + ")", where);
            return true;
        }
        io::nbt::NbtWriter w(e.data.endian);
        w.begin_root_compound();
        for (const io::nbt::NbtEntry& en : c) {
            if (en.name == "block_pos" && dv < 4325) { // 1.21.5
                const auto bp = en.value.as_int_array();
                if (!bp || bp->size() != 3) continue;
                w.write_int("TileX", (*bp)[0]);
                w.write_int("TileY", (*bp)[1]);
                w.write_int("TileZ", (*bp)[2]);
            } else if (en.name == "variant" && dv < 3105) { // 1.19
                w.write_view("Motive", en.value);
            } else if (en.name == "facing" && dv < 3105) {
                w.write_view("Facing", en.value);
            } else {
                w.write_view(en.name, en.value);
            }
        }
        w.end_compound();
        if (auto bytes = std::move(w).finish()) e.data.bytes = std::move(*bytes);
        return false;
    });
    BlockStateRegistry& reg = java.block_registry();
    const usize n = reg.size();
    std::vector<u32> map(n);
    std::vector<JavaTargets::Fit> fit(n, JavaTargets::Fit::Exact);
    std::vector<std::string> subject(n);
    const StateId fallback_id = reg.intern(fallback);
    for (StateId id = 0; id < n; ++id) {
        const BlockState st = reg.get(id); // copy: interning may grow the registry
        BlockState out;
        fit[id] = targets.downgrade(st, target, out);
        switch (fit[id]) {
        case JavaTargets::Fit::Missing:
            map[id] = fallback_id;
            subject[id] = std::format("{} (not in Java {})", st.to_string(), target.name);
            break;
        case JavaTargets::Fit::Approximate:
            map[id] = reg.intern(out);
            subject[id] = std::format("{} written as {} (Java {})", st.to_string(), out.to_string(), target.name);
            break;
        case JavaTargets::Fit::Exact: map[id] = reg.intern(out); break;
        }
    }
    // The block each block entity stood on, before rewriting (a substituted block may need another block entity).
    std::vector<std::string> be_block(java.block_entities.size());
    for (usize i = 0; i < java.block_entities.size(); ++i)
        if (java.contains(java.block_entities[i].pos)) be_block[i] = reg.get(java.block(java.block_entities[i].pos)).id();
    std::unordered_map<u32, std::pair<u64, BlockPos>> counts;
    count_states(java, [&](u32 v) { return v < n && fit[v] != JavaTargets::Fit::Exact; }, counts, false);
    for (const auto& [id, cp] : counts) {
        report.add(fit[id] == JavaTargets::Fit::Missing ? IssueKind::UnmappedBlock : IssueKind::LossyBlock, subject[id], cp.second, cp.first);
        result.changed_blocks += cp.first;
    }
    // Experimental blocks of the target in use.
    std::vector<u8> used(n, 0);
    for (u32 sy = 0; sy < java.sections_y(); ++sy)
        for (u32 sz = 0; sz < java.sections_z(); ++sz)
            for (u32 sx = 0; sx < java.sections_x(); ++sx)
                for (const u32 v : java.section(sx, sy, sz).blocks().palette())
                    if (v < n) used[v] = 1;
    for (StateId id = 0; id < n; ++id) {
        if (!used[id] || fit[id] == JavaTargets::Fit::Missing) continue;
        const std::string_view f = targets.experimental_feature(reg.get(map[id]).id(), target);
        if (!f.empty() && std::find(result.features.begin(), result.features.end(), f) == result.features.end()) result.features.emplace_back(f);
    }
    remap_layers(java, [&](u32 v) { return v < n ? map[v] : v; }, false);

    // Block entities: removed with their block, or rewritten into the target's format.
    std::vector<BlockEntity> kept;
    kept.reserve(java.block_entities.size());
    // Block entity ids of substituted blocks: the substitute's own type, or none (then it goes).
    static const std::unordered_map<std::string_view, std::string_view> kSubstituteEntity{
        {"minecraft:crafter", "minecraft:dropper"},         {"minecraft:copper_chest", "minecraft:chest"},
        {"minecraft:chest", "minecraft:chest"},             {"minecraft:sculk_shrieker", "minecraft:sculk_sensor"},
        {"minecraft:calibrated_sculk_sensor", "minecraft:sculk_sensor"}, {"minecraft:skull", "minecraft:skull"}};
    for (usize bi = 0; bi < java.block_entities.size(); ++bi) {
        BlockEntity& be = java.block_entities[bi];
        // Blocks are already rewritten: a block entity on the fallback block lost its block (the target lacks it).
        if (!java.contains(be.pos) || java.block(be.pos) == fallback_id || reg.get(java.block(be.pos)).id() == "minecraft:air") continue;
        if (reg.get(java.block(be.pos)).id() != be_block[bi] && !be_block[bi].empty()) {
            const auto it = kSubstituteEntity.find(be.id);
            if (it == kSubstituteEntity.end()) continue; // e.g. a decorated pot that became a flower pot
            be.id = std::string(it->second);
        }
        std::vector<std::string> lossy, dropped;
        if (auto bytes = detail::downgrade_java_block_entity(be, targets, target, lossy, dropped)) be.data.bytes = std::move(*bytes);
        for (const std::string& l : lossy) report.add(IssueKind::LossyBlock, std::format("{}: {} (Java {})", be.id, l, target.name), be.pos);
        for (const std::string& d : dropped)
            report.add(IssueKind::UnmappedBlockEntity, std::format("{} (not in Java {})", d, target.name), be.pos);
        kept.push_back(std::move(be));
    }
    java.block_entities = std::move(kept);

    // Biomes the target does not have yet.
    BiomeRegistry& biomes = java.biome_registry();
    std::unordered_map<u32, u32> biome_map;
    for (u32 id = 0; id < biomes.size(); ++id)
        for (const NewBiome& nb : kNewBiomes)
            if (biomes.name(id) == nb.java && target.data_version < nb.java_data_version) biome_map[id] = biomes.intern(std::string(nb.java_fallback));
    if (!biome_map.empty())
        remap_biomes(
            java, [&](u32 v) { const auto it = biome_map.find(v); return it == biome_map.end() ? v : it->second; },
            [&](u32 v) { return std::format("{} (not in Java {}): written as {}", biomes.name(v), target.name, biomes.name(biome_map.at(v))); },
            report);
    return result;
}

// ---- Bedrock ----------------------------------------------------------------------------------------

struct BedrockTargets::Impl {
    struct Palette {
        i32 block_version = 0;
        std::vector<u8> missing;
        std::unordered_map<u32, u32> nearest;
        std::unordered_map<u32, i32> changed;
    };
    std::vector<BedrockTarget> newest_first;
    std::vector<Palette> palettes;
    usize pinned_states = 0;

    mutable std::mutex cache_lock;
    mutable std::unordered_map<usize, std::vector<std::vector<u8>>> nbt_cache; // target -> per pinned state

    struct SpellingView {
        std::vector<u8> root; // standalone root {name, states} of a target's spelling
    };
    std::vector<SpellingView> spelling_roots;
    struct ItemSchema {
        std::array<i32, 4> to{};                                                   // version it upgrades to
        std::unordered_map<std::string, std::string> renamed_back;                 // new id -> old id
        std::unordered_map<std::string, std::pair<std::string, i16>> remapped_back; // new id -> (old id, data)
    };
    std::vector<ItemSchema> item_schemas; // oldest first
};

BedrockTargets::BedrockTargets() : impl_(std::make_unique<Impl>()) {}
BedrockTargets::BedrockTargets(BedrockTargets&&) noexcept = default;
BedrockTargets& BedrockTargets::operator=(BedrockTargets&&) noexcept = default;
BedrockTargets::~BedrockTargets() = default;

Result<BedrockTargets> BedrockTargets::load(std::span<const u8> nbt) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Little));
    const NbtCompoundView root = doc.root().entries();
    BedrockTargets t;
    Impl& m = *t.impl_;
    m.pinned_states = static_cast<usize>(root.find("pinned_states").as_int().value_or(0));
    for (const NbtView sv : root.find("spellings").items()) {
        io::nbt::NbtWriter w(Endian::Little);
        w.begin_root_compound();
        w.write_view("name", sv.entries().find("name"));
        w.write_view("states", sv.entries().find("states"));
        w.end_compound();
        BRIDGER_TRY(std::vector<u8> bytes, std::move(w).finish());
        m.spelling_roots.push_back({std::move(bytes)});
    }
    for (const NbtView pv : root.find("palettes").items()) {
        const NbtCompoundView c = pv.entries();
        Impl::Palette p;
        p.block_version = c.find("block_version").as_int().value_or(0);
        if (const auto mb = c.find("missing").as_byte_array()) p.missing.assign(mb->begin(), mb->end());
        if (const auto nr = c.find("nearest").as_int_array())
            for (usize i = 0; i + 1 < nr->size(); i += 2) p.nearest.emplace(static_cast<u32>((*nr)[i]), static_cast<u32>((*nr)[i + 1]));
        if (const auto ch = c.find("changed").as_int_array())
            for (usize i = 0; i + 1 < ch->size(); i += 2) p.changed.emplace(static_cast<u32>((*ch)[i]), (*ch)[i + 1]);
        m.palettes.push_back(std::move(p));
    }
    for (const NbtView sv : root.find("item_schemas").items()) {
        const NbtCompoundView c = sv.entries();
        Impl::ItemSchema is;
        if (const auto to = c.find("to").as_int_array())
            for (usize i = 0; i < 4 && i < to->size(); ++i) is.to[i] = (*to)[i];
        for (const io::nbt::NbtEntry& e : c.find("renamed").entries()) is.renamed_back.try_emplace(str(e.value), std::string(e.name));
        for (const io::nbt::NbtEntry& e : c.find("remapped").entries())
            for (const io::nbt::NbtEntry& meta : e.value.entries()) {
                i16 data = 0;
                for (const char ch : meta.name) data = static_cast<i16>(data * 10 + (ch - '0'));
                is.remapped_back.try_emplace(str(meta.value), std::string(e.name), data);
            }
        m.item_schemas.push_back(std::move(is));
    }
    std::vector<BedrockTarget> oldest_first;
    for (const NbtView tv : root.find("targets").items()) {
        const NbtCompoundView c = tv.entries();
        BedrockTarget bt;
        if (const auto v = c.find("version").as_int_array())
            for (usize i = 0; i < 5 && i < v->size(); ++i) bt.version[i] = (*v)[i];
        bt.name = bedrock_version_name(bt.version);
        bt.protocol = c.find("protocol").as_int().value_or(0);
        bt.storage_version = c.find("storage_version").as_int().value_or(10);
        bt.chunk_version = c.find("chunk_version").as_int().value_or(42);
        bt.palette = static_cast<usize>(c.find("palette").as_int().value_or(0));
        if (bt.palette >= m.palettes.size()) return make_error(ErrorCode::Malformed, "bedrock targets: palette index out of range");
        bt.block_version = m.palettes[bt.palette].block_version;
        oldest_first.push_back(std::move(bt));
    }
    if (oldest_first.empty()) return make_error(ErrorCode::Malformed, "bedrock targets: no targets");
    oldest_first.back().pinned = true;
    m.newest_first.assign(oldest_first.rbegin(), oldest_first.rend());
    return t;
}

Result<const BedrockTargets*> BedrockTargets::embedded() {
    static std::once_flag once;
    static std::optional<Result<BedrockTargets>> loaded;
    std::call_once(once, [] { loaded.emplace(load(std::span<const u8>(embedded::bedrock_targets_nbt, embedded::bedrock_targets_nbt_size))); });
    if (!*loaded) return tl::unexpected((*loaded).error());
    return &**loaded;
}

std::span<const BedrockTarget> BedrockTargets::targets() const noexcept { return impl_->newest_first; }
const BedrockTarget& BedrockTargets::pinned() const noexcept { return impl_->newest_first.front(); }

const BedrockTarget* BedrockTargets::find(std::string_view name) const noexcept {
    for (const BedrockTarget& t : impl_->newest_first)
        if (t.name == name) return &t;
    return nullptr;
}

BedrockTargets::Written BedrockTargets::resolve(const BedrockTarget& target, BedrockStateIndex state) const {
    Written w;
    if (target.pinned) return w;
    const Impl::Palette& p = impl_->palettes[target.palette];
    if (state / 8 < p.missing.size() && ((p.missing[state / 8] >> (state % 8)) & 1)) {
        w.kind = Written::Kind::Missing;
        return w;
    }
    if (const auto it = p.nearest.find(state); it != p.nearest.end()) {
        w.kind = Written::Kind::Nearest;
        w.nearest = it->second;
        return w;
    }
    if (const auto it = p.changed.find(state); it != p.changed.end()) {
        w.kind = Written::Kind::Respelled;
        w.spelling = it->second;
    }
    return w;
}

std::pair<std::string, i16> BedrockTargets::item_for(const BedrockTarget& target, std::string_view name, i16 damage) const {
    std::pair<std::string, i16> out{std::string(name), damage};
    if (target.pinned) return out;
    const std::array<i32, 4> tv{target.version[0], target.version[1], target.version[2], 9999};
    for (auto it = impl_->item_schemas.rbegin(); it != impl_->item_schemas.rend(); ++it) {
        if (!(it->to > tv)) break; // older schemas are already part of the target
        if (const auto r = it->renamed_back.find(out.first); r != it->renamed_back.end()) out.first = r->second;
        else if (const auto m = it->remapped_back.find(out.first); m != it->remapped_back.end()) out = m->second;
    }
    return out;
}

std::span<const u8> BedrockTargets::state_nbt(const BedrockTarget& target, BedrockStateIndex state, const MappingData& mapping) const {
    const usize key = static_cast<usize>(&target - impl_->newest_first.data());
    std::lock_guard g(impl_->cache_lock);
    auto& cache = impl_->nbt_cache[key];
    if (cache.empty()) cache.resize(mapping.bedrock_state_count());
    if (state >= cache.size()) return {};
    std::vector<u8>& slot = cache[state];
    if (!slot.empty()) return slot;
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound();
    const Written how = resolve(target, state);
    if (how.kind == Written::Kind::Respelled && how.spelling >= 0 && static_cast<usize>(how.spelling) < impl_->spelling_roots.size()) {
        const auto doc = io::nbt::NbtDocument::parse(impl_->spelling_roots[static_cast<usize>(how.spelling)].root, Endian::Little);
        if (doc) {
            w.write_view("name", doc->root().entries().find("name"));
            w.write_view("states", doc->root().entries().find("states"));
        }
    } else {
        const BedrockState& st = mapping.bedrock(state);
        w.write_string("name", st.name);
        w.begin_compound("states");
        for (const BedrockProperty& p : st.properties) {
            if (p.type == NbtValueType::Byte) w.write_byte(p.key, static_cast<i8>(p.int_value));
            else if (p.type == NbtValueType::Int) w.write_int(p.key, p.int_value);
            else w.write_string(p.key, p.str_value);
        }
        w.end_compound();
    }
    // A state the target does not have keeps its own spelling and the pinned block version: the old game keeps it as an
    // unknown block, and newer ones read it as what it was.
    w.write_int("version", how.kind == Written::Kind::Missing ? static_cast<i32>(mapping.bedrock_block_version()) : target.block_version);
    w.end_compound();
    slot = std::move(w).finish().value_or(std::vector<u8>{});
    return slot;
}

Result<u64> rewrite_for_bedrock_target(Structure& bedrock, const BedrockTargets& targets, const BedrockTarget& target,
                                       const MappingData& mapping, ConversionReport& report) {
    if (target.pinned) return u64{0};
    // Paintings: "Motive" until 1.20.60, "Motif" after (Chunker BedrockPaintingEntityHandler); motives newer than the
    // target are removed and reported.
    const std::array<i32, 3> target_v{target.version[0], target.version[1], target.version[2]};
    std::erase_if(bedrock.entities, [&](Entity& e) {
        if (e.id == "minecraft:painting") return false;
        const detail::EntityTypeRow* row = detail::entity_type_by_bedrock(e.id);
        if (row == nullptr) return false; // unknown here: left as it is (a copied actor of an add-on, for example)
        const BlockPos where{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)};
        // An identifier the target already knows stays (old "villager" actors are not "villager_v2" ones).
        for (const detail::IdSince& known : row->bedrock)
            if (known.id == e.id && known.since <= target_v) return false;
        const auto id = detail::bedrock_entity_id(*row, target_v);
        if (!id) {
            report.add(IssueKind::UnmappedEntity, e.id + " (not in Bedrock " + target.name + ")", where);
            return true;
        }
        if (*id != e.id) {
            const auto doc = io::nbt::NbtDocument::parse(e.data.bytes, e.data.endian);
            if (!doc) return false;
            io::nbt::NbtWriter w(e.data.endian);
            w.begin_root_compound();
            for (const io::nbt::NbtEntry& en : doc->root().entries()) {
                if (en.name == "identifier") w.write_string("identifier", *id);
                else w.write_view(en.name, en.value);
            }
            w.end_compound();
            if (auto bytes = std::move(w).finish()) e.data.bytes = std::move(*bytes);
            e.id = std::string(*id);
        }
        return false;
    });
    std::erase_if(bedrock.entities, [&](Entity& e) {
        if (e.id != "minecraft:painting") return false;
        const auto doc = io::nbt::NbtDocument::parse(e.data.bytes, e.data.endian);
        if (!doc) return false;
        const std::string motif(doc->root().entries().find("Motif").as_string().value_or(""));
        const BlockPos where{static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)};
        if (const auto since = detail::painting_motive_since(motif); since && target_v < *since) {
            report.add(IssueKind::UnmappedEntity, "minecraft:painting " + motif + " (not in Bedrock " + target.name + ")", where);
            return true;
        }
        if (target_v < std::array<i32, 3>{1, 20, 70}) {
            io::nbt::NbtWriter w(e.data.endian);
            w.begin_root_compound();
            for (const io::nbt::NbtEntry& en : doc->root().entries()) w.write_view(en.name == "Motif" ? "Motive" : en.name, en.value);
            w.end_compound();
            if (auto bytes = std::move(w).finish()) e.data.bytes = std::move(*bytes);
        }
        return false;
    });
    const usize n = mapping.bedrock_state_count();
    auto state_text = [&](BedrockStateIndex i) {
        const BedrockState& st = mapping.bedrock(i);
        std::string s(st.name);
        for (usize k = 0; k < st.properties.size(); ++k) {
            const BedrockProperty& p = st.properties[k];
            s += (k == 0 ? "[" : ",") + std::string(p.key) + "=" +
                 (p.type == NbtValueType::String ? std::string(p.str_value) : std::to_string(p.int_value));
        }
        return st.properties.empty() ? s : s + "]";
    };
    std::vector<BedrockTargets::Written> how(n);
    std::vector<u8> derived_only(n, 0); // Nearest that differs only in states older games compute themselves
    for (usize i = 0; i < n; ++i) {
        how[i] = targets.resolve(target, static_cast<BedrockStateIndex>(i));
        if (how[i].kind == BedrockTargets::Written::Kind::Nearest) {
            // 26.50 started saving fence connections and the stairs' corner shape (BedrockBlockUpgradeSchema
            // 0351_1.26.40_to_1.26.50 addedProperties); older versions derive them from the neighbours, so
            // dropping them loses nothing and is not reported.
            const BedrockState& sa = mapping.bedrock(static_cast<BedrockStateIndex>(i));
            const BedrockState& sb = mapping.bedrock(how[i].nearest);
            bool only = true;
            for (const BedrockProperty& p : sa.properties) {
                const auto same = std::find_if(sb.properties.begin(), sb.properties.end(), [&](const BedrockProperty& q) {
                    return q.key == p.key && q.type == p.type && q.int_value == p.int_value && q.str_value == p.str_value;
                });
                if (same != sb.properties.end()) continue;
                if (!(p.key.starts_with("minecraft:connection_") || p.key == "minecraft:corner")) only = false;
            }
            derived_only[i] = only ? 1 : 0;
        }
    }
    auto affected = [&](u32 v) {
        return v < n && (how[v].kind == BedrockTargets::Written::Kind::Nearest || how[v].kind == BedrockTargets::Written::Kind::Missing);
    };
    u64 changed = 0;
    for (const bool secondary : {false, true}) {
        std::unordered_map<u32, std::pair<u64, BlockPos>> counts;
        count_states(bedrock, affected, counts, secondary);
        for (const auto& [v, cp] : counts) {
            if (derived_only[v]) continue; // exact for that version
            if (how[v].kind == BedrockTargets::Written::Kind::Missing)
                report.add(IssueKind::LossyBlock,
                           std::format("{} kept for newer versions (not in Bedrock {}, which shows it as an unknown block)", state_text(v),
                                       target.name),
                           cp.second, cp.first);
            else
                report.add(IssueKind::LossyBlock,
                           std::format("{} written as {} (Bedrock {})", state_text(v), state_text(how[v].nearest), target.name), cp.second,
                           cp.first);
            changed += cp.first;
        }
        remap_layers(
            bedrock,
            [&](u32 v) -> u32 {
                if (!affected(v) || how[v].kind == BedrockTargets::Written::Kind::Missing) return v; // missing: kept as it is
                return how[v].nearest;
            },
            secondary);
    }

    // Block entities in the target's form (those of kept blocks too: the game keeps them with the block).
    std::vector<BlockEntity> kept;
    kept.reserve(bedrock.block_entities.size());
    for (BlockEntity& be : bedrock.block_entities) {
        if (!bedrock.contains(be.pos)) continue;
        std::vector<std::string> lossy;
        if (auto bytes = detail::downgrade_bedrock_block_entity(be, targets, target, mapping, lossy)) be.data.bytes = std::move(*bytes);
        for (const std::string& l : lossy) report.add(IssueKind::LossyBlock, std::format("{}: {} (Bedrock {})", be.id, l, target.name), be.pos);
        kept.push_back(std::move(be));
    }
    bedrock.block_entities = std::move(kept);

    // Biomes the target does not have yet (Bedrock-domain biome layers hold numeric ids).
    std::unordered_map<u32, u32> biome_map;
    for (const NewBiome& nb : kNewBiomes) {
        const std::array<i32, 3> tv{target.version[0], target.version[1], target.version[2]};
        if (tv < nb.bedrock_version) biome_map[static_cast<u32>(nb.bedrock)] = static_cast<u32>(nb.bedrock_fallback);
    }
    if (!biome_map.empty())
        remap_biomes(
            bedrock, [&](u32 v) { const auto it = biome_map.find(v); return it == biome_map.end() ? v : it->second; },
            [&](u32 v) {
                std::string_view nm = "?";
                for (const NewBiome& nb : kNewBiomes)
                    if (static_cast<u32>(nb.bedrock) == v) nm = nb.java;
                return std::format("{} (not in Bedrock {}): written as biome {}", nm, target.name, biome_map.at(v));
            },
            report);
    return changed;
}

} // namespace bridger::translate
