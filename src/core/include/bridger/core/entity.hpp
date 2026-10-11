#pragma once

// Entities (mobs, armor stands, item frames, ...).

#include <bridger/core/block_entity.hpp>
#include <bridger/core/types.hpp>

#include <string>

namespace bridger {

struct EntityPos {
    f64 x = 0;
    f64 y = 0;
    f64 z = 0;
    friend bool operator==(const EntityPos&, const EntityPos&) = default;
};

struct Entity {
    EntityPos pos;  // relative to the owning structure/world origin
    std::string id; // e.g. "minecraft:armor_stand"
    RawNbt data;    // type-specific fields as found in the source
};

} // namespace bridger
