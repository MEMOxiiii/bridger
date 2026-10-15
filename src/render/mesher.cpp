#include <bridger/render/mesher.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <tuple>

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

// Fluid surface, following the game's liquid renderer: every block has a height (sources 8/9,
// flowing lower with their level, falling and covered fluid full), and each top corner is the
// weighted average of the four blocks around it. Neighbouring fluid blocks therefore share their
// corner heights, so the surface is continuous and slopes where the fluid flows; side faces take the
// two corner heights of their edge, which closes the steps between different levels.
constexpr float kSourceHeight = 8.0f / 9.0f;

// The block's own height, or -1 if it holds no `f`; 2 marks fluid with the same fluid above it.
float own_height(const Ctx& c, int x, int y, int z, Fluid f) {
    const BakedBlock* b = c.at(x, y, z);
    if (b == nullptr || b->fluid != f) return -1.0f;
    const BakedBlock* above = c.at(x, y + 1, z);
    if (above != nullptr && above->fluid == f) return 2.0f;
    if (!b->fluid_only || b->fluid_level == 0) return kSourceHeight; // source or waterlogged block
    if (b->fluid_level >= 8) return 1.0f;                            // falling
    return std::max(0.125f, static_cast<float>(8 - b->fluid_level) / 9.0f);
}

// Height of the top corner shared by the blocks (cx-1 .. cx, cz-1 .. cz) at height y.
float corner_height(const Ctx& c, int cx, int y, int cz, Fluid f) {
    float sum = 0.0f, weight = 0.0f;
    for (int dz = -1; dz <= 0; ++dz)
        for (int dx = -1; dx <= 0; ++dx) {
            const int x = cx + dx, z = cz + dz;
            const float h = own_height(c, x, y, z, f);
            if (h >= 2.0f) return 1.0f; // fluid above any of the four: the corner is full
            if (h >= 0.0f) {
                const float w = h >= 0.8f ? 10.0f : 1.0f; // near-full blocks dominate (as in the game)
                sum += h * w;
                weight += w;
            } else if (c.at(x, y, z) == nullptr) {
                weight += 1.0f; // air pulls the corner down; blocks (glass, walls, ...) do not
            }
        }
    return weight > 0.0f ? sum / weight : kSourceHeight;
}

void mesh_fluid(const Ctx& c, glm::ivec3 pos, Fluid f, SectionMesh& out, u16 water_layer, u16 lava_layer) {
    const glm::vec3 tint = f == Fluid::Water ? tint_color(Tint::Water) : glm::vec3(1.0f);
    const float alpha = f == Fluid::Water ? 0.72f : 1.0f;
    std::vector<Vertex>& dst = f == Fluid::Water ? out.translucent : out.solid;
    const u16 layer = f == Fluid::Water ? water_layer : lava_layer;
    const bool covered = own_height(c, pos.x, pos.y, pos.z, f) >= 2.0f;
    // Corner heights: (x0,z0), (x0,z1), (x1,z1), (x1,z0).
    float h00 = 1.0f, h01 = 1.0f, h11 = 1.0f, h10 = 1.0f;
    if (!covered) {
        h00 = corner_height(c, pos.x, pos.y, pos.z, f);
        h01 = corner_height(c, pos.x, pos.y, pos.z + 1, f);
        h11 = corner_height(c, pos.x + 1, pos.y, pos.z + 1, f);
        h10 = corner_height(c, pos.x + 1, pos.y, pos.z, f);
    }
    const glm::vec3 o(pos);
    for (int face = 0; face < 6; ++face) {
        const glm::ivec3 n = kFaceNormals[static_cast<usize>(face)];
        const BakedBlock* nb = c.at(pos.x + n.x, pos.y + n.y, pos.z + n.z);
        if (nb != nullptr && nb->fluid == f) continue; // inside the same fluid
        if (face == 1) {
            if (covered) continue;
            // A full block above hides the surface only when the surface reaches it.
            if (nb != nullptr && nb->occludes[0] && std::min({h00, h01, h11, h10}) >= 1.0f) continue;
        } else if (nb != nullptr && nb->occludes[static_cast<usize>(face ^ 1)]) {
            continue; // against a wall
        }
        std::array<glm::vec3, 4> p;
        std::array<glm::vec2, 4> uv;
        switch (face) {
        case 0:
            p = {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}};
            uv = {{{0, 1}, {0, 0}, {1, 0}, {1, 1}}};
            break;
        case 1:
            p = {{{0, h00, 0}, {0, h01, 1}, {1, h11, 1}, {1, h10, 0}}};
            uv = {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
            break;
        case 2: // north (z = 0): x from 1 to 0
            p = {{{1, h10, 0}, {1, 0, 0}, {0, 0, 0}, {0, h00, 0}}};
            uv = {{{0, 1 - h10}, {0, 1}, {1, 1}, {1, 1 - h00}}};
            break;
        case 3: // south (z = 1)
            p = {{{0, h01, 1}, {0, 0, 1}, {1, 0, 1}, {1, h11, 1}}};
            uv = {{{0, 1 - h01}, {0, 1}, {1, 1}, {1, 1 - h11}}};
            break;
        case 4: // west (x = 0)
            p = {{{0, h00, 0}, {0, 0, 0}, {0, 0, 1}, {0, h01, 1}}};
            uv = {{{0, 1 - h00}, {0, 1}, {1, 1}, {1, 1 - h01}}};
            break;
        default: // east (x = 1)
            p = {{{1, h11, 1}, {1, 0, 1}, {1, 0, 0}, {1, h10, 0}}};
            uv = {{{0, 1 - h11}, {0, 1}, {1, 1}, {1, 1 - h10}}};
            break;
        }
        for (glm::vec3& v : p) v += o;
        const u32 col = pack_color(tint * kFaceShade[static_cast<usize>(face)], alpha);
        // Split the (possibly non-planar) top along the diagonal that follows the slope.
        const bool flip = face == 1 && std::abs(h00 - h11) > std::abs(h01 - h10);
        emit_quad(dst, p, uv, layer, {col, col, col, col}, false, flip);
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
                if (b->fluid != Fluid::None) mesh_fluid(c, pos, b->fluid, out, kWaterLayer, kLavaLayer);
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

// ---- Distant terrain (LOD) ------------------------------------------------------------------------

namespace {

bool lod_visible(const BakedBlock* b) {
    return b != nullptr && !(b->layer == RenderLayer::Invisible && b->fluid == Fluid::None) && b->map_color != 0;
}

u32 lod_color(const BakedBlock& b) {
    glm::vec3 c(static_cast<float>(b.map_color & 0xFF) / 255.0f, static_cast<float>((b.map_color >> 8) & 0xFF) / 255.0f,
                static_cast<float>((b.map_color >> 16) & 0xFF) / 255.0f);
    if (b.fluid == Fluid::Water || b.map_tinted) c *= tint_color(b.fluid == Fluid::Water ? Tint::Water : b.tint);
    return pack_color(c);
}

} // namespace

ColumnSummary summarize_column(const ColumnBlockFn& at, int min_y, int height) {
    ColumnSummary s;
    s.top.fill(ColumnSummary::kNoTop);
    s.color.fill(0);
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (int y = min_y + height - 1; y >= min_y; --y) {
                const BakedBlock* b = at(x, y, z);
                if (!lod_visible(b)) continue;
                const usize i = static_cast<usize>(z * 16 + x);
                s.top[i] = static_cast<i16>(y);
                s.color[i] = lod_color(*b);
                break;
            }
    return s;
}

ColumnSummary summarize_column(const Structure& column, std::span<const BakedBlock* const> table, int min_y) {
    ColumnSummary s;
    s.top.fill(ColumnSummary::kNoTop);
    s.color.fill(0);
    auto visible = [&](u32 id) -> const BakedBlock* { return id < table.size() && lod_visible(table[id]) ? table[id] : nullptr; };
    usize remaining = 256;
    for (int sy = static_cast<int>(column.sections_y()) - 1; sy >= 0 && remaining > 0; --sy) {
        const PaletteLayer& layer = column.section(0, static_cast<u32>(sy), 0).blocks();
        const auto palette = layer.palette();
        if (std::none_of(palette.begin(), palette.end(), [&](u32 id) { return visible(id) != nullptr; })) continue;
        for (int ly = 15; ly >= 0 && remaining > 0; --ly)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    const usize i = static_cast<usize>(z * 16 + x);
                    if (s.top[i] != ColumnSummary::kNoTop) continue;
                    const BakedBlock* b = visible(layer.get(section_index(x, ly, z)));
                    if (b == nullptr) continue;
                    s.top[i] = static_cast<i16>(min_y + sy * 16 + ly);
                    s.color[i] = lod_color(*b);
                    --remaining;
                }
    }
    return s;
}

void mesh_lod(const ColumnSummary& c, const std::array<const ColumnSummary*, 4>& around, glm::ivec3 origin, int step, u16 white,
              std::map<int, SectionMesh>& out) {
    // Cells of step x step blocks: the highest block of the cell and its colour.
    const int n = 16 / step;
    auto cell = [&](const ColumnSummary& s, int cx, int cz, i16& top, u32& col) {
        top = ColumnSummary::kNoTop;
        col = 0;
        for (int dz = 0; dz < step; ++dz)
            for (int dx = 0; dx < step; ++dx) {
                const usize i = static_cast<usize>((cz * step + dz) * 16 + cx * step + dx);
                if (s.top[i] > top) { top = s.top[i]; col = s.color[i]; }
            }
    };
    std::vector<i16> tops(static_cast<usize>(n * n));
    std::vector<u32> cols(tops.size());
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) cell(c, x, z, tops[static_cast<usize>(z * n + x)], cols[static_cast<usize>(z * n + x)]);

    auto shade = [](u32 col, float f) {
        const glm::vec3 v(static_cast<float>(col & 0xFF) / 255.0f, static_cast<float>((col >> 8) & 0xFF) / 255.0f,
                          static_cast<float>((col >> 16) & 0xFF) / 255.0f);
        return pack_color(v * f);
    };
    static constexpr std::array<glm::vec2, 4> kUv{glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
    // Positions in each section's own frame: (x, y - section base, z).
    auto quad = [&](std::array<glm::vec3, 4> p, u32 col, int section_y_world) {
        const int sy = static_cast<int>(std::floor(static_cast<float>(section_y_world - origin.y) / 16.0f));
        const float base = static_cast<float>(origin.y + sy * 16);
        for (auto& v : p) v.y -= base;
        emit_quad(out[sy].solid, p, kUv, white, {col, col, col, col}, false, false);
    };

    // Tops, merged greedily: runs along x of equal height and colour, grown along z.
    std::vector<u8> done(tops.size(), 0);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const usize i = static_cast<usize>(z * n + x);
            if (done[i] || tops[i] == ColumnSummary::kNoTop) continue;
            auto same = [&](int xx, int zz) {
                const usize j = static_cast<usize>(zz * n + xx);
                return !done[j] && tops[j] == tops[i] && cols[j] == cols[i];
            };
            int w = 1;
            while (x + w < n && same(x + w, z)) ++w;
            int h = 1;
            for (bool grow = true; grow && z + h < n;) {
                for (int xx = x; xx < x + w; ++xx)
                    if (!same(xx, z + h)) { grow = false; break; }
                if (grow) ++h;
            }
            for (int zz = z; zz < z + h; ++zz)
                for (int xx = x; xx < x + w; ++xx) done[static_cast<usize>(zz * n + xx)] = 1;
            const float y = static_cast<float>(tops[i] + 1);
            const float x0 = static_cast<float>(x * step), x1 = static_cast<float>((x + w) * step);
            const float z0 = static_cast<float>(z * step), z1 = static_cast<float>((z + h) * step);
            quad({glm::vec3(x0, y, z0), glm::vec3(x0, y, z1), glm::vec3(x1, y, z1), glm::vec3(x1, y, z0)}, shade(cols[i], kFaceShade[1]), tops[i]);
        }

    // Sides: down to the neighbouring cell's top; at the column border the neighbour column's cell,
    // or a skirt of 8 blocks when it is not loaded (hides cracks against full-detail columns).
    auto neighbour_top = [&](int x, int z) -> int {
        if (x >= 0 && x < n && z >= 0 && z < n) return tops[static_cast<usize>(z * n + x)];
        const ColumnSummary* s = x >= n ? around[0] : x < 0 ? around[1] : z >= n ? around[2] : around[3];
        if (s == nullptr) return INT_MIN;
        i16 t;
        u32 col;
        cell(*s, (x + n) % n, (z + n) % n, t, col);
        return t;
    };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const int top = tops[static_cast<usize>(z * n + x)];
            if (top == ColumnSummary::kNoTop) continue;
            const u32 col = cols[static_cast<usize>(z * n + x)];
            const float x0 = static_cast<float>(x * step), x1 = static_cast<float>((x + 1) * step);
            const float z0 = static_cast<float>(z * step), z1 = static_cast<float>((z + 1) * step);
            // face index (kFaceShade): 2 north (-z), 3 south (+z), 4 west (-x), 5 east (+x)
            const std::array<std::tuple<int, int, int>, 4> sides{{{x + 1, z, 5}, {x - 1, z, 4}, {x, z + 1, 3}, {x, z - 1, 2}}};
            for (const auto& [nx, nz, face] : sides) {
                int low = neighbour_top(nx, nz);
                if (low == INT_MIN) low = top - 8;
                if (low >= top) continue;
                const u32 sc = shade(col, kFaceShade[static_cast<usize>(face)]);
                // Split at section boundaries so every quad lies inside its section's culling box.
                for (int y0 = low + 1; y0 <= top;) {
                    const int section_base = origin.y + static_cast<int>(std::floor(static_cast<float>(y0 - origin.y) / 16.0f)) * 16;
                    const int y1 = std::min(top + 1, section_base + 16);
                    const float a = static_cast<float>(y0), b = static_cast<float>(y1);
                    std::array<glm::vec3, 4> p;
                    switch (face) {
                    case 5: p = {glm::vec3(x1, a, z0), glm::vec3(x1, b, z0), glm::vec3(x1, b, z1), glm::vec3(x1, a, z1)}; break;
                    case 4: p = {glm::vec3(x0, a, z1), glm::vec3(x0, b, z1), glm::vec3(x0, b, z0), glm::vec3(x0, a, z0)}; break;
                    case 3: p = {glm::vec3(x1, a, z1), glm::vec3(x1, b, z1), glm::vec3(x0, b, z1), glm::vec3(x0, a, z1)}; break;
                    default: p = {glm::vec3(x0, a, z0), glm::vec3(x0, b, z0), glm::vec3(x1, b, z0), glm::vec3(x1, a, z0)}; break;
                    }
                    quad(p, sc, y0);
                    y0 = y1;
                }
            }
        }
}

} // namespace bridger::render
