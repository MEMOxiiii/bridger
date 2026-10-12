// Fuzz target: the Bedrock subchunk decoder (untrusted world data) must never crash,
// and every decoded index must point into its palette.

#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto decoded = bridger::formats::bedrock::decode_subchunk(std::span<const bridger::u8>(data, size));
    if (!decoded) return 0;
    for (const auto& layer : decoded->layers)
        for (const auto idx : layer.indices)
            if (idx >= layer.palette.size()) std::abort();
    return 0;
}
