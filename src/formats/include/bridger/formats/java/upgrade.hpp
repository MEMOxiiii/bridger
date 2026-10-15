#pragma once

// How a Java block state saved at `data_version` reads in the pinned version: the renames and value changes
// since 1.13 that every Java reader applies (docs/formats/anvil.md, "Old worlds"). Writers for older target
// versions use it to check that what they wrote reads back as intended.

#include <bridger/core/block_state.hpp>
#include <bridger/core/types.hpp>

namespace bridger::formats::java {

[[nodiscard]] BlockState upgrade_block_state(const BlockState& state, i32 data_version);

} // namespace bridger::formats::java
