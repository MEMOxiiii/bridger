#pragma once

// Streams the chunk columns of a world around the camera: worker threads load columns (through a
// caller-supplied loader, e.g. a Java or Bedrock world reader), bake their block states and mesh
// their sections; the render thread uploads finished meshes within a per-frame budget and drops
// columns that are far away. Sections at a column border are meshed again when the neighbouring
// column arrives, so faces between loaded columns are culled correctly.

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/render/block_models.hpp>
#include <bridger/render/camera.hpp>
#include <bridger/render/renderer.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bridger::render {

class LodCache;

struct LoadedColumn {
    ChunkPos pos;
    i32 min_y = -64;
    Structure column; // Java edition, 16 x h x 16, local (0,0,0) = world (pos.x*16, min_y, pos.z*16)
};

// Thread-safe. nullopt = the column does not exist (or failed; the error is logged by the caller).
using ColumnLoader = std::function<std::optional<LoadedColumn>(ChunkPos)>;

struct StreamingStats {
    usize available = 0;   // columns in the world
    usize loaded = 0;      // columns resident at full detail
    usize distant = 0;     // columns shown as distant terrain (LOD)
    usize queued = 0;      // waiting for a worker
    usize in_flight = 0;   // being loaded or meshed
    u64 quads = 0;
};

class StreamingScene {
public:
    // `models` must outlive the scene; it is used from the workers under an internal lock.
    StreamingScene(BlockModels& models, ColumnLoader loader, std::vector<ChunkPos> available, usize threads = 0);
    ~StreamingScene();
    StreamingScene(const StreamingScene&) = delete;
    StreamingScene& operator=(const StreamingScene&) = delete;

    // Call once per frame from the render thread: schedules loads within `radius_chunks` of the
    // camera (nearest first), uploads up to `upload_budget` finished columns, unloads columns
    // beyond radius + 2. Returns true while work remains.
    bool update(const Camera& camera, Renderer& renderer, int radius_chunks, usize upload_budget = 8);

    // Blocks until every column within the radius is loaded and uploaded (screenshots, tests).
    void load_all(const Camera& camera, Renderer& renderer, int radius_chunks);

    // Distant terrain: columns between the radius passed to update() and `lod_radius_chunks` are
    // shown as LOD meshes (mesher.hpp, mesh_lod), from summaries kept in memory and in `cache`
    // (optional). 0 turns it off.
    void set_distant_terrain(int lod_radius_chunks, std::shared_ptr<LodCache> cache = {});
    [[nodiscard]] Status flush_cache();

    [[nodiscard]] StreamingStats stats() const;
    // Height range of the loaded columns' non-air blocks (for framing a camera); nullopt if none.
    [[nodiscard]] std::optional<std::pair<int, int>> height_range() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::render
