// Translation (palette stage + context stage) of a 256 x 128 x 256 Java structure with
// 24 distinct states, a share of them waterlogged.

#include <bridger/translate/translator.hpp>

#include <benchmark/benchmark.h>

#include <random>

using namespace bridger;

namespace {

constexpr u32 kW = 256, kH = 128, kL = 256;

Structure make_java_structure() {
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto s = Structure::create(blocks, std::make_shared<BiomeRegistry>(), {kW, kH, kL}, {Edition::Java, 5023}).value();
    std::vector<StateId> states;
    for (const char* f : {"north", "south", "east", "west"})
        for (const char* h : {"top", "bottom"})
            for (const char* w : {"true", "false"})
                states.push_back(blocks->intern(*BlockState::parse(std::string("minecraft:oak_stairs[facing=") + f +
                                                                   ",half=" + h + ",shape=straight,waterlogged=" + w + "]")));
    for (const char* b : {"stone", "dirt", "grass_block", "oak_planks", "cobblestone", "glass", "white_wool", "bricks"})
        states.push_back(blocks->intern(*BlockState::parse(std::string("minecraft:") + b)));
    std::mt19937 rng(3);
    // Terrain-like: solid below y=48, sparse above.
    for (u32 cy = 0; cy < s.sections_y(); ++cy)
        for (u32 cz = 0; cz < s.sections_z(); ++cz)
            for (u32 cx = 0; cx < s.sections_x(); ++cx) {
                std::array<u32, kSectionVolume> v{};
                for (usize i = 0; i < kSectionVolume; ++i) {
                    const u32 y = cy * 16 + static_cast<u32>(i >> 8);
                    v[i] = y < 48 ? states[16 + (rng() % 8)] : (rng() % 20 == 0 ? states[rng() % 16] : kAirStateId);
                }
                s.section(cx, cy, cz).blocks().assign(v);
            }
    return s;
}

void BM_TranslateStructure(benchmark::State& state) {
    const Structure java = make_java_structure();
    const auto* mapping = translate::MappingData::embedded().value();
    for (auto _ : state) {
        translate::ConversionReport report;
        auto out = translate::translate_to_bedrock(java, *mapping, {}, report);
        benchmark::DoNotOptimize(out);
    }
    const auto sections = static_cast<i64>(java.sections_x()) * java.sections_y() * java.sections_z();
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * sections);
    state.SetLabel("items = sections (16^3), 8.4M blocks");
}
BENCHMARK(BM_TranslateStructure)->Unit(benchmark::kMillisecond);

// Bedrock -> Java on the Bedrock form of the same structure: palette stage, layer 2 ->
// waterlogged, and the stair-shape context rule at every stair.
void BM_TranslateToJava(benchmark::State& state) {
    const Structure java = make_java_structure();
    const auto* mapping = translate::MappingData::embedded().value();
    translate::ConversionReport forward;
    const Structure bedrock = translate::translate_to_bedrock(java, *mapping, {}, forward).value();
    for (auto _ : state) {
        translate::ConversionReport report;
        auto out = translate::translate_to_java(bedrock, *mapping, {}, report);
        benchmark::DoNotOptimize(out);
    }
    const auto sections = static_cast<i64>(java.sections_x()) * java.sections_y() * java.sections_z();
    state.SetItemsProcessed(static_cast<i64>(state.iterations()) * sections);
    state.SetLabel("items = sections (16^3), 8.4M blocks");
}
BENCHMARK(BM_TranslateToJava)->Unit(benchmark::kMillisecond);

} // namespace
