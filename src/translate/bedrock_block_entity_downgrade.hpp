#pragma once

// Bedrock block entities rewritten for an older target version (bedrock_block_entity_downgrade.cpp). Internal to
// bridger_translate.

#include <bridger/core/block_entity.hpp>
#include <bridger/translate/targets.hpp>

#include <optional>
#include <string>
#include <vector>

namespace bridger::translate::detail {

// The block entity's NBT as `target` stores it; nullopt: unchanged. `lossy` collects short notes for the report.
[[nodiscard]] std::optional<std::vector<u8>> downgrade_bedrock_block_entity(const BlockEntity& be, const BedrockTargets& targets,
                                                                           const BedrockTarget& target, const MappingData& mapping,
                                                                           std::vector<std::string>& lossy);

} // namespace bridger::translate::detail
