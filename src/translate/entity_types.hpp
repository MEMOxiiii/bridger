#pragma once

// Vanilla entity types and their identifiers in both editions, by game version (generated from Chunker's type
// resolvers: entity_types.inc, tools/mapgen/build_entity_types.py). Internal to bridger_translate.

#include <bridger/core/types.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace bridger::translate::detail {

using GameVersion3 = std::array<i32, 3>; // {major, minor, patch}; Bedrock 26.50 is {1, 26, 50} (Chunker's convention)

struct IdSince {
    GameVersion3 since;
    std::string_view id;
};

struct EntityTypeRow {
    std::string_view name; // Chunker's type name, lower case: "sheep", "iron_golem"
    std::span<const IdSince> java;
    std::span<const IdSince> bedrock;
};

// The type of an identifier under any of its names (old ones included); nullptr when unknown.
[[nodiscard]] const EntityTypeRow* entity_type_by_java(std::string_view java_id) noexcept;
[[nodiscard]] const EntityTypeRow* entity_type_by_bedrock(std::string_view bedrock_id) noexcept;

// The identifier the type has in `version`; nullopt when the type does not exist there yet (or not in that edition).
[[nodiscard]] std::optional<std::string_view> java_entity_id(const EntityTypeRow& row, GameVersion3 version) noexcept;
[[nodiscard]] std::optional<std::string_view> bedrock_entity_id(const EntityTypeRow& row, GameVersion3 version) noexcept;

// "1.20.1" -> {1, 20, 1}, "26.3" -> {26, 3, 0}; Bedrock "26.50" -> {1, 26, 50}, "1.21.50" -> {1, 21, 50}. Empty or
// unreadable: the newest version ({99, 0, 0}).
[[nodiscard]] GameVersion3 java_version3(std::string_view name) noexcept;
[[nodiscard]] GameVersion3 bedrock_version3(std::string_view name) noexcept;

} // namespace bridger::translate::detail
