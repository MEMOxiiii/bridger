// Fuzz target: the NBT reader must never crash or read out of bounds, and any
// document it accepts must round-trip byte-exactly through NbtValue and the writer.
// Each input is tried as big-endian (Java) and little-endian (Bedrock) NBT, so
// real files work directly as seeds (e.g. tests/data/nbt).

#include <bridger/io/nbt/nbt_value.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::io::nbt;

namespace {

void check(std::span<const u8> input, Endian endian) {
    NbtLimits limits;
    limits.max_tags = 1u << 20; // keep each run fast
    const auto doc = NbtDocument::parse(input, endian, limits);
    if (!doc) return;

    // Walk everything through the zero-copy API.
    if (const auto c = doc->root().as_compound())
        for (const NbtEntry& e : *c) (void)e.value.payload_bytes();

    const NbtRoot tree = materialize(*doc);
    const auto bytes = write_nbt(tree, endian);
    if (!bytes) std::abort(); // a valid document must be writable
    const auto consumed = input.first(doc->consumed_bytes());
    if (!std::equal(bytes->begin(), bytes->end(), consumed.begin(), consumed.end())) std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const u8> input(data, size);
    check(input, Endian::Big);
    check(input, Endian::Little);
    return 0;
}
