// Java world folder and level.dat, docs/formats/anvil.md.

#include "../nbt_helpers.hpp"

#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>

namespace bridger::formats::java {
namespace fs = std::filesystem;
using io::nbt::NbtCompoundView;
using io::nbt::TagType;

Result<JavaLevelInfo> read_java_level_dat(std::span<const u8> bytes, const io::DecompressLimits& limits) {
    BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(bytes, limits, {}, "level.dat"));
    const NbtCompoundView data = nbt.doc.root().entries().find("Data", TagType::Compound).entries();
    JavaLevelInfo info;
    if (const auto n = data.find("LevelName").as_string()) {
        BRIDGER_TRY(info.level_name, detail::utf8(*n));
    }
    if (const auto dv = data.find("DataVersion").as_int()) info.data_version = *dv;
    if (const io::nbt::NbtView player = data.find("Player", TagType::Compound)) {
        BRIDGER_TRY(info.player, detail::encode_compound(player.entries()));
    }
    if (const auto f = data.find("version").as_int()) info.storage_format = *f;
    if (const auto vn = data.find("Version").entries().find("Name").as_string()) {
        BRIDGER_TRY(info.version_name, detail::utf8(*vn));
    }
    // Spawn: SpawnX/Y/Z ints (long-standing), or a "spawn" compound with an int-array "pos" (newer versions).
    const auto sx = data.find("SpawnX").as_int(), sy = data.find("SpawnY").as_int(), sz = data.find("SpawnZ").as_int();
    if (sx && sy && sz) info.spawn = BlockPos{*sx, *sy, *sz};
    else if (const auto pos = detail::int3(data.find("spawn").entries().find("pos"))) info.spawn = BlockPos{(*pos)[0], (*pos)[1], (*pos)[2]};
    // Seed: WorldGenSettings.seed (1.16+) or RandomSeed (older).
    if (const auto s = data.find("WorldGenSettings").entries().find("seed").as_long()) info.seed = *s;
    else if (const auto r = data.find("RandomSeed").as_long()) info.seed = *r;
    return info;
}

Result<JavaWorld> open_java_world(const fs::path& path) {
    JavaWorld w;
    std::error_code ec;
    w.root = fs::is_directory(path, ec) ? path : path.parent_path();
    const fs::path level = w.root / "level.dat";
    if (!fs::is_regular_file(level, ec)) return make_error(ErrorCode::FileNotFound, "not a Java world: level.dat is missing");
    BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(level));
    BRIDGER_TRY(w.level, read_java_level_dat(f.data()));

    // Java 26.1+ (snapshot 26.1-6) keeps each dimension under dimensions/<namespace>/<name>/;
    // older worlds keep the Overworld at the root and use DIM-1 / DIM1 [3].
    const fs::path modern = w.root / "dimensions" / "minecraft";
    w.modern_layout = fs::is_directory(modern / "overworld", ec);
    const fs::path overworld = w.modern_layout ? modern / "overworld" : w.root;
    // Anvil (.mca, 1.2+); McRegion (.mcr, Beta 1.3 - 1.1) only when there is no Anvil file, because
    // 1.2 kept the .mcr files next to the converted ones.
    std::vector<fs::path> mcr;
    if (fs::is_directory(overworld / "region", ec)) {
        for (const auto& e : fs::directory_iterator(overworld / "region", ec)) {
            if (!e.is_regular_file()) continue;
            BRIDGER_TRY(const std::string name, platform::path_to_utf8(e.path().filename()));
            if (!name.starts_with("r.")) continue;
            if (name.ends_with(".mca")) w.overworld_regions.push_back(e.path());
            else if (name.ends_with(".mcr")) mcr.push_back(e.path());
        }
        if (ec) return make_error(ErrorCode::IoError, "cannot list region/: " + ec.message());
    }
    if (w.overworld_regions.empty()) w.overworld_regions = std::move(mcr);
    // Alpha (before Beta 1.3) kept one file per chunk, c.<x>.<z>.dat in base-36 folders; any later
    // version converts it to McRegion when it opens the world.
    if (w.overworld_regions.empty() && !w.modern_layout) {
        for (auto it = fs::recursive_directory_iterator(w.root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() >= 2) it.disable_recursion_pending();
            if (it.depth() != 2 || !it->is_regular_file()) continue;
            const auto name = platform::path_to_utf8(it->path().filename());
            if (name && name->starts_with("c.") && name->ends_with(".dat"))
                return make_error(ErrorCode::UnsupportedVersion,
                                  "this world uses the Alpha format (before Beta 1.3); open it once in Java Beta 1.3 or newer, "
                                  "which converts it, then convert that copy");
        }
    }
    std::sort(w.overworld_regions.begin(), w.overworld_regions.end());
    // The Nether and the End: Anvil files only (McRegion-era Nether folders are read by any newer version first).
    const auto list_mca = [&](const fs::path& dir, std::vector<fs::path>& out) -> Status {
        if (!fs::is_directory(dir, ec)) return {};
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            BRIDGER_TRY(const std::string name, platform::path_to_utf8(e.path().filename()));
            if (name.starts_with("r.") && name.ends_with(".mca")) out.push_back(e.path());
        }
        if (ec) return make_error(ErrorCode::IoError, "cannot list " + platform::path_to_utf8(dir).value_or("?") + ": " + ec.message());
        std::sort(out.begin(), out.end());
        return {};
    };
    BRIDGER_TRY_STATUS(list_mca(w.modern_layout ? modern / "the_nether" / "region" : w.root / "DIM-1" / "region", w.nether_regions));
    BRIDGER_TRY_STATUS(list_mca(w.modern_layout ? modern / "the_end" / "region" : w.root / "DIM1" / "region", w.end_regions));
    w.has_nether = !w.nether_regions.empty();
    w.has_end = !w.end_regions.empty();
    // A server's separate Nether/End worlds sit next to the world folder.
    const fs::path parent = w.root.parent_path();
    const fs::path stem = w.root.filename();
    w.server_nether_folder = fs::is_directory(parent / (stem.native() + fs::path("_nether").native()) / "DIM-1" / "region", ec);
    w.server_end_folder = fs::is_directory(parent / (stem.native() + fs::path("_the_end").native()) / "DIM1" / "region", ec);
    w.has_entity_regions = fs::is_directory(overworld / "entities", ec) && !fs::is_empty(overworld / "entities", ec);

    // 26.1+: the seed moved from level.dat to data/minecraft/world_gen_settings.dat, a saved-data
    // file {DataVersion, data: {seed, generate_structures, bonus_chest, dimensions}} (field names
    // from the 26.3 server's WorldOptions codec; saved-data wrapper "data").
    const fs::path wgs = w.root / "data" / "minecraft" / "world_gen_settings.dat";
    if (!w.level.seed && fs::is_regular_file(wgs, ec)) {
        BRIDGER_TRY(const platform::MappedFile wf, platform::MappedFile::open(wgs));
        BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(wf.data(), {}, {}, "world_gen_settings.dat"));
        const NbtCompoundView root = nbt.doc.root().entries();
        if (const auto s = root.find("data").entries().find("seed").as_long()) w.level.seed = *s;
        else if (const auto s2 = root.find("seed").as_long()) w.level.seed = *s2;
    }
    return w;
}

} // namespace bridger::formats::java
