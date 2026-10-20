#include <bridger/render/scene.hpp>

#include <bridger/core/parallel.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bridger::render {

Result<std::vector<MeshedSection>> mesh_structure(const Structure& java, BlockModels& models, glm::ivec3 origin, usize threads,
                                                  SceneStats* stats) {
    if (java.source().edition != Edition::Java) return make_error(ErrorCode::InvalidArgument, "the viewer needs a Java structure");
    const auto t0 = std::chrono::steady_clock::now();
    // Bake every registered state once; air stays nullptr.
    const BlockStateRegistry& reg = java.block_registry();
    std::vector<const BakedBlock*> table(reg.size(), nullptr);
    for (StateId id = 0; id < reg.size(); ++id) {
        const BlockState& s = reg.get(id);
        if (s.id() == "minecraft:air" || s.id() == "minecraft:cave_air" || s.id() == "minecraft:void_air" ||
            s.id() == "minecraft:structure_void")
            continue;
        table[id] = &models.bake(s);
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Sections in the world grid: the structure's local origin may not be section-aligned.
    const Extent size = java.size();
    const glm::ivec3 lo(origin.x >> 4, origin.y >> 4, origin.z >> 4);
    const glm::ivec3 hi((origin.x + static_cast<i32>(size.x) - 1) >> 4, (origin.y + static_cast<i32>(size.y) - 1) >> 4,
                        (origin.z + static_cast<i32>(size.z) - 1) >> 4);
    std::vector<glm::ivec3> grid;
    if (size.volume() > 0)
        for (i32 y = lo.y; y <= hi.y; ++y)
            for (i32 z = lo.z; z <= hi.z; ++z)
                for (i32 x = lo.x; x <= hi.x; ++x) grid.push_back({x * 16, y * 16, z * 16});

    std::vector<MeshedSection> out(grid.size());
    auto lookup = [&](glm::ivec3 world) -> const BakedBlock* {
        const BlockPos local{world.x - origin.x, world.y - origin.y, world.z - origin.z};
        if (!java.contains(local)) return nullptr;
        const StateId id = java.block(local);
        return id < table.size() ? table[id] : nullptr;
    };
    parallel_for(grid.size(), threads, [&](usize i) {
        const glm::ivec3 so = grid[i];
        auto blocks = std::make_unique<std::array<const BakedBlock*, kPaddedVolume>>();
        for (int y = -1; y <= 16; ++y)
            for (int z = -1; z <= 16; ++z)
                for (int x = -1; x <= 16; ++x) (*blocks)[padded_index(x, y, z)] = lookup(so + glm::ivec3(x, y, z));
        out[i].origin = so;
        mesh_section(*blocks, so, out[i].mesh);
    });
    const auto t2 = std::chrono::steady_clock::now();
    if (stats != nullptr) {
        stats->sections = out.size();
        stats->non_empty = 0;
        stats->quads = 0;
        for (const MeshedSection& m : out) {
            stats->non_empty += m.mesh.empty() ? 0u : 1u;
            stats->quads += (m.mesh.solid.size() + m.mesh.cutout.size() + m.mesh.translucent.size()) / 4;
        }
        stats->bake_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats->mesh_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
    std::erase_if(out, [](const MeshedSection& m) { return m.mesh.empty(); });
    return out;
}

Camera frame_camera(glm::vec3 box_min, glm::vec3 box_max, float aspect, float yaw_degrees, float pitch_degrees) {
    Camera c;
    c.yaw_degrees = yaw_degrees;
    c.pitch_degrees = pitch_degrees;
    const glm::vec3 centre = (box_min + box_max) * 0.5f;
    const float radius = std::max(2.0f, glm::length(box_max - box_min) * 0.5f);
    const float fov = glm::radians(c.fov_y_degrees) * std::min(1.0f, aspect);
    const float dist = radius / std::tan(fov * 0.5f) * 0.9f;
    c.position = glm::dvec3(centre - c.forward() * dist);
    c.far_plane = std::max(4096.0f, dist * 4.0f);
    return c;
}

} // namespace bridger::render
