// Viewer performance on a synthetic 128 x 64 x 128 terrain (512 K solid blocks with caves, a grass
// surface and glass), textured by a synthetic resource pack written at start (no Mojang assets):
//  - BM_MeshStructure: bake + mesh every section (all threads, and one thread);
//  - BM_MeshSectionDense: one 16^3 section of random solid/air (the mesher's worst case);
//  - BM_RenderFrame: steady-state offscreen frames at 1920 x 1080 (skipped without a Vulkan 1.3 GPU).

#include <bridger/render/assets.hpp>
#include <bridger/render/block_models.hpp>
#include <bridger/render/image_io.hpp>
#include <bridger/render/mesher.hpp>
#include <bridger/render/renderer.hpp>
#include <bridger/render/scene.hpp>

#include <benchmark/benchmark.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>

using namespace bridger;
using namespace bridger::render;

namespace {

constexpr u32 kW = 128, kH = 64, kL = 128;

void write_text(const std::filesystem::path& p, std::string_view text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void write_texture(const std::filesystem::path& p, u8 r, u8 g, u8 b, bool holes) {
    std::vector<u8> rgba(16 * 16 * 4);
    for (usize i = 0; i < 256; ++i) {
        const u8 n = static_cast<u8>((i * 37) % 23);
        rgba[i * 4 + 0] = static_cast<u8>(r - n);
        rgba[i * 4 + 1] = static_cast<u8>(g - n);
        rgba[i * 4 + 2] = static_cast<u8>(b - n);
        rgba[i * 4 + 3] = holes && (i % 5 == 0) ? 0 : 255;
    }
    std::filesystem::create_directories(p.parent_path());
    (void)write_png(p, 16, 16, rgba);
}

// The pack lives for the whole run (benchmarks share it).
struct World {
    std::filesystem::path dir;
    std::optional<BlockModels> models;
    std::optional<Structure> structure;

    World() {
        dir = std::filesystem::temp_directory_path() / "bridger-bench-render";
        std::filesystem::remove_all(dir);
        const auto a = dir / "assets" / "minecraft";
        write_text(a / "models/block/cube_all.json", R"({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{
            "down":{"texture":"#all","cullface":"down"},"up":{"texture":"#all","cullface":"up"},
            "north":{"texture":"#all","cullface":"north"},"south":{"texture":"#all","cullface":"south"},
            "west":{"texture":"#all","cullface":"west"},"east":{"texture":"#all","cullface":"east"}}}]})");
        const std::pair<const char*, std::array<u8, 3>> blocks[] = {
            {"stone", {125, 125, 125}}, {"dirt", {134, 96, 67}}, {"grass_block", {110, 170, 70}},
            {"deepslate", {80, 80, 85}}, {"glass", {200, 230, 255}}};
        for (const auto& [name, c] : blocks) {
            const std::string n(name);
            write_text(a / ("models/block/" + n + ".json"), R"({"parent":"block/cube_all","textures":{"all":"block/)" + n + R"("}})");
            write_text(a / ("blockstates/" + n + ".json"), R"({"variants":{"":{"model":"block/)" + n + R"("}}})");
            write_texture(a / ("textures/block/" + n + ".png"), c[0], c[1], c[2], n == "glass");
        }
        auto assets = AssetSource::open({dir});
        models.emplace(std::move(*BlockModels::create(std::make_shared<AssetSource>(std::move(*assets)))));

        auto reg = std::make_shared<BlockStateRegistry>();
        auto s = Structure::create(reg, std::make_shared<BiomeRegistry>(), {kW, kH, kL}, {Edition::Java, 5023}).value();
        const StateId stone = reg->intern(*BlockState::parse("minecraft:stone"));
        const StateId deep = reg->intern(*BlockState::parse("minecraft:deepslate"));
        const StateId dirt = reg->intern(*BlockState::parse("minecraft:dirt"));
        const StateId grass = reg->intern(*BlockState::parse("minecraft:grass_block"));
        const StateId glass = reg->intern(*BlockState::parse("minecraft:glass"));
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> pct(0, 99);
        for (u32 z = 0; z < kL; ++z)
            for (u32 x = 0; x < kW; ++x) {
                const u32 top = 28 + static_cast<u32>(4.0 * std::sin(x * 0.11) + 3.0 * std::cos(z * 0.07));
                for (u32 y = 0; y <= top; ++y) {
                    StateId id = y < 12 ? deep : y + 3 < top ? stone : y < top ? dirt : grass;
                    if (y > 2 && y + 4 < top && pct(rng) < 12) continue; // caves: interior faces to mesh
                    (void)s.set_block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}, id);
                }
                if (pct(rng) < 2) (void)s.set_block({static_cast<i32>(x), static_cast<i32>(top + 1), static_cast<i32>(z)}, glass);
            }
        structure.emplace(std::move(s));
    }
    ~World() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

World& world() {
    static World w;
    return w;
}

void BM_MeshStructure(benchmark::State& state) {
    World& w = world();
    const usize threads = static_cast<usize>(state.range(0));
    u64 quads = 0;
    for (auto _ : state) {
        SceneStats st;
        auto sections = mesh_structure(*w.structure, *w.models, {0, 0, 0}, threads, &st);
        benchmark::DoNotOptimize(sections);
        quads = st.quads;
    }
    state.SetItemsProcessed(state.iterations() * static_cast<i64>(kW) * kH * kL);
    state.counters["quads"] = static_cast<double>(quads);
}
BENCHMARK(BM_MeshStructure)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond)->UseRealTime();

void BM_MeshSectionDense(benchmark::State& state) {
    World& w = world();
    const BakedBlock* stone = &w.models->bake(*BlockState::parse("minecraft:stone"));
    std::array<const BakedBlock*, kPaddedVolume> blocks{};
    std::mt19937 rng(3);
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) blocks[padded_index(x, y, z)] = (rng() & 1) ? stone : nullptr;
    SectionMesh mesh;
    for (auto _ : state) {
        mesh = {};
        mesh_section(blocks, {0, 0, 0}, mesh);
        benchmark::DoNotOptimize(mesh);
    }
    state.SetItemsProcessed(state.iterations() * 4096);
    state.counters["quads"] = static_cast<double>(mesh.solid.size() / 4);
}
BENCHMARK(BM_MeshSectionDense)->Unit(benchmark::kMicrosecond);

void BM_RenderFrame(benchmark::State& state) {
    World& w = world();
    RendererConfig cfg;
    cfg.width = 1920;
    cfg.height = 1080;
    auto r = Renderer::create(cfg);
    if (!r) {
        state.SkipWithError("no Vulkan 1.3 GPU");
        return;
    }
    auto sections = mesh_structure(*w.structure, *w.models).value();
    for (const auto& s : sections) (void)r->upload_section(s.origin, s.mesh);
    (void)r->upload_textures(w.models->texture_pixels(), static_cast<u32>(w.models->texture_count()));
    const Camera cam = frame_camera(glm::vec3(0), glm::vec3(kW, kH, kL), 16.0f / 9.0f);
    FrameSettings fs;
    for (int i = 0; i < 4; ++i) (void)r->render(cam, fs); // uploads, pipeline warm-up
    u64 quads = 0;
    for (auto _ : state) {
        auto st = r->render(cam, fs); // waits for the frame two back: steady state = GPU-bound frame time
        quads = st ? st->quads_drawn : 0;
    }
    (void)r->read_pixels(); // waits for the last frame
    state.counters["quads"] = static_cast<double>(quads);
    state.counters["fps"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.SetLabel(r->device_name());
}
BENCHMARK(BM_RenderFrame)->Unit(benchmark::kMillisecond)->UseRealTime();

} // namespace

// Distant terrain: summarize a loaded 16 x 384 x 16 column of rolling hills (no assets needed), then
// build its LOD mesh at 2-block cells; what one distant column costs a streaming worker after loading.
void BM_LodColumn(benchmark::State& state) {
    render::BakedBlock stone, grass;
    stone.layer = grass.layer = render::RenderLayer::Solid;
    stone.map_color = 0xFF808080u;
    grass.map_color = 0xFF7F7F7Fu;
    grass.tint = render::Tint::Grass;
    grass.map_tinted = true;
    auto blocks = std::make_shared<BlockStateRegistry>();
    const StateId sid = blocks->intern(BlockState("minecraft:stone", {})), gid = blocks->intern(BlockState("minecraft:grass_block", {}));
    auto column = Structure::create(blocks, std::make_shared<BiomeRegistry>(), Extent{16, 384, 16}, {Edition::Java, 0}).value();
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            const int h = 64 + static_cast<int>(6.0 * std::sin(x * 0.4) + 4.0 * std::cos(z * 0.3));
            for (int y = -64; y <= h; ++y) column.set_block({x, y + 64, z}, y == h ? gid : sid);
        }
    std::vector<const render::BakedBlock*> table(blocks->size(), nullptr);
    table[sid] = &stone;
    table[gid] = &grass;
    const render::ColumnSummary hills = render::summarize_column(column, table, -64);
    usize quads = 0;
    for (auto _ : state) {
        const render::ColumnSummary s = render::summarize_column(column, table, -64);
        std::map<int, render::SectionMesh> out;
        render::mesh_lod(s, {&hills, &hills, &hills, &hills}, {0, -64, 0}, 2, 3, out);
        quads = 0;
        for (const auto& [y, m] : out) quads += m.solid.size() / 4;
        benchmark::DoNotOptimize(out);
    }
    state.counters["quads"] = static_cast<double>(quads);
}
BENCHMARK(BM_LodColumn)->Unit(benchmark::kMicrosecond);

void BM_LodMeshOnly(benchmark::State& state) {
    render::ColumnSummary s;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            s.top[static_cast<usize>(z * 16 + x)] = static_cast<i16>(64 + static_cast<int>(6.0 * std::sin(x * 0.4) + 4.0 * std::cos(z * 0.3)));
            s.color[static_cast<usize>(z * 16 + x)] = 0xFF30A030u;
        }
    for (auto _ : state) {
        std::map<int, render::SectionMesh> out;
        render::mesh_lod(s, {&s, &s, &s, &s}, {0, -64, 0}, 2, 3, out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_LodMeshOnly)->Unit(benchmark::kMicrosecond);
