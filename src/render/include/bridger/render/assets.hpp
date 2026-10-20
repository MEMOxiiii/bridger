#pragma once

// Minecraft Java assets, loaded at runtime from the user's own files (never shipped, AGENTS.md
// safety rules): the client jar of the pinned version plus optional resource packs (zips or
// folders) layered on top, highest priority last, as the game does (docs/formats/assets.md).

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::render {

class AssetSource {
public:
    // `packs`: zip/jar files or folders, lowest priority first (put the client jar first).
    [[nodiscard]] static Result<AssetSource> open(const std::vector<std::filesystem::path>& packs);
    AssetSource(AssetSource&&) noexcept;
    AssetSource& operator=(AssetSource&&) noexcept;
    ~AssetSource();

    // A file such as "assets/minecraft/models/block/stone.json" from the highest-priority pack
    // that has it. Thread-safe.
    [[nodiscard]] std::optional<std::vector<u8>> read(std::string_view path) const;
    [[nodiscard]] usize pack_count() const noexcept;

    // The launcher's client jar for `version` (e.g. "26.3") in the game folder (platform::minecraft_java_dir), if present.
    [[nodiscard]] static std::optional<std::filesystem::path> find_client_jar(std::string_view version);

private:
    AssetSource();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::render
