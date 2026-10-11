#pragma once

// Free-fly camera. Right-handed, +Y up (Minecraft convention), Vulkan clip space
// (depth 0..1, Y down) handled in projection().

#include <glm/glm.hpp>

namespace bridger::render {

class Camera {
public:
    glm::dvec3 position{0.0, 80.0, 0.0}; // double: worlds span +-30M blocks
    float yaw_degrees = 0.0f;            // 0 = looking toward -Z
    float pitch_degrees = 0.0f;          // clamped to (-89, 89)
    float fov_y_degrees = 70.0f;
    float near_plane = 0.05f;
    float far_plane = 4096.0f;

    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::vec3 right() const noexcept;

    // View matrix relative to `origin` (camera-relative rendering keeps float precision far from 0,0).
    [[nodiscard]] glm::mat4 view(const glm::dvec3& origin) const noexcept;
    [[nodiscard]] glm::mat4 projection(float aspect) const noexcept;

    void rotate(float delta_yaw_degrees, float delta_pitch_degrees) noexcept;
    void move_local(const glm::vec3& delta) noexcept; // x = right, y = up, z = forward
};

} // namespace bridger::render
