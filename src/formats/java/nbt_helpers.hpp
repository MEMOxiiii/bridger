#pragma once

// Helpers shared by the Java format readers (internal to bridger_formats).

#include <bridger/core/block_entity.hpp>
#include <bridger/core/block_state.hpp>
#include <bridger/core/result.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace bridger::formats::detail {

// Gzip-compressed or raw NBT (big-endian, Java).
struct JavaNbt {
    std::vector<u8> storage; // decompressed bytes (empty when the input was raw)
    io::nbt::NbtDocument doc;
    io::Compression compression = io::Compression::None;
};

inline Result<JavaNbt> parse_java_nbt(std::span<const u8> bytes, const io::DecompressLimits& dl,
                                      const io::nbt::NbtLimits& nl, std::string_view what) {
    JavaNbt out;
    out.compression = io::detect_compression(bytes);
    std::span<const u8> nbt = bytes;
    if (out.compression == io::Compression::Gzip) {
        BRIDGER_TRY(out.storage, io::decompress(bytes, io::Compression::Gzip, dl));
        nbt = out.storage;
    } else if (out.compression != io::Compression::None) {
        return make_error(ErrorCode::UnsupportedFormat,
                          std::string(what) + ": expected gzip-compressed NBT, found " + std::string(io::to_string(out.compression)));
    }
    BRIDGER_TRY(out.doc, io::nbt::NbtDocument::parse(nbt, Endian::Big, nl));
    if (!out.doc.root().is(io::nbt::TagType::Compound))
        return make_error(ErrorCode::Malformed, std::string(what) + ": root tag is not a compound");
    return out;
}

inline Result<std::string> utf8(std::string_view raw) { return io::nbt::mutf8_to_utf8(raw); }

// Palette entry -> BlockState. Two spellings exist: {Name, Properties} (minecraft.wiki;
// files from older versions, Litematica) and {id, properties} (observed in Mojang's own
// 26.3 structure files, docs/formats/structure-nbt.md). Both are accepted.
inline Result<BlockState> state_from_compound(io::nbt::NbtCompoundView c) {
    auto name = c.find("Name").as_string();
    io::nbt::NbtView props_view = c.find("Properties");
    if (!name) {
        name = c.find("id").as_string();
        props_view = c.find("properties");
    }
    if (!name) return make_error(ErrorCode::Malformed, "palette entry without Name/id");
    BRIDGER_TRY(std::string id, utf8(*name));
    std::vector<BlockProperty> props;
    for (const io::nbt::NbtEntry& e : props_view.entries()) {
        const auto v = e.value.as_string();
        if (!v) return make_error(ErrorCode::Malformed, "block property value is not a string");
        BRIDGER_TRY(std::string key, utf8(e.name));
        BRIDGER_TRY(std::string value, utf8(*v));
        props.push_back({std::move(key), std::move(value)});
    }
    if (id.find(':') == std::string::npos) id = "minecraft:" + id;
    return BlockState(std::move(id), std::move(props));
}

// Encodes the entries of `c` (minus `excluded`) as an unnamed big-endian root compound.
inline Result<RawNbt> encode_compound(io::nbt::NbtCompoundView c, std::initializer_list<std::string_view> excluded = {}) {
    io::nbt::NbtWriter w(Endian::Big);
    w.begin_root_compound();
    bool any = false;
    for (const io::nbt::NbtEntry& e : c) {
        if (std::find(excluded.begin(), excluded.end(), e.name) != excluded.end()) continue;
        w.write_view(e.name, e.value);
        any = true;
    }
    w.end_compound();
    if (!any) return RawNbt{};
    BRIDGER_TRY(std::vector<u8> bytes, std::move(w).finish());
    return RawNbt{std::move(bytes), Endian::Big};
}

// List of exactly 3 ints (or Int array of 3) -> values.
inline std::optional<std::array<i32, 3>> int3(io::nbt::NbtView v) {
    if (const auto a = v.as_int_array(); a && a->size() == 3) return std::array<i32, 3>{(*a)[0], (*a)[1], (*a)[2]};
    if (const auto l = v.as_list(); l && l->size() == 3 && l->element_type() == io::nbt::TagType::Int)
        return std::array<i32, 3>{*l->at(0).as_int(), *l->at(1).as_int(), *l->at(2).as_int()};
    return std::nullopt;
}

// {x: Int, y: Int, z: Int} -> values.
inline std::optional<std::array<i32, 3>> xyz(io::nbt::NbtCompoundView c) {
    const auto x = c.find("x").as_int(), y = c.find("y").as_int(), z = c.find("z").as_int();
    if (!x || !y || !z) return std::nullopt;
    return std::array<i32, 3>{*x, *y, *z};
}

} // namespace bridger::formats::detail
