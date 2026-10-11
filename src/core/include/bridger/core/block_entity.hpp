#pragma once

// Block entities and preserved NBT.
//
// core cannot depend on io (dependency rule), so NBT is kept here as encoded bytes:
// a complete NBT document (unnamed root compound) in the source edition's byte
// order. bridger_io decodes it when needed. Keeping the original bytes is what
// lets writers reuse data verbatim when the strict conditions of ADR 0001 hold,
// and translate it otherwise. Position keys inside it are never trusted:
// writers regenerate positions from the IR.

#include <bridger/core/types.hpp>

#include <string>
#include <vector>

namespace bridger {

struct RawNbt {
    std::vector<u8> bytes; // empty = no data
    Endian endian = Endian::Big;

    [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
    friend bool operator==(const RawNbt&, const RawNbt&) = default;
};

struct BlockEntity {
    BlockPos pos;   // relative to the owning structure/world origin
    std::string id; // e.g. "minecraft:chest"
    RawNbt data;    // type-specific fields as found in the source
};

} // namespace bridger
