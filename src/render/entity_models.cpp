// Block-entity geometry for blocks that still have no JSON model in Java 26.3 (chests, shulker
// boxes; beds and signs became JSON models). Box sizes and texture offsets are
// the game's entity model values (docs/formats/assets.md, "Block entities"); a box at texture
// offset (u, v) with size (w, h, d) uses the usual cube net:
//   row v .. v+d:        [u+d, u+d+w) top        [u+d+w, u+d+2w) bottom
//   row v+d .. v+d+h:    [u, u+d) west  [u+d, u+d+w) north  [u+d+w, u+2d+w) east  [u+2d+w, u+2d+2w) south

#include "entity_models.hpp"

#include <cmath>
#include <string>

namespace bridger::render {
namespace {

struct Box {
    glm::vec3 from, to;      // block pixels (0..16), canonical orientation (front faces north)
    int u, v, w, h, d;       // texture net
    std::string_view texture;
    bool skip_top = false, skip_bottom = false;
    bool flipped = true; // drawn upside down by the game (chests); shulker boxes are not
};

// The net rectangle of a face (x, y, w, h) in texture pixels.
std::array<int, 4> net_rect(const Box& b, int face) {
    switch (face) {
    case 1: return {b.u + b.d, b.v, b.w, b.d};                       // up
    case 0: return {b.u + b.d + b.w, b.v, b.w, b.d};                 // down
    case 4: return {b.u, b.v + b.d, b.d, b.h};                       // west
    case 2: return {b.u + b.d, b.v + b.d, b.w, b.h};                 // north (front)
    case 5: return {b.u + b.d + b.w, b.v + b.d, b.d, b.h};           // east
    default: return {b.u + 2 * b.d + b.w, b.v + b.d, b.w, b.h};      // south
    }
}

std::array<glm::vec3, 4> corners(int face, const glm::vec3& f, const glm::vec3& t) {
    switch (face) {
    case 0: return {{{f.x, f.y, t.z}, {f.x, f.y, f.z}, {t.x, f.y, f.z}, {t.x, f.y, t.z}}};
    case 1: return {{{f.x, t.y, f.z}, {f.x, t.y, t.z}, {t.x, t.y, t.z}, {t.x, t.y, f.z}}};
    case 2: return {{{t.x, t.y, f.z}, {t.x, f.y, f.z}, {f.x, f.y, f.z}, {f.x, t.y, f.z}}};
    case 3: return {{{f.x, t.y, t.z}, {f.x, f.y, t.z}, {t.x, f.y, t.z}, {t.x, t.y, t.z}}};
    case 4: return {{{f.x, t.y, f.z}, {f.x, f.y, f.z}, {f.x, f.y, t.z}, {f.x, t.y, t.z}}};
    default: return {{{t.x, t.y, t.z}, {t.x, f.y, t.z}, {t.x, f.y, f.z}, {t.x, t.y, f.z}}};
    }
}

u8 face_of(const glm::vec3& n) {
    const glm::vec3 a = glm::abs(n);
    if (a.y >= a.x && a.y >= a.z) return n.y < 0 ? 0 : 1;
    if (a.z >= a.x) return n.z < 0 ? 2 : 3;
    return n.x < 0 ? 4 : 5;
}

// Adds a box rotated about the block centre by `yaw` degrees (0 = front north, 90 = front east).
void add_box(BakedModel& m, const Box& b, float yaw, const TextureRegionFn& region, std::vector<u16>& used) {
    const float a = glm::radians(-yaw), c = std::cos(a), s = std::sin(a);
    for (int face = 0; face < 6; ++face) {
        if ((face == 1 && b.skip_top) || (face == 0 && b.skip_bottom)) continue;
        // Entity models are built with Y pointing down and X mirrored, and drawn rotated
        // 180 degrees about X: the block's up face shows the net's "down" rectangle, north shows
        // "south", west shows "east" (the empty seam column of a double chest half is its inner
        // side), and the side rectangles are mirrored vertically.
        static constexpr std::array<int, 6> kNetFace{1, 0, 3, 2, 5, 4};
        static constexpr std::array<int, 6> kNetFaceUpright{0, 1, 2, 3, 5, 4};
        const int net = b.flipped ? kNetFace[static_cast<usize>(face)] : kNetFaceUpright[static_cast<usize>(face)];
        const auto r = net_rect(b, net);
        if (r[2] <= 0 || r[3] <= 0) continue;
        const bool side = face >= 2;
        const u16 layer = region(b.texture, r[0], r[1], r[2], r[3], b.flipped && face <= 1, b.flipped && side);
        used.push_back(layer);
        BakedQuad q;
        const auto cs = corners(face, b.from / 16.0f, b.to / 16.0f);
        const std::array<glm::vec2, 4> uv{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
        for (usize i = 0; i < 4; ++i) {
            glm::vec3 p = cs[i] - glm::vec3(0.5f);
            p = {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
            q.pos[i] = p + glm::vec3(0.5f);
            q.uv[i] = uv[i];
        }
        q.texture = layer;
        q.normal_face = face_of(glm::cross(q.pos[1] - q.pos[0], q.pos[2] - q.pos[0]));
        q.full_face = false;
        m.quads.push_back(q);
    }
}

float facing_yaw(std::string_view f) {
    if (f == "east") return 90;
    if (f == "south") return 180;
    if (f == "west") return 270;
    return 0;
}

std::string_view name_of(const std::string& id) {
    const auto c = id.find(':');
    return c == std::string::npos ? std::string_view(id) : std::string_view(id).substr(c + 1);
}

} // namespace

bool bake_entity_block(BakedBlock& block, const BlockState& s, const TextureRegionFn& region, std::vector<u16>& used) {
    const std::string_view name = name_of(s.id());
    const float yaw = facing_yaw(s.property("facing").value_or("north"));
    BakedModel m;

    if (name == "chest" || name == "trapped_chest" || name == "ender_chest") {
        const std::string_view type = s.property("type").value_or("single");
        const bool trapped = name == "trapped_chest";
        if (name == "ender_chest" || type == "single") {
            const std::string_view tex = name == "ender_chest" ? "entity/chest/ender" : trapped ? "entity/chest/trapped" : "entity/chest/normal";
            add_box(m, {{1, 0, 1}, {15, 10, 15}, 0, 19, 14, 10, 14, tex}, yaw, region, used);
            add_box(m, {{1, 9, 1}, {15, 14, 15}, 0, 0, 14, 5, 14, tex}, yaw, region, used);
            add_box(m, {{7, 7, 0}, {9, 11, 1}, 0, 0, 2, 4, 1, tex}, yaw, region, used);
        } else {
            // Double chest halves: 15 pixels wide, meeting at the block edge toward the partner.
            // Facing north, the "left" half's partner is east (+x) (docs/formats/bedrock-world.md).
            const bool left = type == "left";
            const std::string_view tex = trapped ? (left ? "entity/chest/trapped_left" : "entity/chest/trapped_right")
                                                 : (left ? "entity/chest/normal_left" : "entity/chest/normal_right");
            const float x0 = left ? 1.0f : 0.0f, x1 = left ? 16.0f : 15.0f;
            add_box(m, {{x0, 0, 1}, {x1, 10, 15}, 0, 19, 15, 10, 14, tex}, yaw, region, used);
            add_box(m, {{x0, 9, 1}, {x1, 14, 15}, 0, 0, 15, 5, 14, tex}, yaw, region, used);
            const float lx = left ? 15.0f : 0.0f;
            add_box(m, {{lx, 7, 0}, {lx + 1, 11, 1}, 0, 0, 1, 4, 1, tex}, yaw, region, used);
        }
    } else if (name == "shulker_box" || name.ends_with("_shulker_box")) {
        const std::string color = name == "shulker_box" ? std::string() : std::string(name.substr(0, name.size() - 12));
        const std::string tex = color.empty() ? "entity/shulker/shulker" : "entity/shulker/shulker_" + color;
        // Base (y 0..8) and lid (y 4..16) overlap: their notched side rows interlock. The inner
        // faces (base top, lid bottom) are hollow in the texture and skipped.
        add_box(m, {{0, 0, 0}, {16, 8, 16}, 0, 28, 16, 8, 16, std::string_view(tex), true, false, false}, 0, region, used);
        add_box(m, {{0, 4, 0}, {16, 16, 16}, 0, 0, 16, 12, 16, std::string_view(tex), false, true, false}, 0, region, used);
    } else {
        return false;
    }
    block.variants.push_back(std::move(m));
    return true;
}

} // namespace bridger::render
