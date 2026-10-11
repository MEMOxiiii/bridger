#pragma once

// Owning NBT tree: for editing and for preserving data verbatim.
// Byte-exact round-trips are guaranteed: compound entry order (including duplicate
// keys), empty-list element types, raw modified-UTF-8 strings and float bit
// patterns are all preserved.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bridger::io::nbt {

class NbtValue;
struct NbtNamed;

struct NbtList {
    TagType element_type = TagType::End;
    std::vector<NbtValue> items;
};

struct NbtCompound {
    std::vector<NbtNamed> entries;

    // First entry named `name` (raw modified UTF-8), or nullptr.
    [[nodiscard]] NbtValue* find(std::string_view name) noexcept;
    [[nodiscard]] const NbtValue* find(std::string_view name) const noexcept;
    // Replaces the first entry named `name`, or appends it.
    void set(std::string name, NbtValue value);
    // Removes every entry named `name`; returns how many were removed.
    usize erase(std::string_view name);
};

class NbtValue {
public:
    // Alternative index == on-disk tag id (monostate stands for TAG_End).
    using Storage = std::variant<std::monostate, i8, i16, i32, i64, f32, f64, std::vector<i8>, std::string, NbtList,
                                 NbtCompound, std::vector<i32>, std::vector<i64>>;

    NbtValue() = default;
    // Copying is recursive and meant for trusted, shallow data. Parsing, writing,
    // comparing and destroying are iterative, so hostile input at the maximum
    // depth (kMaxNbtDepth) cannot overflow the stack.
    NbtValue(const NbtValue&) = default;
    NbtValue& operator=(const NbtValue&) = default;
    NbtValue(NbtValue&&) noexcept = default;
    NbtValue& operator=(NbtValue&&) noexcept = default;
    ~NbtValue();

    template <typename T>
        requires std::is_constructible_v<Storage, T&&>
    NbtValue(T&& v) : storage_(std::forward<T>(v)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] TagType type() const noexcept { return static_cast<TagType>(storage_.index()); }

    template <typename T>
    [[nodiscard]] T* get_if() noexcept {
        return std::get_if<T>(&storage_);
    }
    template <typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&storage_);
    }

    [[nodiscard]] Storage& storage() noexcept { return storage_; }
    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

    // Deep, bitwise comparison (floats compared by bit pattern).
    friend bool operator==(const NbtValue& a, const NbtValue& b) noexcept;

private:
    Storage storage_;
};

struct NbtNamed {
    std::string name; // raw modified UTF-8
    NbtValue value;
};

// A complete NBT document: a named root tag.
struct NbtRoot {
    std::string name;
    NbtValue value;
    friend bool operator==(const NbtRoot&, const NbtRoot&) = default;
};

// Copies a validated view into an owning tree.
[[nodiscard]] NbtValue materialize(NbtView view);
[[nodiscard]] NbtRoot materialize(const NbtDocument& doc);

// Parses and materializes in one step.
[[nodiscard]] Result<NbtRoot> read_nbt(std::span<const u8> data, Endian endian, const NbtLimits& limits = {});

} // namespace bridger::io::nbt
