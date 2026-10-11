#pragma once

// NBT writer: a streaming API for format writers (no tree needed) plus encoding
// of whole NbtValue trees. Strings are written as given (raw modified UTF-8); use
// mutf8.hpp to convert from UTF-8.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/nbt/nbt_value.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/tag.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace bridger::io::nbt {

class NbtWriter {
public:
    explicit NbtWriter(Endian endian) : out_(endian) {}

    // Every document starts with exactly one root.
    void begin_root_compound(std::string_view name = {});

    // Inside a compound every call needs a name; inside a list the name is ignored
    // and the element type and count declared in begin_list() are enforced.
    void begin_compound(std::string_view name = {});
    void end_compound();
    void begin_list(std::string_view name, TagType element_type, usize count);
    void end_list();

    void write_byte(std::string_view name, i8 v);
    void write_short(std::string_view name, i16 v);
    void write_int(std::string_view name, i32 v);
    void write_long(std::string_view name, i64 v);
    void write_float(std::string_view name, f32 v);
    void write_double(std::string_view name, f64 v);
    void write_string(std::string_view name, std::string_view raw_mutf8);
    void write_byte_array(std::string_view name, std::span<const u8> bytes);
    void write_int_array(std::string_view name, std::span<const i32> values);
    void write_long_array(std::string_view name, std::span<const i64> values);
    // Writes any value (recursively) as a named entry or list element.
    void write_value(std::string_view name, const NbtValue& value);
    // Copies a validated view verbatim; its endianness may differ from the writer's.
    void write_view(std::string_view name, NbtView view);

    // Fails if the document is incomplete or the API was misused (e.g. wrong list
    // element type, too many elements, strings longer than 65535 bytes).
    [[nodiscard]] Result<std::vector<u8>> finish() &&;

private:
    struct Frame {
        bool is_list;
        TagType element_type; // lists only
        usize remaining;      // lists only
    };

    bool begin_tag(std::string_view name, TagType type); // header or list-element bookkeeping
    void write_string_payload(std::string_view raw);
    void write_payload(const NbtValue& value);
    void write_view_payload(NbtView view);
    void fail(const char* message);

    BinaryWriter out_;
    std::vector<Frame> stack_;
    bool root_written_ = false;
    const char* error_ = nullptr;
};

// Encodes a complete document.
[[nodiscard]] Result<std::vector<u8>> write_nbt(const NbtRoot& root, Endian endian);

} // namespace bridger::io::nbt
