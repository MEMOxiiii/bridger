#include <bridger/core/block_state.hpp>

#include <algorithm>

namespace bridger {
namespace {

// Identifier characters: lenient enough for mods, strict enough to keep the text
// form unambiguous.
bool valid_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    return std::none_of(s.begin(), s.end(), [](char c) {
        return c == '[' || c == ']' || c == ',' || c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\0';
    });
}

void sort_unique(std::vector<BlockProperty>& props) {
    std::stable_sort(props.begin(), props.end(),
                     [](const BlockProperty& a, const BlockProperty& b) { return a.name < b.name; });
    // Last occurrence wins for duplicate keys.
    std::vector<BlockProperty> unique;
    unique.reserve(props.size());
    for (auto& p : props) {
        if (!unique.empty() && unique.back().name == p.name)
            unique.back() = std::move(p);
        else
            unique.push_back(std::move(p));
    }
    props = std::move(unique);
}

} // namespace

BlockState::BlockState(std::string id, std::vector<BlockProperty> properties)
    : id_(std::move(id)), properties_(std::move(properties)) {
    sort_unique(properties_);
}

Result<BlockState> BlockState::parse(std::string_view text) {
    auto bad = [&text](const std::string& why) {
        return make_error(ErrorCode::Malformed, "invalid block state '" + std::string(text) + "': " + why);
    };

    std::string_view id = text;
    std::string_view props_text;
    if (const usize open = text.find('['); open != std::string_view::npos) {
        if (text.back() != ']') return bad("missing ']'");
        id = text.substr(0, open);
        props_text = text.substr(open + 1, text.size() - open - 2);
    }
    if (!valid_token(id)) return bad("empty or invalid id");
    if (id.find(':') == std::string_view::npos) {
        // Not the same as the game's own parser in every corner case; "stone" -> "minecraft:stone".
        return parse(std::string("minecraft:") + std::string(text));
    }
    if (id.front() == ':' || id.back() == ':') return bad("empty namespace or path");

    std::vector<BlockProperty> props;
    if (text.find('[') != std::string_view::npos) {
        if (props_text.empty()) return bad("empty property list");
        usize start = 0;
        while (start <= props_text.size()) {
            const usize comma = props_text.find(',', start);
            const std::string_view pair =
                props_text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            const usize eq = pair.find('=');
            if (eq == std::string_view::npos) return bad("property without '='");
            const std::string_view key = pair.substr(0, eq);
            const std::string_view value = pair.substr(eq + 1);
            if (!valid_token(key) || !valid_token(value)) return bad("empty or invalid property");
            props.push_back({std::string(key), std::string(value)});
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        std::sort(props.begin(), props.end(),
                  [](const BlockProperty& a, const BlockProperty& b) { return a.name < b.name; });
        for (usize i = 1; i < props.size(); ++i)
            if (props[i].name == props[i - 1].name) return bad("duplicate property '" + props[i].name + "'");
    }
    BlockState state;
    state.id_ = std::string(id);
    state.properties_ = std::move(props);
    return state;
}

std::optional<std::string_view> BlockState::property(std::string_view name) const noexcept {
    const auto it = std::lower_bound(properties_.begin(), properties_.end(), name,
                                     [](const BlockProperty& p, std::string_view n) { return p.name < n; });
    if (it == properties_.end() || it->name != name) return std::nullopt;
    return std::string_view(it->value);
}

std::string BlockState::to_string() const {
    std::string s = id_;
    if (!properties_.empty()) {
        s += '[';
        for (usize i = 0; i < properties_.size(); ++i) {
            if (i > 0) s += ',';
            s += properties_[i].name;
            s += '=';
            s += properties_[i].value;
        }
        s += ']';
    }
    return s;
}

} // namespace bridger
