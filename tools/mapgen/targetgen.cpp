// bridger_targetgen: computes, for every Bedrock target version, how the pinned palette's states are written for
// that version (docs/versions.md, "Target versions"). Input: the archived palettes converted by
// build_bedrock_targets.py and tools/mapgen/overrides/bedrock_targets.json. Each state T of a target's palette is
// upgraded with Bridger's own upgrader and resolved to the pinned palette (the same lookup conversions use), giving
// the exact map pinned state -> T. Pinned states no T reaches do not exist in that version.
//
//   bridger_targetgen --palettes <dir> --targets <json> --out <bedrock-targets.nbt> --source <text>

#include <bridger/convert/pipeline.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace bridger;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[noreturn]] void die(const std::string& m) {
    std::fprintf(stderr, "targetgen: %s\n", m.c_str());
    std::exit(1);
}

std::vector<int> version_of(const std::string& file) { // "1.21.80.27_beta.nbt" -> {1,21,80,27}
    std::vector<int> v;
    std::string num;
    for (const char c : file.substr(0, file.find_first_of("_n"))) {
        if (c == '.') { if (!num.empty()) v.push_back(std::stoi(num)); num.clear(); }
        else if (c >= '0' && c <= '9') num += c;
    }
    if (!num.empty()) v.push_back(std::stoi(num));
    return v;
}

// Canonical "name[k=v(type),...]" of a state, to tell whether the target spells it differently.
std::string canonical(std::string_view name, std::vector<std::string> props) {
    std::sort(props.begin(), props.end());
    std::string s(name);
    for (const auto& p : props) s += "|" + p;
    return s;
}

std::string prop_text(std::string_view k, const translate::BedrockProperty& p) {
    if (p.type == translate::NbtValueType::String) return std::string(k) + "=s:" + std::string(p.str_value);
    return std::string(k) + (p.type == translate::NbtValueType::Byte ? "=b:" : "=i:") + std::to_string(p.int_value);
}

std::string entry_canonical(io::nbt::NbtCompoundView e) {
    std::vector<std::string> props;
    for (const io::nbt::NbtEntry& p : e.find("states").entries()) {
        translate::BedrockProperty bp;
        if (const auto b = p.value.as_byte()) { bp.type = translate::NbtValueType::Byte; bp.int_value = *b; }
        else if (const auto i = p.value.as_int()) { bp.type = translate::NbtValueType::Int; bp.int_value = *i; }
        else if (const auto s = p.value.as_string()) { bp.type = translate::NbtValueType::String; bp.str_value = *s; }
        props.push_back(prop_text(p.name, bp));
    }
    return canonical(e.find("name").as_string().value_or(""), std::move(props));
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    for (const char* k : {"--palettes", "--targets", "--items", "--out", "--source"})
        if (!a.contains(k)) die(std::string("missing ") + k);

    const auto m = translate::MappingData::embedded();
    if (!m) die(m.error().describe());
    const translate::MappingData& map = **m;
    const auto lookup = convert::bedrock_state_lookup(map);
    const usize n = map.bedrock_state_count();
    std::vector<std::string> pinned(n);
    for (usize i = 0; i < n; ++i) {
        const auto& st = map.bedrock(static_cast<translate::BedrockStateIndex>(i));
        std::vector<std::string> props;
        for (const auto& p : st.properties) props.push_back(prop_text(p.key, p));
        pinned[i] = canonical(st.name, std::move(props));
    }

    std::vector<std::pair<std::vector<int>, fs::path>> palettes;
    for (const auto& e : fs::directory_iterator(a["--palettes"]))
        if (e.path().extension() == ".nbt") palettes.emplace_back(version_of(e.path().filename().string()), e.path());
    std::sort(palettes.begin(), palettes.end());
    if (palettes.empty()) die("no palettes");

    std::ifstream tf(a["--targets"]);
    const json targets = json::parse(tf)["targets"];

    // Pinned states by block name, for the nearest-state fallback.
    std::map<std::string, std::vector<usize>, std::less<>> by_name;
    for (usize i = 0; i < n; ++i) by_name[std::string(map.bedrock(static_cast<translate::BedrockStateIndex>(i)).name)].push_back(i);
    auto props_of = [&](usize i) {
        std::vector<std::string> v;
        for (const auto& p : map.bedrock(static_cast<translate::BedrockStateIndex>(i)).properties) v.push_back(prop_text(p.key, p));
        return v;
    };

    // Output tables shared by all targets: distinct target spellings, and one map per distinct palette.
    std::vector<std::vector<u8>> spellings;
    std::map<std::vector<u8>, i32> spelling_id;
    struct PaletteMap {
        std::string file;
        i32 block_version = 0;
        std::vector<u8> missing;    // bitset over pinned states: 1 = the block does not exist in this version
        std::vector<i32> nearest;   // pairs (pinned state, pinned state written instead): same block, closest states
        std::vector<i32> changed;   // pairs (pinned state, spelling id): written with another name or states
    };
    std::vector<PaletteMap> maps;
    std::map<std::string, i32> map_of_file;
    std::vector<i32> target_palette;

    for (const json& t : targets) {
        const std::vector<int> ver = t["version"].get<std::vector<int>>();
        std::vector<int> limit = ver;
        limit.push_back(9999);
        const fs::path* chosen = nullptr;
        for (const auto& [pv, path] : palettes)
            if (pv <= limit) chosen = &path;
        if (chosen == nullptr) die("no palette for a target");
        const std::string fname = chosen->filename().string();
        if (const auto it = map_of_file.find(fname); it != map_of_file.end()) {
            target_palette.push_back(it->second);
            std::printf("%d.%d.%d: palette %s (shared)\n", ver[0], ver[1], ver[2], fname.c_str());
            continue;
        }
        const auto file = platform::MappedFile::open(*chosen);
        if (!file) die(file.error().describe());
        const auto doc = io::nbt::NbtDocument::parse(file->data(), Endian::Little);
        if (!doc) die(doc.error().describe());

        std::vector<std::optional<std::vector<u8>>> target_of(n);
        std::vector<std::string> target_canon(n);
        PaletteMap pm;
        pm.file = fname;
        usize unresolved = 0, states = 0;
        for (const io::nbt::NbtView ev : doc->root().entries().find("blocks").items()) {
            const io::nbt::NbtCompoundView e = ev.entries();
            ++states;
            pm.block_version = std::max(pm.block_version, e.find("version").as_int().value_or(0));
            const auto idx = lookup(e);
            if (!idx) { ++unresolved; continue; }
            if (target_of[*idx]) continue; // the first spelling wins
            io::nbt::NbtWriter one(Endian::Little); // a standalone root {name, states} to keep
            one.begin_root_compound();
            one.write_view("name", e.find("name"));
            one.write_view("states", e.find("states"));
            one.end_compound();
            auto bytes = std::move(one).finish();
            if (!bytes) die(bytes.error().describe());
            target_of[*idx] = std::move(*bytes);
            target_canon[*idx] = entry_canonical(e);
        }
        pm.missing.assign((n + 7) / 8, 0);
        usize missing = 0, nearest = 0;
        for (usize i = 0; i < n; ++i) {
            if (target_of[i]) {
                if (target_canon[i] != pinned[i]) {
                    auto [it, fresh] = spelling_id.try_emplace(*target_of[i], static_cast<i32>(spellings.size()));
                    if (fresh) spellings.push_back(*target_of[i]);
                    pm.changed.push_back(static_cast<i32>(i));
                    pm.changed.push_back(it->second);
                }
                continue;
            }
            // Same block with the most equal properties, if the block exists in this version.
            const auto mine = props_of(i);
            usize best = n, best_score = 0;
            for (const usize j : by_name[std::string(map.bedrock(static_cast<translate::BedrockStateIndex>(i)).name)]) {
                if (!target_of[j]) continue;
                const auto theirs = props_of(j);
                usize score = 1;
                for (const auto& p : mine) score += std::count(theirs.begin(), theirs.end(), p) ? 1u : 0u;
                if (score > best_score) { best_score = score; best = j; }
            }
            if (best < n) {
                pm.nearest.push_back(static_cast<i32>(i));
                pm.nearest.push_back(static_cast<i32>(best));
                ++nearest;
            } else {
                pm.missing[i / 8] = static_cast<u8>(pm.missing[i / 8] | (1u << (i % 8)));
                ++missing;
            }
        }
        std::printf("%d.%d.%d: palette %s, %zu states (%zu unresolved); pinned states: %zu missing, %zu nearest, %zu respelled\n",
                    ver[0], ver[1], ver[2], fname.c_str(), states, unresolved, missing, nearest, pm.changed.size() / 2);
        map_of_file[fname] = static_cast<i32>(maps.size());
        target_palette.push_back(static_cast<i32>(maps.size()));
        maps.push_back(std::move(pm));
    }

    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_string("source", a["--source"]);
    w.write_int("pinned_states", static_cast<i32>(n));
    w.begin_list("spellings", io::nbt::TagType::Compound, spellings.size());
    for (const auto& sp : spellings) {
        const auto d = io::nbt::NbtDocument::parse(sp, Endian::Little);
        w.begin_compound();
        w.write_view("name", d->root().entries().find("name"));
        w.write_view("states", d->root().entries().find("states"));
        w.end_compound();
    }
    w.end_list();
    w.begin_list("palettes", io::nbt::TagType::Compound, maps.size());
    for (const PaletteMap& pm : maps) {
        w.begin_compound();
        w.write_string("file", pm.file);
        w.write_int("block_version", pm.block_version);
        w.write_byte_array("missing", pm.missing);
        w.write_int_array("nearest", pm.nearest);
        w.write_int_array("changed", pm.changed);
        w.end_compound();
    }
    w.end_list();
    // Item schemas (BedrockItemUpgradeSchema): renamed ids and remapped data values, oldest first.
    std::ifstream itf(a["--items"]);
    const json items = json::parse(itf);
    w.begin_list("item_schemas", io::nbt::TagType::Compound, items.size());
    for (const json& sc : items) {
        w.begin_compound();
        std::vector<i32> to = sc["to"].get<std::vector<i32>>();
        while (to.size() < 4) to.push_back(0);
        w.write_int_array("to", to);
        w.begin_compound("renamed");
        for (const auto& [from, into] : sc["renamed"].items()) w.write_string(from, into.get<std::string>());
        w.end_compound();
        w.begin_compound("remapped");
        for (const auto& [from, metas] : sc["remapped"].items()) {
            w.begin_compound(from);
            for (const auto& [meta, into] : metas.items()) w.write_string(meta, into.get<std::string>());
            w.end_compound();
        }
        w.end_compound();
        w.end_compound();
    }
    w.end_list();
    w.begin_list("targets", io::nbt::TagType::Compound, targets.size());
    for (usize ti = 0; ti < targets.size(); ++ti) {
        const json& t = targets[ti];
        std::vector<i32> v5 = t["version"].get<std::vector<i32>>();
        while (v5.size() < 5) v5.push_back(0);
        w.begin_compound();
        w.write_int_array("version", v5);
        w.write_int("protocol", t["protocol"].get<i32>());
        w.write_int("storage_version", t["storage_version"].get<i32>());
        w.write_int("chunk_version", t["chunk_version"].get<i32>());
        w.write_int("palette", target_palette[ti]);
        w.end_compound();
    }
    w.end_list();
    w.end_compound();
    auto bytes = std::move(w).finish();
    if (!bytes) die(bytes.error().describe());
    std::ofstream(a["--out"], std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    std::printf("wrote %s (%zu bytes, %zu spellings, %zu palettes)\n", a["--out"].c_str(), bytes->size(), spellings.size(), maps.size());
    return 0;
}
