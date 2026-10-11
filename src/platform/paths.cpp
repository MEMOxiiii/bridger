// OS-independent path helpers.

#include <bridger/platform/paths.hpp>

#include <array>
#include <random>

namespace bridger::platform {

Result<std::filesystem::path> sibling_temp_path(const std::filesystem::path& path, std::string_view suffix) {
    if (!path.has_filename()) return make_error(ErrorCode::InvalidArgument, "path has no file name");

    thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        return (static_cast<u64>(rd()) << 32) ^ rd();
    }()};
    constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    u64 r = rng();
    std::string tag(16, '0');
    for (char& c : tag) {
        c = kHex[r & 15];
        r >>= 4;
    }

    BRIDGER_TRY(std::string name, path_to_utf8(path.filename()));
    name += '.';
    name += tag;
    name += '.';
    name += suffix;
    BRIDGER_TRY(std::filesystem::path file, path_from_utf8(name));
    return path.parent_path() / file;
}

} // namespace bridger::platform
