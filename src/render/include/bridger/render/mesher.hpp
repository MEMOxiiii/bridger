#pragma once

// Section mesher: one 16^3 section (plus a one-block border of neighbours) -> vertices per render
// layer. Face culling against opaque neighbours, smooth ambient occlusion on full faces, the
// game's directional face shading, tints, and fluid surfaces. Pure CPU, thread-safe for distinct
// outputs; the block table must be baked beforehand (BlockModels::bake).

#include <bridger/core/structure.hpp>
#include <bridger/render/block_models.hpp>

#include <array>
#include <climits>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace bridger::render {

// 24 bytes. Position is section-local in blocks (0..16); uv in 0..1 (unorm16) within `layer`.
struct Vertex {
    float x, y, z;
    u16 u, v;
    u16 layer;
    u16 flags; // bit 0: cutout (alpha test)
    u32 color; // RGBA8: tint * shading * ambient occlusion
};
static_assert(sizeof(Vertex) == 24);

struct SectionMesh {
    std::vector<Vertex> solid, cutout, translucent; // quads: 4 vertices each
    [[nodiscard]] bool empty() const noexcept { return solid.empty() && cutout.empty() && translucent.empty(); }
};

inline constexpr int kPadded = 18; // 16 + one block on each side
inline constexpr usize kPaddedVolume = static_cast<usize>(kPadded) * kPadded * kPadded;

[[nodiscard]] constexpr usize padded_index(int x, int y, int z) noexcept { // x, y, z in -1..16
    return (static_cast<usize>(y + 1) * kPadded + static_cast<usize>(z + 1)) * kPadded + static_cast<usize>(x + 1);
}

// `blocks[padded_index(x, y, z)]` points to the baked block at that position (nullptr = air or
// outside the loaded area). `section_world` is the section's world block origin (for per-position
// variant choice).
void mesh_section(const std::array<const BakedBlock*, kPaddedVolume>& blocks, glm::ivec3 section_world, SectionMesh& out);

// ---- Distant terrain (LOD) -----------------------------------------------------------------------
// One column reduced to its top visible block per x,z, which is all distant terrain needs (and what
// the LOD cache stores, lod_cache.hpp).
struct ColumnSummary {
    static constexpr i16 kNoTop = INT16_MIN;
    std::array<i16, 256> top{};   // world y of the top block, index z*16 + x; kNoTop: nothing
    std::array<u32, 256> color{}; // RGBA8 of that block's top face (tinted, face shading not applied)
    friend bool operator==(const ColumnSummary&, const ColumnSummary&) = default;
};
// Block at column-local (x, z) and world y; nullptr = air.
using ColumnBlockFn = std::function<const BakedBlock*(int x, int y, int z)>;
[[nodiscard]] ColumnSummary summarize_column(const ColumnBlockFn& at, int min_y, int height);
// The same for a loaded column (16 x h x 16, local y 0 = world `min_y`), with `table` mapping its state ids
// to baked blocks (nullptr = air). Walks sections from the top and skips those with nothing to draw.
[[nodiscard]] ColumnSummary summarize_column(const Structure& column, std::span<const BakedBlock* const> table, int min_y);

// Mesh of a summarized column in cells of `step` x `step` blocks (1, 2, 4, 8 or 16): each cell a box
// from its highest block down to its neighbours, top faces of equal height and colour merged
// greedily. `around` = the summaries of the +x, -x, +z, -z columns (nullptr: a short skirt instead).
// Output per section (key = section index from `origin.y`), positions in that section's frame, all
// in the solid layer with `white_layer`.
void mesh_lod(const ColumnSummary& column, const std::array<const ColumnSummary*, 4>& around, glm::ivec3 origin, int step,
              u16 white_layer, std::map<int, SectionMesh>& out);

} // namespace bridger::render
