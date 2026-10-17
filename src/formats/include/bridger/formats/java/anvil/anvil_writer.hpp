#pragma once

// Writing Java worlds (docs/formats/anvil.md, "Writing"): chunk NBT in the Java 26.3 layout,
// region files, level.dat and data/minecraft/world_gen_settings.dat. The world uses the 26.1+
// folder layout (dimensions/minecraft/overworld/region).

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::formats::java {

// `column`: a Java structure 16 x h x 16 (h a multiple of 16) whose local (0,0,0) is world
// (pos.x*16, min_y, pos.z*16); block entities have column-local positions. Returns the
// uncompressed chunk NBT.
[[nodiscard]] Result<std::vector<u8>> encode_chunk(const Structure& column, ChunkPos pos, i32 min_y, i32 data_version);

// The column's entities as a chunk of an entity region file (entities/r.<x>.<z>.mca, Java 1.17+):
// {DataVersion, Position: [I; x, z], Entities: [...]} (big-endian NBT). Each entity gets "id", "Pos" (absolute doubles)
// and a "UUID" (int[4], unique per world: Bridger's marker, chunk x, chunk z, index); its tile fields ("block_pos" or
// TileX/TileY/TileZ), column-local in Structure::entities, are made absolute. docs/formats/anvil.md, "Entities".
[[nodiscard]] Result<std::vector<u8>> encode_entity_chunk(const std::vector<Entity>& entities, ChunkPos pos, i32 min_y, i32 data_version);

// Collects the chunks of one region and builds r.<x>.<z>.mca (zlib per chunk; payloads over
// 255 sectors go to c.<x>.<z>.mcc files next to it).
class RegionWriter {
public:
    RegionWriter(i32 region_x, i32 region_z) : rx_(region_x), rz_(region_z) {}
    // `chunk_nbt` is uncompressed; local coordinates 0..31.
    [[nodiscard]] Status add(u32 local_x, u32 local_z, std::span<const u8> chunk_nbt);
    // Writes the region file (and any .mcc files) into `region_dir`.
    [[nodiscard]] Status write(const std::filesystem::path& region_dir) const;
    [[nodiscard]] usize chunk_count() const noexcept;

private:
    i32 rx_, rz_;
    std::array<std::vector<u8>, 1024> compressed_; // zlib payloads, empty = no chunk
};

// Java versions whose save format changed (DataVersions, minecraft.wiki "Data version").
inline constexpr i32 kJava1_21_9 = 4554; // spawn point as a compound
inline constexpr i32 kJava26_1 = 4786;   // dimensions/ folders and world_gen_settings.dat (26.1 snapshot 6)

struct JavaLevelOptions {
    std::string level_name = "Bridger World";
    BlockPos spawn{0, 64, 0};
    std::optional<i64> seed;
    i32 data_version = 0;
    std::string version_name; // e.g. "26.3"
    i32 game_type = 1;        // creative
    i64 last_played_ms = 0;
    // Experimental features to switch on ("update_1_20"): data packs and enabled_features, as Chunker's
    // JavaLevelWriter.enableExperiments writes them. Needed when the world holds experimental blocks.
    std::vector<std::string> experiments;
    // The single-player player: a Player compound (big-endian NBT), written as Data.Player.
    std::optional<RawNbt> player;
};

// Gzip-compressed level.dat ({Data: {...}}) and, from 26.1, world_gen_settings.dat (seed only). The layout follows
// data_version: before 26.1 WorldGenSettings is written into level.dat instead, before 1.21.9 the spawn is SpawnX/Y/Z.
[[nodiscard]] Result<std::vector<u8>> encode_java_level_dat(const JavaLevelOptions& options);
[[nodiscard]] Result<std::vector<u8>> encode_world_gen_settings(i64 seed, i32 data_version);

} // namespace bridger::formats::java
