#include "app.hpp"

#include "host.hpp"
#include "settings.hpp"
#include "ui_bundle.hpp"
#include "updater.hpp"
#include "viewer.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/convert/view_world.hpp>
#include <bridger/core/block_registry.hpp>
#include <bridger/core/log.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/platform/process.hpp>
#include <bridger/render/asset_download.hpp>
#include <bridger/render/renderer.hpp>
#include <bridger/render/scene.hpp>
#include <bridger/render/lod_cache.hpp>
#include <bridger/render/streaming.hpp>
#include <bridger/translate/targets.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace bridger::app {
namespace {

using json = nlohmann::json;
using formats::FormatId;
namespace fs = std::filesystem;

constexpr glm::vec3 kCharcoalLinear{0.059f, 0.071f, 0.090f};
constexpr glm::vec3 kSky{0.62f, 0.76f, 1.0f};

std::string utf8(const fs::path& p) { return platform::path_to_utf8(p).value_or(std::string()); }

// "<stem> (<n>)<ext>" next to `like`: a free name for an output that must not be the input.
fs::path numbered(const fs::path& like, int n) {
    fs::path name = like.stem();
    name += platform::path_from_utf8(" (" + std::to_string(n) + ")").value_or(fs::path());
    name += like.extension();
    return like.parent_path() / name;
}

std::string format_key(FormatId id) { return std::string(formats::info(id).name); }

bool is_structure_format(FormatId f) {
    return f == FormatId::LegacySchematic || f == FormatId::SpongeSchem || f == FormatId::Litematic || f == FormatId::StructureNbt ||
           f == FormatId::McStructure;
}

// What the convert dialog offers for an input (docs/formats/README.md, supported paths).
// Game versions a format can be written for, newest (the default) first. "checked": "game" for the versions
// whose output passed the in-game checklist (docs/testing.md), "data" for those checked against their block data.
json versions_for(FormatId output) {
    json out = json::array();
    const bool bedrock = output == FormatId::McWorld || output == FormatId::McStructure;
    if (bedrock) {
        if (const auto t = translate::BedrockTargets::embedded())
            for (const auto& v : (*t)->targets()) out.push_back({{"name", v.name}, {"checked", v.pinned ? "game" : "data"}});
    } else if (const auto t = translate::JavaTargets::embedded()) {
        for (const auto& v : (*t)->targets()) out.push_back({{"name", v.name}, {"checked", v.pinned ? "game" : "data"}});
    }
    return out;
}

json targets_for(FormatId input) {
    std::vector<FormatId> ids;
    if (is_structure_format(input))
        ids = {FormatId::McWorld, FormatId::McStructure, FormatId::SpongeSchem, FormatId::Litematic, FormatId::StructureNbt};
    else if (input == FormatId::JavaWorld)
        ids = {FormatId::McWorld, FormatId::JavaWorld}; // the same edition for another game version
    else if (input == FormatId::McWorld)
        ids = {FormatId::JavaWorld, FormatId::McWorld};
    json out = json::array();
    for (const FormatId id : ids) {
        const auto& f = formats::info(id);
        out.push_back({{"id", f.name},
                       {"ext", f.extension},
                       {"edition", id == FormatId::McWorld || id == FormatId::McStructure ? "bedrock" : "java"},
                       {"description", f.description},
                       {"versions", versions_for(id)}});
    }
    return out;
}

// Which game version saved the file (convert/source_version.hpp).
json version_json(const convert::GameVersionInfo& v) {
    return {{"edition", v.edition == Edition::Java ? "java" : "bedrock"},
            {"name", v.name},
            {"detail", v.detail},
            {"exact", v.exact},
            {"text", convert::describe(v)}};
}

json report_json(const translate::ConversionReport& report) {
    using translate::IssueKind;
    json out = json::object();
    for (const IssueKind kind : {IssueKind::UnmappedBlock, IssueKind::LossyBlock, IssueKind::UnmappedBlockEntity, IssueKind::UnmappedEntity,
                                 IssueKind::UnmappedBiome, IssueKind::DroppedSecondaryLayer, IssueKind::RemovedBlockEntity,
                                 IssueKind::RemovedEntity, IssueKind::ClippedBlock}) {
        const auto* subjects = report.issues(kind);
        if (subjects == nullptr || subjects->empty()) continue;
        std::vector<std::pair<const std::string*, const translate::IssueEntry*>> sorted;
        for (const auto& [subject, entry] : *subjects) sorted.emplace_back(&subject, &entry);
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
        if (sorted.size() > 500) sorted.resize(500);
        json list = json::array();
        for (const auto& [subject, entry] : sorted) {
            json examples = json::array();
            for (const BlockPos& p : entry->examples) examples.push_back({p.x, p.y, p.z});
            list.push_back({{"subject", *subject}, {"count", entry->count}, {"examples", std::move(examples)}});
        }
        out[std::string(translate::to_string(kind))] = std::move(list);
    }
    return out;
}

u64 count_blocks(const Structure& s) {
    u64 n = 0;
    const Extent e = s.size();
    for (u32 y = 0; y < e.y; ++y)
        for (u32 z = 0; z < e.z; ++z)
            for (u32 x = 0; x < e.x; ++x)
                if (s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}) != kAirStateId) ++n;
    return n;
}

// Tasks from worker threads and dialog callbacks, run on the UI thread (WebView2 and the
// renderer are single-threaded).
class TaskQueue {
public:
    void post(std::function<void()> f) {
        std::lock_guard g(lock_);
        tasks_.push_back(std::move(f));
    }
    bool run_all() {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard g(lock_);
            now.swap(tasks_);
        }
        for (auto& f : now) f();
        return !now.empty();
    }

private:
    std::mutex lock_;
    std::vector<std::function<void()>> tasks_;
};

struct Document {
    std::string path, name;
    FormatId format = FormatId::SpongeSchem;
    bool world = false;
    glm::dvec3 lo{0.0}, hi{1.0};
    std::unique_ptr<convert::ViewWorld> view_world;
    std::unique_ptr<render::StreamingScene> streaming;
    std::shared_ptr<render::LodCache> distant_cache;
};

const FileFilter kOpenFilters[] = {
    {"Minecraft structures and worlds", "schem;litematic;nbt;schematic;mcstructure;mcworld;dat"},
    {"All files", "*"},
};
const FileFilter kJarFilter[] = {{"Minecraft client jar", "jar"}};
const FileFilter kPackFilter[] = {{"Resource pack", "zip"}};
const FileFilter kWorldFilter[] = {{"Bedrock world", "mcworld"}};
const FileFilter kSaveFilters[] = {
    {"Bedrock world", "mcworld"},          {"Bedrock structure", "mcstructure"}, {"Sponge schematic", "schem"},
    {"Litematica schematic", "litematic"}, {"Structure block file", "nbt"},
};

const char* platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

} // namespace

struct App::Impl {
    std::unique_ptr<Host> host; // the window, the web view and everything else from the OS (host.hpp)
    std::optional<render::Renderer> renderer; // empty without a usable GPU: the app still converts
    std::string viewer_error;
    UiBundle bundle;
    TaskQueue tasks;
    Settings settings;
    bool ui_ready = false;
    std::vector<std::string> outbox; // messages posted before the page is ready

    // Assets and viewer.
    std::unique_ptr<render::BlockModels> models;
    std::string assets_error, assets_jar;
    ViewerController viewer;
    render::FrameSettings frame;
    glm::vec3 sky = kCharcoalLinear;
    bool dirty = true;
    std::optional<float> slice;
    bool fly = false;
    FlyKeys page_keys; // fly-mode keys sent by the page (FlyInput::Page)
    // The 3D view runs only while the UI shows it (docs/app/bridge.md, `preview`): no rendering and no
    // world streaming while the user is on the convert page.
    bool previewing = false;

    // Document and background work.
    Document doc;
    bool has_doc = false;
    std::thread open_worker;
    std::atomic<u64> open_generation{0};
    std::thread convert_worker;
    std::atomic<bool> convert_cancel{false};
    bool converting = false;
    std::string pending_open;
    // Textures downloaded from Mojang on request (render/asset_download.hpp).
    std::thread download_worker;
    std::atomic<bool> download_cancel{false};
    bool downloading = false;
    bool using_downloaded_jar = false;
    // Self-update (updater.hpp, docs/app/updates.md): the latest release found, the package staged next to the program,
    // and whether it was installed already (then the app restarts or quits).
    std::thread update_worker;
    std::atomic<bool> update_cancel{false};
    bool update_busy = false;
    std::optional<update::Release> update_release;
    std::optional<fs::path> update_package;
    bool update_installed = false;
    std::vector<fs::path> asset_paths; // client jar + resource packs of `models` (distant terrain cache key)
    // Time to first frame (docs/performance.md): from open() to the first frame showing the document,
    // and for worlds until every chunk in the render distance is on screen.
    std::chrono::steady_clock::time_point open_started;
    bool ttff_pending = false, stream_pending = false;

    // Statistics for the HUD.
    u32 frames = 0;
    double frame_ms_sum = 0.0;
    render::FrameStats last_stats;
    std::chrono::steady_clock::time_point stats_time = std::chrono::steady_clock::now();

    ~Impl() {
        convert_cancel = true;
        download_cancel = true;
        update_cancel = true;
        if (convert_worker.joinable()) convert_worker.join();
        if (download_worker.joinable()) download_worker.join();
        if (update_worker.joinable()) update_worker.join();
        if (open_worker.joinable()) open_worker.join();
        doc.streaming.reset();
        doc.view_world.reset();
        renderer.reset(); // before the host: on Windows it draws into the host's shared texture
        host.reset();
    }

    // ---- Messages -------------------------------------------------------------------------

    void send(const json& j) {
        std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
        if (!ui_ready || !host) {
            outbox.push_back(std::move(text));
            return;
        }
        host->post_json(text);
    }

    void toast(std::string_view kind, std::string_view message) { send({{"type", "toast"}, {"kind", kind}, {"message", message}}); }

    json assets_json() const {
        json j = {{"ok", models != nullptr}, {"jar", assets_jar}, {"error", assets_error},
                  {"viewer", renderer.has_value()}, {"viewer_error", viewer_error},
                  {"downloaded", using_downloaded_jar},
                  // The textures can be fetched from Mojang: missing, the viewer works, and no jar was chosen by hand.
                  {"can_download", models == nullptr && renderer.has_value() && settings.client_jar.empty()},
                  {"downloading", downloading}};
        if (const auto m = translate::MappingData::embedded()) {
            j["java_version"] = (*m)->java_version();
            j["bedrock_version"] = (*m)->bedrock_version();
        }
        return j;
    }

    json settings_json() const {
        return {{"client_jar", settings.client_jar}, {"packs", settings.packs}, {"render_distance", settings.render_distance},
                {"distant_terrain", settings.distant_terrain}, {"recent", settings.recent}, {"check_updates", settings.check_updates}};
    }

    void send_hello() {
        send({{"type", "hello"},
              {"version", BRIDGER_VERSION_STRING},
              {"platform", platform_name()},
              {"fly_input", host->fly_input() == FlyInput::Native ? "native" : "page"},
              {"gpu", renderer ? renderer->device_name() : std::string()},
              {"assets", assets_json()},
              {"settings", settings_json()}});
    }

    void on_message(std::string_view text) {
        const json m = json::parse(text, nullptr, false);
        if (!m.is_object() || !m.contains("type") || !m["type"].is_string()) return;
        const std::string type = m["type"];
        auto num = [&](const char* k, float d = 0.0f) { return m.contains(k) && m[k].is_number() ? m[k].get<float>() : d; };
        auto str = [&](const char* k) { return m.contains(k) && m[k].is_string() ? m[k].get<std::string>() : std::string(); };
        const float vh = renderer ? static_cast<float>(renderer->height()) : 1.0f;
        if (type == "ready") {
            ui_ready = true;
            send_hello();
            for (auto& s : outbox) host->post_json(s);
            outbox.clear();
            if (!pending_open.empty()) open(std::exchange(pending_open, {}));
            if (smoke) {
                smoke_stage = "interface ready";
                if (smoke_open.empty()) smoke_reached("interface ready");
            } else {
                check_for_update(false); // never from a smoke test
            }
        } else if (type == "update_check") {
            check_for_update(true);
        } else if (type == "update_install") {
            start_update_download();
        } else if (type == "update_cancel") {
            update_cancel = true;
        } else if (type == "update_skip") {
            if (update_release) {
                settings.skipped_version = update_release->version.str();
                save_settings(settings);
            }
        } else if (type == "update_restart") {
            install_update(true);
        } else if (type == "update_page") {
            if (update_release && update_release->page_url.starts_with("https://github.com/" + std::string(update::kRepository) + "/"))
                host->open_url(update_release->page_url);
        } else if (type == "orbit") {
            viewer.orbit(num("dx"), num("dy"));
        } else if (type == "pan") {
            viewer.pan(num("dx"), num("dy"), vh);
        } else if (type == "zoom") {
            viewer.zoom(num("delta"));
        } else if (type == "reset_camera") {
            if (has_doc) viewer.focus(doc.lo, doc.hi, aspect());
        } else if (type == "preview") {
            set_preview(m.value("on", false));
        } else if (type == "fly") {
            if (previewing) set_fly(m.value("on", false));
        } else if (type == "look") { // fly mode driven by the page (pointer lock)
            if (fly) viewer.look(num("dx"), num("dy"));
        } else if (type == "fly_keys") {
            const json k = m.value("keys", json::object());
            auto key = [&](const char* name) { return k.contains(name) && k[name].is_boolean() && k[name].get<bool>(); };
            page_keys = {key("forward"), key("back"), key("left"), key("right"), key("up"), key("down"), key("fast")};
        } else if (type == "view") {
            if (m.contains("map") && m["map"].is_boolean()) viewer.set_map(m["map"].get<bool>());
            if (m.contains("slice")) slice = m["slice"].is_number() ? std::optional<float>(m["slice"].get<float>()) : std::nullopt;
            dirty = true;
        } else if (type == "focus") {
            focus_positions(m.value("positions", json::array()));
        } else if (type == "clear_focus") {
            if (renderer) renderer->set_outlines({});
            dirty = true;
        } else if (type == "open") {
            open(str("path"));
        } else if (type == "open_dialog") {
            if (m.value("folder", false)) host->open_folder({}, on_ui([this](std::string f) { open(f); }));
            else host->open_file(kOpenFilters, on_ui([this](std::string f) { open(f); }));
        } else if (type == "pick_output") {
            pick_output(str("format"));
        } else if (type == "pick_into_world") {
            host->open_file(kWorldFilter, on_ui([this](std::string f) { send({{"type", "into_world_picked"}, {"path", f}}); }));
        } else if (type == "convert") {
            start_convert(m);
        } else if (type == "cancel_convert") {
            convert_cancel = true;
        } else if (type == "reveal") {
            host->reveal(str("path"));
        } else if (type == "pick_client_jar") {
            host->open_file(kJarFilter, on_ui([this](std::string f) {
                settings.client_jar = f;
                settings_changed(true);
            }));
        } else if (type == "add_pack") {
            host->open_file(kPackFilter, on_ui([this](std::string f) {
                if (std::find(settings.packs.begin(), settings.packs.end(), f) == settings.packs.end()) settings.packs.push_back(f);
                settings_changed(true);
            }));
        } else if (type == "download_textures") {
            start_texture_download();
        } else if (type == "cancel_textures") {
            download_cancel = true;
        } else if (type == "settings") {
            apply_settings(m);
        }
    }

    // ---- Dialog answers ---------------------------------------------------------------------------

    // A dialog callback (any thread) that runs `f` with the chosen path on the UI thread; nothing when cancelled.
    PathCallback on_ui(std::function<void(std::string)> f) {
        return [this, f = std::move(f)](std::optional<std::string> path) {
            if (path) tasks.post([f, p = std::move(*path)] { f(p); });
        };
    }

    // The output folder for a Java world: a new world folder inside the chosen one, named after the input.
    void output_folder_picked(std::optional<std::string> folder) {
        if (!folder) return send({{"type", "output_picked"}, {"path", nullptr}});
        const auto parent = platform::path_from_utf8(*folder);
        const auto input = platform::path_from_utf8(doc.path);
        if (!parent || !input) return;
        const fs::path first = *parent / input->stem();
        fs::path out = first;
        for (int i = 2; fs::exists(out) && i < 100; ++i) out = numbered(first, i);
        send({{"type", "output_picked"}, {"path", utf8(out)}});
    }

    // ---- Settings and assets ----------------------------------------------------------------

    void apply_settings(const json& m) {
        bool assets_changed = false;
        if (m.contains("client_jar") && m["client_jar"].is_string()) {
            assets_changed |= m["client_jar"].get<std::string>() != settings.client_jar;
            settings.client_jar = m["client_jar"];
        }
        if (m.contains("packs") && m["packs"].is_array()) {
            std::vector<std::string> packs;
            for (const auto& p : m["packs"])
                if (p.is_string()) packs.push_back(p.get<std::string>());
            assets_changed |= packs != settings.packs;
            settings.packs = std::move(packs);
        }
        if (m.contains("render_distance") && m["render_distance"].is_number_integer())
            settings.render_distance = std::clamp(m["render_distance"].get<int>(), 2, 48);
        if (m.contains("distant_terrain") && m["distant_terrain"].is_number_integer())
            settings.distant_terrain = std::clamp(m["distant_terrain"].get<int>(), 0, 96);
        if (m.contains("check_updates") && m["check_updates"].is_boolean()) settings.check_updates = m["check_updates"].get<bool>();
        apply_view_distance();
        settings_changed(assets_changed);
    }

    void settings_changed(bool assets_changed) {
        save_settings(settings);
        if (assets_changed) {
            const std::string reopen = has_doc ? doc.path : std::string();
            close_document();
            load_assets();
            if (!reopen.empty()) open(reopen); // re-mesh with the new textures
        }
        send({{"type", "settings"}, {"settings", settings_json()}, {"assets", assets_json()}});
    }

    void load_assets() {
        if (open_worker.joinable()) open_worker.join();
        doc.streaming.reset(); // uses the models
        models.reset();
        assets_error.clear();
        assets_jar.clear();
        using_downloaded_jar = false;
        if (!renderer) {
            assets_error = "the 3D preview is off on this computer";
            return;
        }
        std::vector<fs::path> packs;
        if (!settings.client_jar.empty()) {
            auto p = platform::path_from_utf8(settings.client_jar);
            if (p) packs.push_back(*p);
        } else {
            const auto m = translate::MappingData::embedded();
            const std::string version = m ? std::string((*m)->java_version()) : std::string("26.3");
            std::error_code ec;
            const auto downloaded = render::downloaded_client_jar_path(version);
            // BRIDGER_SKIP_LAUNCHER_JAR (testing): act as if Minecraft Java were not installed.
            const auto launcher = std::getenv("BRIDGER_SKIP_LAUNCHER_JAR") ? std::nullopt : render::AssetSource::find_client_jar(version);
            if (launcher) packs.push_back(*launcher);
            else if (downloaded && fs::exists(*downloaded, ec)) {
                packs.push_back(*downloaded); // fetched from Mojang earlier (asset_download.hpp)
                using_downloaded_jar = true;
            } else
                assets_error = "Minecraft Java " + version + " was not found in the launcher folder";
        }
        if (packs.empty()) {
            if (assets_error.empty()) assets_error = "no client jar";
            return;
        }
        assets_jar = utf8(packs.front());
        for (const auto& p : settings.packs)
            if (auto pp = platform::path_from_utf8(p)) packs.push_back(*pp);
        asset_paths = packs;
        auto assets = render::AssetSource::open(packs);
        if (!assets) {
            assets_error = assets.error().describe();
            return;
        }
        auto bm = render::BlockModels::create(std::make_shared<render::AssetSource>(std::move(*assets)));
        if (!bm) {
            assets_error = bm.error().describe();
            return;
        }
        models = std::make_unique<render::BlockModels>(std::move(*bm));
    }

    // ---- Distant terrain ---------------------------------------------------------------------

    // The distant terrain cache of a world (render/lod_cache.hpp): one folder per world, its last save
    // and the resource packs, so a cached column is never stale. The 8 most recently used are kept.
    std::shared_ptr<render::LodCache> distant_cache(const fs::path& world) {
        const auto base = platform::local_app_data_dir();
        if (!base) return nullptr;
        u64 h = 0xcbf29ce484222325ull;
        auto mix = [&h](std::string_view bytes) {
            for (const char c : bytes) h = (h ^ static_cast<u8>(c)) * 0x100000001b3ull;
            h = (h ^ 0xFF) * 0x100000001b3ull;
        };
        std::error_code ec;
        auto stamp = [&](const fs::path& p) {
            mix(utf8(p));
            mix(std::to_string(fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0));
            mix(std::to_string(fs::last_write_time(p, ec).time_since_epoch().count()));
        };
        stamp(fs::is_directory(world, ec) ? world / "level.dat" : world); // saving a world rewrites level.dat
        for (const fs::path& a : asset_paths) stamp(a);
        mix(BRIDGER_VERSION_STRING);
        const fs::path root = *base / "Bridger" / "cache" / "distant";
        const fs::path dir = root / std::format("{:016x}", h);
        fs::create_directories(dir, ec);
        fs::last_write_time(dir, fs::file_time_type::clock::now(), ec);
        std::vector<std::pair<fs::file_time_type, fs::path>> all;
        for (const auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory(ec)) all.emplace_back(e.last_write_time(ec), e.path());
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (usize i = 8; i < all.size(); ++i)
            if (all[i].second != dir) fs::remove_all(all[i].second, ec); // only Bridger's own cache folders
        return std::make_shared<render::LodCache>(dir);
    }

    // Render distance and distant terrain of the open world: streaming radii and fog.
    void apply_view_distance() {
        if (!has_doc || !doc.world) return;
        const int far = settings.distant_terrain > settings.render_distance ? settings.distant_terrain : settings.render_distance;
        if (doc.streaming) doc.streaming->set_distant_terrain(far > settings.render_distance ? far : 0, doc.distant_cache);
        const double r = far * 16.0;
        frame.fog_start = static_cast<float>(r * (far > settings.render_distance ? 0.7 : 0.8));
        frame.fog_end = static_cast<float>(r);
        dirty = true;
    }

    // ---- Opening ---------------------------------------------------------------------------

    float aspect() const {
        return renderer ? static_cast<float>(renderer->width()) / static_cast<float>(std::max(1u, renderer->height())) : 1.6f;
    }

    void close_document() {
        if (open_worker.joinable()) open_worker.join();
        doc.streaming.reset();
        doc.view_world.reset();
        doc = Document{};
        has_doc = false;
        if (renderer) {
            renderer->clear_sections();
            renderer->set_outlines({});
        }
        slice.reset();
        set_fly(false);
        previewing = false;
        viewer.set_map(false);
        dirty = true;
    }

    void set_preview(bool on) {
        if (on && (!has_doc || !renderer)) return;
        if (!on) set_fly(false);
        if (on && !previewing) {
            open_started = std::chrono::steady_clock::now();
            ttff_pending = true;
            stream_pending = doc.streaming != nullptr;
        }
        previewing = on;
        dirty = true;
    }

    void open(const std::string& path_utf8) {
        if (path_utf8.empty()) return;
        if (!ui_ready) {
            pending_open = path_utf8;
            return;
        }
        if (converting)
            return toast("info", "A conversion is running. Let it finish or cancel it before opening another file.");
        auto path = platform::path_from_utf8(path_utf8);
        if (!path) return toast("error", path.error().describe());
        // level.dat stands for its world folder.
        if (path->filename() == L"level.dat") path = path->parent_path();
        std::error_code ec;
        if (fs::is_directory(*path, ec) && !convert::is_world_path(*path))
            return toast("error", "\"" + utf8(path->filename()) + "\" is not a world folder: it has no level.dat. Choose the world's own folder (in .minecraft/saves, or minecraftWorlds for Bedrock).");
        close_document();
        open_started = std::chrono::steady_clock::now();
        const u64 gen = ++open_generation;
        const std::string name = utf8(path->filename());
        send({{"type", "loading"}, {"name", name}, {"step", "Reading"}, {"fraction", 0.05}});
        host->set_title("Bridger \xE2\x80\x94 " + name);
        const bool world = convert::is_world_path(*path);
        const std::string full = utf8(*path);
        open_worker = std::thread([this, gen, p = *path, full, name, world] {
            auto stale = [&] { return gen != open_generation.load(); };
            auto progress = [&](std::string_view step, double f) {
                tasks.post([this, gen, name, step = std::string(step), f] {
                    if (gen == open_generation) send({{"type", "loading"}, {"name", name}, {"step", step}, {"fraction", f}});
                });
            };
            auto failed = [&](std::string msg) {
                tasks.post([this, gen, msg = std::move(msg)] {
                    if (gen != open_generation) return;
                    send({{"type", "open_failed"}, {"message", msg}});
                    if (smoke) {
                        smoke_stage = "opening failed: " + msg;
                        smoke_failed = true;
                    }
                });
            };
            if (world) {
                auto vw = convert::ViewWorld::open(p);
                if (stale()) return;
                if (!vw) return failed(vw.error().describe());
                auto shared = std::make_shared<convert::ViewWorld>(std::move(*vw));
                tasks.post([this, gen, full, name, shared] {
                    if (gen == open_generation) opened_world(full, name, *shared);
                });
                return;
            }
            auto vs = convert::load_view_structure(p);
            if (stale()) return;
            if (!vs) return failed(vs.error().describe());
            progress("Counting blocks", 0.5);
            const u64 blocks = count_blocks(vs->java);
            auto view = std::make_shared<convert::ViewStructure>(std::move(*vs));
            // The file page appears now; the 3D model is built behind it (`preview_ready`).
            tasks.post([this, gen, full, name, view, blocks] {
                if (gen == open_generation) opened_structure(full, name, *view, blocks);
            });
            auto meshes = std::make_shared<std::optional<std::vector<render::MeshedSection>>>();
            if (models) {
                auto s = render::mesh_structure(view->java, *models, {0, 0, 0}, 0, nullptr);
                if (s) *meshes = std::move(*s);
                else log::error("{}", s.error().describe());
            }
            tasks.post([this, gen, meshes] {
                if (gen == open_generation) structure_meshed(*meshes);
            });
        });
    }

    void structure_meshed(const std::optional<std::vector<render::MeshedSection>>& sections) {
        if (sections && renderer && models) {
            for (const auto& s : *sections) (void)renderer->upload_section(s.origin, s.mesh);
            (void)renderer->upload_textures(models->texture_pixels(), static_cast<u32>(models->texture_count()));
        }
        dirty = true;
        send({{"type", "preview_ready"}, {"missing_models", models ? models->missing().size() : 0}});
        smoke_preview_ready();
    }

    void opened_structure(const std::string& path, const std::string& name, const convert::ViewStructure& v, u64 blocks) {
        doc.path = path;
        doc.name = name;
        doc.format = v.format;
        doc.world = false;
        const Extent e = v.java.size();
        doc.lo = glm::dvec3(0.0);
        doc.hi = glm::dvec3(e.x, e.y, e.z);
        has_doc = true;
        viewer.frame(doc.lo, doc.hi, aspect());
        frame.fog_start = 0;
        frame.fog_end = 0;
        dirty = true;
        settings.add_recent(path);
        save_settings(settings);
        const auto data_version = v.java.source().data_version;
        send({{"type", "opened"},
              {"doc",
               {{"kind", "structure"},
                {"path", path},
                {"name", name},
                {"format", format_key(v.format)},
                {"edition", v.format == FormatId::McStructure ? "bedrock" : "java"},
                {"size", {e.x, e.y, e.z}},
                {"blocks", blocks},
                {"block_entities", v.java.block_entities.size()},
                {"entities", v.java.entities.size()},
                {"data_version", data_version != 0 ? json(data_version) : json(nullptr)},
                {"source_version", version_json(v.source_version)},
                {"warnings", v.warnings},
                {"report", report_json(v.report)},
                {"targets", targets_for(v.format)}}},
              {"settings", settings_json()}});
    }

    void opened_world(const std::string& path, const std::string& name, convert::ViewWorld& w) {
        const convert::ViewWorldInfo info = w.info();
        doc.path = path;
        doc.name = name;
        doc.format = info.format;
        doc.world = true;
        doc.view_world = std::make_unique<convert::ViewWorld>(std::move(w));
        has_doc = true;
        const BlockPos sp = info.spawn;
        // Frame a modest area around the spawn (framing the whole render distance would put the camera
        // hundreds of blocks away, often inside a mountain).
        doc.lo = glm::dvec3(sp.x - 32, sp.y - 16, sp.z - 32);
        doc.hi = glm::dvec3(sp.x + 32, sp.y + 24, sp.z + 32);
        if (models) {
            const convert::ViewWorld* vw = doc.view_world.get();
            doc.streaming = std::make_unique<render::StreamingScene>(
                *models,
                [vw](ChunkPos p) -> std::optional<render::LoadedColumn> {
                    auto c = vw->load(p);
                    if (!c) return std::nullopt;
                    return render::LoadedColumn{c->pos, c->min_y, std::move(c->column)};
                },
                vw->chunks());
            if (auto p = platform::path_from_utf8(path)) doc.distant_cache = distant_cache(*p);
        }
        viewer.frame(doc.lo, doc.hi, aspect());
        apply_view_distance();
        settings.add_recent(path);
        save_settings(settings);
        send({{"type", "opened"},
              {"doc",
               {{"kind", "world"},
                {"path", path},
                {"name", info.level_name.empty() ? name : info.level_name},
                {"file_name", name},
                {"format", format_key(info.format)},
                {"edition", info.format == FormatId::McWorld ? "bedrock" : "java"},
                {"chunks", info.chunk_count},
                {"spawn", {sp.x, sp.y, sp.z}},
                {"seed", info.seed ? json(*info.seed) : json(nullptr)},
                {"source_version", version_json(info.source_version)},
                {"warnings", json::array()},
                {"report", json::object()},
                {"targets", targets_for(info.format)}}},
              {"settings", settings_json()}});
        send({{"type", "preview_ready"}, {"missing_models", 0}});
        smoke_preview_ready();
    }

    void focus_positions(const json& positions) {
        if (!renderer || !positions.is_array() || positions.empty()) return;
        std::vector<render::OutlineBox> boxes;
        glm::dvec3 lo(1e30), hi(-1e30);
        for (const auto& p : positions) {
            if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) continue;
            const glm::dvec3 b(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
            boxes.push_back({b - 0.02, b + 1.02, glm::vec4(1.0f, 0.72f, 0.25f, 1.0f)});
            lo = glm::min(lo, b);
            hi = glm::max(hi, b + 1.0);
        }
        if (boxes.empty()) return;
        renderer->set_outlines(std::move(boxes));
        viewer.focus(lo, hi, aspect());
        dirty = true;
    }

    // ---- Converting ------------------------------------------------------------------------

    void pick_output(const std::string& format) {
        const auto id = formats::format_from_name(format);
        if (!id || !has_doc) return;
        const auto input = platform::path_from_utf8(doc.path);
        if (!input) return;
        if (*id == FormatId::JavaWorld) {
            host->open_folder(utf8(input->parent_path()), [this](std::optional<std::string> folder) {
                tasks.post([this, folder] { output_folder_picked(folder); });
            });
            return;
        }
        const auto& f = formats::info(*id);
        fs::path first = input->parent_path() / input->stem();
        first += platform::path_from_utf8(f.extension).value_or(fs::path());
        // The same format (another game version): never suggest the input itself.
        fs::path suggested = first;
        for (int i = 2; fs::exists(suggested) && i < 100; ++i) suggested = numbered(first, i);
        const FileFilter* filter = &kSaveFilters[0];
        for (const auto& sf : kSaveFilters)
            if ("." + sf.patterns == f.extension) filter = &sf;
        host->save_file(*filter, utf8(suggested), [this](std::optional<std::string> path) {
            tasks.post([this, path] { send({{"type", "output_picked"}, {"path", path ? json(*path) : json(nullptr)}}); });
        });
    }

    void start_convert(const json& m) {
        if (converting || !has_doc) return;
        convert::ConvertOptions o;
        auto in = platform::path_from_utf8(doc.path);
        auto out = platform::path_from_utf8(m.value("output", std::string()));
        if (!in || !out || out->empty()) return toast("error", "Choose where to save the converted file first.");
        o.input = *in;
        o.output = *out;
        if (const auto f = formats::format_from_name(m.value("format", std::string()))) o.output_format = *f;
        o.level_name = m.value("level_name", std::string());
        o.target_version = m.value("target_version", std::string());
        o.overwrite = m.value("overwrite", false);
        o.placement.air = m.value("air", std::string("keep")) == "replace" ? convert::AirMode::Replace : convert::AirMode::Keep;
        const std::string height = m.value("height", std::string("fail"));
        o.placement.height = height == "clip"    ? convert::HeightPolicy::Clip
                             : height == "shift" ? convert::HeightPolicy::ShiftToFit
                                                 : convert::HeightPolicy::Fail;
        o.placement.clear_entities = m.value("clear_entities", false);
        if (m.contains("place_at") && m["place_at"].is_array() && m["place_at"].size() == 3 && m["place_at"][0].is_number_integer() &&
            m["place_at"][1].is_number_integer() && m["place_at"][2].is_number_integer())
            o.placement.place_at = BlockPos{m["place_at"][0].get<i32>(), m["place_at"][1].get<i32>(), m["place_at"][2].get<i32>()};
        if (const std::string into = m.value("into_world", std::string()); !into.empty())
            if (auto p = platform::path_from_utf8(into)) o.into_world = *p;
        o.fallback = translate::Fallback::Air;
        convert_cancel = false;
        o.cancel = &convert_cancel;
        auto last = std::make_shared<std::pair<float, std::string>>(-1.0f, std::string());
        o.progress = [this, last](float f, std::string_view step) {
            if (f - last->first < 0.004f && step == last->second) return;
            *last = {f, std::string(step)};
            tasks.post([this, f, step = std::string(step)] { send({{"type", "convert_progress"}, {"fraction", f}, {"step", step}}); });
        };
        converting = true;
        if (convert_worker.joinable()) convert_worker.join();
        send({{"type", "convert_progress"}, {"fraction", 0.0}, {"step", "Starting"}});
        convert_worker = std::thread([this, o] {
            const auto t0 = std::chrono::steady_clock::now();
            auto r = convert::convert(o);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!r) {
                const bool cancelled = r.error().code() == ErrorCode::Cancelled;
                return tasks.post([this, cancelled, msg = r.error().describe()] {
                    converting = false;
                    send({{"type", "convert_failed"}, {"message", msg}, {"cancelled", cancelled}});
                });
            }
            json summary = json::parse(convert::summary_to_json(*r, o), nullptr, false);
            summary["report"] = report_json(r->report);
            summary["ms"] = ms;
            summary["output"] = utf8(o.output);
            tasks.post([this, summary = std::move(summary)] {
                converting = false;
                send({{"type", "convert_done"}, {"summary", summary}});
            });
        });
    }

    // ---- Texture download -------------------------------------------------------------------

    // Only on the user's request: Mojang's client jar for the pinned version, verified and saved in
    // Bridger's own folder (docs/safety.md, "Network access").
    void start_texture_download() {
        if (downloading || models || !renderer) return;
        const auto m = translate::MappingData::embedded();
        const std::string version = m ? std::string((*m)->java_version()) : std::string("26.3");
        if (download_worker.joinable()) download_worker.join();
        downloading = true;
        download_cancel = false;
        send({{"type", "textures_progress"}, {"fraction", 0.0}, {"step", "Starting"}});
        download_worker = std::thread([this, version] {
            auto last = std::make_shared<float>(-1.0f);
            auto r = render::download_client_jar(version, [this, last](float f, std::string_view step) {
                if (f - *last >= 0.005f || f >= 1.0f) {
                    *last = f;
                    tasks.post([this, f, step = std::string(step)] { send({{"type", "textures_progress"}, {"fraction", f}, {"step", step}}); });
                }
                return !download_cancel.load();
            });
            tasks.post([this, ok = r.has_value(), cancelled = !r && r.error().code() == ErrorCode::Cancelled,
                        msg = r ? std::string() : r.error().describe()] {
                downloading = false;
                if (!ok) {
                    send({{"type", "textures_failed"}, {"message", msg}, {"cancelled", cancelled}});
                    return;
                }
                const std::string reopen = has_doc && !converting ? doc.path : std::string();
                if (!reopen.empty()) close_document();
                load_assets();
                send({{"type", "settings"}, {"settings", settings_json()}, {"assets", assets_json()}});
                send({{"type", "textures_done"}});
                toast("success", models ? "Textures ready. The 3D preview is available." : "Downloaded, but the textures could not be loaded: " + assets_error);
                if (!reopen.empty()) open(reopen); // build the 3D model with the new textures
            });
        });
    }

    // ---- Self-update (docs/app/updates.md) ---------------------------------------------------

    static std::optional<fs::path> app_dir() {
        const auto exe = platform::executable_path();
        if (!exe) return std::nullopt;
        return exe->parent_path();
    }

    // Automatic checks run at most once a day and only when enabled; `manual` (Settings, "Check now") always runs and
    // also reports "up to date" and errors.
    void check_for_update(bool manual) {
        if (update_busy) return;
        const i64 now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!manual && (!settings.check_updates || now - settings.last_update_check < 24 * 3600)) return;
        if (!manual) {
            settings.last_update_check = now;
            save_settings(settings);
        }
        if (update_worker.joinable()) update_worker.join();
        update_busy = true;
        update_worker = std::thread([this, manual] {
            auto r = update::fetch_latest();
            tasks.post([this, manual, r = std::move(r)]() mutable {
                update_busy = false;
                const auto current = update::Version::parse(BRIDGER_VERSION_STRING).value_or(update::Version{});
                if (!r) {
                    log::warn("update check: {}", r.error().describe());
                    if (manual) send({{"type", "update_failed"}, {"message", "Could not check for updates: " + r.error().describe()}, {"cancelled", false}});
                    return;
                }
                if (!(current < r->version) || (!manual && r->version.str() == settings.skipped_version)) {
                    if (manual) send({{"type", "update_none"}, {"current", current.str()}});
                    return;
                }
                update_release = std::move(*r);
                send({{"type", "update_available"},
                      {"version", update_release->version.str()},
                      {"current", current.str()},
                      {"notes", update_release->notes},
                      {"size", update_release->zip_size},
                      // Only the Windows zip installs itself; elsewhere the release page is offered (docs/adr/0010).
                      {"installable", update::kSelfInstall && (!update_release->sha256.empty() || !update_release->sha256_url.empty())}});
            });
        });
    }

    void start_update_download() {
        if (update_busy || !update_release || update_package) return;
        const auto dir = app_dir();
        if (!dir) return send({{"type", "update_failed"}, {"message", "The program's folder is unknown."}, {"cancelled", false}});
        if (update_worker.joinable()) update_worker.join();
        update_busy = true;
        update_cancel = false;
        send({{"type", "update_progress"}, {"fraction", 0.0}, {"step", "Starting"}});
        update_worker = std::thread([this, release = *update_release, staging = update::staging_dir(*dir)] {
            std::error_code ec;
            fs::remove_all(staging, ec); // an unfinished earlier attempt (only Bridger writes there)
            auto last = std::make_shared<float>(-1.0f);
            auto r = update::download_and_stage(release, staging, [this, last](float f, std::string_view step) {
                if (f - *last >= 0.005f || f >= 0.9f) {
                    *last = f;
                    tasks.post([this, f, step = std::string(step)] { send({{"type", "update_progress"}, {"fraction", f}, {"step", step}}); });
                }
                return !update_cancel.load();
            });
            if (!r) fs::remove_all(staging, ec);
            tasks.post([this, r = std::move(r)] {
                update_busy = false;
                if (!r) {
                    const bool cancelled = r.error().code() == ErrorCode::Cancelled;
                    send({{"type", "update_failed"}, {"message", r.error().describe()}, {"cancelled", cancelled}});
                    return;
                }
                update_package = *r;
                send({{"type", "update_ready"}, {"version", update_release->version.str()}});
            });
        });
    }

    // Swaps the program's files; with `restart` the new version is started and this one quits. Without it (the app is
    // closing) the new version starts next time.
    void install_update(bool restart) {
        if (!update_package || update_installed) return;
        if (restart && converting) return toast("info", "Bridger restarts after the conversion finishes. Try again then.");
        const auto dir = app_dir();
        if (!dir) return;
        if (const auto st = update::install(*update_package, *dir); !st) {
            update_package.reset();
            return send({{"type", "update_failed"}, {"message", st.error().describe()}, {"cancelled", false}});
        }
        update_installed = true;
        if (!restart) return;
        const auto exe = platform::executable_path();
        if (const auto st = exe ? platform::launch_detached(*exe, {}) : Status(tl::unexpected(exe.error())); !st) {
            toast("error", "Bridger was updated; start it again to use the new version.");
            return;
        }
        host->quit();
    }

    // ---- Fly mode --------------------------------------------------------------------------

    void set_fly(bool on) {
        if (on == fly || (on && !has_doc)) return;
        fly = on;
        viewer.set_fly(on);
        page_keys = {};
        if (host->fly_input() == FlyInput::Native) host->set_fly_capture(on);
        send({{"type", "fly"}, {"on", on}});
        dirty = true;
    }

    // ---- Main loop -------------------------------------------------------------------------

    bool init() {
        HostEvents ev;
        ev.message = [this](std::string_view text) { on_message(text); };
        ev.web_ready = [this](bool ok, std::string_view err) {
            if (!ok) {
                fatal("The user interface could not start: " + std::string(err));
                host->quit();
            }
        };
        ev.resized = [this](u32 w, u32 h) {
            if (renderer) {
                if (host->scene_mode() == SceneMode::SharedTexture) (void)renderer->set_shared_target(host->shared_handle(), w, h);
                else (void)renderer->resize(w, h);
            }
            dirty = true;
        };
        ev.close_requested = [this] { host->quit(); };
        ev.focus_lost = [this] { set_fly(false); };
        ev.drop_hover = [this](bool inside) { send({{"type", "drop"}, {"state", inside ? "enter" : "leave"}}); };
        ev.drop_file = [this](std::string path) { open(path); };
        ev.fly_look = [this](float dx, float dy) {
            if (fly) viewer.look(dx, dy);
        };
        ev.fly_wheel = [this](float d) {
            if (fly) viewer.zoom(d);
        };
        ev.fly_escape = [this] { set_fly(false); };
        host = Host::create(std::move(ev));
        if (!host) return false;

        render::RendererConfig cfg;
        cfg.width = host->width();
        cfg.height = host->height();
        cfg.shared_target = host->scene_mode() == SceneMode::SharedTexture;
        cfg.validation = std::getenv("BRIDGER_VULKAN_VALIDATION") != nullptr; // debugging: Vulkan validation layers
        // The 3D preview is optional: without a Vulkan 1.3 GPU (or, on Windows, without texture sharing with
        // Direct3D) the app still opens files and converts them; the preview says why it is off.
        if (std::getenv("BRIDGER_NO_VIEWER") != nullptr) viewer_error = "turned off by BRIDGER_NO_VIEWER"; // testing
        else if (auto r = render::Renderer::create(cfg)) renderer.emplace(std::move(*r));
        else viewer_error = r.error().describe();
        if (!host->attach_scene(renderer ? renderer->adapter_luid() : std::nullopt))
            return fatal("Could not set up the window's drawing surface (Direct3D 11 / DirectComposition).");
        if (renderer && cfg.shared_target)
            if (auto s = renderer->set_shared_target(host->shared_handle(), cfg.width, cfg.height); !s) {
                viewer_error = s.error().describe();
                renderer.reset();
            }
        if (!renderer) log::warn("3D preview disabled: {}", viewer_error);
        settings = load_settings();
        load_assets();
        bundle = UiBundle::embedded();
        return true;
    }

    bool fatal(const std::string& message) {
        log::error("{}", message);
        if (host && !smoke) host->error_box(message);
        smoke_failed = true;
        return false;
    }

    // ---- Smoke test (RunOptions::smoke_test) ------------------------------------------------------

    std::optional<int> smoke; // seconds to keep the window after everything came up
    std::string smoke_stage = "starting";
    std::chrono::steady_clock::time_point smoke_started, smoke_done;
    bool smoke_up = false, smoke_failed = false;
    double smoke_first_frame_ms = -1.0;
    double present_ms_sum = 0.0;  // handing a frame's pixels to the host (SceneMode::Pixels)
    double gpu_wait_ms_sum = 0.0; // waiting for the GPU to finish that frame
    u64 present_frames = 0;
    std::string smoke_open;

    void smoke_reached(std::string stage) {
        if (!smoke || smoke_up) return;
        smoke_stage = std::move(stage);
        smoke_up = true;
        smoke_done = std::chrono::steady_clock::now();
    }

    // The preview's model is ready: open the preview (a click otherwise), or finish when there is no 3D view.
    void smoke_preview_ready() {
        if (!smoke) return;
        smoke_stage = "3D model ready";
        if (renderer) send({{"type", "show_preview"}});
        else smoke_reached("file page shown (no 3D preview)");
    }

    void smoke_tick() {
        if (!smoke) return;
        const auto now = std::chrono::steady_clock::now();
        const bool timed_out = !smoke_up && now - smoke_started > std::chrono::seconds(90);
        if (!smoke_failed && !timed_out && (!smoke_up || now - smoke_done < std::chrono::seconds(*smoke))) return;
        const bool ok = smoke_up && !smoke_failed;
        const std::string preview = renderer ? renderer->device_name() : "off, " + viewer_error;
        std::string first = smoke_first_frame_ms >= 0 ? std::format("; first frame {:.0f} ms after opening", smoke_first_frame_ms) : "";
        if (present_frames > 0 && renderer)
            first += std::format("; {} frames of {}x{} through memory: GPU wait {:.2f} ms, hand-over {:.2f} ms per frame", present_frames,
                                 renderer->width(), renderer->height(), gpu_wait_ms_sum / static_cast<double>(present_frames),
                                 present_ms_sum / static_cast<double>(present_frames));
        std::printf("smoke test: %s (reached: %s; interface %s; 3D preview: %s%s)\n", ok ? "ok" : "FAILED", smoke_stage.c_str(),
                    ui_ready ? "ready" : "not ready", preview.c_str(), first.c_str());
        if (const char* shot = std::getenv("BRIDGER_SMOKE_SCREENSHOT"); shot != nullptr && *shot != 0)
            std::printf("smoke test: screenshot %s\n", host->save_screenshot(shot) ? shot : "not available on this system");
        std::fflush(stdout);
        if (!ok) smoke_failed = true;
        host->quit();
    }

    void start_web() {
#ifdef NDEBUG
        const bool dev_tools = std::getenv("BRIDGER_DEVTOOLS") != nullptr;
#else
        const bool dev_tools = true;
#endif
        host->start_web(
            [this](std::string_view path) -> std::optional<Host::WebFile> {
                const auto f = bundle.find(path);
                if (!f) return std::nullopt;
                return Host::WebFile{f->bytes, std::string(f->mime)};
            },
            dev_tools);
    }

    void tick_stats(double frame_ms, bool rendered) {
        if (rendered) {
            ++frames;
            frame_ms_sum += frame_ms;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - stats_time).count();
        if (elapsed < 0.5) return;
        json j = {{"type", "stats"},
                  {"fps", frames / elapsed},
                  {"frame_ms", frames > 0 ? frame_ms_sum / frames : 0.0},
                  {"sections", last_stats.sections_drawn},
                  {"quads", last_stats.quads_drawn},
                  {"gpu_mb", renderer ? static_cast<double>(renderer->gpu_bytes()) / (1024.0 * 1024.0) : 0.0}};
        if (doc.streaming) {
            const auto st = doc.streaming->stats();
            j["chunks_loaded"] = st.loaded;
            j["chunks_distant"] = st.distant;
            j["chunks_total"] = st.available;
            j["chunks_pending"] = st.queued + st.in_flight;
        }
        send(j);
        frames = 0;
        frame_ms_sum = 0.0;
        stats_time = now;
    }

    // One pass of the main loop (Host::run): background results, the camera, streaming, and a frame when something
    // changed. Returns true while the next pass should come as soon as possible.
    bool tick() {
        if (tasks.run_all()) dirty = true;
        smoke_tick();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;

        FlyKeys keys;
        if (fly) {
            if (host->fly_input() == FlyInput::Native) {
                const HostFlyKeys k = host->fly_keys();
                keys = {k.forward, k.back, k.left, k.right, k.up, k.down, k.fast};
            } else {
                keys = page_keys;
            }
        }
        viewer.update(dt, keys);
        const glm::vec3 sky_goal = has_doc ? kSky : kCharcoalLinear;
        sky = glm::mix(sky_goal, sky, std::exp(-6.0f * dt));
        const bool sky_moving = glm::length(sky - sky_goal) > 0.002f;

        streaming_busy = false;
        if (doc.streaming && previewing) {
            render::Camera center = viewer.camera();
            center.position = viewer.stream_center();
            streaming_busy = doc.streaming->update(center, *renderer, settings.render_distance, 32);
        }
        const auto busy = [&] { return dirty || fly || (previewing && streaming_busy); };

        const render::Camera& cam = viewer.camera();
        const bool moved = cam.position != last_camera.position || cam.yaw_degrees != last_camera.yaw_degrees ||
                           cam.pitch_degrees != last_camera.pitch_degrees || viewer.map_half_extent() != last_extent;
        if (!renderer || !previewing || !(dirty || moved || sky_moving || streaming_busy || fly)) {
            tick_stats(0.0, false);
            return busy();
        }
        dirty = false;
        last_camera = cam;
        last_extent = viewer.map_half_extent();

        frame.sky_color = sky;
        frame.map_mode = viewer.map();
        frame.map_half_extent = viewer.map_half_extent();
        frame.slice_y = slice;
        const bool shared = host->scene_mode() == SceneMode::SharedTexture;
        if (shared) host->begin_frame();
        auto r = renderer->render(cam, frame);
        if (!r) log::error("{}", r.error().describe());
        else last_stats = *r;
        if (shared) {
            host->present();
        } else {
            // Linux, macOS: the frame goes through memory (docs/adr/0010); its cost is reported by the smoke test.
            using ms = std::chrono::duration<double, std::milli>;
            const auto p0 = std::chrono::steady_clock::now();
            const auto px = renderer->pixels(); // waits for the GPU to finish the frame
            const auto p1 = std::chrono::steady_clock::now();
            if (px) host->present_pixels(*px, renderer->width(), renderer->height());
            gpu_wait_ms_sum += ms(p1 - p0).count();
            present_ms_sum += ms(std::chrono::steady_clock::now() - p1).count();
            ++present_frames;
        }
        tick_stats(r ? r->cpu_ms : 0.0, true);
        const auto since_open = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count(); };
        if (ttff_pending && has_doc) {
            ttff_pending = false;
            log::info("time to first frame: {:.0f} ms ({})", since_open(), doc.name);
            if (smoke && !smoke_up) {
                smoke_first_frame_ms = since_open();
                smoke_reached("3D preview drew its first frame");
            }
        }
        if (stream_pending && !streaming_busy && doc.streaming) {
            stream_pending = false;
            const auto st = doc.streaming->stats();
            log::info("render distance loaded: {:.0f} ms, {} chunks, {} quads", since_open(), st.loaded, st.quads);
        }
        return busy();
    }

    int run(const RunOptions& options) {
        smoke = options.smoke_test;
        smoke_open = options.open_path;
        smoke_started = std::chrono::steady_clock::now();
        if (!smoke)
            if (const auto dir = app_dir()) update::cleanup(*dir); // files left by an update installed last time
        if (!init()) return 1;
        if (bundle.empty()) return fatal("The user interface is missing from this build.") ? 0 : 1;
        start_web();
        host->show();
        if (!options.open_path.empty()) open(options.open_path);
        last_tick = std::chrono::steady_clock::now();
        host->run([this] { return tick(); });
        if (smoke) return smoke_failed ? 1 : 0;
        install_update(false); // a downloaded update the user did not restart for: installed on the way out
        return 0;
    }

    // Main loop state (tick).
    std::chrono::steady_clock::time_point last_tick;
    render::Camera last_camera;
    float last_extent = 0.0f;
    bool streaming_busy = false;
};

App::App() : impl_(std::make_unique<Impl>()) {}
App::~App() = default;
int App::run(const RunOptions& options) { return impl_->run(options); }

} // namespace bridger::app
