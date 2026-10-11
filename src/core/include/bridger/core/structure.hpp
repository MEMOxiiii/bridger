#pragma once

// A bounded volume (schematics, .mcstructure): a dense grid of sections covering
// width x height x length blocks, plus block entities, entities and metadata.
// Local coordinates run from (0,0,0) to size-1. Positions outside the size read as
// air and cannot be written.

#include <bridger/core/block_entity.hpp>
#include <bridger/core/block_registry.hpp>
#include <bridger/core/entity.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/section.hpp>
#include <bridger/core/types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bridger {

struct Extent {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
    [[nodiscard]] constexpr u64 volume() const noexcept { return u64{x} * y * z; }
    friend bool operator==(const Extent&, const Extent&) = default;
};

struct StructureMetadata {
    std::string name;
    std::string author;
    std::optional<i64> date_ms; // milliseconds since the Unix epoch
    std::vector<std::string> required_mods;
    RawNbt original; // the format's full metadata compound, kept verbatim
};

class Structure {
public:
    // Fails if the size needs more sections than `max_sections` (memory guard).
    [[nodiscard]] static Result<Structure> create(std::shared_ptr<BlockStateRegistry> blocks,
                                                  std::shared_ptr<BiomeRegistry> biomes, Extent size,
                                                  SourceVersion source, u64 max_sections = u64{1} << 22);

    [[nodiscard]] Extent size() const noexcept { return size_; }
    [[nodiscard]] SourceVersion source() const noexcept { return source_; }
    [[nodiscard]] BlockStateRegistry& block_registry() const noexcept { return *blocks_; }
    [[nodiscard]] BiomeRegistry& biome_registry() const noexcept { return *biomes_; }
    [[nodiscard]] const std::shared_ptr<BlockStateRegistry>& block_registry_ptr() const noexcept { return blocks_; }
    [[nodiscard]] const std::shared_ptr<BiomeRegistry>& biome_registry_ptr() const noexcept { return biomes_; }

    [[nodiscard]] bool contains(BlockPos p) const noexcept {
        return p.x >= 0 && p.y >= 0 && p.z >= 0 && static_cast<u32>(p.x) < size_.x &&
               static_cast<u32>(p.y) < size_.y && static_cast<u32>(p.z) < size_.z;
    }

    [[nodiscard]] StateId block(BlockPos p) const noexcept;
    [[nodiscard]] StateId secondary(BlockPos p) const noexcept;
    [[nodiscard]] std::optional<BiomeId> biome(BlockPos p) const noexcept;
    // Return false (and change nothing) outside the structure.
    bool set_block(BlockPos p, StateId id);
    bool set_secondary(BlockPos p, StateId id);
    bool set_biome(BlockPos p, BiomeId id);

    // Section grid: sections_x() * sections_y() * sections_z() sections.
    [[nodiscard]] u32 sections_x() const noexcept { return sx_; }
    [[nodiscard]] u32 sections_y() const noexcept { return sy_; }
    [[nodiscard]] u32 sections_z() const noexcept { return sz_; }
    [[nodiscard]] Section& section(u32 x, u32 y, u32 z) noexcept { return sections_[section_slot(x, y, z)]; }
    [[nodiscard]] const Section& section(u32 x, u32 y, u32 z) const noexcept {
        return sections_[section_slot(x, y, z)];
    }

    // Paste offset as given by the source format (e.g. Sponge "Offset").
    BlockPos offset;
    StructureMetadata metadata;
    std::vector<BlockEntity> block_entities;
    std::vector<Entity> entities;

    [[nodiscard]] usize memory_bytes() const noexcept;

private:
    Structure() = default;
    [[nodiscard]] usize section_slot(u32 x, u32 y, u32 z) const noexcept {
        return (static_cast<usize>(y) * sz_ + z) * sx_ + x;
    }

    std::shared_ptr<BlockStateRegistry> blocks_;
    std::shared_ptr<BiomeRegistry> biomes_;
    Extent size_;
    SourceVersion source_;
    u32 sx_ = 0;
    u32 sy_ = 0;
    u32 sz_ = 0;
    std::vector<Section> sections_;
};

} // namespace bridger
