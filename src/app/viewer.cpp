#include "viewer.hpp"

#include <algorithm>
#include <cmath>

namespace bridger::app {
namespace {

constexpr float kOrbitDegreesPerPixel = 0.3f;
constexpr float kLookDegreesPerPixel = 0.12f;
constexpr double kZoomStep = 0.85; // distance factor per wheel step

// Frame-rate independent exponential smoothing toward a goal.
// Snaps when close, so the camera comes to rest exactly (the app stops redrawing then).
double approach(double current, double goal, double rate, float dt) {
    const double next = goal + (current - goal) * std::exp(-rate * static_cast<double>(dt));
    return std::abs(next - goal) < 1e-4 * std::max(1.0, std::abs(goal)) ? goal : next;
}

float approach_angle(float current, float goal, float rate, float dt) {
    return static_cast<float>(approach(static_cast<double>(current), static_cast<double>(goal), static_cast<double>(rate), dt));
}

glm::dvec3 forward_of(float yaw_degrees, float pitch_degrees) {
    render::Camera c;
    c.yaw_degrees = yaw_degrees;
    c.pitch_degrees = pitch_degrees;
    return glm::dvec3(c.forward());
}

double fit_distance(double radius, float fov_y_degrees, float aspect) {
    const double fov = glm::radians(static_cast<double>(fov_y_degrees)) * std::min(1.0, static_cast<double>(aspect));
    return radius / std::tan(fov * 0.5) * 0.9;
}

} // namespace

void ViewerController::frame(glm::dvec3 lo, glm::dvec3 hi, float aspect) {
    scene_radius_ = std::max(2.0, glm::length(hi - lo) * 0.5);
    goal_target_ = target_ = (lo + hi) * 0.5;
    goal_yaw_ = yaw_ = -35.0f;
    goal_pitch_ = pitch_ = -28.0f;
    goal_distance_ = distance_ = fit_distance(scene_radius_, camera_.fov_y_degrees, aspect);
    goal_map_extent_ = map_extent_ = std::max(hi.x - lo.x, hi.z - lo.z) * 0.55 + 1.0;
    camera_.far_plane = static_cast<float>(std::max(4096.0, distance_ * 4.0));
    fly_speed = static_cast<float>(std::clamp(scene_radius_ * 0.6, 4.0, 30.0)); // crossing a build takes a few seconds
    if (fly_) set_fly(false);
    apply_orbit();
}

void ViewerController::focus(glm::dvec3 lo, glm::dvec3 hi, float aspect) {
    if (fly_) set_fly(false);
    const double radius = std::max(1.5, glm::length(hi - lo) * 0.5);
    goal_target_ = (lo + hi) * 0.5;
    goal_distance_ = std::max(4.0, fit_distance(radius, camera_.fov_y_degrees, aspect) * 1.6);
    goal_map_extent_ = std::max(8.0, radius * 2.0);
}

void ViewerController::orbit(float dx, float dy) {
    goal_yaw_ += dx * kOrbitDegreesPerPixel;
    goal_pitch_ = std::clamp(goal_pitch_ - dy * kOrbitDegreesPerPixel, -89.0f, 89.0f);
}

void ViewerController::pan(float dx, float dy, float viewport_height) {
    const double h = static_cast<double>(std::max(1.0f, viewport_height));
    if (map_) {
        const double per_pixel = 2.0 * goal_map_extent_ / h;
        goal_target_ += glm::dvec3(-static_cast<double>(dx) * per_pixel, 0.0, -static_cast<double>(dy) * per_pixel);
        return;
    }
    // World units per pixel at the target's distance.
    const double per_pixel = 2.0 * distance_ * std::tan(glm::radians(static_cast<double>(camera_.fov_y_degrees)) * 0.5) / h;
    const glm::dvec3 f = forward_of(yaw_, pitch_);
    const glm::dvec3 r = glm::normalize(glm::cross(f, glm::dvec3(0, 1, 0)));
    const glm::dvec3 u = glm::cross(r, f);
    goal_target_ += (-r * static_cast<double>(dx) + u * static_cast<double>(dy)) * per_pixel;
}

void ViewerController::zoom(float steps) {
    const double factor = std::pow(kZoomStep, static_cast<double>(steps));
    if (map_) {
        goal_map_extent_ = std::clamp(goal_map_extent_ * factor, 4.0, 4096.0);
        return;
    }
    if (fly_) {
        fly_speed = std::clamp(fly_speed / static_cast<float>(factor), 1.0f, 400.0f);
        return;
    }
    goal_distance_ = std::clamp(goal_distance_ * factor, 1.0, std::max(64.0, scene_radius_ * 20.0));
}

void ViewerController::set_fly(bool on) {
    if (on == fly_) return;
    fly_ = on;
    velocity_ = glm::dvec3(0.0);
    if (on) {
        map_ = false;
        // Start from where the orbit camera is.
        distance_ = goal_distance_;
        target_ = goal_target_;
        yaw_ = goal_yaw_;
        pitch_ = goal_pitch_;
        apply_orbit();
        return;
    }
    // Back to orbiting around a point in front of the camera.
    const glm::dvec3 f(camera_.forward());
    goal_distance_ = distance_ = std::clamp(distance_, 4.0, 64.0);
    goal_target_ = target_ = camera_.position + f * distance_;
    goal_yaw_ = yaw_ = camera_.yaw_degrees;
    goal_pitch_ = pitch_ = camera_.pitch_degrees;
}

void ViewerController::look(float dx, float dy) {
    if (!fly_) return;
    camera_.rotate(dx * kLookDegreesPerPixel, -dy * kLookDegreesPerPixel);
}

void ViewerController::set_map(bool on) {
    if (on && fly_) set_fly(false);
    map_ = on;
}

void ViewerController::update(float dt, const FlyKeys& k) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    if (fly_) {
        // Horizontal movement follows the view direction (like the game), vertical is absolute.
        const double yaw = glm::radians(static_cast<double>(camera_.yaw_degrees));
        const glm::dvec3 fwd(std::sin(yaw), 0.0, -std::cos(yaw));
        const glm::dvec3 right(std::cos(yaw), 0.0, std::sin(yaw));
        glm::dvec3 wish(0.0);
        if (k.forward) wish += fwd;
        if (k.back) wish -= fwd;
        if (k.right) wish += right;
        if (k.left) wish -= right;
        if (k.up) wish.y += 1.0;
        if (k.down) wish.y -= 1.0;
        if (glm::length(wish) > 0.0) wish = glm::normalize(wish);
        const double speed = static_cast<double>(fly_speed) * (k.sprint ? 3.0 : 1.0);
        velocity_ = glm::mix(wish * speed, velocity_, std::exp(-10.0 * static_cast<double>(dt))); // quick acceleration and stop
        camera_.position += velocity_ * static_cast<double>(dt);
        return;
    }
    constexpr double kRate = 14.0;
    target_.x = approach(target_.x, goal_target_.x, kRate, dt);
    target_.y = approach(target_.y, goal_target_.y, kRate, dt);
    target_.z = approach(target_.z, goal_target_.z, kRate, dt);
    distance_ = approach(distance_, goal_distance_, kRate, dt);
    yaw_ = approach_angle(yaw_, goal_yaw_, static_cast<float>(kRate), dt);
    pitch_ = approach_angle(pitch_, goal_pitch_, static_cast<float>(kRate), dt);
    map_extent_ = approach(map_extent_, goal_map_extent_, kRate, dt);
    apply_orbit();
}

void ViewerController::apply_orbit() {
    camera_.yaw_degrees = yaw_;
    camera_.pitch_degrees = pitch_;
    if (map_) {
        // The renderer looks down from 512 blocks above camera.position (renderer.cpp, map mode).
        camera_.position = glm::dvec3(target_.x, 0.0, target_.z);
        return;
    }
    camera_.position = target_ - forward_of(yaw_, pitch_) * distance_;
}

} // namespace bridger::app
