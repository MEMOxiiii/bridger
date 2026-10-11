#include <bridger/core/section.hpp>

#include <algorithm>
#include <cassert>

namespace bridger {
namespace {

// Per-thread open-addressing table for assign(): value -> palette slot. Entries are
// invalidated by bumping a generation counter instead of clearing the table.
struct AssignTable {
    static constexpr usize kSlots = 8192; // > 2 * 4096, power of two
    std::array<u32, kSlots> keys{};
    std::array<u16, kSlots> slots{};
    std::array<u32, kSlots> generation{};
    u32 current = 0;

    void reset() {
        if (++current == 0) { // wrapped: really clear
            generation.fill(0);
            current = 1;
        }
    }
    // Returns the slot for `key`, inserting `next` if absent (and setting `inserted`).
    u16 lookup(u32 key, u16 next, bool& inserted) {
        usize h = (key * 0x9E3779B1u) & (kSlots - 1);
        for (;;) {
            if (generation[h] != current) {
                generation[h] = current;
                keys[h] = key;
                slots[h] = next;
                inserted = true;
                return next;
            }
            if (keys[h] == key) {
                inserted = false;
                return slots[h];
            }
            h = (h + 1) & (kSlots - 1);
        }
    }
};

} // namespace

u16 PaletteLayer::find_or_add(u32 value) {
    for (usize i = 0; i < palette_.size(); ++i)
        if (palette_[i] == value) return static_cast<u16>(i);
    // A layer holds 4096 positions, so a palette that has outgrown that is full of
    // stale entries from set(); compact before growing further.
    if (palette_.size() >= kSectionVolume) {
        compact();
        for (usize i = 0; i < palette_.size(); ++i)
            if (palette_[i] == value) return static_cast<u16>(i);
    }
    palette_.push_back(value);
    return static_cast<u16>(palette_.size() - 1);
}

void PaletteLayer::set(usize index, u32 value) {
    assert(index < kSectionVolume);
    if (width_ == Width::Uniform) {
        if (palette_[0] == value) return;
        idx8_.assign(kSectionVolume, 0);
        width_ = Width::U8;
    }
    const u16 slot = find_or_add(value);
    if (width_ == Width::U8 && slot > 0xFF) {
        idx16_.assign(idx8_.begin(), idx8_.end());
        idx8_.clear();
        idx8_.shrink_to_fit();
        width_ = Width::U16;
    }
    if (width_ == Width::U8)
        idx8_[index] = static_cast<u8>(slot);
    else
        idx16_[index] = slot;
}

void PaletteLayer::fill(u32 value) {
    palette_.assign(1, value);
    idx8_.clear();
    idx8_.shrink_to_fit();
    idx16_.clear();
    idx16_.shrink_to_fit();
    width_ = Width::Uniform;
}

void PaletteLayer::assign(std::span<const u32, kSectionVolume> values) {
    thread_local AssignTable table;
    table.reset();

    std::vector<u32> palette;
    std::array<u16, kSectionVolume> indices; // NOLINT(cppcoreguidelines-pro-type-member-init): fully written below
    u32 last_value = values[0];
    bool inserted = false;
    u16 last_slot = table.lookup(last_value, 0, inserted);
    palette.push_back(last_value);
    for (usize i = 0; i < kSectionVolume; ++i) {
        const u32 v = values[i];
        if (v != last_value) { // runs are common: skip the hash lookup
            last_slot = table.lookup(v, static_cast<u16>(palette.size()), inserted);
            if (inserted) palette.push_back(v);
            last_value = v;
        }
        indices[i] = last_slot;
    }

    palette_ = std::move(palette);
    idx8_.clear();
    idx16_.clear();
    if (palette_.size() == 1) {
        width_ = Width::Uniform;
        idx8_.shrink_to_fit();
        idx16_.shrink_to_fit();
    } else if (palette_.size() <= 256) {
        width_ = Width::U8;
        idx8_.resize(kSectionVolume);
        for (usize i = 0; i < kSectionVolume; ++i) idx8_[i] = static_cast<u8>(indices[i]);
        idx16_.shrink_to_fit();
    } else {
        width_ = Width::U16;
        idx16_.assign(indices.begin(), indices.end());
        idx8_.shrink_to_fit();
    }
}

void PaletteLayer::remap(std::span<const u32> new_values) {
    assert(new_values.size() == palette_.size());
    std::copy(new_values.begin(), new_values.end(), palette_.begin());
}

void PaletteLayer::copy_to(std::span<u32, kSectionVolume> out) const noexcept {
    switch (width_) {
    case Width::Uniform: std::fill(out.begin(), out.end(), palette_[0]); return;
    case Width::U8:
        for (usize i = 0; i < kSectionVolume; ++i) out[i] = palette_[idx8_[i]];
        return;
    case Width::U16:
        for (usize i = 0; i < kSectionVolume; ++i) out[i] = palette_[idx16_[i]];
        return;
    }
}

void PaletteLayer::compact() {
    if (width_ == Width::Uniform) return;
    std::array<u32, kSectionVolume> values; // NOLINT(cppcoreguidelines-pro-type-member-init)
    copy_to(values);
    assign(values);
}

usize PaletteLayer::memory_bytes() const noexcept {
    return sizeof(*this) + palette_.capacity() * sizeof(u32) + idx8_.capacity() + idx16_.capacity() * sizeof(u16);
}

Section::Section(const Section& other)
    : blocks_(other.blocks_), secondary_(other.secondary_ ? std::make_unique<PaletteLayer>(*other.secondary_) : nullptr),
      biomes_(other.biomes_ ? std::make_unique<PaletteLayer>(*other.biomes_) : nullptr), source_(other.source_) {}

Section& Section::operator=(const Section& other) {
    if (this != &other) {
        Section copy(other);
        *this = std::move(copy);
    }
    return *this;
}

PaletteLayer& Section::ensure_secondary() {
    if (!secondary_) secondary_ = std::make_unique<PaletteLayer>(kAirStateId);
    return *secondary_;
}

void Section::set_secondary(i32 x, i32 y, i32 z, StateId id) {
    if (!secondary_ && id == kAirStateId) return; // stays unallocated
    ensure_secondary().set(section_index(x, y, z), id);
}

PaletteLayer& Section::ensure_biomes(BiomeId fill) {
    if (!biomes_) biomes_ = std::make_unique<PaletteLayer>(fill);
    return *biomes_;
}

void Section::set_biome(i32 x, i32 y, i32 z, BiomeId id) {
    ensure_biomes(id).set(section_index(x, y, z), id);
}

usize Section::memory_bytes() const noexcept {
    usize n = sizeof(*this) - sizeof(PaletteLayer) + blocks_.memory_bytes();
    if (secondary_) n += secondary_->memory_bytes();
    if (biomes_) n += biomes_->memory_bytes();
    return n;
}

} // namespace bridger
