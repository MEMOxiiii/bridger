#pragma once

// Interning tables: a canonical key maps to a dense uint32 id. Hot-path code works
// with ids only. Both tables are thread-safe; returned references stay valid for
// the table's lifetime.
//
// Ids are only meaningful together with the source edition/version of the data
// that produced them (see Section::source()); translation (Phase 3) maps them.

#include <bridger/core/block_state.hpp>
#include <bridger/core/types.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bridger {

using StateId = u32;
using BiomeId = u32;

inline constexpr StateId kAirStateId = 0; // "minecraft:air" is always interned first

// String -> id interning.
class StringInterner {
public:
    [[nodiscard]] u32 intern(std::string_view key);
    [[nodiscard]] std::optional<u32> find(std::string_view key) const;
    [[nodiscard]] const std::string& name(u32 id) const; // id must be valid
    [[nodiscard]] usize size() const;

private:
    struct Hash {
        using is_transparent = void;
        usize operator()(std::string_view s) const noexcept;
    };
    mutable std::shared_mutex mutex_;
    std::deque<std::string> names_; // stable addresses
    std::unordered_map<std::string_view, u32, Hash, std::equal_to<>> ids_;
};

class BlockStateRegistry {
public:
    BlockStateRegistry(); // interns minecraft:air as kAirStateId

    [[nodiscard]] StateId intern(const BlockState& state);
    [[nodiscard]] std::optional<StateId> find(const BlockState& state) const;
    [[nodiscard]] const BlockState& get(StateId id) const; // id must be valid
    [[nodiscard]] std::string_view canonical(StateId id) const { return keys_.name(id); }
    [[nodiscard]] usize size() const { return keys_.size(); }

private:
    StringInterner keys_; // canonical text form
    mutable std::shared_mutex mutex_;
    std::deque<BlockState> states_; // index == id
};

using BiomeRegistry = StringInterner;

} // namespace bridger
