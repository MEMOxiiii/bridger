#pragma once

// Vulkan 1.3 renderer (docs/adr/0005): dynamic rendering, one texture array, three passes per
// frame (solid, cutout, translucent back-to-front), frustum culling per section, fog, a Y slice
// (hide everything above a height), outline boxes, and an orthographic top-down "map" mode.
// Three targets: a window surface, an offscreen image (CLI screenshots and tests), or a texture
// shared with Direct3D 11 (the desktop app composites it under its user interface; docs/adr/0009).
// No Vulkan types in this header.

#include <bridger/core/result.hpp>
#include <bridger/render/camera.hpp>
#include <bridger/render/mesher.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bridger::render {

struct RendererConfig {
    u32 width = 1280;
    u32 height = 720;
    // Window target: instance extensions the window system needs and a function that creates the
    // surface for a VkInstance (passed as void*), returning the VkSurfaceKHR handle as u64.
    // Empty `make_surface` = offscreen.
    std::vector<std::string> instance_extensions;
    std::function<u64(void* vk_instance)> make_surface;
    // Shared target (Windows): render into a D3D11 B8G8R8A8 texture passed by NT handle with
    // set_shared_target(). render() returns once the GPU has finished the frame.
    bool shared_target = false;
    bool vsync = true;
    bool validation = false; // Vulkan validation layers when available (debug builds)
};

struct FrameSettings {
    glm::vec3 sky_color{0.62f, 0.76f, 1.0f};
    float fog_start = 160.0f; // blocks; 0 disables fog
    float fog_end = 320.0f;
    std::optional<float> slice_y;     // hide blocks with world y >= slice_y
    bool map_mode = false;            // orthographic, looking straight down
    float map_half_extent = 64.0f;    // blocks from the centre to the screen edge (map mode)
    bool wireframe_sections = false;  // debug: section bounds
};

struct OutlineBox {
    glm::dvec3 min, max; // world blocks
    glm::vec4 color{1.0f, 0.85f, 0.2f, 1.0f};
};

struct FrameStats {
    u32 sections_drawn = 0;
    u32 sections_culled = 0;
    u64 quads_drawn = 0;
    double cpu_ms = 0.0;
};

class Renderer {
public:
    [[nodiscard]] static Result<Renderer> create(const RendererConfig& config);
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    ~Renderer();

    // RGBA8 16 x 16 layers (BlockModels::texture_pixels); mipmaps are generated here.
    [[nodiscard]] Status upload_textures(std::span<const u32> rgba_layers, u32 layer_count);

    // Section meshes keyed by their world origin (multiples of 16). Replaces an existing one.
    [[nodiscard]] Status upload_section(glm::ivec3 origin, const SectionMesh& mesh);
    void remove_section(glm::ivec3 origin);
    void clear_sections();
    [[nodiscard]] usize section_count() const noexcept;
    [[nodiscard]] u64 gpu_bytes() const noexcept;

    void set_outlines(std::vector<OutlineBox> boxes);

    // New target size (window resize or a different screenshot size). Not for the shared target.
    [[nodiscard]] Status resize(u32 width, u32 height);

    // Shared target only: (re)binds the D3D11 texture to render into. The caller keeps owning the
    // handle and the texture, and must not destroy them while the renderer still uses them.
    [[nodiscard]] Status set_shared_target(void* nt_handle, u32 width, u32 height);

    // The GPU's locally unique id (Windows LUID, 8 bytes) so a D3D11 device can pick the same
    // adapter; nullopt when the driver does not report one.
    [[nodiscard]] std::optional<u64> adapter_luid() const;

    // Draws one frame (window target: presents; offscreen: renders into the image).
    [[nodiscard]] Result<FrameStats> render(const Camera& camera, const FrameSettings& settings);

    // Offscreen target only: the last rendered frame as RGBA8 rows (top to bottom).
    [[nodiscard]] Result<std::vector<u8>> read_pixels();

    [[nodiscard]] std::string device_name() const;
    [[nodiscard]] u32 width() const noexcept;
    [[nodiscard]] u32 height() const noexcept;

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::render
