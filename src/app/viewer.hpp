#pragma once

// Camera control of the desktop viewer (no OS or GPU code; tests/unit/app/viewer_test.cpp).
// Three modes:
//  - orbit (default): drag to orbit around a target, pan, wheel to zoom; motion is smoothed;
//  - fly: Minecraft-style free camera (WASD, Space/Shift up/down, Ctrl to sprint, mouse look);
//  - map: orthographic top-down view, drag to pan, wheel to zoom.
// focus() animates the camera to a box (report highlights, "frame").

#include <bridger/render/camera.hpp>

#include <glm/glm.hpp>

namespace bridger::app {

struct FlyKeys {
    bool forward = false, back = false, left = false, right = false, up = false, down = false, sprint = false;
};

class ViewerController {
public:
    // Frames a box from the south-east, slightly above (immediately, no animation).
    void frame(glm::dvec3 box_min, glm::dvec3 box_max, float aspect);
    // Animates to a box, keeping the current direction.
    void focus(glm::dvec3 box_min, glm::dvec3 box_max, float aspect);

    // Pointer deltas in pixels; `viewport_height` scales panning so the scene follows the pointer.
    void orbit(float dx, float dy);
    void pan(float dx, float dy, float viewport_height);
    void zoom(float wheel_steps); // > 0 zooms in

    void set_fly(bool on);
    [[nodiscard]] bool fly() const noexcept { return fly_; }
    void look(float dx, float dy); // fly mode: mouse look (pixels)

    void set_map(bool on);
    [[nodiscard]] bool map() const noexcept { return map_; }
    [[nodiscard]] float map_half_extent() const noexcept { return static_cast<float>(map_extent_); }

    // Advances smoothing and fly movement by `dt` seconds.
    void update(float dt, const FlyKeys& keys);

    [[nodiscard]] const render::Camera& camera() const noexcept { return camera_; }
    [[nodiscard]] glm::dvec3 target() const noexcept { return target_; }
    // Where world streaming should centre: what the user looks at (the orbit target, the map
    // centre) or, when flying, the camera itself.
    [[nodiscard]] glm::dvec3 stream_center() const noexcept { return fly_ ? camera_.position : target_; }

    float fly_speed = 10.0f; // blocks per second (sprint: x3)

private:
    void apply_orbit();

    render::Camera camera_;
    // Orbit: goal values (inputs) and current values (smoothed toward the goal).
    glm::dvec3 target_{0.0}, goal_target_{0.0};
    double distance_ = 32.0, goal_distance_ = 32.0;
    float yaw_ = -35.0f, goal_yaw_ = -35.0f;
    float pitch_ = -28.0f, goal_pitch_ = -28.0f;
    bool fly_ = false;
    glm::dvec3 velocity_{0.0};
    bool map_ = false;
    double map_extent_ = 64.0, goal_map_extent_ = 64.0;
    double scene_radius_ = 16.0;
};

} // namespace bridger::app
