#pragma once

// Target game versions: writing for an older version than the pinned one (docs/versions.md, "Target
// versions"). Data: data/java-targets.nbt (block states of every Java release since 1.18, from Mojang's data
// generator; tools/mapgen/build_java_targets.py) and data/bedrock-targets.nbt (how each pinned Bedrock state is
// written for every Bedrock version since 1.18.30, from BedrockBlockPaletteArchive; build_bedrock_targets.py).
//
// The translators always produce pinned-version states; these passes then rewrite them for the target, in the
// palette stage, and report every state the target cannot hold exactly.

#include <bridger/core/block_state.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::translate {

// ---- Java -------------------------------------------------------------------------------------------

struct JavaTarget {
    std::string name;     // "1.21.4"
    i32 data_version = 0; // written into the output
    bool pinned = false;  // the version the mapping data is for (no rewriting)
    usize index = 0;      // internal
};

class JavaTargets {
public:
    [[nodiscard]] static Result<const JavaTargets*> embedded();
    [[nodiscard]] static Result<JavaTargets> load(std::span<const u8> nbt);
    JavaTargets(JavaTargets&&) noexcept;
    JavaTargets& operator=(JavaTargets&&) noexcept;
    ~JavaTargets();

    [[nodiscard]] std::span<const JavaTarget> targets() const noexcept; // newest (pinned) first
    [[nodiscard]] const JavaTarget* find(std::string_view name) const noexcept;
    [[nodiscard]] const JavaTarget& pinned() const noexcept;

    enum class Fit : u8 { Exact, Approximate, Missing };
    // A pinned-version state as `target` spells it. Approximate: a property or value the target lacks was
    // dropped or replaced by its default. Missing: the block does not exist in `target` (`out` untouched).
    [[nodiscard]] Fit downgrade(const BlockState& state, const JavaTarget& target, BlockState& out) const;
    // Whether `target` has this block id at all (e.g. biomes and items are checked elsewhere).
    [[nodiscard]] bool has_block(std::string_view id, const JavaTarget& target) const;
    [[nodiscard]] bool has_item(std::string_view id, const JavaTarget& target) const;
    // The experimental feature a block needs in `target` ("update_1_20" for cherry wood in 1.19.4), or empty: the
    // block is a regular part of that version. Mojang's reports list feature-flagged blocks without marking them.
    [[nodiscard]] std::string_view experimental_feature(std::string_view id, const JavaTarget& target) const;

private:
    JavaTargets();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Rewrites a Java structure (pinned states and block entity forms) for `target`: renamed and reshaped states are
// respelled, states the target cannot hold exactly are reported (LossyBlock), blocks it does not have become
// `fallback` (UnmappedBlock), biomes it does not have become the closest it has (UnmappedBiome), and block
// entities are stored in the target's format (items, text, signs, banners). Returns the number of blocks
// changed. No-op for the pinned target.
struct JavaRewrite {
    u64 changed_blocks = 0;
    std::vector<std::string> features; // experimental features the written blocks need (see experimental_feature)
};
[[nodiscard]] Result<JavaRewrite> rewrite_for_java_target(Structure& java, const JavaTargets& targets, const JavaTarget& target,
                                                         const BlockState& fallback, ConversionReport& report);

// ---- Bedrock ----------------------------------------------------------------------------------------

struct BedrockTarget {
    std::string name;                 // "1.21.50", "26.40" (bedrock_version_name)
    std::array<i32, 5> version{};     // level.dat lastOpenedWithVersion / MinimumCompatibleClientVersion
    i32 protocol = 0;                 // level.dat NetworkVersion
    i32 storage_version = 0;          // level.dat header and StorageVersion
    i32 chunk_version = 0;            // the Version key byte of every chunk
    i32 block_version = 0;            // "version" of palette entries
    bool pinned = false;
    usize palette = 0;                // internal
};

class BedrockTargets {
public:
    [[nodiscard]] static Result<const BedrockTargets*> embedded();
    [[nodiscard]] static Result<BedrockTargets> load(std::span<const u8> nbt);
    BedrockTargets(BedrockTargets&&) noexcept;
    BedrockTargets& operator=(BedrockTargets&&) noexcept;
    ~BedrockTargets();

    [[nodiscard]] std::span<const BedrockTarget> targets() const noexcept; // newest (pinned) first
    [[nodiscard]] const BedrockTarget* find(std::string_view name) const noexcept;
    [[nodiscard]] const BedrockTarget& pinned() const noexcept;

    struct Written {
        enum class Kind : u8 { Same, Respelled, Nearest, Missing } kind = Kind::Same;
        BedrockStateIndex nearest = 0; // Nearest: the pinned state written instead (same block, closest states)
        i32 spelling = -1;             // Respelled: index of the target's {name, states}
    };
    [[nodiscard]] Written resolve(const BedrockTarget& target, BedrockStateIndex state) const;
    // Little-endian NBT root {name, states, version} of pinned state `state` for `target` (after
    // rewrite_for_bedrock_target, only Same and Respelled states remain). Cached; thread-safe.
    [[nodiscard]] std::span<const u8> state_nbt(const BedrockTarget& target, BedrockStateIndex state, const MappingData& mapping) const;
    // An item id and data value (pinned names) as `target` names it (BedrockItemUpgradeSchema, applied backwards).
    [[nodiscard]] std::pair<std::string, i16> item_for(const BedrockTarget& target, std::string_view name, i16 damage) const;

private:
    BedrockTargets();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Rewrites a Bedrock-domain structure (pinned state indices in its block layers) for `target`: Nearest states
// become their substitute (LossyBlock), Missing ones become `fallback` (UnmappedBlock), newer biomes the closest
// the target has (UnmappedBiome), and block entities take the target's form (signs before 1.19.80, item names,
// the Block tag of items). No-op for the pinned target. Returns the number of blocks changed.
[[nodiscard]] Result<u64> rewrite_for_bedrock_target(Structure& bedrock, const BedrockTargets& targets, const BedrockTarget& target,
                                                    const MappingData& mapping, BedrockStateIndex fallback, ConversionReport& report);

} // namespace bridger::translate
