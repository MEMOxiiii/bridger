#include <bridger/convert/placement.hpp>

#include <string>

namespace bridger::convert {
namespace {

// Copy of `s` restricted to local y in [y_begin, y_end), counting dropped non-air blocks.
Result<Structure> clip_y(const Structure& s, u32 y_begin, u32 y_end, StateId air, translate::ConversionReport& report) {
    const Extent size = s.size();
    BRIDGER_TRY(Structure out, Structure::create(s.block_registry_ptr(), s.biome_registry_ptr(),
                                                 Extent{size.x, y_end - y_begin, size.z}, s.source()));
    for (u32 sy = 0; sy < out.sections_y(); ++sy) // `air` is not 0 in the Bedrock id domain
        for (u32 sz = 0; sz < out.sections_z(); ++sz)
            for (u32 sx = 0; sx < out.sections_x(); ++sx) out.section(sx, sy, sz).blocks().fill(air);
    out.offset = s.offset;
    out.metadata = s.metadata;
    for (u32 y = 0; y < size.y; ++y)
        for (u32 z = 0; z < size.z; ++z)
            for (u32 x = 0; x < size.x; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                const StateId block = s.block(p);
                if (y < y_begin || y >= y_end) {
                    if (block != air) report.add(translate::IssueKind::ClippedBlock, "block outside build height", p);
                    continue;
                }
                const BlockPos q{p.x, static_cast<i32>(y - y_begin), p.z};
                out.set_block(q, block);
                if (s.has_secondary(p)) out.set_secondary(q, s.secondary(p));
                if (const auto b = s.biome(p)) out.set_biome(q, *b);
            }
    for (const BlockEntity& be : s.block_entities) {
        if (be.pos.y < static_cast<i32>(y_begin) || be.pos.y >= static_cast<i32>(y_end)) {
            report.add(translate::IssueKind::ClippedBlock, "block entity " + be.id + " outside build height", be.pos);
            continue;
        }
        BlockEntity moved = be;
        moved.pos.y -= static_cast<i32>(y_begin);
        out.block_entities.push_back(std::move(moved));
    }
    out.entities = s.entities;
    return out;
}

} // namespace

Result<Placement> place_in_new_world(Structure& structure, const PlacementPolicy& policy, const WorldBounds& bounds,
                                     translate::ConversionReport& report, StateId air) {
    const Extent size = structure.size();
    Placement p;
    if (policy.place_at) {
        p.origin = *policy.place_at;
    } else {
        p.origin = {-static_cast<i32>(size.x / 2), bounds.surface_y + 1, -static_cast<i32>(size.z / 2)};
    }

    const i64 height = size.y;
    const i64 top = i64{p.origin.y} + height - 1;
    const i64 below = i64{bounds.min_y} - p.origin.y; // blocks under the floor
    const i64 above = top - bounds.max_y;             // blocks over the ceiling
    if (height > 0 && (below > 0 || above > 0)) {
        const std::string overflow = "structure spans y " + std::to_string(p.origin.y) + ".." + std::to_string(top) +
                                     " but the world allows " + std::to_string(bounds.min_y) + ".." +
                                     std::to_string(bounds.max_y) + " (" +
                                     (below > 0 ? std::to_string(below) + " layers below" : "") +
                                     (below > 0 && above > 0 ? ", " : "") +
                                     (above > 0 ? std::to_string(above) + " layers above" : "") + ")";
        switch (policy.height) {
        case HeightPolicy::Fail:
            return make_error(ErrorCode::OutOfBounds, overflow + ". Use --clip or --shift-to-fit, or --place-at.");
        case HeightPolicy::ShiftToFit: {
            const i64 room = i64{bounds.max_y} - bounds.min_y + 1;
            if (height > room)
                return make_error(ErrorCode::OutOfBounds, overflow + "; it is taller than the world, use --clip");
            p.origin.y = below > 0 ? bounds.min_y : static_cast<i32>(bounds.max_y - height + 1);
            break;
        }
        case HeightPolicy::Clip: {
            const u32 y_begin = below > 0 ? static_cast<u32>(below) : 0u;
            const u32 y_end = above > 0 ? static_cast<u32>(height - above) : static_cast<u32>(height);
            BRIDGER_TRY(structure, clip_y(structure, y_begin, y_end, air, report));
            p.origin.y += static_cast<i32>(y_begin);
            break;
        }
        }
    }
    p.spawn = {p.origin.x - 2, std::max(p.origin.y, bounds.surface_y + 1), p.origin.z + static_cast<i32>(size.z / 2)};
    return p;
}

} // namespace bridger::convert
