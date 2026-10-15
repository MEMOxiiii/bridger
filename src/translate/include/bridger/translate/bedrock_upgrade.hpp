#pragma once

// Upgrading block states from old Bedrock worlds and structures to the pinned Bedrock palette
// (docs/formats/bedrock-world.md, "Old worlds"). Data: BedrockBlockUpgradeSchema (CC0), converted
// by tools/mapgen/build_bedrock_upgrade.py into the embedded data/bedrock-upgrade.nbt.
//
// Palette entries have had three forms:
//   {name, val}                       1.2.13 - 1.12: string id + legacy data value
//   {name, states, version}           1.13+: typed states, upgraded schema by schema
//   numeric id + data                 before 1.2.13 (no palette); the world reader passes these as
//                                     {"bridger:legacy_id": Int, "val": Short} (Bridger's own form)

#include <bridger/core/result.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::translate {

inline constexpr std::string_view kLegacyIdKey = "bridger:legacy_id";

struct UpgradeValue {
    NbtValueType type = NbtValueType::Int;
    i32 int_value = 0;      // Byte/Int
    std::string str_value;  // String
    friend bool operator==(const UpgradeValue&, const UpgradeValue&) = default;
};

struct UpgradedState {
    std::string name;
    std::vector<std::pair<std::string, UpgradeValue>> states;
    i32 version = 0;

    // Views for MappingData::find_bedrock (valid while this object lives).
    [[nodiscard]] std::vector<BedrockProperty> properties() const;
};

class BedrockUpgrader {
public:
    // The embedded data, parsed on first use.
    [[nodiscard]] static Result<const BedrockUpgrader*> embedded();
    [[nodiscard]] static Result<BedrockUpgrader> load(std::span<const u8> nbt);

    BedrockUpgrader(BedrockUpgrader&&) noexcept;
    BedrockUpgrader& operator=(BedrockUpgrader&&) noexcept;
    ~BedrockUpgrader();

    // Applies every schema newer than `state.version` (the rule of PocketMine-MP's
    // BlockStateUpgrader: a version that several schemas share is applied again, because Mojang
    // changed states without bumping it).
    [[nodiscard]] UpgradedState upgrade(UpgradedState state) const;
    // (string id, data) -> its 1.12 state, upgraded; data values the table lacks use data 0.
    [[nodiscard]] std::optional<UpgradedState> from_legacy(std::string_view id, i32 data) const;
    [[nodiscard]] std::optional<UpgradedState> from_legacy_id(i32 numeric_id, i32 data) const;
    // Any of the three entry forms above; nullopt if the entry is malformed or its legacy id unknown.
    [[nodiscard]] std::optional<UpgradedState> from_entry(io::nbt::NbtCompoundView entry) const;

    [[nodiscard]] usize schema_count() const noexcept;
    [[nodiscard]] i32 latest_version() const noexcept;

private:
    BedrockUpgrader();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::translate
