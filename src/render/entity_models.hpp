#pragma once

// Blocks the game draws with block-entity renderers instead of JSON models (chests and shulker
// boxes in Java 26.3). Geometry and texture layouts follow the game's entity models as
// described in docs/formats/assets.md; each box face samples a rectangle of the entity texture,
// resampled into its own 16 x 16 layer by `region` (internal to bridger_render).

#include <bridger/render/block_models.hpp>

#include <functional>
#include <string_view>
#include <vector>

namespace bridger::render {

// Returns the texture layer holding the rectangle (x, y, w, h) of an entity texture such as
// "entity/chest/normal", optionally mirrored.
using TextureRegionFn = std::function<u16(std::string_view texture, int x, int y, int w, int h, bool flip_x, bool flip_y)>;

// Fills `block` (variants) and records used layers; false if the block is not one of these.
bool bake_entity_block(BakedBlock& block, const BlockState& state, const TextureRegionFn& region, std::vector<u16>& used);

} // namespace bridger::render
