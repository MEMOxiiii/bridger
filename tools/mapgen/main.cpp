// bridger-mapgen: compiles Java <-> Bedrock mapping sources into a .brmap blob and
// a coverage report. Inputs and their provenance: docs/adr/0007-mapping-data-sources.md.
// Blob layout: src/translate/include/bridger/translate/mapping_data.hpp.
//
// bridger_mapgen --java-blocks <reports/blocks.json> --geyser <geyser-mappings dir>
//                --bedrock-palette <block_palette.nbt> --overrides <overrides dir>
//                --out <file.brmap> --report <coverage.md>

#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/nbt_view.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace bridger;
using namespace bridger::io::nbt;

namespace {

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "mapgen: %s\n", message.c_str());
    std::exit(1);
}

std::vector<u8> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) die("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

json read_json(const fs::path& p) {
    std::ifstream in(p);
    if (!in) die("cannot read " + p.string());
    return json::parse(in, nullptr, true, /*ignore_comments=*/true);
}

// NBT file -> decompressed bytes (gzip detected automatically).
std::vector<u8> read_nbt_file(const fs::path& p) {
    auto raw = read_bytes(p);
    if (io::detect_compression(raw) == io::Compression::Gzip) {
        auto out = io::decompress(raw, io::Compression::Gzip, {usize{1} << 30});
        if (!out) die(p.string() + ": " + out.error().describe());
        return std::move(*out);
    }
    return raw;
}

// ---------------------------------------------------------------- Java states

struct JavaBlock {
    std::string name;
    std::vector<std::string> prop_names;
    std::vector<std::vector<std::string>> prop_values;
    u32 first_state = 0;
    u32 default_state = 0;
    std::vector<std::vector<u8>> states; // per state (id order): value index per property
};

struct JavaData {
    std::vector<JavaBlock> blocks;
    std::vector<std::pair<u32, u32>> state_owner; // state id -> (block index, state offset)
};

JavaData load_java(const fs::path& path) {
    const json report = read_json(path);
    JavaData data;
    usize total = 0;
    for (const auto& [name, b] : report.items()) total += b.at("states").size();
    data.state_owner.assign(total, {UINT32_MAX, 0});

    for (const auto& [name, b] : report.items()) {
        JavaBlock block;
        block.name = name;
        if (b.contains("properties"))
            for (const auto& [pname, values] : b["properties"].items()) {
                block.prop_names.push_back(pname);
                block.prop_values.push_back(values.get<std::vector<std::string>>());
            }
        std::vector<json> states(b["states"].begin(), b["states"].end());
        std::sort(states.begin(), states.end(), [](const json& x, const json& y) { return x["id"] < y["id"]; });
        block.first_state = states.front()["id"];
        for (usize k = 0; k < states.size(); ++k) {
            const json& s = states[k];
            if (s["id"].get<u32>() != block.first_state + k) die(name + ": state ids are not contiguous");
            if (s.value("default", false)) block.default_state = s["id"];
            std::vector<u8> idx;
            for (usize p = 0; p < block.prop_names.size(); ++p) {
                const std::string v = s["properties"][block.prop_names[p]];
                const auto& vals = block.prop_values[p];
                idx.push_back(static_cast<u8>(std::find(vals.begin(), vals.end(), v) - vals.begin()));
            }
            block.states.push_back(std::move(idx));
            data.state_owner.at(s["id"].get<u32>()) = {static_cast<u32>(data.blocks.size()), static_cast<u32>(k)};
        }
        data.blocks.push_back(std::move(block));
    }
    for (const auto& o : data.state_owner)
        if (o.first == UINT32_MAX) die("Java state ids have gaps");
    return data;
}

std::string java_state_string(const JavaData& j, u32 id) {
    const auto [bi, k] = j.state_owner[id];
    const JavaBlock& b = j.blocks[bi];
    std::string s = b.name;
    if (!b.prop_names.empty()) {
        s += '[';
        // canonical: sorted by property name
        std::vector<usize> order(b.prop_names.size());
        for (usize i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](usize x, usize y) { return b.prop_names[x] < b.prop_names[y]; });
        for (usize n = 0; n < order.size(); ++n) {
            const usize p = order[n];
            if (n) s += ',';
            s += b.prop_names[p] + "=" + b.prop_values[p][b.states[k][p]];
        }
        s += ']';
    }
    return s;
}

std::optional<std::string> java_property(const JavaData& j, u32 id, std::string_view prop) {
    const auto [bi, k] = j.state_owner[id];
    const JavaBlock& b = j.blocks[bi];
    for (usize p = 0; p < b.prop_names.size(); ++p)
        if (b.prop_names[p] == prop) return b.prop_values[p][b.states[k][p]];
    return std::nullopt;
}

// ---------------------------------------------------------------- Bedrock palette

struct BProp {
    std::string key;
    u8 type; // 1 byte, 3 int, 8 string
    i32 ivalue = 0;
    std::string svalue;
};
struct BState {
    std::string name;
    std::vector<BProp> props; // sorted by key
};

// Type-insensitive identity key ("n:" numbers, "s:" strings), matching how the
// third-party mapping writes values.
std::string state_key(const std::string& name, const std::vector<BProp>& props) {
    std::string k = name + "|";
    for (const auto& p : props)
        k += p.key + "=" + (p.type == 8 ? "s:" + p.svalue : "n:" + std::to_string(p.ivalue)) + ";";
    return k;
}

std::vector<BProp> read_states(NbtCompoundView c, const std::string& context) {
    std::vector<BProp> props;
    for (const NbtEntry& e : c) {
        BProp p;
        p.key = std::string(e.name);
        switch (e.value.type()) {
        case TagType::Byte: p.type = 1; p.ivalue = *e.value.as_byte(); break;
        case TagType::Short: p.type = 3; p.ivalue = *e.value.as_short(); break;
        case TagType::Int: p.type = 3; p.ivalue = *e.value.as_int(); break;
        case TagType::String: p.type = 8; p.svalue = std::string(*e.value.as_string()); break;
        default: die(context + ": unsupported state value type for " + p.key);
        }
        props.push_back(std::move(p));
    }
    std::sort(props.begin(), props.end(), [](const BProp& a, const BProp& b) { return a.key < b.key; });
    return props;
}

struct Palette {
    std::vector<BState> states;
    std::unordered_map<std::string, u32> by_key;
    u32 block_version = 0;
};

Palette load_palette(const fs::path& path) {
    const auto bytes = read_nbt_file(path);
    auto doc = NbtDocument::parse(bytes, Endian::Big);
    if (!doc) die("palette: " + doc.error().describe());
    Palette pal;
    for (const NbtView entry : doc->root().entries().find("blocks").items()) {
        const auto c = entry.entries();
        BState s;
        s.name = std::string(*c.find("name").as_string());
        s.props = read_states(c.find("states").entries(), s.name);
        const u32 version = static_cast<u32>(*c.find("version").as_int());
        if (pal.block_version == 0) pal.block_version = version;
        if (version != pal.block_version) die("palette mixes block versions");
        const std::string key = state_key(s.name, s.props);
        if (!pal.by_key.emplace(key, static_cast<u32>(pal.states.size())).second) die("duplicate palette state " + key);
        pal.states.push_back(std::move(s));
    }
    return pal;
}

// ---------------------------------------------------------------- blob writer

class Blob {
public:
    void u8_(u8 v) { out_.push_back(v); }
    void u16_(u16 v) { for (int i = 0; i < 2; ++i) out_.push_back(static_cast<u8>(v >> (8 * i))); }
    void u32_(u32 v) { for (int i = 0; i < 4; ++i) out_.push_back(static_cast<u8>(v >> (8 * i))); }
    void str(std::string_view s) {
        if (s.size() > 0xFFFF) die("string too long");
        u16_(static_cast<u16>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    u32 intern(const std::string& s) {
        auto [it, inserted] = strings_.emplace(s, static_cast<u32>(order_.size()));
        if (inserted) order_.push_back(s);
        return it->second;
    }
    const std::vector<std::string>& strings() const { return order_; }
    std::vector<u8>& bytes() { return out_; }

private:
    std::vector<u8> out_;
    std::unordered_map<std::string, u32> strings_;
    std::vector<std::string> order_;
};

struct Args {
    fs::path java_blocks, geyser, palette, overrides, out, report;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const fs::path v = argv[i + 1];
        if (k == "--java-blocks") a.java_blocks = v;
        else if (k == "--geyser") a.geyser = v;
        else if (k == "--bedrock-palette") a.palette = v;
        else if (k == "--overrides") a.overrides = v;
        else if (k == "--out") a.out = v;
        else if (k == "--report") a.report = v;
        else die("unknown argument " + k);
    }
    if (a.java_blocks.empty() || a.geyser.empty() || a.palette.empty() || a.out.empty() || a.report.empty())
        die("usage: bridger_mapgen --java-blocks F --geyser DIR --bedrock-palette F --overrides DIR --out F --report F");
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const JavaData java = load_java(args.java_blocks);
    const Palette palette = load_palette(args.palette);
    const usize java_count = java.state_owner.size();
    std::printf("Java states: %zu (%zu blocks); Bedrock palette states: %zu (block version %u)\n", java_count,
                java.blocks.size(), palette.states.size(), palette.block_version);

    // --- Java -> Bedrock from GeyserMC/mappings blocks.nbt: entry i = Java state i;
    //     name = "minecraft:" + bedrock_identifier (default: the Java block's path), states = "state".
    const auto geyser_bytes = read_nbt_file(args.geyser / "blocks.nbt");
    auto geyser = NbtDocument::parse(geyser_bytes, Endian::Big);
    if (!geyser) die("geyser blocks.nbt: " + geyser.error().describe());
    const NbtListView entries = geyser->root().entries().find("bedrock_mappings").items();
    if (entries.size() != java_count)
        die(std::format("geyser has {} entries but Java has {} states (version mismatch?)", entries.size(), java_count));

    std::vector<u32> bedrock_of(java_count, UINT32_MAX);
    std::vector<u8> flags(java_count, 0);
    usize renamed = 0;
    {
        u32 i = 0;
        for (const NbtView e : entries) {
            const auto c = e.entries();
            const std::string java_name = java.blocks[java.state_owner[i].first].name;
            std::string name = java_name;
            if (const auto id = c.find("bedrock_identifier").as_string()) name = "minecraft:" + std::string(*id);
            if (name != java_name) ++renamed;
            const auto props = read_states(c.find("state").entries(), name);
            const auto it = palette.by_key.find(state_key(name, props));
            if (it != palette.by_key.end()) bedrock_of[i] = it->second;
            if (java_property(java, i, "waterlogged") == "true") flags[i] |= 1; // kFlagWaterlogged
            ++i;
        }
    }

    // --- Overrides (tools/mapgen/overrides/blocks.json): each entry has a comment and a test.
    usize override_count = 0;
    if (!args.overrides.empty() && fs::exists(args.overrides / "blocks.json")) {
        std::unordered_map<std::string, u32> java_by_string;
        for (u32 i = 0; i < java_count; ++i) java_by_string.emplace(java_state_string(java, i), i);
        for (const json& o : read_json(args.overrides / "blocks.json").at("overrides")) {
            if (!o.contains("comment")) die("override without a comment: " + o.dump());
            const std::string js = o.at("java");
            const auto jit = java_by_string.find(js);
            if (jit == java_by_string.end()) die("override for unknown Java state " + js);
            std::vector<BProp> props;
            for (const auto& [k, v] : o.at("bedrock").value("states", json::object()).items()) {
                BProp p;
                p.key = k;
                if (v.is_string()) { p.type = 8; p.svalue = v; } else { p.type = 3; p.ivalue = v.get<i32>(); }
                props.push_back(p);
            }
            std::sort(props.begin(), props.end(), [](const BProp& a, const BProp& b) { return a.key < b.key; });
            const auto pit = palette.by_key.find(state_key(o.at("bedrock").at("name"), props));
            if (pit == palette.by_key.end()) die("override target not in the Bedrock palette: " + o.dump());
            bedrock_of[jit->second] = pit->second;
            flags[jit->second] |= 2; // kFlagOverride
            if (o.value("lossy", false)) flags[jit->second] |= 4;
            ++override_count;
        }
    }

    // --- Items and biomes
    const json items = read_json(args.geyser / "items.json");
    const json biomes = read_json(args.geyser / "biomes.json");

    // --- Coverage
    usize mapped = 0, waterlogged = 0, lossy = 0;
    std::vector<std::string> unmapped;
    for (u32 i = 0; i < java_count; ++i) {
        if (bedrock_of[i] == UINT32_MAX) {
            unmapped.push_back(java_state_string(java, i));
            continue;
        }
        ++mapped;
        if (flags[i] & 1) ++waterlogged;
        if (flags[i] & 4) ++lossy;
    }
    // Reverse direction: how many Bedrock states are reachable from Java.
    std::vector<bool> reached(palette.states.size(), false);
    for (const u32 b : bedrock_of)
        if (b != UINT32_MAX) reached[b] = true;
    const usize reachable = static_cast<usize>(std::count(reached.begin(), reached.end(), true));

    // --- Write the blob
    Blob blob;
    // Intern every string first so the table precedes the sections that use it.
    for (const auto& b : java.blocks) {
        blob.intern(b.name);
        for (usize p = 0; p < b.prop_names.size(); ++p) {
            blob.intern(b.prop_names[p]);
            for (const auto& v : b.prop_values[p]) blob.intern(v);
        }
    }
    for (const auto& s : palette.states) {
        blob.intern(s.name);
        for (const auto& p : s.props) {
            blob.intern(p.key);
            if (p.type == 8) blob.intern(p.svalue);
        }
    }
    for (const auto& [k, v] : items.items()) {
        blob.intern(k);
        blob.intern(v.at("bedrock_identifier"));
    }
    for (const auto& [k, v] : biomes.items()) blob.intern(k);

    std::vector<u8>& out = blob.bytes();
    out.insert(out.end(), {'B', 'R', 'M', 'A', 'P', 0});
    blob.u16_(1); // format version
    // DataVersion and version id come from the server jar's version.json, which
    // fetch_sources.py extracts next to the generated reports.
    u32 data_version = 0;
    std::string java_version = "unknown";
    if (const fs::path vj = args.java_blocks.parent_path().parent_path() / "version.json"; fs::exists(vj)) {
        const json v = read_json(vj);
        data_version = v.value("world_version", 0u);
        java_version = v.value("id", "unknown");
    }
    blob.u32_(data_version);
    blob.str(java_version);
    blob.str("26.50");
    blob.u32_(palette.block_version);

    blob.u32_(static_cast<u32>(blob.strings().size()));
    for (const auto& s : blob.strings()) blob.str(s);

    blob.u32_(static_cast<u32>(java.blocks.size()));
    for (const auto& b : java.blocks) {
        blob.u32_(blob.intern(b.name));
        blob.u32_(b.first_state);
        blob.u32_(static_cast<u32>(b.states.size()));
        blob.u32_(b.default_state);
        blob.u8_(static_cast<u8>(b.prop_names.size()));
        for (usize p = 0; p < b.prop_names.size(); ++p) {
            blob.u32_(blob.intern(b.prop_names[p]));
            blob.u8_(static_cast<u8>(b.prop_values[p].size()));
            for (const auto& v : b.prop_values[p]) blob.u32_(blob.intern(v));
        }
        for (const auto& st : b.states)
            for (const u8 vi : st) blob.u8_(vi);
    }

    blob.u32_(static_cast<u32>(java_count));
    for (u32 i = 0; i < java_count; ++i) {
        blob.u32_(bedrock_of[i]); // UINT32_MAX = unmapped
        blob.u8_(flags[i]);
    }

    blob.u32_(static_cast<u32>(palette.states.size()));
    for (const auto& s : palette.states) {
        blob.u32_(blob.intern(s.name));
        blob.u8_(static_cast<u8>(s.props.size()));
        for (const auto& p : s.props) {
            blob.u32_(blob.intern(p.key));
            blob.u8_(p.type);
            blob.u32_(p.type == 8 ? blob.intern(p.svalue) : static_cast<u32>(p.ivalue));
        }
    }

    blob.u32_(static_cast<u32>(items.size()));
    for (const auto& [k, v] : items.items()) {
        blob.u32_(blob.intern(k));
        blob.u32_(blob.intern(v.at("bedrock_identifier")));
        blob.u16_(static_cast<u16>(static_cast<i16>(v.value("bedrock_data", 0))));
        u32 block_state = UINT32_MAX;
        if (v.value("is_block", false) && v.contains("firstBlockRuntimeId")) {
            // The block's default state (the item places it).
            const u32 first = v["firstBlockRuntimeId"];
            block_state = java.blocks[java.state_owner.at(first).first].default_state;
        }
        blob.u32_(block_state);
    }

    blob.u32_(static_cast<u32>(biomes.size()));
    for (const auto& [k, v] : biomes.items()) {
        blob.u32_(blob.intern(k));
        blob.u32_(static_cast<u32>(v.at("bedrock_id").get<i32>()));
    }

    {
        std::ofstream f(args.out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }

    // --- Coverage report (committed)
    std::ofstream r(args.report);
    r << "# Mapping coverage: Java " << java_version << " -> Bedrock 26.50\n\n";
    r << "Generated by `tools/mapgen` (do not edit). Sources: docs/adr/0007-mapping-data-sources.md.\n\n";
    r << "| Measure | Count |\n|---|---|\n";
    r << "| Java block states | " << java_count << " |\n";
    r << "| Mapped (exactly, as far as the sources tell) | " << mapped - lossy << " |\n";
    r << "| Mapped but lossy (per Bridger overrides) | " << lossy << " |\n";
    r << "| Unmapped (written as the fallback block and reported) | " << unmapped.size() << " |\n";
    r << "| Mapped by a Bridger override | " << override_count << " |\n";
    r << "| Waterlogged states (block + water in Bedrock layer 2, context stage) | " << waterlogged << " |\n";
    r << "| Mapped to a differently named Bedrock block | " << renamed << " |\n";
    r << "| Bedrock palette states reachable from Java (reverse check) | " << reachable << " of "
      << palette.states.size() << " |\n\n";
    r << "Context-dependent states are decided by rules in `src/translate/context_rules.cpp` (double chests, bed "
         "colors, waterlogging), not by this table.\n\n";
    r << "## Unmapped Java states\n\n";
    if (unmapped.empty()) r << "None.\n";
    for (const auto& u : unmapped) r << "- `" << u << "`\n";
    std::printf("mapped %zu / %zu, unmapped %zu, waterlogged %zu, renamed %zu, reverse reachable %zu / %zu\n", mapped,
                java_count, unmapped.size(), waterlogged, renamed, reachable, palette.states.size());
    std::printf("wrote %s (%zu bytes)\n", args.out.string().c_str(), out.size());
    return 0;
}
