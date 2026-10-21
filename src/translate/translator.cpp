#include <bridger/translate/block_entity_translator.hpp>
#include <bridger/translate/translator.hpp>

#include "entity_translator.hpp"

#include <unordered_map>
#include <unordered_set>

namespace bridger::translate {
namespace {

struct Translated {
    BedrockStateIndex bedrock = 0;
    JavaStateId java = UINT32_MAX; // UINT32_MAX: not a known Java state
    bool waterlogged = false;
    bool context = false;
    bool unmapped = false;
};

struct BlockPosHash {
    usize operator()(const BlockPos& p) const noexcept {
        return std::hash<u64>{}((static_cast<u64>(static_cast<u32>(p.x)) * 73856093u) ^
                                (static_cast<u64>(static_cast<u32>(p.y)) * 19349663u) ^
                                (static_cast<u64>(static_cast<u32>(p.z)) << 21));
    }
};

} // namespace

Result<Structure> translate_to_bedrock(const Structure& java, const MappingData& map, const TranslateOptions& options,
                                       ConversionReport& report) {
    if (java.source().edition != Edition::Java)
        return make_error(ErrorCode::InvalidArgument, "translate_to_bedrock needs a Java structure");

    const auto air = map.find_bedrock("minecraft:air", {});
    const BedrockProperty still_water{"liquid_depth", NbtValueType::Int, 0, {}};
    const auto water = map.find_bedrock("minecraft:water", std::span<const BedrockProperty>(&still_water, 1));
    const auto marker = map.find_bedrock("minecraft:info_update", {});
    if (!air || !water || !marker) return make_error(ErrorCode::Malformed, "mapping data lacks air, water or marker");
    const BedrockStateIndex fallback = options.fallback == Fallback::Air ? *air : *marker;

    BRIDGER_TRY(Structure out, Structure::create(java.block_registry_ptr(), java.biome_registry_ptr(), java.size(),
                                                 {Edition::Bedrock, map.bedrock_block_version()}));
    out.offset = java.offset;
    out.metadata = java.metadata;

    // --- Palette stage cache: Java registry id -> translation (each distinct state once).
    const BlockStateRegistry& registry = java.block_registry();
    std::vector<std::optional<Translated>> cache(registry.size());
    // Returns by value: a reference into `cache` could dangle if it grew.
    auto translate_state = [&](StateId id) -> Translated {
        if (id >= cache.size()) cache.resize(registry.size());
        if (id >= cache.size()) return Translated{fallback, UINT32_MAX, false, false, true};
        if (cache[id]) return *cache[id];
        Translated t;
        const BlockState& state = registry.get(id);
        if (const auto jid = map.find_java_state(state)) {
            const JavaStateMapping m = map.map(*jid);
            t.java = *jid;
            if (m.bedrock == UINT32_MAX) {
                t.unmapped = true;
                t.bedrock = fallback;
            } else {
                t.bedrock = m.bedrock;
                t.waterlogged = (m.flags & kFlagWaterlogged) != 0;
            }
            t.context = is_context_dependent(state.id());
        } else {
            t.unmapped = true;
            t.bedrock = fallback;
        }
        cache[id] = t;
        return t;
    };

    // --- Biomes: Java biome name -> Bedrock numeric id, once per name.
    const BiomeRegistry& biome_names = java.biome_registry();
    std::unordered_map<u32, u32> biome_cache;
    auto translate_biome = [&](u32 java_biome, BlockPos where) -> u32 {
        if (const auto it = biome_cache.find(java_biome); it != biome_cache.end()) return it->second;
        const std::string& name = biome_names.name(java_biome);
        u32 id = static_cast<u32>(kDefaultBedrockBiome);
        if (const auto b = map.find_biome(name)) id = static_cast<u32>(*b);
        else report.add(IssueKind::UnmappedBiome, name, where);
        biome_cache.emplace(java_biome, id);
        return id;
    };

    std::vector<std::pair<BlockPos, JavaStateId>> context_positions;
    const Extent size = java.size();
    for (u32 cy = 0; cy < java.sections_y(); ++cy) {
        for (u32 cz = 0; cz < java.sections_z(); ++cz) {
            for (u32 cx = 0; cx < java.sections_x(); ++cx) {
                const Section& js = java.section(cx, cy, cz);
                Section& os = out.section(cx, cy, cz);

                // Translate the section palette, then remap (index array untouched).
                PaletteLayer layer = js.blocks();
                std::vector<u32> palette;
                palette.reserve(layer.palette().size());
                bool needs_positions = false;
                for (const u32 id : layer.palette()) {
                    const Translated t = translate_state(id);
                    palette.push_back(t.bedrock);
                    needs_positions |= t.waterlogged || t.unmapped || t.context;
                }
                layer.remap(palette);
                os.blocks() = std::move(layer);

                // Context stage: visit only positions whose state needs it.
                if (needs_positions) {
                    for (u32 y = 0; y < 16 && cy * 16 + y < size.y; ++y)
                        for (u32 z = 0; z < 16 && cz * 16 + z < size.z; ++z)
                            for (u32 x = 0; x < 16 && cx * 16 + x < size.x; ++x) {
                                const i32 ix = static_cast<i32>(x);
                                const i32 iy = static_cast<i32>(y);
                                const i32 iz = static_cast<i32>(z);
                                const Translated t = translate_state(js.block(ix, iy, iz));
                                const BlockPos pos{static_cast<i32>(cx * 16 + x), static_cast<i32>(cy * 16 + y),
                                                   static_cast<i32>(cz * 16 + z)};
                                if (t.waterlogged) {
                                    // Allocate with Bedrock air as the empty value (0 is a real Bedrock state).
                                    (void)os.ensure_secondary(*air);
                                    os.set_secondary(ix, iy, iz, *water);
                                }
                                if (t.unmapped) report.add(IssueKind::UnmappedBlock, registry.canonical(js.block(ix, iy, iz)), pos);
                                if (t.context) context_positions.emplace_back(pos, t.java);
                            }
                }

                if (const PaletteLayer* jb = js.biome_layer()) {
                    PaletteLayer biomes = *jb;
                    std::vector<u32> ids;
                    const BlockPos where{static_cast<i32>(cx * 16), static_cast<i32>(cy * 16), static_cast<i32>(cz * 16)};
                    for (const u32 b : biomes.palette()) ids.push_back(translate_biome(b, where));
                    biomes.remap(ids);
                    os.ensure_biomes(ids.empty() ? kDefaultBedrockBiome : ids[0]) = std::move(biomes);
                }
            }
        }
    }

    // --- Block entities (reads the source structure only).
    std::unordered_set<BlockPos, BlockPosHash> has_entity;
    for (const BlockEntity& be : java.block_entities) {
        const Translated t = translate_state(java.block(be.pos));
        if (t.java == UINT32_MAX) {
            report.add(IssueKind::UnmappedBlockEntity, be.id, be.pos);
            continue;
        }
        if (auto translated = translate_block_entity(be, t.java, map, report)) {
            has_entity.insert(be.pos);
            out.block_entities.push_back(std::move(*translated));
        }
    }
    // Context rules that need a Bedrock block entity even without Java data.
    for (const auto& [pos, jid] : context_positions) {
        if (has_entity.contains(pos)) continue;
        if (auto synthesized = synthesize_block_entity(pos, jid, map)) out.block_entities.push_back(std::move(*synthesized));
    }

    // --- Entities: item frames (Bedrock blocks) and paintings (actors); every other entity is reported.
    for (const Entity& e : java.entities) detail::translate_entity(e, out, map, *air, *water, options, report);

    return out;
}

} // namespace bridger::translate
