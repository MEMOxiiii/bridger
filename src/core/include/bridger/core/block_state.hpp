#pragma once

// Canonical block state: namespaced id + properties sorted by key.
// Text form (used by Sponge palettes and for display): "minecraft:oak_stairs[facing=east,half=top]".

#include <bridger/core/result.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridger {

struct BlockProperty {
    std::string name;
    std::string value;
    friend bool operator==(const BlockProperty&, const BlockProperty&) = default;
};

class BlockState {
public:
    BlockState() = default;
    // Properties are sorted; duplicate keys are rejected by parse(), and here the last one wins.
    BlockState(std::string id, std::vector<BlockProperty> properties);

    // Parses "ns:id[k=v,...]". A missing namespace means "minecraft:". Whitespace
    // is not allowed. Duplicate property keys are an error.
    [[nodiscard]] static Result<BlockState> parse(std::string_view text);

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::vector<BlockProperty>& properties() const noexcept { return properties_; }
    [[nodiscard]] std::optional<std::string_view> property(std::string_view name) const noexcept;

    // Canonical text form (properties sorted, brackets omitted when empty).
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const BlockState&, const BlockState&) = default;

private:
    std::string id_;
    std::vector<BlockProperty> properties_; // sorted by name, unique
};

inline constexpr std::string_view kAirId = "minecraft:air";

} // namespace bridger
