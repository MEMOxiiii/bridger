#include <bridger/render/image_io.hpp>

#include <bridger/platform/atomic_replace.hpp>

#include <stb_image_write.h>

#include <vector>

namespace bridger::render {

Result<std::vector<u8>> encode_png(u32 width, u32 height, std::span<const u8> rgba) {
    if (rgba.size() < static_cast<usize>(width) * height * 4) return make_error(ErrorCode::InvalidArgument, "image data too short");
    std::vector<u8> out;
    const auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<u8>*>(ctx);
        const auto* p = static_cast<const u8*>(data);
        v->insert(v->end(), p, p + size);
    };
    if (stbi_write_png_to_func(sink, &out, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), static_cast<int>(width * 4)) == 0)
        return make_error(ErrorCode::IoError, "PNG encoding failed");
    return out;
}

Status write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba) {
    BRIDGER_TRY(const std::vector<u8> png, encode_png(width, height, rgba));
    return platform::write_file_atomically(path, png, [](const std::filesystem::path&) -> Status { return {}; });
}

} // namespace bridger::render
