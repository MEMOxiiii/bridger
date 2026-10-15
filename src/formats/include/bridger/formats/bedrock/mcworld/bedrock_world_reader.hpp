#pragma once

// Reading a Bedrock world (.mcworld) one chunk column at a time (docs/formats/bedrock-world.md,
// "Reading worlds"). The zip is unpacked into a scratch directory and its LevelDB opened
// read-only; the input file itself is never modified. Overworld only.

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::bedrock {

struct BedrockLevelInfo {
    std::string level_name;
    BlockPos spawn{0, 64, 0};
    i64 seed = 0;
    i32 generator = 1; // 0 old, 1 infinite, 2 flat, 5 void
};

struct BedrockColumn {
    ChunkPos pos;
    i32 min_y = -64;
    // Bedrock-domain column, 16 x 384 x 16: block layers hold Bedrock state indices (layer 2
    // the liquids), biome layers Bedrock numeric ids, block entities Bedrock NBT with
    // column-local positions (pairx/pairz too).
    Structure column;
    std::map<std::string, u64> unknown_blocks; // palette entries not in the pinned palette (read as air)
    std::vector<std::string> warnings;
};

struct SubchunkLayers;
struct LegacyTerrain;

// A Bedrock world folder: level.dat and db/CURRENT (path is the folder or its level.dat).
[[nodiscard]] bool is_bedrock_world_folder(const std::filesystem::path& path);

class BedrockWorldReader {
public:
    [[nodiscard]] static Result<BedrockWorldReader> open(std::span<const u8> mcworld, const std::filesystem::path& work_dir);
    // A .mcworld file or a world folder (level.dat + db/, or its level.dat); a folder's database is
    // copied into work_dir first, so the input is never opened for writing.
    [[nodiscard]] static Result<BedrockWorldReader> open_path(const std::filesystem::path& input, const std::filesystem::path& work_dir);
    BedrockWorldReader(BedrockWorldReader&&) noexcept;
    BedrockWorldReader& operator=(BedrockWorldReader&&) noexcept;
    ~BedrockWorldReader();

    [[nodiscard]] const BedrockLevelInfo& level() const noexcept;
    [[nodiscard]] const std::vector<ChunkPos>& overworld_chunks() const noexcept; // sorted (z, x)
    [[nodiscard]] bool has_other_dimensions() const noexcept;
    [[nodiscard]] usize entity_count() const noexcept; // actors (digp/actorprefix) + legacy entities

    // Thread-safe. `lookup` resolves palette entries to pinned-palette indices.
    [[nodiscard]] Result<BedrockColumn> read_column(ChunkPos pos, std::shared_ptr<BlockStateRegistry> blocks,
                                                    std::shared_ptr<BiomeRegistry> biomes, u32 bedrock_data_version,
                                                    const StateLookupFn& lookup, u32 air_state, i32 default_biome) const;

    // Thread-safe. Layer-1 blocks of one subchunk (Bridger order), for neighbour lookups across
    // column borders; nullopt if the subchunk is absent or unreadable (unknown entries read as air).
    [[nodiscard]] std::optional<std::array<u32, kSectionVolume>> read_subchunk_blocks(ChunkPos pos, i32 subchunk_y,
                                                                                    const StateLookupFn& lookup,
                                                                                    u32 air_state) const;

private:
    BedrockWorldReader();
    [[nodiscard]] static Result<BedrockWorldReader> open_unpacked(BedrockWorldReader r, const std::vector<u8>& level_bytes);
    [[nodiscard]] Result<u8> chunk_version(ChunkPos pos) const;
    [[nodiscard]] Result<SubchunkLayers> subchunk_layers(ChunkPos pos, i32 subchunk_y, u8 version,
                                                                const LegacyTerrain* terrain) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::formats::bedrock
