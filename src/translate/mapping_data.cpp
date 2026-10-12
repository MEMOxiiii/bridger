#include <bridger/translate/mapping_data.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace bridger::embedded {
extern const unsigned char brmap_java_26_3_bedrock_26_50[];
extern const std::size_t brmap_java_26_3_bedrock_26_50_size;
} // namespace bridger::embedded

namespace bridger::translate {
namespace {

// Bounds-checked little-endian reader for the blob.
class Reader {
public:
    explicit Reader(std::span<const u8> d) : d_(d) {}
    bool ok() const noexcept { return ok_; }
    u8 u8_() { return need(1) ? d_[p_++] : 0; }
    u16 u16_() {
        if (!need(2)) return 0;
        const u16 v = static_cast<u16>(d_[p_] | (d_[p_ + 1] << 8));
        p_ += 2;
        return v;
    }
    u32 u32_() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | d_[p_ + static_cast<usize>(i)];
        p_ += 4;
        return v;
    }
    std::string_view str() {
        const u16 n = u16_();
        if (!need(n)) return {};
        std::string_view s(reinterpret_cast<const char*>(d_.data() + p_), n);
        p_ += n;
        return s;
    }
    bool bytes(usize n, std::span<const u8>& out) {
        if (!need(n)) return false;
        out = d_.subspan(p_, n);
        p_ += n;
        return true;
    }

private:
    bool need(usize n) {
        if (!ok_ || n > d_.size() - p_) {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::span<const u8> d_;
    usize p_ = 0;
    bool ok_ = true;
};

struct StringHash {
    using is_transparent = void;
    usize operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

} // namespace

struct MappingData::Index {
    struct JavaBlock {
        std::string_view name;
        u32 first_state = 0;
        u32 state_count = 0;
        u32 default_state = 0;
        std::vector<std::string_view> prop_names;
        std::vector<std::vector<std::string_view>> prop_values;
        std::span<const u8> state_values; // state_count * prop_count value indices
        // packed value tuple (radix over prop_values sizes, in listed order) -> state offset
        std::unordered_map<u64, u32> by_tuple;
    };
    std::vector<JavaBlock> blocks;
    std::unordered_map<std::string_view, u32, StringHash, std::equal_to<>> block_by_name;
    std::vector<u32> owner; // java state -> block index
    std::unordered_map<std::string, u32> bedrock_by_key;
    std::unordered_map<std::string_view, ItemMapping, StringHash, std::equal_to<>> items;
    std::unordered_map<std::string_view, i32, StringHash, std::equal_to<>> biomes;
};

namespace {

u64 pack_tuple(const MappingData::Index::JavaBlock& b, std::span<const u8> values) {
    u64 key = 0;
    for (usize p = 0; p < values.size(); ++p) key = key * (b.prop_values[p].size() + 1) + values[p];
    return key;
}

std::string bedrock_key(std::string_view name, std::span<const BedrockProperty> props) {
    std::vector<const BedrockProperty*> sorted;
    for (const auto& p : props) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->key < b->key; });
    std::string k(name);
    k += '|';
    for (const auto* p : sorted) {
        k += p->key;
        k += '=';
        k += p->type == NbtValueType::String ? "s:" + std::string(p->str_value) : "n:" + std::to_string(p->int_value);
        k += ';';
    }
    return k;
}

} // namespace

MappingData::MappingData() : index_(std::make_unique<Index>()) {}
MappingData::MappingData(MappingData&&) noexcept = default;
MappingData& MappingData::operator=(MappingData&&) noexcept = default;
MappingData::~MappingData() = default;

Result<MappingData> MappingData::load(std::span<const u8> blob) {
    auto bad = [](const char* why) { return make_error(ErrorCode::Malformed, std::string("mapping data: ") + why); };
    Reader r(blob);
    std::span<const u8> magic;
    if (!r.bytes(6, magic) || std::memcmp(magic.data(), "BRMAP\0", 6) != 0) return bad("bad magic");
    if (r.u16_() != kBrmapFormatVersion) return make_error(ErrorCode::UnsupportedVersion, "mapping data: format version");

    MappingData m;
    Index& ix = *m.index_;
    m.java_data_version_ = r.u32_();
    m.java_version_ = r.str();
    m.bedrock_version_ = r.str();
    m.bedrock_block_version_ = r.u32_();

    std::vector<std::string_view> strings(r.u32_());
    for (auto& s : strings) s = r.str();
    if (!r.ok()) return bad("truncated string table");
    auto str = [&](u32 i) -> std::string_view { return i < strings.size() ? strings[i] : std::string_view{}; };

    const u32 block_count = r.u32_();
    for (u32 b = 0; b < block_count && r.ok(); ++b) {
        Index::JavaBlock jb;
        jb.name = str(r.u32_());
        jb.first_state = r.u32_();
        jb.state_count = r.u32_();
        jb.default_state = r.u32_();
        const u8 props = r.u8_();
        for (u8 p = 0; p < props; ++p) {
            jb.prop_names.push_back(str(r.u32_()));
            std::vector<std::string_view> values(r.u8_());
            for (auto& v : values) v = str(r.u32_());
            jb.prop_values.push_back(std::move(values));
        }
        if (!r.bytes(static_cast<usize>(jb.state_count) * props, jb.state_values)) return bad("truncated Java block");
        for (u32 k = 0; k < jb.state_count; ++k)
            jb.by_tuple.emplace(pack_tuple(jb, jb.state_values.subspan(static_cast<usize>(k) * props, props)), k);
        ix.block_by_name.emplace(jb.name, static_cast<u32>(ix.blocks.size()));
        ix.blocks.push_back(std::move(jb));
    }

    const u32 java_count = r.u32_();
    m.java_mappings_.resize(java_count);
    ix.owner.assign(java_count, UINT32_MAX);
    for (u32 i = 0; i < java_count; ++i) {
        m.java_mappings_[i].bedrock = r.u32_();
        m.java_mappings_[i].flags = r.u8_();
    }
    for (u32 b = 0; b < ix.blocks.size(); ++b) {
        const auto& jb = ix.blocks[b];
        if (static_cast<u64>(jb.first_state) + jb.state_count > java_count) return bad("Java block out of range");
        for (u32 k = 0; k < jb.state_count; ++k) ix.owner[jb.first_state + k] = b;
    }

    const u32 bedrock_count = r.u32_();
    m.bedrock_states_.resize(bedrock_count);
    for (u32 i = 0; i < bedrock_count && r.ok(); ++i) {
        BedrockState& s = m.bedrock_states_[i];
        s.name = str(r.u32_());
        const u8 props = r.u8_();
        for (u8 p = 0; p < props; ++p) {
            BedrockProperty bp;
            bp.key = str(r.u32_());
            bp.type = static_cast<NbtValueType>(r.u8_());
            const u32 v = r.u32_();
            if (bp.type == NbtValueType::String) bp.str_value = str(v);
            else bp.int_value = static_cast<i32>(v);
            s.properties.push_back(bp);
        }
        ix.bedrock_by_key.emplace(bedrock_key(s.name, s.properties), i);
    }
    for (const auto& jm : m.java_mappings_)
        if (jm.bedrock != UINT32_MAX && jm.bedrock >= bedrock_count) return bad("mapping target out of range");

    const u32 item_count = r.u32_();
    for (u32 i = 0; i < item_count && r.ok(); ++i) {
        const std::string_view java = str(r.u32_());
        ItemMapping im;
        im.bedrock_name = str(r.u32_());
        im.damage = static_cast<i16>(r.u16_());
        const u32 bs = r.u32_();
        if (bs != UINT32_MAX && bs < java_count) im.java_block_state = bs;
        ix.items.emplace(java, im);
    }
    const u32 biome_count = r.u32_();
    for (u32 i = 0; i < biome_count && r.ok(); ++i) {
        const std::string_view java = str(r.u32_());
        ix.biomes.emplace(java, static_cast<i32>(r.u32_()));
    }
    if (!r.ok()) return bad("truncated");
    return m;
}

Result<const MappingData*> MappingData::embedded() {
    static std::once_flag once;
    static std::optional<Result<MappingData>> loaded;
    std::call_once(once, [] {
        loaded.emplace(load(std::span<const u8>(embedded::brmap_java_26_3_bedrock_26_50,
                                                 embedded::brmap_java_26_3_bedrock_26_50_size)));
    });
    if (!*loaded) return tl::unexpected((*loaded).error());
    return &**loaded;
}

std::optional<JavaStateId> MappingData::find_java_state(const BlockState& state, bool* missing_properties) const {
    if (missing_properties != nullptr) *missing_properties = false;
    const auto bit = index_->block_by_name.find(std::string_view(state.id()));
    if (bit == index_->block_by_name.end()) return std::nullopt;
    const Index::JavaBlock& b = index_->blocks[bit->second];

    // Start from the default state's values; override with the given properties.
    const usize nprops = b.prop_names.size();
    const u32 def = b.default_state - b.first_state;
    std::vector<u8> values(b.state_values.begin() + static_cast<std::ptrdiff_t>(def * nprops),
                           b.state_values.begin() + static_cast<std::ptrdiff_t>((def + 1) * nprops));
    usize matched = 0;
    for (const BlockProperty& p : state.properties()) {
        const auto pit = std::find(b.prop_names.begin(), b.prop_names.end(), p.name);
        if (pit == b.prop_names.end()) return std::nullopt; // unknown property
        const usize pi = static_cast<usize>(pit - b.prop_names.begin());
        const auto& vals = b.prop_values[pi];
        const auto vit = std::find(vals.begin(), vals.end(), p.value);
        if (vit == vals.end()) return std::nullopt; // unknown value
        values[pi] = static_cast<u8>(vit - vals.begin());
        ++matched;
    }
    if (missing_properties != nullptr) *missing_properties = matched < nprops;
    const auto tit = b.by_tuple.find(pack_tuple(b, values));
    if (tit == b.by_tuple.end()) return std::nullopt;
    return b.first_state + tit->second;
}

std::string MappingData::java_state_string(JavaStateId id) const {
    if (id >= index_->owner.size()) return "<invalid>";
    const Index::JavaBlock& b = index_->blocks[index_->owner[id]];
    std::vector<BlockProperty> props;
    const usize n = b.prop_names.size();
    for (usize p = 0; p < n; ++p)
        props.push_back({std::string(b.prop_names[p]),
                         std::string(b.prop_values[p][b.state_values[(id - b.first_state) * n + p]])});
    return BlockState(std::string(b.name), std::move(props)).to_string();
}

std::optional<BedrockStateIndex> MappingData::find_bedrock(std::string_view name,
                                                          std::span<const BedrockProperty> props) const {
    const auto it = index_->bedrock_by_key.find(bedrock_key(name, props));
    if (it == index_->bedrock_by_key.end()) return std::nullopt;
    return it->second;
}

std::optional<ItemMapping> MappingData::find_item(std::string_view java_item) const {
    const auto it = index_->items.find(java_item);
    if (it == index_->items.end()) return std::nullopt;
    return it->second;
}

std::optional<i32> MappingData::find_biome(std::string_view java_biome) const {
    const auto it = index_->biomes.find(java_biome);
    if (it == index_->biomes.end()) return std::nullopt;
    return it->second;
}

} // namespace bridger::translate
