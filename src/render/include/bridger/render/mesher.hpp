#pragma once

// Section mesher: one 16^3 section (plus a one-block border of neighbours) -> vertices per render
// layer. Face culling against opaque neighbours, smooth ambient occlusion on full faces, the
// game's directional face shading, tints, and fluid surfaces. Pure CPU, thread-safe for distinct
// outputs; the block table must be baked beforehand (BlockModels::bake).

#include <bridger/render/block_models.hpp>

#include <array>
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

} // namespace bridger::render
