#pragma once

// From a Java structure to section meshes ready for the renderer: bakes every block state the
// structure uses (single-threaded, BlockModels is not thread-safe), then meshes all sections in
// parallel. Also places a camera that frames the structure.

#include <bridger/core/structure.hpp>
#include <bridger/render/block_models.hpp>
#include <bridger/render/camera.hpp>
#include <bridger/render/mesher.hpp>

#include <vector>

namespace bridger::render {

struct MeshedSection {
    glm::ivec3 origin; // world block origin of the section
    SectionMesh mesh;
};

struct SceneStats {
    usize sections = 0;
    usize non_empty = 0;
    u64 quads = 0;
    double bake_ms = 0;
    double mesh_ms = 0;
};

// `java` must be a Java-edition structure. Its local (0,0,0) is placed at world `origin`.
[[nodiscard]] Result<std::vector<MeshedSection>> mesh_structure(const Structure& java, BlockModels& models, glm::ivec3 origin = {0, 0, 0},
                                                                usize threads = 0, SceneStats* stats = nullptr);

// A camera looking at the structure's box from the south-east, slightly above.
[[nodiscard]] Camera frame_camera(glm::vec3 box_min, glm::vec3 box_max, float aspect, float yaw_degrees = -35.0f,
                                  float pitch_degrees = -28.0f);

} // namespace bridger::render
