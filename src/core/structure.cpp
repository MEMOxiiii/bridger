#include <bridger/core/structure.hpp>

namespace bridger {
namespace {

constexpr u32 sections_for(u32 blocks) noexcept { return (blocks + 15) / 16; }

} // namespace

Result<Structure> Structure::create(std::shared_ptr<BlockStateRegistry> blocks, std::shared_ptr<BiomeRegistry> biomes,
                                    Extent size, SourceVersion source, u64 max_sections) {
    if (!blocks || !biomes) return make_error(ErrorCode::InvalidArgument, "structure needs registries");
    const u32 sx = sections_for(size.x);
    const u32 sy = sections_for(size.y);
    const u32 sz = sections_for(size.z);
    const u64 count = u64{sx} * sy * sz;
    if (count > max_sections)
        return make_error(ErrorCode::LimitExceeded, "structure of " + std::to_string(size.x) + "x" +
                                                        std::to_string(size.y) + "x" + std::to_string(size.z) +
                                                        " blocks exceeds the section limit");
    Structure s;
    s.blocks_ = std::move(blocks);
    s.biomes_ = std::move(biomes);
    s.size_ = size;
    s.source_ = source;
    s.sx_ = sx;
    s.sy_ = sy;
    s.sz_ = sz;
    s.sections_.assign(static_cast<usize>(count), Section(source));
    return s;
}

StateId Structure::block(BlockPos p) const noexcept {
    if (!contains(p)) return kAirStateId;
    return section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .block(p.x & 15, p.y & 15, p.z & 15);
}

StateId Structure::secondary(BlockPos p) const noexcept {
    if (!contains(p)) return kAirStateId;
    return section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .secondary(p.x & 15, p.y & 15, p.z & 15);
}

std::optional<BiomeId> Structure::biome(BlockPos p) const noexcept {
    if (!contains(p)) return std::nullopt;
    return section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .biome(p.x & 15, p.y & 15, p.z & 15);
}

bool Structure::set_block(BlockPos p, StateId id) {
    if (!contains(p)) return false;
    section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .set_block(p.x & 15, p.y & 15, p.z & 15, id);
    return true;
}

bool Structure::set_secondary(BlockPos p, StateId id) {
    if (!contains(p)) return false;
    section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .set_secondary(p.x & 15, p.y & 15, p.z & 15, id);
    return true;
}

bool Structure::set_biome(BlockPos p, BiomeId id) {
    if (!contains(p)) return false;
    section(static_cast<u32>(p.x >> 4), static_cast<u32>(p.y >> 4), static_cast<u32>(p.z >> 4))
        .set_biome(p.x & 15, p.y & 15, p.z & 15, id);
    return true;
}

usize Structure::memory_bytes() const noexcept {
    usize n = sizeof(*this);
    for (const Section& s : sections_) n += s.memory_bytes();
    for (const BlockEntity& be : block_entities) n += sizeof(be) + be.id.capacity() + be.data.bytes.capacity();
    for (const Entity& e : entities) n += sizeof(e) + e.id.capacity() + e.data.bytes.capacity();
    return n;
}

} // namespace bridger
