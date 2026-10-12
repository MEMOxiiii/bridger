// Field list and types follow a level.dat written by Bedrock itself (Chunker's test
// resource bedrock_level.dat, 1.20.30, MIT), updated to the pinned version and a
// superflat generator. Unknown/new fields are filled in by the game on first load.

#include <bridger/formats/bedrock/mcworld/level_dat.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

namespace bridger::formats::bedrock {

std::string default_flat_world_layers() {
    return R"({"biome_id":1,"block_layers":[{"block_name":"minecraft:bedrock","count":1},)"
           R"({"block_name":"minecraft:dirt","count":2},{"block_name":"minecraft:grass_block","count":1}],)"
           R"("encoding_version":6,"structure_options":null,"world_version":"version.post_1_18"})";
}

std::vector<u8> encode_level_dat(const LevelDatOptions& o) {
    using io::nbt::NbtWriter;
    using io::nbt::TagType;
    NbtWriter w(Endian::Little);
    auto b = [&w](std::string_view k, bool v) { w.write_byte(k, v ? 1 : 0); };
    auto i = [&w](std::string_view k, i32 v) { w.write_int(k, v); };
    auto str = [&w](std::string_view k, std::string_view v) {
        auto m = io::nbt::utf8_to_mutf8(v);
        w.write_string(k, m ? *m : std::string(v));
    };
    auto version_list = [&w, &o](std::string_view k) {
        w.begin_list(k, TagType::Int, 5);
        for (const i32 v : o.version) w.write_int({}, v);
        w.end_list();
    };

    w.begin_root_compound("");
    str("BiomeOverride", "");
    b("CenterMapsToOrigin", false);
    b("ConfirmedPlatformLockedContent", false);
    i("Difficulty", 2);
    str("FlatWorldLayers", o.flat_world_layers);
    b("ForceGameType", false);
    i("GameType", o.game_type);
    i("Generator", o.generator);
    str("InventoryVersion", std::to_string(o.version[0]) + "." + std::to_string(o.version[1]) + "." +
                                std::to_string(o.version[2]));
    b("LANBroadcast", true);
    b("LANBroadcastIntent", true);
    w.write_long("LastPlayed", o.last_played);
    str("LevelName", o.level_name);
    i("LimitedWorldOriginX", o.spawn.x);
    i("LimitedWorldOriginY", 32767);
    i("LimitedWorldOriginZ", o.spawn.z);
    version_list("MinimumCompatibleClientVersion");
    b("MultiplayerGame", true);
    b("MultiplayerGameIntent", true);
    i("NetherScale", 8);
    i("NetworkVersion", o.network_version);
    i("Platform", 2);
    i("PlatformBroadcastIntent", 3);
    w.write_long("RandomSeed", o.random_seed);
    b("SpawnV1Villagers", false);
    i("SpawnX", o.spawn.x);
    i("SpawnY", o.spawn.y);
    i("SpawnZ", o.spawn.z);
    i("StorageVersion", o.storage_version);
    w.write_long("Time", 6000); // noon
    i("WorldVersion", 1);
    i("XBLBroadcastIntent", 3);

    w.begin_compound("abilities");
    b("attackmobs", true);
    b("attackplayers", true);
    b("build", true);
    b("doorsandswitches", true);
    w.write_float("flySpeed", 0.05f);
    b("flying", false);
    b("instabuild", false);
    b("invulnerable", false);
    b("lightning", false);
    b("mayfly", false);
    b("mine", true);
    b("op", false);
    b("opencontainers", true);
    b("teleport", false);
    w.write_float("walkSpeed", 0.1f);
    w.end_compound();

    str("baseGameVersion", "*");
    b("bonusChestEnabled", false);
    b("bonusChestSpawned", false);
    b("cheatsEnabled", false);
    b("commandblockoutput", true);
    b("commandblocksenabled", true);
    b("commandsEnabled", false);
    w.write_long("currentTick", 0);
    i("daylightCycle", 0);
    b("dodaylightcycle", true);
    b("doentitydrops", true);
    b("dofiretick", true);
    b("doimmediaterespawn", false);
    b("doinsomnia", true);
    b("dolimitedcrafting", false);
    b("domobloot", true);
    b("domobspawning", true);
    b("dotiledrops", true);
    b("doweathercycle", true);
    b("drowningdamage", true);
    i("editorWorldType", 0);
    i("eduOffer", 0);
    b("educationFeaturesEnabled", false);
    w.begin_compound("experiments");
    b("experiments_ever_used", false);
    b("saved_with_toggled_experiments", false);
    w.end_compound();
    b("falldamage", true);
    b("firedamage", true);
    b("freezedamage", true);
    i("functioncommandlimit", 10000);
    b("hasBeenLoadedInCreative", o.game_type == 1);
    b("hasLockedBehaviorPack", false);
    b("hasLockedResourcePack", false);
    b("immutableWorld", false);
    b("isCreatedInEditor", false);
    b("isExportedFromEditor", false);
    b("isFromLockedTemplate", false);
    b("isFromWorldTemplate", false);
    b("isRandomSeedAllowed", false);
    b("isSingleUseWorld", false);
    b("isWorldTemplateOptionLocked", false);
    b("keepinventory", false);
    version_list("lastOpenedWithVersion");
    w.write_float("lightningLevel", 0.0f);
    i("lightningTime", 100000);
    i("limitedWorldDepth", 16);
    i("limitedWorldWidth", 16);
    i("maxcommandchainlength", 65535);
    b("mobgriefing", true);
    b("naturalregeneration", true);
    i("permissionsLevel", 0);
    i("playerPermissionsLevel", 1);
    i("playerssleepingpercentage", 100);
    str("prid", "");
    b("pvp", true);
    w.write_float("rainLevel", 0.0f);
    i("rainTime", 100000);
    i("randomtickspeed", 1);
    b("recipesunlock", true);
    b("requiresCopiedPackRemovalCheck", false);
    b("respawnblocksexplode", true);
    b("sendcommandfeedback", true);
    i("serverChunkTickRange", 4);
    b("showbordereffect", true);
    b("showcoordinates", true);
    b("showdeathmessages", true);
    b("showtags", true);
    b("spawnMobs", false); // keep the build undisturbed
    i("spawnradius", 0);
    b("startWithMapEnabled", false);
    b("texturePacksRequired", false);
    b("tntexplodes", true);
    b("useMsaGamertagsOnly", false);
    w.write_long("worldStartCount", 4294967294LL);
    w.begin_compound("world_policies");
    w.end_compound();
    w.end_compound();

    std::vector<u8> nbt = std::move(w).finish().value(); // our own fixed structure: cannot fail
    std::vector<u8> out(8 + nbt.size());
    io::store_le<i32>(out.data(), o.storage_version);
    io::store_le<i32>(out.data() + 4, static_cast<i32>(nbt.size()));
    std::copy(nbt.begin(), nbt.end(), out.begin() + 8);
    return out;
}

} // namespace bridger::formats::bedrock
