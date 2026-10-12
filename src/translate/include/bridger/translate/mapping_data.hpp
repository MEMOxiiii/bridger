#pragma once

// Compiled Java <-> Bedrock mapping data (.brmap), produced by tools/mapgen from the
// sources in docs/adr/0007-mapping-data-sources.md, for exactly one Java version and
// one Bedrock version (docs/versions.md). The blob is embedded in the binary.
//
// Bedrock states are identified by their index in the pinned Bedrock block palette
// ("BedrockStateIndex"). IR sections whose source edition is Bedrock store these
// indices directly (docs/adr/0008-translation-design.md).
//
// .brmap layout (all integers little-endian, strings = u16 length + UTF-8 bytes):
//   "BRMAP\0" u16 format_version
//   u32 java_data_version, str java_version, str bedrock_version, u32 bedrock_block_version
//   u32 string_count, string_count * str                      (string table)
//   u32 java_block_count, per block:
//       u32 name, u32 first_state, u32 state_count, u32 default_state, u8 prop_count,
//       prop_count * { u32 name, u8 value_count, value_count * u32 value },
//       state_count * prop_count * u8 value_index               (explicit, in state id order)
//   u32 java_state_count, java_state_count * { u32 bedrock_index, u8 flags }
//   u32 bedrock_state_count, per state:
//       u32 name, u8 prop_count, prop_count * { u32 key, u8 nbt_type (1/3/8), i32 value or u32 string }
//   u32 item_count, item_count * { u32 java_name, u32 bedrock_name, i16 damage, u32 java_block_state or 0xFFFFFFFF }
//   u32 biome_count, biome_count * { u32 java_name, i32 bedrock_id }

#include <bridger/core/block_state.hpp>
#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::translate {

using JavaStateId = u32;        // Java block state id of the pinned Java version
using BedrockStateIndex = u32;  // index into the pinned Bedrock block palette

inline constexpr u16 kBrmapFormatVersion = 1;

// Per-Java-state flags stored in the blob.
enum MappingFlags : u8 {
    kFlagWaterlogged = 1 << 0, // the Java state has waterlogged=true: Bedrock needs water in layer 2
    kFlagOverride = 1 << 1,    // mapped by a Bridger override, not the third-party source
    kFlagLossy = 1 << 2,       // the Bedrock state cannot represent everything (per override)
};

struct JavaStateMapping {
    BedrockStateIndex bedrock = 0;
    u8 flags = 0;
};

enum class NbtValueType : u8 { Byte = 1, Int = 3, String = 8 };

struct BedrockProperty {
    std::string_view key;
    NbtValueType type = NbtValueType::Int;
    i32 int_value = 0;           // Byte/Int
    std::string_view str_value;  // String
};

struct BedrockState {
    std::string_view name; // "minecraft:oak_stairs"
    std::vector<BedrockProperty> properties;
};

struct ItemMapping {
    std::string_view bedrock_name;
    i16 damage = 0;                              // Bedrock "Damage" (legacy data value) for this item
    std::optional<JavaStateId> java_block_state; // block items: the state used for the Bedrock "Block" tag
};

class MappingData {
public:
    // Parses and validates a .brmap blob. The bytes must outlive the object.
    [[nodiscard]] static Result<MappingData> load(std::span<const u8> blob);
    // The blob embedded in this binary (loaded once, thread-safe).
    [[nodiscard]] static Result<const MappingData*> embedded();

    [[nodiscard]] u32 java_data_version() const noexcept { return java_data_version_; }
    [[nodiscard]] std::string_view java_version() const noexcept { return java_version_; }
    [[nodiscard]] std::string_view bedrock_version() const noexcept { return bedrock_version_; }
    [[nodiscard]] u32 bedrock_block_version() const noexcept { return bedrock_block_version_; }

    [[nodiscard]] usize java_state_count() const noexcept { return java_mappings_.size(); }
    [[nodiscard]] usize bedrock_state_count() const noexcept { return bedrock_states_.size(); }

    // Java state for a canonical block state. Properties missing from `state` take the
    // block's default value; `missing_properties` is set when that happened.
    // nullopt: unknown block, unknown property or value.
    [[nodiscard]] std::optional<JavaStateId> find_java_state(const BlockState& state,
                                                            bool* missing_properties = nullptr) const;
    [[nodiscard]] std::string java_state_string(JavaStateId id) const;

    [[nodiscard]] JavaStateMapping map(JavaStateId id) const noexcept { return java_mappings_[id]; }
    [[nodiscard]] const BedrockState& bedrock(BedrockStateIndex i) const noexcept { return bedrock_states_[i]; }
    // Index of a Bedrock state by name + exact properties (e.g. water for layer 2).
    [[nodiscard]] std::optional<BedrockStateIndex> find_bedrock(std::string_view name,
                                                               std::span<const BedrockProperty> props) const;

    [[nodiscard]] std::optional<ItemMapping> find_item(std::string_view java_item) const;
    [[nodiscard]] std::optional<i32> find_biome(std::string_view java_biome) const;

    struct Index; // lookup tables built at load time (defined in mapping_data.cpp)

    MappingData(MappingData&&) noexcept;
    MappingData& operator=(MappingData&&) noexcept;
    ~MappingData();

private:
    MappingData();

    u32 java_data_version_ = 0;
    std::string_view java_version_;
    std::string_view bedrock_version_;
    u32 bedrock_block_version_ = 0;
    std::vector<JavaStateMapping> java_mappings_;
    std::vector<BedrockState> bedrock_states_;
    std::unique_ptr<Index> index_;
};

} // namespace bridger::translate
