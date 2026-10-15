#pragma once

// On-disk cache of column summaries for distant terrain (mesher.hpp, ColumnSummary), so reopening a
// world shows its far terrain without decoding those chunks again (docs/formats/assets.md, "Distant
// terrain"). One file per 32 x 32 region, `r.<x>.<z>.blod`:
//   "BLOD" | u32 version (1) | i32 region x | i32 region z | 128-byte presence bitmap (slot = x + z*32)
//   | per present slot, in slot order: 256 x i16 top, 256 x u32 colour | u64 FNV-1a of everything before
// all little-endian. A file that fails any check is ignored (and rewritten). The caller picks a
// directory that already identifies the world, its modification time and the resource packs, so an
// entry is never stale.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/render/mesher.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace bridger::render {

class LodCache {
public:
    explicit LodCache(std::filesystem::path dir);
    ~LodCache();
    LodCache(const LodCache&) = delete;
    LodCache& operator=(const LodCache&) = delete;

    // Thread-safe.
    [[nodiscard]] std::optional<ColumnSummary> get(ChunkPos pos);
    void put(ChunkPos pos, const ColumnSummary& summary);
    // Writes the regions changed since the last flush (atomic replace per file).
    [[nodiscard]] Status flush();

    [[nodiscard]] const std::filesystem::path& dir() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::render
