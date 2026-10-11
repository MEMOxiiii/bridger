#pragma once

// Collects everything that could not be converted exactly, so nothing is lost
// silently (AGENTS.md, safety rules). Phase 3 extends this with mapping-specific
// detail; the contract stays: every non-exact outcome is counted, with examples.

#include <bridger/core/types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bridger::translate {

enum class IssueKind : u8 {
    UnmappedBlock,          // no target state; fallback block written
    LossyBlock,             // mapped, but some properties could not be represented
    UnmappedBlockEntity,
    UnmappedEntity,
    UnmappedBiome,
    DroppedSecondaryLayer,  // e.g. Bedrock layer-2 block with no Java equivalent
    RemovedBlockEntity,     // placement overwrote an existing block entity
    RemovedEntity,          // --clear-entities
    ClippedBlock,           // --clip
};

[[nodiscard]] std::string_view to_string(IssueKind kind) noexcept;

struct IssueEntry {
    u64 count = 0;
    std::vector<BlockPos> examples; // first few positions, capped by kMaxExamples
};

class ConversionReport {
public:
    static constexpr usize kMaxExamples = 8;

    // `subject` identifies what failed, e.g. "minecraft:oak_stairs[facing=east,...]".
    void add(IssueKind kind, std::string_view subject, BlockPos where, u64 count = 1);

    // Merges another report (e.g. from a worker thread) into this one.
    void merge(const ConversionReport& other);

    [[nodiscard]] u64 total(IssueKind kind) const noexcept;
    [[nodiscard]] u64 total() const noexcept;
    [[nodiscard]] bool exact() const noexcept { return total() == 0; }

    using SubjectMap = std::unordered_map<std::string, IssueEntry>;
    [[nodiscard]] const SubjectMap* issues(IssueKind kind) const noexcept;

private:
    std::unordered_map<IssueKind, SubjectMap> issues_;
};

} // namespace bridger::translate
