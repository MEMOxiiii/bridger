#include <bridger/convert/placement_policy.hpp>

#include <array>
#include <charconv>
#include <string>

namespace bridger::convert {

Result<AirMode> parse_air_mode(std::string_view text) {
    if (text == "keep") return AirMode::Keep;
    if (text == "replace") return AirMode::Replace;
    return make_error(ErrorCode::InvalidArgument,
                      "invalid --air value '" + std::string(text) + "' (expected keep or replace)");
}

Result<BlockPos> parse_block_pos(std::string_view text) {
    std::array<i32, 3> v{};
    std::string_view rest = text;
    for (usize i = 0; i < v.size(); ++i) {
        const usize comma = rest.find(',');
        const bool last = i + 1 == v.size();
        if (last != (comma == std::string_view::npos))
            return make_error(ErrorCode::InvalidArgument,
                              "invalid position '" + std::string(text) + "' (expected x,y,z)");
        const std::string_view part = last ? rest : rest.substr(0, comma);
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), v[i]);
        if (ec != std::errc{} || ptr != part.data() + part.size() || part.empty())
            return make_error(ErrorCode::InvalidArgument,
                              "invalid coordinate '" + std::string(part) + "' in '" + std::string(text) + "'");
        if (!last) rest.remove_prefix(comma + 1);
    }
    return BlockPos{v[0], v[1], v[2]};
}

std::string_view to_string(AirMode mode) noexcept {
    return mode == AirMode::Keep ? "keep" : "replace";
}

std::string_view to_string(HeightPolicy policy) noexcept {
    switch (policy) {
    case HeightPolicy::Fail: return "fail";
    case HeightPolicy::Clip: return "clip";
    case HeightPolicy::ShiftToFit: return "shift-to-fit";
    }
    return "fail";
}

} // namespace bridger::convert
