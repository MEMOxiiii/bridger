#include <bridger/core/block_registry.hpp>

#include <xxhash.h>

#include <mutex>

namespace bridger {

usize StringInterner::Hash::operator()(std::string_view s) const noexcept {
    return static_cast<usize>(XXH3_64bits(s.data(), s.size()));
}

u32 StringInterner::intern(std::string_view key) {
    {
        std::shared_lock lock(mutex_);
        if (const auto it = ids_.find(key); it != ids_.end()) return it->second;
    }
    std::unique_lock lock(mutex_);
    if (const auto it = ids_.find(key); it != ids_.end()) return it->second; // raced
    const auto id = static_cast<u32>(names_.size());
    const std::string& stored = names_.emplace_back(key);
    ids_.emplace(std::string_view(stored), id);
    return id;
}

std::optional<u32> StringInterner::find(std::string_view key) const {
    std::shared_lock lock(mutex_);
    if (const auto it = ids_.find(key); it != ids_.end()) return it->second;
    return std::nullopt;
}

const std::string& StringInterner::name(u32 id) const {
    std::shared_lock lock(mutex_);
    return names_.at(id);
}

usize StringInterner::size() const {
    std::shared_lock lock(mutex_);
    return names_.size();
}

BlockStateRegistry::BlockStateRegistry() {
    const StateId air = intern(BlockState(std::string(kAirId), {}));
    (void)air; // always kAirStateId: first entry
}

StateId BlockStateRegistry::intern(const BlockState& state) {
    const std::string key = state.to_string();
    // Hold our lock across both steps so ids_ and states_ stay index-aligned.
    std::unique_lock lock(mutex_);
    const u32 id = keys_.intern(key);
    if (id == states_.size()) states_.push_back(state);
    return id;
}

std::optional<StateId> BlockStateRegistry::find(const BlockState& state) const { return keys_.find(state.to_string()); }

const BlockState& BlockStateRegistry::get(StateId id) const {
    std::shared_lock lock(mutex_);
    return states_.at(id);
}

} // namespace bridger
