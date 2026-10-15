#include <bridger/convert/view_world.hpp>

#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

#include <format>
#include <map>
#include <random>
#include <unordered_map>

namespace bridger::convert {
namespace fs = std::filesystem;

bool is_world_path(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec) || path.filename() == "level.dat") return true;
    auto ext = platform::path_to_utf8(path.extension());
    return ext && (*ext == ".mcworld" || *ext == ".MCWORLD");
}

struct ViewWorld::Impl {
    ViewWorldInfo info;
    std::vector<ChunkPos> chunks;
    // Java
    std::vector<formats::java::RegionFile> regions;
    std::unordered_map<u64, std::pair<usize, u32>> java_slot; // chunk -> (region index, local slot)
    // Bedrock
    std::optional<formats::bedrock::BedrockWorldReader> bedrock;
    fs::path scratch;
    const translate::MappingData* mapping = nullptr;
    u32 air = 0;

    ~Impl() {
        bedrock.reset();
        if (!scratch.empty()) {
            std::error_code ec;
            fs::remove_all(scratch, ec);
        }
    }
};

ViewWorld::ViewWorld() : impl_(std::make_unique<Impl>()) {}
ViewWorld::ViewWorld(ViewWorld&&) noexcept = default;
ViewWorld& ViewWorld::operator=(ViewWorld&&) noexcept = default;
ViewWorld::~ViewWorld() = default;

const ViewWorldInfo& ViewWorld::info() const noexcept { return impl_->info; }
const std::vector<ChunkPos>& ViewWorld::chunks() const noexcept { return impl_->chunks; }

Result<ViewWorld> ViewWorld::open(const fs::path& path) {
    ViewWorld w;
    Impl& m = *w.impl_;
    BRIDGER_TRY(m.mapping, translate::MappingData::embedded());
    m.air = m.mapping->find_bedrock("minecraft:air", {}).value_or(0);
    std::error_code ec;
    const bool bedrock_folder = formats::bedrock::is_bedrock_world_folder(path);
    if (!bedrock_folder && (fs::is_directory(path, ec) || path.filename() == "level.dat")) {
        BRIDGER_TRY(const formats::java::JavaWorld jw, formats::java::open_java_world(path));
        m.info.format = formats::FormatId::JavaWorld;
        m.info.level_name = jw.level.level_name;
        m.info.spawn = jw.level.spawn.value_or(BlockPos{0, 64, 0});
        m.info.seed = jw.level.seed;
        for (const fs::path& rp : jw.overworld_regions) {
            auto r = formats::java::RegionFile::open(rp);
            if (!r) continue;
            const usize idx = m.regions.size();
            for (u32 slot = 0; slot < 1024; ++slot)
                if (r->has_chunk(slot % 32, slot / 32)) {
                    const ChunkPos p{r->region_x() * 32 + static_cast<i32>(slot % 32), r->region_z() * 32 + static_cast<i32>(slot / 32)};
                    m.java_slot.emplace(p.packed(), std::pair{idx, slot});
                    m.chunks.push_back(p);
                }
            m.regions.push_back(std::move(*r));
        }
    } else {
        std::random_device rd;
        m.scratch = fs::temp_directory_path(ec) / std::format("bridger-view-{:08x}{:08x}", rd(), rd());
        fs::create_directories(m.scratch, ec);
        BRIDGER_TRY(formats::bedrock::BedrockWorldReader reader, formats::bedrock::BedrockWorldReader::open_path(path, m.scratch));
        m.info.format = formats::FormatId::McWorld;
        m.info.level_name = reader.level().level_name;
        m.info.spawn = reader.level().spawn;
        m.info.seed = reader.level().seed;
        m.chunks = reader.overworld_chunks();
        m.bedrock.emplace(std::move(reader));
    }
    m.info.chunk_count = m.chunks.size();
    return w;
}

std::optional<ViewColumn> ViewWorld::load(ChunkPos pos) const {
    const Impl& m = *impl_;
    if (!m.bedrock) {
        const auto it = m.java_slot.find(pos.packed());
        if (it == m.java_slot.end()) return std::nullopt;
        const auto& region = m.regions[it->second.first];
        auto raw = region.read_chunk(it->second.second % 32, it->second.second / 32);
        if (!raw || !*raw) return std::nullopt;
        auto chunk = formats::java::decode_chunk(**raw, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>());
        if (!chunk || !chunk->full) return std::nullopt;
        return ViewColumn{chunk->pos, chunk->min_y, std::move(chunk->column)};
    }
    auto col = m.bedrock->read_column(pos, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                      m.mapping->bedrock_block_version(), bedrock_state_lookup(*m.mapping), m.air,
                                      translate::kDefaultBedrockBiome);
    if (!col) return std::nullopt;
    translate::ConversionReport report;
    auto java = translate::translate_to_java(col->column, *m.mapping, {}, report);
    if (!java) return std::nullopt;
    return ViewColumn{pos, col->min_y, std::move(*java)};
}

} // namespace bridger::convert
