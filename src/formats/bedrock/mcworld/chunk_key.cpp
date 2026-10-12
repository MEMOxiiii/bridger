#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/io/endian.hpp>

namespace bridger::formats::bedrock {

std::string chunk_key(i32 chunk_x, i32 chunk_z, Dimension dim, ChunkTag tag, std::optional<i8> subchunk_y) {
    std::string key(dim == Dimension::Overworld ? 9 : 13, '\0');
    io::store_le<i32>(key.data(), chunk_x);
    io::store_le<i32>(key.data() + 4, chunk_z);
    usize p = 8;
    if (dim != Dimension::Overworld) {
        io::store_le<i32>(key.data() + 8, static_cast<i32>(dim));
        p = 12;
    }
    key[p] = static_cast<char>(tag);
    if (subchunk_y) key.push_back(static_cast<char>(*subchunk_y));
    return key;
}

} // namespace bridger::formats::bedrock
