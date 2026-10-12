// Block model baking. Format: minecraft.wiki "Tutorials/Models" (blockstates, models, elements,
// faces, uvlock, rotation) [docs/formats/assets.md]. Default face UVs and vertex order follow the
// game's FaceBakery behaviour as documented there.

#include <bridger/render/block_models.hpp>

#include "entity_models.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace bridger::render {

using json = nlohmann::json;

namespace {

constexpr std::array<std::string_view, 6> kFaceNames{"down", "up", "north", "south", "west", "east"};

int face_index(std::string_view n) {
    for (int i = 0; i < 6; ++i)
        if (kFaceNames[static_cast<usize>(i)] == n) return i;
    return -1;
}

// "minecraft:block/stone" / "block/stone" -> ("minecraft", "block/stone").
std::pair<std::string, std::string> split_id(std::string_view id) {
    const auto c = id.find(':');
    if (c == std::string_view::npos) return {"minecraft", std::string(id)};
    return {std::string(id.substr(0, c)), std::string(id.substr(c + 1))};
}

// The face an outward normal points to.
u8 face_of_normal(const glm::vec3& n) {
    const glm::vec3 a = glm::abs(n);
    if (a.y >= a.x && a.y >= a.z) return n.y < 0 ? 0 : 1;
    if (a.z >= a.x) return n.z < 0 ? 2 : 3;
    return n.x < 0 ? 4 : 5;
}

// Rotation of a point around the block centre by the variant's x then y (degrees, multiples of 90).
// x=90 turns "up" into "north"; y=90 turns "north" into "east" (minecraft.wiki).
glm::vec3 rotate_variant(glm::vec3 p, int rx, int ry) {
    const glm::vec3 c(0.5f);
    p -= c;
    for (int i = 0; i < ((rx / 90) % 4 + 4) % 4; ++i) p = {p.x, p.z, -p.y};  // up -> north
    for (int i = 0; i < ((ry / 90) % 4 + 4) % 4; ++i) p = {-p.z, p.y, p.x};  // north -> east
    return p + c;
}

int rotate_face(int face, int rx, int ry) {
    if (face < 0) return face;
    const glm::vec3 n = rotate_variant(glm::vec3(kFaceNormals[static_cast<usize>(face)]) * 0.5f + 0.5f, rx, ry) - 0.5f;
    return face_of_normal(n);
}

// Default UV of a face from the element bounds (0..16), [u0, v0, u1, v1].
std::array<float, 4> default_uv(int face, const glm::vec3& f, const glm::vec3& t) {
    switch (face) {
    case 0: return {f.x, 16 - t.z, t.x, 16 - f.z};
    case 1: return {f.x, f.z, t.x, t.z};
    case 2: return {16 - t.x, 16 - t.y, 16 - f.x, 16 - f.y};
    case 3: return {f.x, 16 - t.y, t.x, 16 - f.y};
    case 4: return {f.z, 16 - t.y, t.z, 16 - f.y};
    default: return {16 - t.z, 16 - t.y, 16 - f.z, 16 - f.y};
    }
}

// Corners of a face in order top-left, bottom-left, bottom-right, top-right (as seen from outside),
// which is counter-clockwise around the outward normal.
std::array<glm::vec3, 4> face_corners(int face, const glm::vec3& f, const glm::vec3& t) {
    switch (face) {
    case 0: return {{{f.x, f.y, t.z}, {f.x, f.y, f.z}, {t.x, f.y, f.z}, {t.x, f.y, t.z}}};
    case 1: return {{{f.x, t.y, f.z}, {f.x, t.y, t.z}, {t.x, t.y, t.z}, {t.x, t.y, f.z}}};
    case 2: return {{{t.x, t.y, f.z}, {t.x, f.y, f.z}, {f.x, f.y, f.z}, {f.x, t.y, f.z}}};
    case 3: return {{{f.x, t.y, t.z}, {f.x, f.y, t.z}, {t.x, f.y, t.z}, {t.x, t.y, t.z}}};
    case 4: return {{{f.x, t.y, f.z}, {f.x, f.y, f.z}, {f.x, f.y, t.z}, {f.x, t.y, t.z}}};
    default: return {{{t.x, t.y, t.z}, {t.x, f.y, t.z}, {t.x, f.y, f.z}, {t.x, t.y, f.z}}};
    }
}

glm::vec3 read_vec3(const json& j, glm::vec3 def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

struct Texture {
    std::array<u32, 256> rgba{};
    bool any_clear = false;   // some pixel alpha == 0
    bool any_partial = false; // some pixel 0 < alpha < 255
    bool mostly_partial = false; // over 30 % of the visible pixels are partly transparent
    u32 average = 0;
};

void classify(Texture& t) {
    u32 visible = 0, partial = 0;
    t.any_clear = t.any_partial = false;
    for (const u32 px : t.rgba) {
        const u32 a = px >> 24;
        if (a == 0) t.any_clear = true;
        else {
            ++visible;
            if (a < 250) {
                t.any_partial = true;
                ++partial;
            }
        }
    }
    t.mostly_partial = visible > 0 && partial * 10 > visible * 3;
}

// Model JSON merged along its parent chain.
struct TextureRef {
    std::string value;          // "#var" or a sprite id
    bool force_translucent = false;
};

struct ResolvedModel {
    std::unordered_map<std::string, TextureRef> textures;
    const json* elements = nullptr; // owned by the model cache
    bool ambient_occlusion = true;
};

} // namespace

struct BlockModels::Impl {
    std::shared_ptr<const AssetSource> assets;
    std::unordered_map<std::string, json> model_json;      // "minecraft:block/stone" -> parsed json (or null)
    std::unordered_map<std::string, json> blockstate_json; // "minecraft:stone" -> parsed json (or null)
    std::unordered_map<std::string, u16> texture_index;
    std::vector<Texture> textures;
    std::vector<u32> pixels;
    std::unordered_map<std::string, std::unique_ptr<BakedBlock>> baked; // canonical state -> baked
    std::vector<std::string> missing;
    u16 missing_texture = 0;

    const json* load_json(std::unordered_map<std::string, json>& cache, const std::string& key, const std::string& path) {
        auto it = cache.find(key);
        if (it == cache.end()) {
            json j;
            if (const auto bytes = assets->read(path)) {
                j = json::parse(bytes->begin(), bytes->end(), nullptr, false);
                if (j.is_discarded()) j = json();
            }
            it = cache.emplace(key, std::move(j)).first;
        }
        return it->second.is_null() ? nullptr : &it->second;
    }

    const json* model(std::string_view id) {
        auto [ns, path] = split_id(id);
        return load_json(model_json, ns + ":" + path, "assets/" + ns + "/models/" + path + ".json");
    }

    u16 add_texture(const Texture& t) {
        textures.push_back(t);
        pixels.insert(pixels.end(), t.rgba.begin(), t.rgba.end());
        return static_cast<u16>(textures.size() - 1);
    }

    u16 texture(std::string_view name) {
        auto [ns, path] = split_id(name);
        const std::string key = ns + ":" + path;
        if (const auto it = texture_index.find(key); it != texture_index.end()) return it->second;
        u16 idx = missing_texture;
        if (const auto bytes = assets->read("assets/" + ns + "/textures/" + path + ".png")) {
            int w = 0, h = 0, n = 0;
            stbi_uc* data = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &n, 4);
            if (data != nullptr && w > 0 && h > 0) {
                // Animated strips: the first square frame. Other sizes: nearest-neighbour to 16 x 16.
                const int frame = std::min(w, h);
                Texture t;
                u64 sum[4] = {0, 0, 0, 0};
                u32 counted = 0;
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x) {
                        const int sx = x * frame / 16, sy = y * frame / 16;
                        const stbi_uc* p = data + (static_cast<usize>(sy) * static_cast<usize>(w) + static_cast<usize>(sx)) * 4;
                        const u32 px = u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
                        t.rgba[static_cast<usize>(y * 16 + x)] = px;
                        if (p[3] == 0) t.any_clear = true;
                        else if (p[3] < 255) t.any_partial = true;
                        if (p[3] > 0) {
                            for (int c = 0; c < 4; ++c) sum[c] += p[c];
                            ++counted;
                        }
                    }
                if (counted > 0)
                    t.average = static_cast<u32>(sum[0] / counted) | (static_cast<u32>(sum[1] / counted) << 8) |
                                (static_cast<u32>(sum[2] / counted) << 16) | (0xFFu << 24);
                classify(t);
                idx = add_texture(t);
            }
            stbi_image_free(data);
        }
        texture_index.emplace(key, idx);
        return idx;
    }

    struct Image {
        int w = 0, h = 0;
        std::vector<u8> rgba;
    };
    std::unordered_map<std::string, Image> images; // decoded entity textures

    // A rectangle of a texture, resampled (nearest) into its own 16 x 16 layer.
    u16 texture_region(std::string_view name, int x, int y, int w, int h, bool flip_x, bool flip_y) {
        auto [ns, path] = split_id(name);
        const std::string key = ns + ":" + path;
        const std::string region_key = key + "#" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," +
                                       std::to_string(h) + (flip_x ? "x" : "") + (flip_y ? "y" : "");
        if (const auto it = texture_index.find(region_key); it != texture_index.end()) return it->second;
        auto img_it = images.find(key);
        if (img_it == images.end()) {
            Image img;
            if (const auto bytes = assets->read("assets/" + ns + "/textures/" + path + ".png")) {
                int n = 0;
                stbi_uc* data = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &img.w, &img.h, &n, 4);
                if (data != nullptr) {
                    img.rgba.assign(data, data + static_cast<usize>(img.w) * static_cast<usize>(img.h) * 4);
                    stbi_image_free(data);
                }
            }
            img_it = images.emplace(key, std::move(img)).first;
        }
        const Image& img = img_it->second;
        u16 idx = missing_texture;
        if (!img.rgba.empty()) {
            // Entity textures may be authored at a larger scale than 64 x 64: scale the rectangle.
            const float sc = static_cast<float>(img.w) / 64.0f;
            Texture t;
            for (int py = 0; py < 16; ++py)
                for (int px = 0; px < 16; ++px) {
                    const int lx = flip_x ? 15 - px : px, ly = flip_y ? 15 - py : py;
                    const int sx = std::clamp(static_cast<int>((static_cast<float>(x) + (static_cast<float>(lx) + 0.5f) * static_cast<float>(w) / 16.0f) * sc), 0, img.w - 1);
                    const int sy = std::clamp(static_cast<int>((static_cast<float>(y) + (static_cast<float>(ly) + 0.5f) * static_cast<float>(h) / 16.0f) * sc), 0, img.h - 1);
                    const u8* p = img.rgba.data() + (static_cast<usize>(sy) * static_cast<usize>(img.w) + static_cast<usize>(sx)) * 4;
                    t.rgba[static_cast<usize>(py * 16 + px)] = u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
                    if (p[3] == 0) t.any_clear = true;
                    else if (p[3] < 255) t.any_partial = true;
                }
            t.average = t.rgba[8 * 16 + 8] | 0xFF000000u;
            classify(t);
            idx = add_texture(t);
        }
        texture_index.emplace(region_key, idx);
        return idx;
    }

    // Parent chain: textures merged child-first, elements from the nearest model that has them.
    ResolvedModel resolve(std::string_view id) {
        ResolvedModel r;
        std::string cur(id);
        for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
            const json* m = model(cur);
            if (m == nullptr) break;
            if (const auto t = m->find("textures"); t != m->end() && t->is_object())
                for (const auto& [k, v] : t->items()) {
                    // A string, or (Java 26.x) {"sprite": "...", "force_translucent": bool}.
                    if (v.is_string()) r.textures.try_emplace(k, TextureRef{v.get<std::string>(), false});
                    else if (v.is_object() && v.contains("sprite") && v["sprite"].is_string())
                        r.textures.try_emplace(k, TextureRef{v["sprite"].get<std::string>(), v.value("force_translucent", false)});
                }
            if (r.elements == nullptr)
                if (const auto e = m->find("elements"); e != m->end() && e->is_array()) r.elements = &*e;
            if (const auto ao = m->find("ambientocclusion"); ao != m->end() && ao->is_boolean() && depth == 0)
                r.ambient_occlusion = ao->get<bool>();
            const auto p = m->find("parent");
            cur = (p != m->end() && p->is_string()) ? p->get<std::string>() : std::string();
            if (cur == "builtin/generated" || cur == "minecraft:builtin/generated" || cur == "builtin/entity") break;
        }
        return r;
    }

    // "#side" -> "block/oak_planks" through the texture variables; `translucent` is set when the
    // sprite is declared force_translucent.
    std::string resolve_texture(const ResolvedModel& m, std::string ref, bool& translucent) {
        for (int i = 0; i < 16 && !ref.empty() && ref[0] == '#'; ++i) {
            const auto it = m.textures.find(ref.substr(1));
            if (it == m.textures.end()) return {};
            ref = it->second.value;
            translucent = translucent || it->second.force_translucent;
        }
        return ref.empty() || ref[0] == '#' ? std::string() : ref;
    }

    // Bakes one model reference of a variant (model id + x/y rotation + uvlock).
    bool force_translucent = false; // set while baking a block whose sprites ask for it

    void bake_model(BakedModel& out, std::string_view id, int rx, int ry, bool uvlock, std::vector<u16>& used_textures) {
        const ResolvedModel m = resolve(id);
        if (m.elements == nullptr) return;
        for (const json& e : *m.elements) {
            if (!e.is_object()) continue;
            const glm::vec3 from = read_vec3(e.value("from", json()), glm::vec3(0)), to = read_vec3(e.value("to", json()), glm::vec3(16));
            const bool shade = e.value("shade", true);
            // Element rotation (minecraft.wiki: origin, axis, angle -45..45, rescale).
            glm::vec3 origin(8.0f);
            char axis = 0;
            float angle = 0;
            bool rescale = false;
            if (const auto rot = e.find("rotation"); rot != e.end() && rot->is_object()) {
                origin = read_vec3(rot->value("origin", json()), glm::vec3(8));
                const std::string a = rot->value("axis", "y");
                axis = a.empty() ? 'y' : a[0];
                angle = rot->value("angle", 0.0f);
                rescale = rot->value("rescale", false);
            }
            const auto faces = e.find("faces");
            if (faces == e.end() || !faces->is_object()) continue;
            for (const auto& [fname, fj] : faces->items()) {
                const int face = face_index(fname);
                if (face < 0 || !fj.is_object()) continue;
                bool forced = false;
                const std::string tex = resolve_texture(m, fj.value("texture", ""), forced);
                const u16 layer = tex.empty() ? missing_texture : texture(tex);
                used_textures.push_back(layer);
                if (forced) force_translucent = true;

                std::array<glm::vec3, 4> corners = face_corners(face, from, to);
                std::array<float, 4> uvr = default_uv(face, from, to);
                if (const auto uvj = fj.find("uv"); uvj != fj.end() && uvj->is_array() && uvj->size() == 4)
                    uvr = {(*uvj)[0].get<float>(), (*uvj)[1].get<float>(), (*uvj)[2].get<float>(), (*uvj)[3].get<float>()};
                const int face_rot = ((fj.value("rotation", 0) / 90) % 4 + 4) % 4;

                // Element rotation.
                if (axis != 0 && angle != 0) {
                    const float a = glm::radians(angle), c = std::cos(a), s = std::sin(a);
                    const float scale = rescale ? 1.0f / std::cos(std::abs(a)) : 1.0f;
                    for (glm::vec3& p : corners) {
                        glm::vec3 q = p - origin;
                        if (axis == 'x') q = {q.x * 1.0f, (q.y * c - q.z * s) * scale, (q.y * s + q.z * c) * scale};
                        else if (axis == 'y') q = {(q.x * c + q.z * s) * scale, q.y, (-q.x * s + q.z * c) * scale};
                        else q = {(q.x * c - q.y * s) * scale, (q.x * s + q.y * c) * scale, q.z};
                        p = q + origin;
                    }
                }

                BakedQuad quad;
                const std::array<glm::vec2, 4> uv_corners{{{uvr[0], uvr[1]}, {uvr[0], uvr[3]}, {uvr[2], uvr[3]}, {uvr[2], uvr[1]}}};
                for (usize i = 0; i < 4; ++i) {
                    quad.pos[i] = rotate_variant(corners[i] / 16.0f, rx, ry);
                    quad.uv[i] = uv_corners[(i + static_cast<usize>(face_rot)) % 4] / 16.0f;
                }
                const glm::vec3 normal = glm::normalize(glm::cross(quad.pos[1] - quad.pos[0], quad.pos[2] - quad.pos[0]));
                quad.normal_face = face_of_normal(normal);
                if (uvlock && (rx != 0 || ry != 0)) {
                    // World-aligned UVs: recompute from the rotated geometry like the default UVs.
                    glm::vec3 lo(1e9f), hi(-1e9f);
                    for (const glm::vec3& p : quad.pos) {
                        lo = glm::min(lo, p);
                        hi = glm::max(hi, p);
                    }
                    const auto d = default_uv(quad.normal_face, lo * 16.0f, hi * 16.0f);
                    const auto rc = face_corners(quad.normal_face, lo, hi);
                    const std::array<glm::vec2, 4> dc{{{d[0], d[1]}, {d[0], d[3]}, {d[2], d[3]}, {d[2], d[1]}}};
                    // Match each rotated vertex to the nearest canonical corner.
                    for (usize i = 0; i < 4; ++i) {
                        usize best = 0;
                        float bd = 1e9f;
                        for (usize k = 0; k < 4; ++k) {
                            const float dd = glm::dot(quad.pos[i] - rc[k], quad.pos[i] - rc[k]);
                            if (dd < bd) {
                                bd = dd;
                                best = k;
                            }
                        }
                        quad.uv[i] = dc[best] / 16.0f;
                    }
                }
                quad.texture = layer;
                quad.tint_index = static_cast<i8>(fj.value("tintindex", -1));
                quad.shade = shade;
                if (const auto cf = fj.find("cullface"); cf != fj.end() && cf->is_string())
                    quad.cull_face = static_cast<i8>(rotate_face(face_index(cf->get<std::string>()), rx, ry));
                // Full face: an axis-aligned quad covering the whole block side.
                if (axis == 0 || angle == 0) {
                    glm::vec3 lo(1e9f), hi(-1e9f);
                    for (const glm::vec3& p : quad.pos) {
                        lo = glm::min(lo, p);
                        hi = glm::max(hi, p);
                    }
                    const int ax = quad.normal_face / 2; // 0 y, 1 z, 2 x
                    const glm::vec3 eps(1e-4f);
                    const bool at_edge = ax == 0 ? (quad.normal_face == 0 ? lo.y < eps.y : hi.y > 1 - eps.y)
                                       : ax == 1 ? (quad.normal_face == 2 ? lo.z < eps.z : hi.z > 1 - eps.z)
                                                 : (quad.normal_face == 4 ? lo.x < eps.x : hi.x > 1 - eps.x);
                    const glm::vec3 size = hi - lo;
                    const bool covers = ax == 0 ? (size.x > 1 - 1e-3f && size.z > 1 - 1e-3f)
                                      : ax == 1 ? (size.x > 1 - 1e-3f && size.y > 1 - 1e-3f)
                                                : (size.z > 1 - 1e-3f && size.y > 1 - 1e-3f);
                    quad.full_face = at_edge && covers;
                }
                out.quads.push_back(quad);
            }
        }
    }

    // A variant entry {model, x, y, uvlock} (object) or a weighted list of them.
    void bake_variant_entry(BakedBlock& b, const json& entry, std::vector<u16>& used) {
        auto one = [&](const json& v) {
            BakedModel m;
            if (v.is_object() && v.contains("model") && v["model"].is_string())
                bake_model(m, v["model"].get<std::string>(), v.value("x", 0), v.value("y", 0), v.value("uvlock", false), used);
            return m;
        };
        if (entry.is_array()) {
            for (const json& v : entry) b.variants.push_back(one(v));
        } else {
            b.variants.push_back(one(entry));
        }
    }

    static bool matches_key(const BlockState& s, std::string_view key) {
        if (key.empty() || key == "normal") return true;
        usize start = 0;
        while (start < key.size()) {
            usize end = key.find(',', start);
            if (end == std::string_view::npos) end = key.size();
            const std::string_view kv = key.substr(start, end - start);
            const auto eq = kv.find('=');
            if (eq == std::string_view::npos) return false;
            const auto v = s.property(kv.substr(0, eq));
            if (!v || *v != kv.substr(eq + 1)) return false;
            start = end + 1;
        }
        return true;
    }

    static bool matches_when(const BlockState& s, const json& when) {
        if (!when.is_object()) return true;
        if (const auto o = when.find("OR"); o != when.end() && o->is_array()) {
            for (const json& w : *o)
                if (matches_when(s, w)) return true;
            return false;
        }
        if (const auto a = when.find("AND"); a != when.end() && a->is_array()) {
            for (const json& w : *a)
                if (!matches_when(s, w)) return false;
            return true;
        }
        for (const auto& [k, v] : when.items()) {
            const std::string want = v.is_string() ? v.get<std::string>() : v.dump();
            const auto have = s.property(k);
            if (!have) return false;
            bool ok = false;
            usize start = 0;
            while (start <= want.size()) {
                usize end = want.find('|', start);
                if (end == std::string::npos) end = want.size();
                if (want.substr(start, end - start) == *have) ok = true;
                start = end + 1;
            }
            if (!ok) return false;
        }
        return true;
    }

    std::unique_ptr<BakedBlock> bake_block(const BlockState& s) {
        auto b = std::make_unique<BakedBlock>();
        const std::string& id = s.id();
        auto [ns, name] = split_id(id);
        b->tint = tint_of(name);

        // Fluids: the water/lava blocks have no geometry of their own (fluid renderer).
        if (name == "water" || name == "lava") {
            b->fluid = name == "water" ? Fluid::Water : Fluid::Lava;
            b->fluid_only = true;
            b->layer = name == "water" ? RenderLayer::Translucent : RenderLayer::Solid;
            if (const auto lv = s.property("level")) b->fluid_level = static_cast<u8>(std::clamp(std::atoi(std::string(*lv).c_str()), 0, 15));
            b->tint = name == "water" ? Tint::Water : Tint::None;
            b->map_color = name == "water" ? 0xFFE4763Fu : 0xFF1F5ACFu;
            return b;
        }
        if (s.property("waterlogged") == "true" || name == "seagrass" || name == "tall_seagrass" || name == "kelp" ||
            name == "kelp_plant" || name == "bubble_column")
            b->fluid = Fluid::Water;

        std::vector<u16> used;
        force_translucent = false;
        // Block entities rendered by the game without JSON models (chests, beds, ...).
        const TextureRegionFn region = [this](std::string_view t, int x, int y, int w, int h, bool fx, bool fy) {
            return texture_region(t, x, y, w, h, fx, fy);
        };
        if (bake_entity_block(*b, s, region, used)) {
            // geometry provided
        } else if (const json* bs = load_json(blockstate_json, ns + ":" + name, "assets/" + ns + "/blockstates/" + name + ".json")) {
            if (const auto v = bs->find("variants"); v != bs->end() && v->is_object()) {
                for (const auto& [key, entry] : v->items())
                    if (matches_key(s, key)) {
                        bake_variant_entry(*b, entry, used);
                        break;
                    }
            } else if (const auto mp = bs->find("multipart"); mp != bs->end() && mp->is_array()) {
                // Every matching part is added to every combination: bake one combined model,
                // taking the first entry of weighted lists.
                BakedModel combined;
                for (const json& part : *mp) {
                    if (!matches_when(s, part.value("when", json()))) continue;
                    const json& apply = part["apply"];
                    const json& pick = apply.is_array() && !apply.empty() ? apply[0] : apply;
                    if (pick.is_object() && pick.contains("model") && pick["model"].is_string())
                        bake_model(combined, pick["model"].get<std::string>(), pick.value("x", 0), pick.value("y", 0), pick.value("uvlock", false), used);
                }
                b->variants.push_back(std::move(combined));
            }
        } else {
            missing.push_back(id);
            BakedModel cube;
            bake_missing_cube(cube);
            b->variants.push_back(std::move(cube));
            used.push_back(missing_texture);
        }

        bool any_quads = false;
        for (const BakedModel& m : b->variants) any_quads |= !m.quads.empty();
        if (!any_quads) {
            b->layer = RenderLayer::Invisible;
            return b;
        }
        bool clear = false, partial = false;
        for (const u16 t : used) {
            // A few soft edge pixels (beds, chests) are drawn as cutout; mostly see-through
            // textures (stained glass, ice) as translucent.
            clear |= textures[t].any_clear || textures[t].any_partial;
            partial |= textures[t].mostly_partial;
        }
        b->layer = (partial || force_translucent) ? RenderLayer::Translucent : clear ? RenderLayer::Cutout : RenderLayer::Solid;
        // Occlusion: a solid full face that culls against its neighbour hides that neighbour's face.
        if (b->layer == RenderLayer::Solid && b->variants.size() >= 1)
            for (const BakedQuad& q : b->variants[0].quads)
                if (q.full_face && q.cull_face >= 0 && q.cull_face == q.normal_face) b->occludes[static_cast<usize>(q.cull_face)] = true;
        // Glass, ice, stained glass: identical neighbours hide each other's faces (not leaves).
        b->cull_same = b->layer != RenderLayer::Solid && name.find("leaves") == std::string::npos;
        // Map colour: the up face of the first variant, else any face.
        for (const BakedQuad& q : b->variants[0].quads)
            if (q.normal_face == 1) {
                b->map_color = textures[q.texture].average;
                break;
            }
        if (b->map_color == 0 && !b->variants[0].quads.empty()) b->map_color = textures[b->variants[0].quads[0].texture].average;
        return b;
    }

    void bake_missing_cube(BakedModel& m) {
        for (int f = 0; f < 6; ++f) {
            BakedQuad q;
            const auto c = face_corners(f, glm::vec3(0), glm::vec3(1));
            const std::array<glm::vec2, 4> uv{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
            for (usize i = 0; i < 4; ++i) {
                q.pos[i] = c[i];
                q.uv[i] = uv[i];
            }
            q.texture = missing_texture;
            q.cull_face = static_cast<i8>(f);
            q.normal_face = static_cast<u8>(f);
            q.full_face = true;
            m.quads.push_back(q);
        }
    }

    static Tint tint_of(std::string_view n) {
        if (n == "grass_block" || n == "short_grass" || n == "tall_grass" || n == "fern" || n == "large_fern" ||
            n == "sugar_cane" || n == "potted_fern" || n == "bush")
            return Tint::Grass;
        if (n == "birch_leaves") return Tint::FoliageBirch;
        if (n == "spruce_leaves") return Tint::FoliageSpruce;
        if (n == "oak_leaves" || n == "jungle_leaves" || n == "acacia_leaves" || n == "dark_oak_leaves" ||
            n == "mangrove_leaves" || n == "vine")
            return Tint::Foliage;
        if (n == "water" || n == "water_cauldron") return Tint::Water;
        if (n == "redstone_wire") return Tint::Redstone;
        if (n == "lily_pad") return Tint::LilyPad;
        return Tint::None;
    }
};

BlockModels::BlockModels() : impl_(std::make_unique<Impl>()) {}
BlockModels::BlockModels(BlockModels&&) noexcept = default;
BlockModels& BlockModels::operator=(BlockModels&&) noexcept = default;
BlockModels::~BlockModels() = default;

Result<BlockModels> BlockModels::create(std::shared_ptr<const AssetSource> assets) {
    if (!assets) return make_error(ErrorCode::InvalidArgument, "BlockModels needs an asset source");
    BlockModels m;
    m.impl_->assets = std::move(assets);
    // Layer 0: the game's "missing texture" (magenta/black checker), used for anything unresolved.
    Texture missing;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) missing.rgba[static_cast<usize>(y * 16 + x)] = ((x < 8) != (y < 8)) ? 0xFF000000u : 0xFFF800F8u;
    missing.average = 0xFF7C007Cu;
    m.impl_->missing_texture = m.impl_->add_texture(missing);
    if (!m.impl_->assets->read("assets/minecraft/blockstates/stone.json"))
        return make_error(ErrorCode::FileNotFound, "the resource packs contain no Minecraft block models (expected the client jar)");
    // Reserved layers 1 and 2: the still water and lava textures used by the fluid mesher.
    const std::array<std::pair<const char*, u32>, 2> fluids{{{"block/water_still", 0xFFC0C0C0u}, {"block/lava_still", 0xFF1F5ACFu}}};
    for (usize i = 0; i < fluids.size(); ++i) {
        const u16 want = static_cast<u16>(i + 1);
        if (m.impl_->texture(fluids[i].first) != want) {
            Texture t;
            t.rgba.fill(fluids[i].second);
            t.average = fluids[i].second;
            m.impl_->texture_index["minecraft:" + std::string(fluids[i].first)] = m.impl_->add_texture(t);
        }
    }
    return m;
}

const BakedBlock& BlockModels::bake(const BlockState& state) {
    const std::string key = state.to_string();
    auto it = impl_->baked.find(key);
    if (it == impl_->baked.end()) it = impl_->baked.emplace(key, impl_->bake_block(state)).first;
    return *it->second;
}

usize BlockModels::texture_count() const noexcept { return impl_->textures.size(); }
const std::vector<u32>& BlockModels::texture_pixels() const noexcept { return impl_->pixels; }
u16 BlockModels::texture(std::string_view name) { return impl_->texture(name); }
const std::vector<std::string>& BlockModels::missing() const noexcept { return impl_->missing; }

glm::vec3 tint_color(Tint tint, u8 power) noexcept {
    auto rgb = [](u32 c) { return glm::vec3((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) / 255.0f; };
    switch (tint) {
    case Tint::Grass: return rgb(0x91BD59);       // plains grass
    case Tint::Foliage: return rgb(0x77AB2F);     // plains foliage
    case Tint::FoliageBirch: return rgb(0x80A755);
    case Tint::FoliageSpruce: return rgb(0x619961);
    case Tint::Water: return rgb(0x3F76E4);
    case Tint::LilyPad: return rgb(0x208030);
    case Tint::Redstone: {
        const float f = static_cast<float>(power) / 15.0f;
        return {f * 0.6f + (power > 0 ? 0.4f : 0.3f), std::clamp(f * f * 0.7f - 0.5f, 0.0f, 1.0f), std::clamp(f * f * 0.6f - 0.7f, 0.0f, 1.0f)};
    }
    case Tint::None: break;
    }
    return glm::vec3(1.0f);
}

} // namespace bridger::render
