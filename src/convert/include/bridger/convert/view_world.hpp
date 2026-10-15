#pragma once

// A world opened for viewing, one chunk column at a time: a Java world folder (Anvil regions) or
// a Bedrock .mcworld (translated to Java states, since the viewer draws Java block models).
// Opening never modifies the input; a Bedrock world is unzipped into a scratch directory that is
// removed when the object is destroyed.

#include <bridger/convert/source_version.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/formats/format.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bridger::convert {

struct ViewColumn {
    ChunkPos pos;
    i32 min_y = -64;
    Structure column; // Java edition
};

struct ViewWorldInfo {
    formats::FormatId format = formats::FormatId::JavaWorld;
    std::string level_name;
    BlockPos spawn{0, 64, 0};
    std::optional<i64> seed;
    usize chunk_count = 0;
    GameVersionInfo source_version;
};

class ViewWorld {
public:
    [[nodiscard]] static Result<ViewWorld> open(const std::filesystem::path& path);
    ViewWorld(ViewWorld&&) noexcept;
    ViewWorld& operator=(ViewWorld&&) noexcept;
    ~ViewWorld();

    [[nodiscard]] const ViewWorldInfo& info() const noexcept;
    [[nodiscard]] const std::vector<ChunkPos>& chunks() const noexcept;
    // Thread-safe. nullopt if the column is absent, not fully generated, or unreadable.
    [[nodiscard]] std::optional<ViewColumn> load(ChunkPos pos) const;

private:
    ViewWorld();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// True if `path` names a world (a Java world folder or level.dat, or a .mcworld file).
[[nodiscard]] bool is_world_path(const std::filesystem::path& path);

} // namespace bridger::convert
