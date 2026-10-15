#pragma once

// Java block entities rewritten for an older target version (java_block_entity_downgrade.cpp). Internal to
// bridger_translate.

#include <bridger/core/block_entity.hpp>
#include <bridger/translate/targets.hpp>

#include <optional>
#include <string>
#include <vector>

namespace bridger::translate::detail {

// The block entity's NBT as `target` stores it; nullopt: unchanged (pinned target, or unreadable data).
// `lossy` and `dropped` collect short notes for the report (data the target cannot hold; items it lacks).
[[nodiscard]] std::optional<std::vector<u8>> downgrade_java_block_entity(const BlockEntity& be, const JavaTargets& targets,
                                                                        const JavaTarget& target, std::vector<std::string>& lossy,
                                                                        std::vector<std::string>& dropped);

} // namespace bridger::translate::detail
