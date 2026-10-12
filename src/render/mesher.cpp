#include <bridger/render/mesher.hpp>

#include <algorithm>
#include <cmath>

namespace bridger::render {
namespace {

// The game's per-face brightness (down, up, north, south, west, east).
constexpr std::array<float, 6> kFaceShade{0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};
constexpr std::array<float, 4> kAo{1.0f, 0.78f, 0.62f, 0.48f};

u32 pack_color(glm::vec3 c, float alpha = 1.0f) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    const auto b = [](float v) { return static_cast<u32>(std::lround(v * 255.0f)); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(alpha) << 24);
}

u16 unorm(float v) { return static_cast<u16>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); }

bool opaque_cube(const BakedBlock* b) {
    return b != nullptr && b->occludes[0] && b->occludes[1] && b->occludes[2] && b->occludes[3] && b->occludes[4] && b->occludes[5];
}

u32 hash_pos(glm::ivec3 p) {
    u32 h = static_cast<u32>(p.x) * 73856093u ^ static_cast<u32>(p.y) * 19349663u ^ static_cast<u32>(p.z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

std::vector<Vertex>& layer_of(SectionMesh& m, RenderLayer l) {
    switch (l) {
    case RenderLayer::Cutout: return m.cutout;
    case RenderLayer::Translucent: return m.translucent;
    default: return m.solid;
    }
}

struct Ctx {
    const std::array<const BakedBlock*, kPaddedVolume>& blocks;
    const BakedBlock* at(int x, int y, int z) const { return blocks[padded_index(x, y, z)]; }
};

// Ambient occlusion level (0..3) at a corner of a full face.
int corner_ao(const Ctx& c, glm::ivec3 pos, int face, glm::vec3 corner) {
    const glm::ivec3 n = kFaceNormals[static_cast<usize>(face)];
    const int axis = face / 2 == 0 ? 1 : face / 2 == 1 ? 2 : 0;
    const int t1 = axis == 0 ? 1 : 0, t2 = axis == 2 ? 1 : 2;
    glm::ivec3 d1(0), d2(0);
    d1[t1] = corner[t1] > 0.5f ? 1 : -1;
    d2[t2] = corner[t2] > 0.5f ? 1 : -1;
    const glm::ivec3 base = pos + n;
    const bool s1 = opaque_cube(c.at(base.x + d1.x, base.y + d1.y, base.z + d1.z));
    const bool s2 = opaque_cube(c.at(base.x + d2.x, base.y + d2.y, base.z + d2.z));
    const bool cr = opaque_cube(c.at(base.x + d1.x + d2.x, base.y + d1.y + d2.y, base.z + d1.z + d2.z));
    if (s1 && s2) return 3;
    return static_cast<int>(s1) + static_cast<int>(s2) + static_cast<int>(cr);
}

void emit_quad(std::vector<Vertex>& out, const std::array<glm::vec3, 4>& p, const std::array<glm::vec2, 4>& uv, u16 layer,
               const std::array<u32, 4>& colors, bool cutout, bool flip) {
    // Split along the diagonal with the brighter pair so AO interpolates without artefacts.
    static constexpr std::array<usize, 4> kNormal{0, 1, 2, 3}, kFlipped{1, 2, 3, 0};
    const auto& order = flip ? kFlipped : kNormal;
    for (const usize i : order)
        out.push_back({p[i].x, p[i].y, p[i].z, unorm(uv[i].x), unorm(uv[i].y), layer, static_cast<u16>(cutout ? 1 : 0), colors[i]});
}

// Fluid height at a position: sources 14/16, flowing water lower with its level, 1 under the same fluid.
float fluid_height(const Ctx& c, int x, int y, int z, Fluid f) {
    const BakedBlock* above = c.at(x, y + 1, z);
    if (above != nullptr && above->fluid == f) return 1.0f;
    const BakedBlock* b = c.at(x, y, z);
    if (b == nullptr || !b->fluid_only || b->fluid_level == 0) return 0.875f;
    if (b->fluid_level >= 8) return 1.0f; // falling
    return std::max(0.125f, static_cast<float>(8 - b->fluid_level) / 9.0f);
}

void mesh_fluid(const Ctx& c, glm::ivec3 pos, Fluid f, SectionMesh& out, const BakedBlock& self, u16 water_layer, u16 lava_layer) {
    const float h = fluid_height(c, pos.x, pos.y, pos.z, f);
    const glm::vec3 tint = f == Fluid::Water ? tint_color(Tint::Water) : glm::vec3(1.0f);
    const float alpha = f == Fluid::Water ? 0.75f : 1.0f;
    std::vector<Vertex>& dst = f == Fluid::Water ? out.translucent : out.solid;
    const u16 layer = f == Fluid::Water ? water_layer : lava_layer;
    const glm::vec3 lo(0.0f), hi(1.0f, h, 1.0f);
    (void)self;
    for (int face = 0; face < 6; ++face) {
        const glm::ivec3 n = kFaceNormals[static_cast<usize>(face)];
        const BakedBlock* nb = c.at(pos.x + n.x, pos.y + n.y, pos.z + n.z);
        if (nb != nullptr && nb->fluid == f) continue;                          // same fluid
        if (face != 1 && nb != nullptr && nb->occludes[static_cast<usize>(face ^ 1)]) continue; // hidden by a wall
        if (face == 1 && h >= 1.0f && nb != nullptr && nb->occludes[0]) continue;
        std::array<glm::vec3, 4> p;
        switch (face) {
        case 0: p = {{{lo.x, lo.y, hi.z}, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}}}; break;
        case 1: p = {{{lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}}}; break;
        case 2: p = {{{hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}}}; break;
        case 3: p = {{{lo.x, hi.y, hi.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}}}; break;
        case 4: p = {{{lo.x, hi.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}}}; break;
        default: p = {{{hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}}}; break;
        }
        for (glm::vec3& v : p) v += glm::vec3(pos);
        const float vh = face <= 1 ? 1.0f : h;
        const std::array<glm::vec2, 4> uv{{{0, 0}, {0, vh}, {1, vh}, {1, 0}}};
        const u32 col = pack_color(tint * kFaceShade[static_cast<usize>(face)], alpha);
        emit_quad(dst, p, uv, layer, {col, col, col, col}, false, false);
    }
}

} // namespace

void mesh_section(const std::array<const BakedBlock*, kPaddedVolume>& blocks, glm::ivec3 section_world, SectionMesh& out) {
    Ctx c{blocks};
    // The fluid textures are looked up through any fluid block's first quad-less entry: the
    // renderer passes them as the reserved layers 1 (water) and 2 (lava), see Scene.
    constexpr u16 kWaterLayer = 1, kLavaLayer = 2;
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                const BakedBlock* b = c.at(x, y, z);
                if (b == nullptr) continue;
                const glm::ivec3 pos(x, y, z);
                if (b->fluid != Fluid::None) mesh_fluid(c, pos, b->fluid, out, *b, kWaterLayer, kLavaLayer);
                if (b->layer == RenderLayer::Invisible || b->variants.empty()) continue;
                const BakedModel& model = b->variants.size() == 1 ? b->variants[0]
                                                                   : b->variants[hash_pos(section_world + pos) % b->variants.size()];
                std::vector<Vertex>& dst = layer_of(out, b->layer);
                const glm::vec3 tint = tint_color(b->tint);
                const bool cutout = b->layer == RenderLayer::Cutout;
                for (const BakedQuad& q : model.quads) {
                    if (q.cull_face >= 0) {
                        const glm::ivec3 n = kFaceNormals[static_cast<usize>(q.cull_face)];
                        const BakedBlock* nb = c.at(x + n.x, y + n.y, z + n.z);
                        if (nb != nullptr && nb->occludes[static_cast<usize>(q.cull_face ^ 1)]) continue;
                        if (nb == b && b->cull_same) continue;
                    }
                    const float shade = q.shade ? kFaceShade[q.normal_face] : 1.0f;
                    const glm::vec3 base = (q.tint_index >= 0 ? tint : glm::vec3(1.0f)) * shade;
                    std::array<u32, 4> colors;
                    std::array<int, 4> ao{0, 0, 0, 0};
                    if (q.full_face && b->layer != RenderLayer::Translucent)
                        for (usize i = 0; i < 4; ++i) ao[i] = corner_ao(c, pos, q.normal_face, q.pos[i]);
                    for (usize i = 0; i < 4; ++i) colors[i] = pack_color(base * kAo[static_cast<usize>(ao[i])]);
                    std::array<glm::vec3, 4> p;
                    for (usize i = 0; i < 4; ++i) p[i] = q.pos[i] + glm::vec3(pos);
                    emit_quad(dst, p, q.uv, q.texture, colors, cutout, ao[0] + ao[2] > ao[1] + ao[3]);
                }
            }
}

} // namespace bridger::render
