// `bridger render <input> <output.png>`: a screenshot of a structure file or a world, rendered
// offscreen with the viewer's renderer and the textures of the user's own Minecraft installation.

#include "commands.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/convert/view_world.hpp>
#include <bridger/core/log.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/render/image_io.hpp>
#include <bridger/render/renderer.hpp>
#include <bridger/render/scene.hpp>
#include <bridger/render/lod_cache.hpp>
#include <bridger/render/streaming.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <optional>

namespace bridger::cli {

int run_render(const RenderArgs& a) {
    namespace fs = std::filesystem;
    auto fail = [](const Error& e) {
        log::error("{}", e.describe());
        return kExitError;
    };
    u32 w = 1600, h = 900;
    if (std::sscanf(a.size.c_str(), "%ux%u", &w, &h) != 2 || w < 16 || h < 16 || w > 8192 || h > 8192) {
        log::error("--size must look like 1600x900");
        return kExitError;
    }
    auto in = platform::path_from_utf8(a.input);
    auto out = platform::path_from_utf8(a.output);
    if (!in || !out) return fail(!in ? in.error() : out.error());

    const auto t0 = std::chrono::steady_clock::now();
    const bool world = convert::is_world_path(*in);
    std::optional<convert::ViewStructure> view;
    std::optional<convert::ViewWorld> view_world;
    if (world) {
        auto vw = convert::ViewWorld::open(*in);
        if (!vw) return fail(vw.error());
        view_world.emplace(std::move(*vw));
    } else {
        auto vs = convert::load_view_structure(*in);
        if (!vs) return fail(vs.error());
        for (const auto& warning : vs->warnings) log::warn("{}", warning);
        view.emplace(std::move(*vs));
    }

    // Assets: the client jar of the pinned Java version, then any resource packs.
    std::vector<fs::path> packs;
    if (!a.client_jar.empty()) {
        auto p = platform::path_from_utf8(a.client_jar);
        if (!p) return fail(p.error());
        packs.push_back(*p);
    } else {
        const auto m = translate::MappingData::embedded();
        const std::string version = m ? std::string((*m)->java_version()) : std::string("26.3");
        const auto jar = render::AssetSource::find_client_jar(version);
        if (!jar) {
            log::error("Minecraft Java {0} is not installed (expected %APPDATA%/.minecraft/versions/{0}/{0}.jar); use --client-jar", version);
            return kExitError;
        }
        packs.push_back(*jar);
    }
    for (const auto& p : a.packs) {
        auto pp = platform::path_from_utf8(p);
        if (!pp) return fail(pp.error());
        packs.push_back(*pp);
    }
    auto assets = render::AssetSource::open(packs);
    if (!assets) return fail(assets.error());
    auto models = render::BlockModels::create(std::make_shared<render::AssetSource>(std::move(*assets)));
    if (!models) return fail(models.error());

    render::RendererConfig cfg;
    cfg.width = w;
    cfg.height = h;
    auto renderer = render::Renderer::create(cfg);
    if (!renderer) return fail(renderer.error());

    render::FrameSettings settings;
    settings.fog_start = 0;
    settings.fog_end = 0;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    render::Camera cam;
    std::string summary;
    std::optional<render::StreamingScene> scene;
    if (world) {
        const BlockPos sp = view_world->info().spawn;
        const convert::ViewWorld* vw = &*view_world;
        scene.emplace(*models, [vw](ChunkPos p) -> std::optional<render::LoadedColumn> {
            auto c = vw->load(p);
            if (!c) return std::nullopt;
            return render::LoadedColumn{c->pos, c->min_y, std::move(c->column)};
        }, view_world->chunks());
        if (a.distant > a.radius) {
            std::shared_ptr<render::LodCache> cache;
            if (!a.lod_cache.empty()) {
                auto dir = platform::path_from_utf8(a.lod_cache);
                if (!dir) return fail(dir.error());
                cache = std::make_shared<render::LodCache>(*dir);
            }
            scene->set_distant_terrain(a.distant, std::move(cache));
        }
        cam.position = glm::dvec3(sp.x, sp.y, sp.z);
        scene->load_all(cam, *renderer, a.radius);
        if (const auto st = scene->flush_cache(); !st) log::warn("distant terrain cache: {}", st.error().describe());
        const float r = static_cast<float>(a.radius) * 16.0f;
        const auto hr = scene->height_range();
        const float top = hr ? static_cast<float>(hr->second) : static_cast<float>(sp.y + 16);
        cam = render::frame_camera(glm::vec3(static_cast<float>(sp.x) - r * 0.5f, static_cast<float>(sp.y) - 8.0f, static_cast<float>(sp.z) - r * 0.5f),
                                   glm::vec3(static_cast<float>(sp.x) + r * 0.5f, std::max(top, static_cast<float>(sp.y) + 16.0f), static_cast<float>(sp.z) + r * 0.5f),
                                   aspect, a.yaw, a.pitch);
        if (a.zoom > 0.0f && a.zoom != 1.0f) {
            const glm::dvec3 centre(sp.x, sp.y, sp.z);
            cam.position = centre + (cam.position - centre) / static_cast<double>(a.zoom);
        }
        settings.fog_start = r * 1.1f;
        settings.fog_end = r * 1.6f;
        if (a.distant > a.radius) {
            settings.fog_start = static_cast<float>(a.distant) * 16.0f * 0.75f;
            settings.fog_end = static_cast<float>(a.distant) * 16.0f;
        }
        if (a.map) {
            settings.map_mode = true;
            settings.map_half_extent = r * 0.9f;
            cam.position = glm::dvec3(sp.x, 0, sp.z);
        }
        const auto st = scene->stats();
        summary = std::format("{} world '{}': {} of {} chunks loaded, {} distant, {} quads", formats::info(view_world->info().format).name,
                              view_world->info().level_name, st.loaded, st.available, st.distant, st.quads);
    } else {
        render::SceneStats stats;
        auto sections = render::mesh_structure(view->java, *models, {0, 0, 0}, 0, &stats);
        if (!sections) return fail(sections.error());
        for (const auto& sec : *sections)
            if (auto st = renderer->upload_section(sec.origin, sec.mesh); !st) return fail(st.error());
        const Extent e = view->java.size();
        const glm::vec3 box_max(static_cast<float>(e.x), static_cast<float>(e.y), static_cast<float>(e.z));
        cam = render::frame_camera(glm::vec3(0), box_max, aspect, a.yaw, a.pitch);
        if (a.zoom > 0.0f && a.zoom != 1.0f) {
            const glm::dvec3 centre = glm::dvec3(box_max) * 0.5;
            cam.position = centre + (cam.position - centre) / static_cast<double>(a.zoom);
        }
        if (a.map) {
            settings.map_mode = true;
            settings.map_half_extent = std::max(box_max.x, box_max.z) * 0.55f + 1.0f;
            cam.position = glm::dvec3(static_cast<double>(box_max.x) * 0.5, 0.0, static_cast<double>(box_max.z) * 0.5);
        }
        summary = std::format("{}: {} x {} x {}, {} sections, {} quads (bake {:.0f} ms, mesh {:.0f} ms)", a.input, e.x, e.y, e.z,
                              stats.non_empty, stats.quads, stats.bake_ms, stats.mesh_ms);
    }
    // Textures last: baking may add layers (entity faces) while meshing.
    if (auto st = renderer->upload_textures(models->texture_pixels(), static_cast<u32>(models->texture_count())); !st) return fail(st.error());
    if (!a.slice.empty()) settings.slice_y = std::stof(a.slice);

    auto frame = renderer->render(cam, settings);
    if (!frame) return fail(frame.error());
    auto pixels = renderer->read_pixels();
    if (!pixels) return fail(pixels.error());
    if (auto st = render::write_png(*out, renderer->width(), renderer->height(), *pixels); !st) return fail(st.error());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s\n", std::format("{}, GPU {} -> {} in {:.0f} ms", summary, renderer->device_name(), a.output, ms).c_str());
    if (!models->missing().empty()) {
        log::warn("{} block states had no model in the resource packs", models->missing().size());
        for (const auto& m : models->missing()) log::debug("no model: {}", m);
    }
    return kExitOk;
}

} // namespace bridger::cli
