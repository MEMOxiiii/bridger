#include <bridger/render/camera.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace bridger::render {

glm::vec3 Camera::forward() const noexcept {
    const float yaw = glm::radians(yaw_degrees);
    const float pitch = glm::radians(pitch_degrees);
    return glm::normalize(glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                    -std::cos(yaw) * std::cos(pitch)));
}

glm::vec3 Camera::right() const noexcept {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 Camera::view(const glm::dvec3& origin) const noexcept {
    const glm::vec3 eye = glm::vec3(position - origin);
    return glm::lookAtRH(eye, eye + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::projection(float aspect) const noexcept {
    glm::mat4 p = glm::perspectiveRH_ZO(glm::radians(fov_y_degrees), aspect, near_plane, far_plane);
    p[1][1] *= -1.0f; // Vulkan clip space has Y pointing down
    return p;
}

void Camera::rotate(float delta_yaw_degrees, float delta_pitch_degrees) noexcept {
    yaw_degrees = std::fmod(yaw_degrees + delta_yaw_degrees, 360.0f);
    pitch_degrees = std::clamp(pitch_degrees + delta_pitch_degrees, -89.0f, 89.0f);
}

void Camera::move_local(const glm::vec3& delta) noexcept {
    const glm::vec3 offset = right() * delta.x + glm::vec3(0.0f, 1.0f, 0.0f) * delta.y + forward() * delta.z;
    position += glm::dvec3(offset);
}

} // namespace bridger::render
