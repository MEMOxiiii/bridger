#include <bridger/formats/detect.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/platform/paths.hpp>

namespace bridger::formats {

Result<FormatId> detect_format(const std::filesystem::path& path, std::span<const u8> head) {
    BRIDGER_TRY(const std::string ext, platform::path_to_utf8(path.extension()));
    const auto id = format_from_extension(ext);
    if (!id) return make_error(ErrorCode::UnsupportedFormat, "unknown file extension '" + ext + "'");

    switch (*id) {
    case FormatId::SpongeSchem:
    case FormatId::LegacySchematic:
    case FormatId::Litematic:
    case FormatId::StructureNbt: {
        // Java structure files: gzip-compressed NBT, or raw NBT starting with a compound tag.
        const bool gzip = io::detect_compression(head) == io::Compression::Gzip;
        const bool raw_compound = !head.empty() && head[0] == 0x0A;
        if (!gzip && !raw_compound)
            return make_error(ErrorCode::UnsupportedFormat,
                              "file has a " + ext + " extension but is neither gzip nor NBT");
        return *id;
    }
    default: return *id;
    }
}

} // namespace bridger::formats
