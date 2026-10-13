#include "app.hpp"

#include "settings.hpp"
#include "ui_bundle.hpp"
#include "viewer.hpp"
#include "win32/compositor.hpp"
#include "win32/shell.hpp"
#include "win32/webview_host.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/convert/view_world.hpp>
#include <bridger/core/block_registry.hpp>
#include <bridger/core/log.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/render/renderer.hpp>
#include <bridger/render/scene.hpp>
#include <bridger/render/streaming.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <SDL3/SDL.h>
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

constexpr u32 kCharcoal = 0x0F1217; // --bg in src/app/ui/src/styles/tokens.css
constexpr glm::vec3 kCharcoalLinear{0.059f, 0.071f, 0.090f};
constexpr glm::vec3 kSky{0.62f, 0.76f, 1.0f};

std::string utf8(const fs::path& p) { return platform::path_to_utf8(p).value_or(std::string()); }

std::string format_key(FormatId id) { return std::string(formats::info(id).name); }

bool is_structure_format(FormatId f) {
    return f == FormatId::LegacySchematic || f == FormatId::SpongeSchem || f == FormatId::Litematic || f == FormatId::StructureNbt ||
           f == FormatId::McStructure;
}

// What the convert dialog offers for an input (docs/formats/README.md, supported paths).
json targets_for(FormatId input) {
    std::vector<FormatId> ids;
    if (is_structure_format(input))
        ids = {FormatId::McWorld, FormatId::McStructure, FormatId::SpongeSchem, FormatId::Litematic, FormatId::StructureNbt};
    else if (input == FormatId::JavaWorld)
        ids = {FormatId::McWorld};
    else if (input == FormatId::McWorld)
        ids = {FormatId::JavaWorld};
    json out = json::array();
    for (const FormatId id : ids) {
        const auto& f = formats::info(id);
        out.push_back({{"id", f.name},
                       {"ext", f.extension},
                       {"edition", id == FormatId::McWorld || id == FormatId::McStructure ? "bedrock" : "java"},
                       {"description", f.description}});
    }
    return out;
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
};

const SDL_DialogFileFilter kOpenFilters[] = {
    {"Minecraft structures and worlds", "schem;litematic;nbt;schematic;mcstructure;mcworld;dat"},
    {"All files", "*"},
};
const SDL_DialogFileFilter kJarFilter[] = {{"Minecraft client jar", "jar"}};
const SDL_DialogFileFilter kPackFilter[] = {{"Resource pack", "zip"}};
const SDL_DialogFileFilter kWorldFilter[] = {{"Bedrock world", "mcworld"}};
const SDL_DialogFileFilter kSaveFilters[] = {
    {"Bedrock world", "mcworld"},          {"Bedrock structure", "mcstructure"}, {"Sponge schematic", "schem"},
    {"Litematica schematic", "litematic"}, {"Structure block file", "nbt"},
};

} // namespace

struct App::Impl {
    SDL_Window* window = nullptr;
    void* hwnd = nullptr;
    std::optional<render::Renderer> renderer; // empty without a usable GPU: the app still converts
    std::string viewer_error;
    std::unique_ptr<Compositor> compositor;
    std::unique_ptr<WebViewHost> web;
    UiBundle bundle;
    TaskQueue tasks;
    Settings settings;
    bool running = true;
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
        if (convert_worker.joinable()) convert_worker.join();
        if (open_worker.joinable()) open_worker.join();
        doc.streaming.reset();
        doc.view_world.reset();
        web.reset();
        renderer.reset();
        compositor.reset();
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    // ---- Messages -------------------------------------------------------------------------

    void send(const json& j) {
        std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
        if (!ui_ready || !web) {
            outbox.push_back(std::move(text));
            return;
        }
        web->post_json(text);
    }

    void toast(std::string_view kind, std::string_view message) { send({{"type", "toast"}, {"kind", kind}, {"message", message}}); }

    json assets_json() const {
        json j = {{"ok", models != nullptr}, {"jar", assets_jar}, {"error", assets_error},
                  {"viewer", renderer.has_value()}, {"viewer_error", viewer_error}};
        if (const auto m = translate::MappingData::embedded()) {
            j["java_version"] = (*m)->java_version();
            j["bedrock_version"] = (*m)->bedrock_version();
        }
        return j;
    }

    json settings_json() const {
        return {{"client_jar", settings.client_jar}, {"packs", settings.packs}, {"render_distance", settings.render_distance}, {"recent", settings.recent}};
    }

    void send_hello() {
        send({{"type", "hello"},
              {"version", BRIDGER_VERSION_STRING},
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
            for (auto& s : outbox) web->post_json(s);
            outbox.clear();
            if (!pending_open.empty()) open(std::exchange(pending_open, {}));
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
            if (m.value("folder", false)) SDL_ShowOpenFolderDialog(&Impl::on_open_picked, this, window, nullptr, false);
            else SDL_ShowOpenFileDialog(&Impl::on_open_picked, this, window, kOpenFilters, 2, nullptr, false);
        } else if (type == "pick_output") {
            pick_output(str("format"));
        } else if (type == "pick_into_world") {
            SDL_ShowOpenFileDialog(&Impl::on_into_world_picked, this, window, kWorldFilter, 1, nullptr, false);
        } else if (type == "convert") {
            start_convert(m);
        } else if (type == "cancel_convert") {
            convert_cancel = true;
        } else if (type == "reveal") {
            reveal_in_explorer(str("path"));
        } else if (type == "pick_client_jar") {
            SDL_ShowOpenFileDialog(&Impl::on_jar_picked, this, window, kJarFilter, 1, nullptr, false);
        } else if (type == "add_pack") {
            SDL_ShowOpenFileDialog(&Impl::on_pack_picked, this, window, kPackFilter, 1, nullptr, false);
        } else if (type == "settings") {
            apply_settings(m);
        }
    }

    // ---- Dialog callbacks (any thread) ------------------------------------------------------

    static std::optional<std::string> first_file(const char* const* list) {
        if (list == nullptr || list[0] == nullptr) return std::nullopt;
        return std::string(list[0]);
    }
    static void on_open_picked(void* self, const char* const* list, int) {
        if (auto f = first_file(list)) static_cast<Impl*>(self)->tasks.post([s = static_cast<Impl*>(self), f = *f] { s->open(f); });
    }
    static void on_into_world_picked(void* self, const char* const* list, int) {
        if (auto f = first_file(list))
            static_cast<Impl*>(self)->tasks.post([s = static_cast<Impl*>(self), f = *f] { s->send({{"type", "into_world_picked"}, {"path", f}}); });
    }
    static void on_output_picked(void* self, const char* const* list, int) {
        auto* s = static_cast<Impl*>(self);
        auto f = first_file(list);
        s->tasks.post([s, f] { s->send({{"type", "output_picked"}, {"path", f ? json(*f) : json(nullptr)}}); });
    }
    static void on_output_folder_picked(void* self, const char* const* list, int) {
        auto* s = static_cast<Impl*>(self);
        auto f = first_file(list);
        s->tasks.post([s, f] {
            if (!f) return s->send({{"type", "output_picked"}, {"path", nullptr}});
            // A new world folder inside the chosen folder, named after the input.
            const auto parent = platform::path_from_utf8(*f);
            const auto input = platform::path_from_utf8(s->doc.path);
            if (!parent || !input) return;
            fs::path out = *parent / input->stem();
            for (int i = 2; fs::exists(out) && i < 100; ++i) out = *parent / (input->stem().wstring() + L" (" + std::to_wstring(i) + L")");
            s->send({{"type", "output_picked"}, {"path", utf8(out)}});
        });
    }
    static void on_jar_picked(void* self, const char* const* list, int) {
        if (auto f = first_file(list))
            static_cast<Impl*>(self)->tasks.post([s = static_cast<Impl*>(self), f = *f] {
                s->settings.client_jar = f;
                s->settings_changed(true);
            });
    }
    static void on_pack_picked(void* self, const char* const* list, int) {
        if (auto f = first_file(list))
            static_cast<Impl*>(self)->tasks.post([s = static_cast<Impl*>(self), f = *f] {
                if (std::find(s->settings.packs.begin(), s->settings.packs.end(), f) == s->settings.packs.end()) s->settings.packs.push_back(f);
                s->settings_changed(true);
            });
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
            if (auto jar = render::AssetSource::find_client_jar(version)) packs.push_back(*jar);
            else assets_error = "Minecraft Java " + version + " was not found in the launcher folder";
        }
        if (packs.empty()) {
            if (assets_error.empty()) assets_error = "no client jar";
            return;
        }
        assets_jar = utf8(packs.front());
        for (const auto& p : settings.packs)
            if (auto pp = platform::path_from_utf8(p)) packs.push_back(*pp);
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
        close_document();
        open_started = std::chrono::steady_clock::now();
        const u64 gen = ++open_generation;
        const std::string name = utf8(path->filename());
        send({{"type", "loading"}, {"name", name}, {"step", "Reading"}, {"fraction", 0.05}});
        SDL_SetWindowTitle(window, ("Bridger \xE2\x80\x94 " + name).c_str());
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
                    if (gen == open_generation) send({{"type", "open_failed"}, {"message", msg}});
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
        const double r = settings.render_distance * 16.0;
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
        }
        viewer.frame(doc.lo, doc.hi, aspect());
        frame.fog_start = static_cast<float>(r * 0.8);
        frame.fog_end = static_cast<float>(r * 1.0);
        dirty = true;
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
                {"warnings", json::array()},
                {"report", json::object()},
                {"targets", targets_for(info.format)}}},
              {"settings", settings_json()}});
        send({{"type", "preview_ready"}, {"missing_models", 0}});
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
            const std::string dir = utf8(input->parent_path());
            SDL_ShowOpenFolderDialog(&Impl::on_output_folder_picked, this, window, dir.c_str(), false);
            return;
        }
        const auto& f = formats::info(*id);
        fs::path suggested = input->parent_path() / input->stem();
        suggested += platform::path_from_utf8(f.extension).value_or(fs::path());
        const std::string def = utf8(suggested);
        const SDL_DialogFileFilter* filter = &kSaveFilters[0];
        for (const auto& sf : kSaveFilters)
            if ("." + std::string(sf.pattern) == f.extension) filter = &sf;
        SDL_ShowSaveFileDialog(&Impl::on_output_picked, this, window, filter, 1, def.c_str());
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

    // ---- Fly mode --------------------------------------------------------------------------

    void set_fly(bool on) {
        if (on == fly || (on && !has_doc)) return;
        fly = on;
        viewer.set_fly(on);
        SDL_SetWindowRelativeMouseMode(window, on);
        if (web) web->set_input_enabled(!on);
        if (on) focus_window(hwnd);
        else if (web) web->focus();
        send({{"type", "fly"}, {"on", on}});
        dirty = true;
    }

    // ---- Main loop -------------------------------------------------------------------------

    bool init() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            log::error("SDL_Init failed: {}", SDL_GetError());
            return false;
        }
        window = SDL_CreateWindow("Bridger", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        if (window == nullptr) {
            log::error("SDL_CreateWindow failed: {}", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window, 960, 600);
        hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        style_title_bar(hwnd, kCharcoal);

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        render::RendererConfig cfg;
        cfg.width = static_cast<u32>(w);
        cfg.height = static_cast<u32>(h);
        cfg.shared_target = true;
        // The 3D preview is optional: without a Vulkan 1.3 GPU (or without texture sharing with
        // Direct3D) the app still opens files and converts them; the preview says why it is off.
        if (std::getenv("BRIDGER_NO_VIEWER") != nullptr) viewer_error = "turned off by BRIDGER_NO_VIEWER"; // testing
        else if (auto r = render::Renderer::create(cfg)) renderer.emplace(std::move(*r));
        else viewer_error = r.error().describe();
        compositor = Compositor::create(hwnd, renderer ? renderer->adapter_luid() : std::nullopt, cfg.width, cfg.height);
        if (!compositor) return fatal("Could not set up the window's drawing surface (Direct3D 11 / DirectComposition).");
        if (renderer)
            if (auto s = renderer->set_shared_target(compositor->shared_handle(), cfg.width, cfg.height); !s) {
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
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bridger", message.c_str(), window);
        return false;
    }

    void start_web() {
        WebViewCallbacks cb;
        cb.resource = [this](std::string_view path) -> std::optional<WebResource> {
            const auto f = bundle.find(path);
            if (!f) return std::nullopt;
            return WebResource{f->bytes, std::string(f->mime)};
        };
        cb.on_message = [this](std::string_view json_text) { on_message(json_text); };
        cb.on_ready = [this](bool ok, std::string_view err) {
            compositor->commit();
            if (!ok)
                fatal("The user interface could not start: " + std::string(err) +
                      "\n\nBridger needs the Microsoft Edge WebView2 Runtime (included in Windows 11).");
        };
        std::string data_dir = "bridger-webview";
        if (const char* local = std::getenv("LOCALAPPDATA")) data_dir = std::string(local) + "\\Bridger\\WebView2";
#ifdef NDEBUG
        const bool dev_tools = std::getenv("BRIDGER_DEVTOOLS") != nullptr;
#else
        const bool dev_tools = true;
#endif
        web = WebViewHost::create(hwnd, compositor->ui_visual(), "https://bridger.local/index.html", data_dir, std::move(cb), dev_tools);
    }

    void handle_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            const auto w = static_cast<u32>(std::max(1, e.window.data1)), h = static_cast<u32>(std::max(1, e.window.data2));
            if (compositor->resize(w, h) && renderer) (void)renderer->set_shared_target(compositor->shared_handle(), w, h);
            if (web) web->resize(e.window.data1, e.window.data2);
            dirty = true;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Focus moving to the WebView inside this window is not "leaving the app".
            if (!window_is_foreground(hwnd)) set_fly(false);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (fly) viewer.look(e.motion.xrel, e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (fly) viewer.zoom(e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
            // Scancodes: physical keys, whatever the keyboard layout (WASD on an Arabic layout too).
            if (fly && e.key.scancode == SDL_SCANCODE_ESCAPE) set_fly(false);
            break;
        case SDL_EVENT_DROP_BEGIN: send({{"type", "drop"}, {"state", "enter"}}); break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data != nullptr) open(e.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE: send({{"type", "drop"}, {"state", "leave"}}); break;
        default: break;
        }
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
            j["chunks_total"] = st.available;
            j["chunks_pending"] = st.queued + st.in_flight;
        }
        send(j);
        frames = 0;
        frame_ms_sum = 0.0;
        stats_time = now;
    }

    int run(const std::string& open_path) {
        if (!init()) return 1;
        if (bundle.empty()) return fatal("The user interface is missing from this build.") ? 0 : 1;
        start_web();
        SDL_ShowWindow(window);
        if (!open_path.empty()) open(open_path);
        auto last = std::chrono::steady_clock::now();
        render::Camera last_camera;
        float last_extent = 0.0f;
        bool streaming_busy = false;
        while (running) {
            SDL_Event e;
            // Idle: wait for input instead of spinning when nothing moves.
            const bool idle = !dirty && !fly && !(previewing && streaming_busy);
            if (idle ? SDL_WaitEventTimeout(&e, 15) : SDL_PollEvent(&e)) {
                handle_event(e);
                while (SDL_PollEvent(&e)) handle_event(e);
            }
            if (tasks.run_all()) dirty = true;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            FlyKeys keys;
            if (fly) {
                const bool* k = SDL_GetKeyboardState(nullptr);
                keys = {k[SDL_SCANCODE_W],     k[SDL_SCANCODE_S],
                        k[SDL_SCANCODE_A],     k[SDL_SCANCODE_D],
                        k[SDL_SCANCODE_SPACE], k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT],
                        k[SDL_SCANCODE_LCTRL]};
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

            const render::Camera& cam = viewer.camera();
            const bool moved = cam.position != last_camera.position || cam.yaw_degrees != last_camera.yaw_degrees ||
                               cam.pitch_degrees != last_camera.pitch_degrees || viewer.map_half_extent() != last_extent;
            if (!renderer || !previewing || !(dirty || moved || sky_moving || streaming_busy || fly)) {
                tick_stats(0.0, false);
                continue;
            }
            dirty = false;
            last_camera = cam;
            last_extent = viewer.map_half_extent();

            frame.sky_color = sky;
            frame.map_mode = viewer.map();
            frame.map_half_extent = viewer.map_half_extent();
            frame.slice_y = slice;
            compositor->wait_for_copy();
            auto r = renderer->render(cam, frame);
            if (!r) log::error("{}", r.error().describe());
            else last_stats = *r;
            compositor->present();
            tick_stats(r ? r->cpu_ms : 0.0, true);
            const auto since_open = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count(); };
            if (ttff_pending && has_doc) {
                ttff_pending = false;
                log::info("time to first frame: {:.0f} ms ({})", since_open(), doc.name);
            }
            if (stream_pending && !streaming_busy && doc.streaming) {
                stream_pending = false;
                const auto st = doc.streaming->stats();
                log::info("render distance loaded: {:.0f} ms, {} chunks, {} quads", since_open(), st.loaded, st.quads);
            }
        }
        return 0;
    }
};

App::App() : impl_(std::make_unique<Impl>()) {}
App::~App() = default;
int App::run(const std::string& open_path) { return impl_->run(open_path); }

} // namespace bridger::app
