#include "entity_types.hpp"

#include <charconv>

namespace bridger::translate::detail {
namespace {

#include "entity_types.inc"

std::optional<std::string_view> id_in(std::span<const IdSince> history, GameVersion3 version) noexcept {
    std::optional<std::string_view> id;
    for (const IdSince& e : history)
        if (e.since <= version) id = e.id; // later entries rename the type
    return id;
}

GameVersion3 parse3(std::string_view name) noexcept {
    GameVersion3 v{0, 0, 0};
    usize part = 0;
    while (!name.empty() && part < 3) {
        const usize dot = name.find('.');
        const std::string_view piece = name.substr(0, dot);
        i32 n = 0;
        const auto [ptr, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), n);
        if (ec != std::errc() || ptr != piece.data() + piece.size()) return {99, 0, 0};
        v[part++] = n;
        name = dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1);
    }
    return part == 0 ? GameVersion3{99, 0, 0} : v;
}

} // namespace

const EntityTypeRow* entity_type_by_java(std::string_view java_id) noexcept {
    for (const EntityTypeRow& r : kEntityTypes)
        for (const IdSince& e : r.java)
            if (e.id == java_id) return &r;
    return nullptr;
}

const EntityTypeRow* entity_type_by_bedrock(std::string_view bedrock_id) noexcept {
    for (const EntityTypeRow& r : kEntityTypes)
        for (const IdSince& e : r.bedrock)
            if (e.id == bedrock_id) return &r;
    return nullptr;
}

std::optional<std::string_view> java_entity_id(const EntityTypeRow& row, GameVersion3 version) noexcept { return id_in(row.java, version); }

std::optional<std::string_view> bedrock_entity_id(const EntityTypeRow& row, GameVersion3 version) noexcept {
    return id_in(row.bedrock, version);
}

GameVersion3 java_version3(std::string_view name) noexcept { return name.empty() ? GameVersion3{99, 0, 0} : parse3(name); }

GameVersion3 bedrock_version3(std::string_view name) noexcept {
    if (name.empty()) return {99, 0, 0};
    const GameVersion3 v = parse3(name);
    return v[0] >= 2 && v[0] != 99 ? GameVersion3{1, v[0], v[1]} : v; // "26.50" is 1.26.50
}

} // namespace bridger::translate::detail
