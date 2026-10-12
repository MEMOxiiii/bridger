// Placing a Bedrock structure into a copy of an existing world (docs/safety.md §5,
// docs/formats/bedrock-world.md "Existing worlds").

#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace bridger::formats::bedrock {
namespace fs = std::filesystem;
namespace {

Status write_bytes(const fs::path& p, std::span<const u8> bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return make_error(ErrorCode::IoError, "cannot write " + p.string());
    return {};
}

Result<std::vector<u8>> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return make_error(ErrorCode::IoError, "cannot read " + p.string());
    return std::vector<u8>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Status unpack(std::span<const u8> mcworld, const fs::path& dir) {
    BRIDGER_TRY(const std::vector<io::ZipEntry> entries, io::read_zip(mcworld));
    for (const io::ZipEntry& e : entries) {
        BRIDGER_TRY(const fs::path rel, platform::path_from_utf8(e.name));
        BRIDGER_TRY_STATUS(write_bytes(dir / rel, e.data));
    }
    if (!fs::exists(dir / "db" / "CURRENT") || !fs::exists(dir / "level.dat"))
        return make_error(ErrorCode::Malformed, "not a Bedrock world: level.dat or db/ missing");
    return {};
}

Result<std::vector<u8>> pack(const fs::path& dir) {
    std::vector<io::ZipEntry> entries;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        BRIDGER_TRY(std::string rel, platform::path_to_utf8(fs::relative(e.path(), dir)));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        BRIDGER_TRY(std::vector<u8> data, read_bytes(e.path()));
        entries.push_back({std::move(rel), std::move(data)});
    }
    std::sort(entries.begin(), entries.end(), [](const io::ZipEntry& a, const io::ZipEntry& b) { return a.name < b.name; });
    return io::write_zip(entries);
}

// Raw NBT blobs deduplicated by content, addressed by u32 ids (the encoder's state domain).
class BlobTable {
public:
    u32 id_of(std::span<const u8> blob) {
        std::string key(reinterpret_cast<const char*>(blob.data()), blob.size());
        const auto [it, inserted] = ids_.emplace(std::move(key), static_cast<u32>(blobs_.size()));
        if (inserted) blobs_.emplace_back(blob.begin(), blob.end());
        return it->second;
    }
    StateNbtFn fn() const {
        return [this](u32 id) -> Result<std::span<const u8>> {
            if (id >= blobs_.size()) return make_error(ErrorCode::InvalidArgument, "unknown palette blob");
            return std::span<const u8>(blobs_[id]);
        };
    }

private:
    std::unordered_map<std::string, u32> ids_;
    std::vector<std::vector<u8>> blobs_;
};

struct PosLess {
    bool operator()(const BlockPos& a, const BlockPos& b) const noexcept { return a < b; }
};

} // namespace

Result<std::vector<u8>> place_into_world(std::span<const u8> existing, const Structure& s, const PlaceOptions& o,
                                         const StateNbtFn& state_nbt, const fs::path& work_dir, PlaceStats* stats) {
    if (s.source().edition != Edition::Bedrock)
        return make_error(ErrorCode::InvalidArgument, "place_into_world needs a Bedrock structure");
    const Extent size = s.size();
    const BlockPos origin = o.origin;
    if (size.volume() > 0) {
        const i64 top = i64{origin.y} + size.y - 1;
        if (origin.y < kOverworldMinY || top > kOverworldMaxY)
            return make_error(ErrorCode::OutOfBounds, "structure outside the world's build height");
    }
    BRIDGER_TRY_STATUS(unpack(existing, work_dir));

    PlaceStats st;
    std::set<std::string> touched;
    {
        BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/false));
        BRIDGER_TRY(const std::span<const u8> air_blob, state_nbt(o.air_state));

        std::map<std::pair<i32, i32>, std::vector<const BlockEntity*>> new_entities;
        for (const BlockEntity& be : s.block_entities)
            new_entities[{floor_div16(be.pos.x + origin.x), floor_div16(be.pos.z + origin.z)}].push_back(&be);

        const i32 x1 = origin.x + static_cast<i32>(size.x) - 1, y1 = origin.y + static_cast<i32>(size.y) - 1,
                  z1 = origin.z + static_cast<i32>(size.z) - 1;
        if (size.volume() > 0) {
            for (i32 cz = floor_div16(origin.z); cz <= floor_div16(z1); ++cz) {
                for (i32 cx = floor_div16(origin.x); cx <= floor_div16(x1); ++cx) {
                    ++st.chunks_touched;
                    BRIDGER_TRY(const auto version_value, db.get(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Version)));
                    const bool chunk_exists = version_value.has_value();
                    if (!chunk_exists) ++st.chunks_created;

                    std::set<BlockPos, PosLess> overwritten; // world positions whose primary block was replaced
                    for (i32 sy = floor_div16(origin.y); sy <= floor_div16(y1); ++sy) {
                        const std::string key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::SubChunkPrefix, static_cast<i8>(sy));
                        BRIDGER_TRY(const auto old, db.get(key));
                        BlobTable blobs;
                        std::array<u32, kSectionVolume> layer0;
                        std::array<u32, kSectionVolume> layer1;
                        const u32 air = blobs.id_of(air_blob);
                        layer0.fill(air);
                        layer1.fill(air);
                        bool had_liquid = false;
                        if (old) {
                            auto raw = decode_subchunk_raw(*old);
                            if (!raw)
                                return make_error(raw.error().code(), "existing subchunk " + std::to_string(cx) + "," +
                                                                          std::to_string(sy) + "," + std::to_string(cz) +
                                                                          " cannot be read (" + raw.error().message() +
                                                                          "); the world was not changed");
                            for (usize l = 0; l < raw->layers.size() && l < 2; ++l) {
                                std::vector<u32> ids;
                                for (const auto& blob : raw->layers[l].palette) ids.push_back(blobs.id_of(blob));
                                auto& dst = l == 0 ? layer0 : layer1;
                                for (usize i = 0; i < kSectionVolume; ++i) dst[i] = ids[raw->layers[l].indices[i]];
                                if (l == 1) had_liquid = true;
                            }
                            if (raw->layers.size() > 2) st.warnings.push_back("a subchunk had more than 2 layers; extra layers dropped");
                        }
                        bool any_liquid = had_liquid;
                        const i32 y0 = sy * 16;
                        for (i32 wy = std::max(y0, origin.y); wy <= std::min(y0 + 15, y1); ++wy)
                            for (i32 wz = std::max(cz * 16, origin.z); wz <= std::min(cz * 16 + 15, z1); ++wz)
                                for (i32 wx = std::max(cx * 16, origin.x); wx <= std::min(cx * 16 + 15, x1); ++wx) {
                                    const BlockPos local{wx - origin.x, wy - origin.y, wz - origin.z};
                                    const u32 v = s.block(local);
                                    if (v == o.air_state && !o.air_replaces) continue; // --air keep
                                    const usize i = section_index(wx & 15, wy & 15, wz & 15);
                                    BRIDGER_TRY(const std::span<const u8> blob, state_nbt(v));
                                    layer0[i] = blobs.id_of(blob);
                                    // The new block decides the liquid layer at its position.
                                    u32 liquid = air;
                                    if (s.has_secondary(local))
                                        if (const u32 l = s.secondary(local); l != o.air_state) {
                                            BRIDGER_TRY(const std::span<const u8> lb, state_nbt(l));
                                            liquid = blobs.id_of(lb);
                                            any_liquid = true;
                                        }
                                    layer1[i] = liquid;
                                    overwritten.insert({wx, wy, wz});
                                }
                        PaletteLayer p0, p1;
                        p0.assign(layer0);
                        std::vector<const PaletteLayer*> layers{&p0};
                        if (any_liquid) {
                            p1.assign(layer1);
                            if (!(p1.is_uniform() && p1.palette()[0] == air)) layers.push_back(&p1);
                        }
                        if (p0.is_uniform() && p0.palette()[0] == air && layers.size() == 1 && !old) continue;
                        BRIDGER_TRY(const std::vector<u8> encoded, encode_subchunk(static_cast<i8>(sy), layers, blobs.fn()));
                        BRIDGER_TRY_STATUS(db.put(key, encoded));
                        touched.insert(key);
                        ++st.subchunks_written;
                    }

                    // Block entities: existing ones at overwritten positions are removed (and reported).
                    const std::string be_key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::BlockEntity);
                    BRIDGER_TRY(const auto old_be, db.get(be_key));
                    io::BinaryWriter bew(Endian::Little);
                    bool be_changed = false;
                    if (old_be) {
                        usize pos = 0;
                        while (pos < old_be->size()) {
                            BRIDGER_TRY(const io::nbt::NbtDocument doc,
                                        io::nbt::NbtDocument::parse(std::span<const u8>(*old_be).subspan(pos), Endian::Little));
                            const auto bytes = std::span<const u8>(*old_be).subspan(pos, doc.consumed_bytes());
                            pos += doc.consumed_bytes();
                            const auto c = doc.root().entries();
                            const auto bx = c.find("x").as_int(), by = c.find("y").as_int(), bz = c.find("z").as_int();
                            if (bx && by && bz && overwritten.contains(BlockPos{*bx, *by, *bz})) {
                                st.removed_block_entities.push_back({std::string(c.find("id").as_string().value_or("?")), {*bx, *by, *bz}});
                                be_changed = true;
                                continue;
                            }
                            bew.write_bytes(bytes); // kept verbatim
                        }
                    }
                    if (const auto it = new_entities.find({cx, cz}); it != new_entities.end()) {
                        for (const BlockEntity* be : it->second) {
                            auto doc = io::nbt::NbtDocument::parse(be->data.bytes, be->data.endian);
                            if (!doc) return tl::unexpected(std::move(doc).error());
                            io::nbt::NbtWriter w(Endian::Little);
                            w.begin_root_compound("");
                            for (const io::nbt::NbtEntry& e : doc->root().entries()) {
                                if (e.name == "x" || e.name == "y" || e.name == "z" || e.name == "isMovable") continue;
                                if (e.name == "pairx" && e.value.as_int()) w.write_int("pairx", *e.value.as_int() + origin.x);
                                else if (e.name == "pairz" && e.value.as_int()) w.write_int("pairz", *e.value.as_int() + origin.z);
                                else w.write_view(e.name, e.value);
                            }
                            w.write_int("x", be->pos.x + origin.x);
                            w.write_int("y", be->pos.y + origin.y);
                            w.write_int("z", be->pos.z + origin.z);
                            w.write_byte("isMovable", 1);
                            w.end_compound();
                            BRIDGER_TRY(const std::vector<u8> bytes, std::move(w).finish());
                            bew.write_bytes(bytes);
                            ++st.block_entities_written;
                            be_changed = true;
                        }
                    }
                    if (be_changed) {
                        BRIDGER_TRY_STATUS(db.put(be_key, bew.view()));
                        touched.insert(be_key);
                    }

                    // --clear-entities: actors whose position is inside the structure's box.
                    // Returns true (and records the removal) for an entity compound inside the box.
                    const auto remove_if_inside = [&](io::nbt::NbtCompoundView c) {
                        const auto pos = c.find("Pos").items();
                        if (pos.size() != 3 || pos.element_type() != io::nbt::TagType::Float) return false;
                        const f64 px = static_cast<f64>(*pos.at(0).as_float()), py = static_cast<f64>(*pos.at(1).as_float()),
                                  pz = static_cast<f64>(*pos.at(2).as_float());
                        if (!(px >= origin.x && px < x1 + 1 && py >= origin.y && py < y1 + 1 && pz >= origin.z && pz < z1 + 1))
                            return false;
                        std::string id(c.find("identifier").as_string().value_or(""));
                        if (id.empty()) id = c.find("id").as_string() ? std::string(*c.find("id").as_string()) : "?";
                        st.removed_entities.push_back({id, {static_cast<i32>(px), static_cast<i32>(py), static_cast<i32>(pz)}});
                        return true;
                    };
                    if (o.clear_entities) {
                        // Legacy per-chunk storage: concatenated NBT roots; kept entries are copied verbatim.
                        const std::string legacy_key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Entity);
                        BRIDGER_TRY(const auto legacy, db.get(legacy_key));
                        if (legacy) {
                            std::vector<u8> kept;
                            usize p = 0;
                            bool changed = false;
                            while (p < legacy->size()) {
                                const auto rest = std::span<const u8>(*legacy).subspan(p);
                                auto doc = io::nbt::NbtDocument::parse(rest, Endian::Little);
                                if (!doc) {
                                    st.warnings.push_back("chunk " + std::to_string(cx) + "," + std::to_string(cz) +
                                                          ": legacy entity data could not be read and was left unchanged");
                                    changed = false;
                                    break;
                                }
                                const usize n = doc->consumed_bytes();
                                if (remove_if_inside(doc->root().entries())) changed = true;
                                else kept.insert(kept.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(n));
                                p += n;
                            }
                            if (changed) {
                                if (kept.empty()) BRIDGER_TRY_STATUS(db.remove(legacy_key));
                                else BRIDGER_TRY_STATUS(db.put(legacy_key, kept));
                                touched.insert(legacy_key);
                            }
                        }
                    }
                    if (o.clear_entities) {
                        const std::string digp = "digp" + chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Version).substr(0, 8);
                        BRIDGER_TRY(const auto list, db.get(digp));
                        if (list && list->size() % 8 == 0) {
                            std::vector<u8> kept;
                            for (usize k = 0; k < list->size(); k += 8) {
                                const std::string actor_key = "actorprefix" + std::string(reinterpret_cast<const char*>(list->data() + k), 8);
                                BRIDGER_TRY(const auto actor, db.get(actor_key));
                                bool remove = false;
                                if (actor)
                                    if (auto doc = io::nbt::NbtDocument::parse(*actor, Endian::Little))
                                        remove = remove_if_inside(doc->root().entries());
                                if (remove) {
                                    BRIDGER_TRY_STATUS(db.remove(actor_key));
                                    touched.insert(actor_key);
                                } else {
                                    kept.insert(kept.end(), list->begin() + static_cast<std::ptrdiff_t>(k),
                                                list->begin() + static_cast<std::ptrdiff_t>(k + 8));
                                }
                            }
                            if (kept.size() != list->size()) {
                                BRIDGER_TRY_STATUS(db.put(digp, kept));
                                touched.insert(digp);
                            }
                        }
                    }

                    // A column the world never generated gets the chunk records a generated one has.
                    if (!chunk_exists) {
                        io::BinaryWriter d3(Endian::Little);
                        for (int i = 0; i < 256; ++i) d3.write<i16>(0);
                        for (int sub = 0; sub < kOverworldSubchunks; ++sub) {
                            d3.write<u8>(static_cast<u8>((1 << 1) | 1));
                            for (int w = 0; w < 128; ++w) d3.write<u32>(0);
                            d3.write<i32>(1);
                            d3.write<i32>(o.default_biome);
                        }
                        const std::string d3_key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Data3D);
                        BRIDGER_TRY_STATUS(db.put(d3_key, d3.view()));
                        const u8 version = kChunkVersion;
                        const std::string v_key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Version);
                        BRIDGER_TRY_STATUS(db.put(v_key, std::span<const u8>(&version, 1)));
                        std::array<u8, 4> fin{};
                        io::store_le<i32>(fin.data(), kFinalizedDone);
                        const std::string f_key = chunk_key(cx, cz, Dimension::Overworld, ChunkTag::FinalizedState);
                        BRIDGER_TRY_STATUS(db.put(f_key, fin));
                        touched.insert(d3_key);
                        touched.insert(v_key);
                        touched.insert(f_key);
                    }
                }
            }
        }
        if (st.chunks_created > 0)
            st.warnings.push_back(std::to_string(st.chunks_created) +
                                  " chunk column(s) did not exist in the world; they contain only the structure (no terrain)");
        if (s.has_biome_data())
            st.warnings.push_back("structure biomes are not applied when placing into an existing world");
        BRIDGER_TRY_STATUS(db.compact());
    }
    st.touched_keys.assign(touched.begin(), touched.end());
    if (stats != nullptr) *stats = std::move(st);
    return pack(work_dir);
}

Status validate_placed_world(std::span<const u8> output, std::span<const u8> source, const std::vector<std::string>& touched_keys,
                             const fs::path& work_dir) {
    BRIDGER_TRY_STATUS(unpack(output, work_dir / "out"));
    BRIDGER_TRY_STATUS(unpack(source, work_dir / "src"));
    {
        BRIDGER_TRY(const std::vector<u8> a, read_bytes(work_dir / "out" / "level.dat"));
        BRIDGER_TRY(const std::vector<u8> b, read_bytes(work_dir / "src" / "level.dat"));
        if (a != b) return make_error(ErrorCode::ValidationFailed, "level.dat changed");
    }
    const std::set<std::string> touched(touched_keys.begin(), touched_keys.end());
    BRIDGER_TRY(LevelDbStore out, LevelDbStore::open(work_dir / "out" / "db", false));
    BRIDGER_TRY(LevelDbStore src, LevelDbStore::open(work_dir / "src" / "db", false));

    std::map<std::string, std::vector<u8>> src_values;
    BRIDGER_TRY_STATUS(src.for_each([&](std::string_view k, std::span<const u8> v) {
        if (!touched.contains(std::string(k))) src_values.emplace(std::string(k), std::vector<u8>(v.begin(), v.end()));
    }));
    usize matched = 0;
    Status failure;
    BRIDGER_TRY_STATUS(out.for_each([&](std::string_view k, std::span<const u8> v) {
        if (!failure) return;
        const std::string key(k);
        if (touched.contains(key)) {
            if (key.size() == 10 && static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::SubChunkPrefix))
                if (auto d = decode_subchunk_raw(v); !d) failure = tl::unexpected(std::move(d).error());
            return;
        }
        const auto it = src_values.find(key);
        if (it == src_values.end()) {
            failure = make_error(ErrorCode::ValidationFailed, "unexpected new key in the output world");
            return;
        }
        if (!std::equal(v.begin(), v.end(), it->second.begin(), it->second.end())) {
            failure = make_error(ErrorCode::ValidationFailed, "an untouched key changed");
            return;
        }
        ++matched;
    }));
    if (!failure) return make_error(ErrorCode::ValidationFailed, "placed world: " + failure.error().describe());
    if (matched != src_values.size())
        return make_error(ErrorCode::ValidationFailed, "placed world lost " + std::to_string(src_values.size() - matched) + " key(s)");
    return {};
}

} // namespace bridger::formats::bedrock
