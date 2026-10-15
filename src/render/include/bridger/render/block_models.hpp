#pragma once

// Block models baked from Minecraft Java blockstate/model JSON (docs/formats/assets.md):
// variants and multipart, parent chains, element rotation, model x/y rotation, uvlock, face UV
// rotation, tint indices. Textures are gathered into one array of 16 x 16 RGBA layers.
//
// Render layer per block is classified from its textures' alpha, as the game's hard-coded lists
// cannot be read from the assets: any partly transparent pixel -> translucent, any fully
// transparent pixel -> cutout, otherwise solid.

#include <bridger/core/block_state.hpp>
#include <bridger/core/result.hpp>
#include <bridger/render/assets.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace bridger::render {

enum class RenderLayer : u8 { Invisible, Solid, Cutout, Translucent };
enum class Tint : u8 { None, Grass, Foliage, FoliageBirch, FoliageSpruce, Water, Redstone, LilyPad };
enum class Fluid : u8 { None, Water, Lava };

// Faces: 0 down (-Y), 1 up (+Y), 2 north (-Z), 3 south (+Z), 4 west (-X), 5 east (+X).
inline constexpr std::array<glm::ivec3, 6> kFaceNormals{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

struct BakedQuad {
    std::array<glm::vec3, 4> pos; // block-local, 0..1 (may exceed for large models)
    std::array<glm::vec2, 4> uv;  // 0..1 within the texture layer
    u16 texture = 0;              // layer in the texture array
    i8 cull_face = -1;            // face whose neighbour hides this quad, -1 = never
    i8 tint_index = -1;
    u8 normal_face = 1;           // axis-aligned face the quad points to (shading)
    bool shade = true;
    bool full_face = false;       // covers the whole block face (eligible for smooth AO)
};

struct BakedModel {
    std::vector<BakedQuad> quads;
};

struct BakedBlock {
    std::vector<BakedModel> variants; // weighted alternatives; the mesher picks one per position
    RenderLayer layer = RenderLayer::Invisible;
    Tint tint = Tint::None;
    Fluid fluid = Fluid::None;       // the block is, or contains (waterlogged), a fluid
    u8 fluid_level = 0;              // Java "level" for water/lava blocks (0 = source)
    bool fluid_only = false;         // water/lava block itself (no model)
    std::array<bool, 6> occludes{};  // the face is fully covered by an opaque surface
    bool cull_same = false;          // faces against the same state are hidden (glass, ice)
    u32 map_color = 0;               // average top colour (RGBA), for distant terrain (LOD)
    bool map_tinted = false;         // the top face takes `tint` (grass, leaves)
};

class BlockModels {
public:
    // Reads blockstates/models/textures lazily from `assets`.
    [[nodiscard]] static Result<BlockModels> create(std::shared_ptr<const AssetSource> assets);
    BlockModels(BlockModels&&) noexcept;
    BlockModels& operator=(BlockModels&&) noexcept;
    ~BlockModels();

    // Bakes (once) and returns the model for a canonical Java block state. Unknown blocks get a
    // magenta/black "missing" cube. Not thread-safe: bake every state before meshing in parallel.
    const BakedBlock& bake(const BlockState& state);

    // Texture array: layer count and RGBA8 pixels (16 x 16 per layer, row-major).
    [[nodiscard]] usize texture_count() const noexcept;
    [[nodiscard]] const std::vector<u32>& texture_pixels() const noexcept; // texture_count * 256 RGBA
    // The layer for a texture path such as "block/stone" (loading it if needed).
    u16 texture(std::string_view name);
    // A plain white layer: distant terrain (LOD) is coloured by its vertex colours only.
    [[nodiscard]] u16 white_texture() const noexcept;

    // Diagnostics: blocks whose blockstate file or models could not be read.
    [[nodiscard]] const std::vector<std::string>& missing() const noexcept;

private:
    BlockModels();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Default (plains) tint colours, RGB in 0..1.
[[nodiscard]] glm::vec3 tint_color(Tint tint, u8 redstone_power = 0) noexcept;

} // namespace bridger::render
