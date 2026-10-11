#include <bridger/render/camera.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using bridger::render::Camera;
using Catch::Approx;

TEST_CASE("Camera faces -Z by default and pitch is clamped", "[render]") {
    Camera cam;
    const glm::vec3 f = cam.forward();
    CHECK(f.x == Approx(0.0f).margin(1e-6));
    CHECK(f.z == Approx(-1.0f));

    cam.rotate(0.0f, 500.0f);
    CHECK(cam.pitch_degrees == Approx(89.0f));
}

TEST_CASE("Camera moves in local space", "[render]") {
    Camera cam;
    cam.position = {0.0, 0.0, 0.0};
    cam.move_local({0.0f, 0.0f, 10.0f}); // forward
    CHECK(cam.position.z == Approx(-10.0));
    cam.move_local({2.0f, 0.0f, 0.0f}); // right = +X when facing -Z
    CHECK(cam.position.x == Approx(2.0));
}
