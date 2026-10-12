// Camera control of the desktop viewer (src/app/viewer.cpp; no window or GPU involved).

#include <viewer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using bridger::app::FlyKeys;
using bridger::app::ViewerController;
using Catch::Approx;

namespace {

void settle(ViewerController& v, float seconds = 3.0f) {
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) v.update(1.0f / 60.0f, {});
}

double distance_to(const ViewerController& v, glm::dvec3 p) { return glm::length(v.camera().position - p); }

} // namespace

TEST_CASE("Framing looks at the box centre from outside it", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {16, 8, 16}, 16.0f / 9.0f);
    const glm::dvec3 centre(8, 4, 8);
    CHECK(glm::length(v.target() - centre) == Approx(0.0).margin(1e-9));
    CHECK(distance_to(v, centre) > 12.0);
    // The camera looks toward the centre.
    const glm::dvec3 to_centre = glm::normalize(centre - v.camera().position);
    CHECK(glm::dot(to_centre, glm::dvec3(v.camera().forward())) == Approx(1.0).margin(1e-4));
}

TEST_CASE("Orbit keeps the distance to the target and comes to rest", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {16, 16, 16}, 1.5f);
    const double d0 = distance_to(v, v.target());
    const glm::dvec3 p0 = v.camera().position;
    v.orbit(200.0f, -50.0f);
    settle(v);
    CHECK(distance_to(v, v.target()) == Approx(d0).epsilon(1e-6));
    CHECK(glm::length(v.camera().position - p0) > 1.0);
    // At rest the camera stops changing exactly (the app stops redrawing).
    const glm::dvec3 rest = v.camera().position;
    v.update(1.0f / 60.0f, {});
    CHECK(v.camera().position == rest);
}

TEST_CASE("Pitch is clamped while orbiting", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {8, 8, 8}, 1.0f);
    v.orbit(0.0f, -100000.0f);
    settle(v);
    CHECK(v.camera().pitch_degrees <= 89.0f);
}

TEST_CASE("Zoom moves toward the target within limits", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {16, 16, 16}, 1.5f);
    const double d0 = distance_to(v, v.target());
    v.zoom(3.0f);
    settle(v);
    CHECK(distance_to(v, v.target()) < d0);
    v.zoom(1000.0f);
    settle(v);
    CHECK(distance_to(v, v.target()) >= 1.0 - 1e-9);
}

TEST_CASE("Pan moves the target sideways, not along the view", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {16, 16, 16}, 1.5f);
    const glm::dvec3 t0 = v.target();
    v.pan(100.0f, 0.0f, 900.0f);
    settle(v);
    const glm::dvec3 moved = v.target() - t0;
    CHECK(glm::length(moved) > 0.1);
    CHECK(std::abs(glm::dot(glm::normalize(moved), glm::dvec3(v.camera().forward()))) < 1e-3);
}

TEST_CASE("Fly mode moves with WASD and returns to orbiting in front of the camera", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {16, 16, 16}, 1.5f);
    settle(v);
    v.set_fly(true);
    REQUIRE(v.fly());
    const glm::dvec3 p0 = v.camera().position;
    FlyKeys keys;
    keys.forward = true;
    for (int i = 0; i < 60; ++i) v.update(1.0f / 60.0f, keys);
    const glm::dvec3 step = v.camera().position - p0;
    CHECK(step.y == Approx(0.0).margin(1e-9)); // W is horizontal, like the game
    CHECK(glm::length(step) > 1.0);

    FlyKeys up;
    up.up = true;
    const double y0 = v.camera().position.y;
    for (int i = 0; i < 30; ++i) v.update(1.0f / 60.0f, up);
    CHECK(v.camera().position.y > y0);

    v.set_fly(false);
    CHECK_FALSE(v.fly());
    const glm::dvec3 ahead = v.camera().position + glm::dvec3(v.camera().forward()) * distance_to(v, v.target());
    CHECK(glm::length(ahead - v.target()) == Approx(0.0).margin(1e-3));
}

TEST_CASE("Map mode zooms its extent and leaves fly mode", "[app]") {
    ViewerController v;
    v.frame({0, 0, 0}, {32, 16, 32}, 1.5f);
    v.set_fly(true);
    v.set_map(true);
    CHECK(v.map());
    CHECK_FALSE(v.fly());
    settle(v);
    const float e0 = v.map_half_extent();
    v.zoom(2.0f);
    settle(v);
    CHECK(v.map_half_extent() < e0);
}
