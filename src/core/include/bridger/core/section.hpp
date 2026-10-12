#pragma once

// 16x16x16 section of the IR (docs/adr/0001).
//
// A PaletteLayer stores 4096 ids as a local palette plus an index array whose
// width depends on the palette size: none (uniform), u8 (<= 256 entries) or u16.
// Index order: y*256 + z*16 + x.
//
// A Section has a primary block layer, an optional secondary layer (the liquid or
// other block sharing the position: Java waterlogged=true <-> Bedrock layer 2), and
// an optional per-block biome layer. Optional layers are allocated on first use.

#include <bridger/core/block_registry.hpp>
#include <bridger/core/types.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bridger {

inline constexpr usize kSectionVolume = 4096;

[[nodiscard]] constexpr usize section_index(i32 x, i32 y, i32 z) noexcept {
    return (static_cast<usize>(y) << 8) | (static_cast<usize>(z) << 4) | static_cast<usize>(x);
}

class PaletteLayer {
public:
    explicit PaletteLayer(u32 fill = 0) : palette_{fill} {}

    [[nodiscard]] u32 get(usize index) const noexcept {
        switch (width_) {
        case Width::Uniform: return palette_[0];
        case Width::U8: return palette_[idx8_[index]];
        case Width::U16: return palette_[idx16_[index]];
        }
        return palette_[0];
    }

    void set(usize index, u32 value);
    void fill(u32 value);

    // Replaces all 4096 values at once (readers use this; much faster than set()).
    void assign(std::span<const u32, kSectionVolume> values);

    [[nodiscard]] bool is_uniform() const noexcept { return width_ == Width::Uniform; }
    // Palette entries; may contain values no longer used after set() (see compact()).
    [[nodiscard]] std::span<const u32> palette() const noexcept { return palette_; }

    // Palette stage of translation: palette()[i] becomes new_values[i]. The index
    // array is untouched. new_values.size() must equal palette().size().
    void remap(std::span<const u32> new_values);

    // Drops unused and duplicate palette entries and picks the narrowest width.
    void compact();

    [[nodiscard]] usize memory_bytes() const noexcept;

    // Decoded copy of all 4096 values.
    void copy_to(std::span<u32, kSectionVolume> out) const noexcept;

private:
    enum class Width : u8 { Uniform, U8, U16 };

    u16 find_or_add(u32 value);

    std::vector<u32> palette_;
    std::vector<u8> idx8_;
    std::vector<u16> idx16_;
    Width width_ = Width::Uniform;
};

// Where a section's data came from; ids are interpreted relative to this.
struct SourceVersion {
    Edition edition = Edition::Java;
    u32 data_version = 0; // Java DataVersion, or Bedrock version encoded by the reader
    friend bool operator==(const SourceVersion&, const SourceVersion&) = default;
};

class Section {
public:
    explicit Section(SourceVersion source = {}) : source_(source) {}
    Section(const Section& other);
    Section& operator=(const Section& other);
    Section(Section&&) noexcept = default;
    Section& operator=(Section&&) noexcept = default;
    ~Section() = default;

    [[nodiscard]] StateId block(i32 x, i32 y, i32 z) const noexcept { return blocks_.get(section_index(x, y, z)); }
    void set_block(i32 x, i32 y, i32 z, StateId id) { blocks_.set(section_index(x, y, z), id); }

    // Secondary layer reads as kAirStateId where it was never set. That value is only
    // meaningful in the Java id domain: callers working with other domains (Bedrock
    // palette indices) must check has_secondary() and allocate the layer with their own
    // empty value via ensure_secondary(fill).
    [[nodiscard]] StateId secondary(i32 x, i32 y, i32 z) const noexcept {
        return secondary_ ? secondary_->get(section_index(x, y, z)) : kAirStateId;
    }
    void set_secondary(i32 x, i32 y, i32 z, StateId id);
    [[nodiscard]] bool has_secondary() const noexcept { return secondary_ != nullptr; }

    [[nodiscard]] std::optional<BiomeId> biome(i32 x, i32 y, i32 z) const noexcept {
        if (!biomes_) return std::nullopt;
        return biomes_->get(section_index(x, y, z));
    }
    // The first set_biome() on a section without biomes fills the whole section with `id`.
    void set_biome(i32 x, i32 y, i32 z, BiomeId id);
    [[nodiscard]] bool has_biomes() const noexcept { return biomes_ != nullptr; }

    [[nodiscard]] PaletteLayer& blocks() noexcept { return blocks_; }
    [[nodiscard]] const PaletteLayer& blocks() const noexcept { return blocks_; }
    [[nodiscard]] PaletteLayer* secondary_layer() noexcept { return secondary_.get(); }
    [[nodiscard]] const PaletteLayer* secondary_layer() const noexcept { return secondary_.get(); }
    [[nodiscard]] PaletteLayer& ensure_secondary(u32 fill = kAirStateId);
    [[nodiscard]] PaletteLayer* biome_layer() noexcept { return biomes_.get(); }
    [[nodiscard]] const PaletteLayer* biome_layer() const noexcept { return biomes_.get(); }
    [[nodiscard]] PaletteLayer& ensure_biomes(BiomeId fill);

    [[nodiscard]] SourceVersion source() const noexcept { return source_; }
    [[nodiscard]] usize memory_bytes() const noexcept;

private:
    PaletteLayer blocks_;
    std::unique_ptr<PaletteLayer> secondary_;
    std::unique_ptr<PaletteLayer> biomes_;
    SourceVersion source_;
};

} // namespace bridger
