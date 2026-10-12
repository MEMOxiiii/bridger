#pragma once

// Where a structure goes in a new Bedrock world (docs/safety.md §5):
// by default its lowest layer sits directly on the superflat surface, centered on
// x = 0, z = 0; --place-at overrides the origin. If it exceeds the build height the
// height policy applies: fail (default), clip (blocks outside are dropped and
// reported) or shift to fit.

#include <bridger/convert/placement_policy.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/translate/conversion_report.hpp>

namespace bridger::convert {

struct Placement {
    BlockPos origin; // world position of the structure's local (0,0,0)
    BlockPos spawn;  // next to the structure, outside its footprint
};

struct WorldBounds {
    i32 min_y = -64;
    i32 max_y = 319;
    i32 surface_y = -61; // top block of the flat layers
};

// May replace `structure` with a clipped copy (HeightPolicy::Clip). `air` is the empty block
// in the structure's id domain (kAirStateId for Java, the palette index of air for Bedrock).
[[nodiscard]] Result<Placement> place_in_new_world(Structure& structure, const PlacementPolicy& policy,
                                                   const WorldBounds& bounds, translate::ConversionReport& report,
                                                   StateId air = kAirStateId);

} // namespace bridger::convert
